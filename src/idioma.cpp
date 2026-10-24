// idioma.cpp: the UI language and the translations (copied from wa-lite and Zas).
//
// explorer-lite is in English unless another language is chosen (tray menu > Language): Windows' language does not
// decide it. The choice is saved in explorer-lite.conf (data_dir()) as language=<code>; language=auto follows Windows.
//
// UI strings are written in English (the source: "en" needs no table) and passed through T(L"...");
// a template with holes goes through Tf(L"{0} items", { n }). The translations come from
// i18n\<code>.json: i18n\generar.py writes i18n_table.inc.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include "idioma.h"
#include "util.h"

namespace {

struct Pair {
    const wchar_t *src, *tr;
};
struct Table {
    const char *code;
    const Pair *pairs;
    size_t count;
};

#include "i18n_table.inc" // kLanguages[] and kTables[] (generated)

std::wstring conf_path() { return data_dir() + L"\\explorer-lite.conf"; }

std::string read_conf() {
    std::wstring path = conf_path();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string text;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
    CloseHandle(f);
    return text;
}

// explorer-lite.conf: language=<code> or language=auto
std::string conf_lang() {
    std::string text = read_conf(), lang;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (line.compare(0, 9, "language=") == 0) lang = line.substr(9);
        pos = end + 1;
    }
    return lang;
}

// explorer-lite's code for a language name ("pt-BR", "fr-CA", "zh-Hant-TW"...), or NULL
const char *supported(const std::string &name) {
    std::string n = name;
    for (char &c : n) c = (char)tolower((unsigned char)c);
    if (n.empty()) return NULL;
    auto has = [](const char *code) -> const char * {
        for (const UiLanguage &l : kLanguages)
            if (strcmp(l.code, code) == 0) return l.code;
        return NULL;
    };
    // variants with their own translation
    if (n.compare(0, 2, "pt") == 0) return has(n.find("pt-pt") == 0 ? "pt-PT" : "pt-BR");
    if (n.compare(0, 2, "zh") == 0)
        return has(n.find("hant") != std::string::npos || n.find("-tw") != std::string::npos || n.find("-hk") != std::string::npos ||
                           n.find("-mo") != std::string::npos ? "zh-TW" : "zh-CN");
    if (n.compare(0, 2, "sr") == 0 || n.compare(0, 2, "bs") == 0) return has("sr-Latn");
    if (n.compare(0, 2, "nb") == 0 || n.compare(0, 2, "nn") == 0 || n.compare(0, 2, "no") == 0) return has("nb");
    for (const UiLanguage &l : kLanguages) {
        size_t k = strlen(l.code);
        if (n == l.code || (n.compare(0, k, l.code) == 0 && (n.size() == k || n[k] == '-'))) return l.code;
    }
    // "ca-ES", "eu-ES"...: by the two-letter code
    std::string two = n.substr(0, n.find('-'));
    for (const UiLanguage &l : kLanguages)
        if (two == l.code) return l.code;
    return NULL;
}

const char *windows_lang() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0)) return "en";
    char a[LOCALE_NAME_MAX_LENGTH] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, a, sizeof(a), NULL, NULL);
    const char *code = supported(a);
    return code ? code : "en";
}

const char *detect() {
    // tests: EXPLORER_LITE_LANG overrides everything
    char env[32];
    DWORD n = GetEnvironmentVariableA("EXPLORER_LITE_LANG", env, sizeof(env));
    if (n > 0 && n < sizeof(env)) {
        if (const char *code = supported(env)) return code;
    }
    std::string c = conf_lang();
    if (c == "auto") return windows_lang();
    if (const char *code = supported(c)) return code;
    return "en";
}

const char *g_lang;
std::unordered_map<std::wstring, const wchar_t *> g_table;

void load_table() {
    g_table.clear();
    for (const Table &t : kTables)
        if (strcmp(t.code, g_lang) == 0)
            for (size_t i = 0; i < t.count; i++) g_table[t.pairs[i].src] = t.pairs[i].tr;
}

} // namespace

const char *ui_lang() {
    if (!g_lang) {
        g_lang = detect();
        load_table();
    }
    return g_lang;
}

void set_ui_lang(const char *code) {
    // explorer-lite.conf: the language= line is changed (or added); the rest, as is
    std::string text = read_conf(), out;
    bool done = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        size_t next = text.find_first_not_of("\r\n", end);
        std::string eol = text.substr(end, (next == std::string::npos ? text.size() : next) - end);
        if (line.compare(0, 9, "language=") == 0) {
            if (!done) out += std::string("language=") + code + (eol.empty() ? "\n" : eol);
            done = true;
        } else {
            out += line + eol;
        }
        pos = next == std::string::npos ? text.size() : next;
    }
    if (!done) out += (out.empty() || out.back() == '\n' ? "" : "\n") + std::string("language=") + code + "\n";
    HANDLE f = CreateFileW(conf_path().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(f, out.data(), (DWORD)out.size(), &put, NULL);
        CloseHandle(f);
    }
    const char *c = supported(code);
    g_lang = c ? c : std::string(code) == "auto" ? windows_lang() : "en";
    load_table();
}

bool ui_lang_auto() {
    char env[32];
    DWORD n = GetEnvironmentVariableA("EXPLORER_LITE_LANG", env, sizeof(env));
    if (n > 0 && n < sizeof(env) && supported(env)) return false;
    return conf_lang() == "auto";
}

const wchar_t *T(const wchar_t *src) {
    if (!src) return src;
    ui_lang();
    if (g_table.empty()) return src;
    auto it = g_table.find(src);
    return it == g_table.end() ? src : it->second;
}

std::wstring Tf(const wchar_t *src, std::initializer_list<std::wstring> args) {
    std::wstring s = T(src);
    int i = 0;
    for (const std::wstring &a : args) {
        std::wstring hole = L"{" + std::to_wstring(i++) + L"}";
        size_t p = s.find(hole);
        if (p != std::wstring::npos) s.replace(p, hole.size(), a);
    }
    return s;
}

const wchar_t *ui_font() {
    std::string l = ui_lang();
    if (l == "th") return L"Leelawadee UI";
    if (l == "hi") return L"Nirmala UI";
    if (l == "ja") return L"Yu Gothic UI";
    if (l == "ko") return L"Malgun Gothic";
    if (l == "zh-CN") return L"Microsoft YaHei UI";
    if (l == "zh-TW") return L"Microsoft JhengHei UI";
    return NULL;
}

const UiLanguage *ui_languages(int *count) {
    *count = (int)(sizeof(kLanguages) / sizeof(kLanguages[0]));
    return kLanguages;
}
