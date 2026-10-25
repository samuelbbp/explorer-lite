// conf.cpp: see conf.h.
#include "conf.h"
#include <cstdlib>
#include <cstring>
#include <vector>
#include "util.h"

namespace {

std::vector<std::string> g_lines;
bool g_loaded;

std::wstring path() { return data_dir() + L"\\explorer-lite.conf"; }

void load() {
    if (g_loaded) return;
    g_loaded = true;
    HANDLE f = CreateFileW(path().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string text;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
    CloseHandle(f);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        if (end > pos) g_lines.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }
}

} // namespace

std::string conf_get(const char *key, const char *def) {
    load();
    size_t k = strlen(key);
    for (const std::string &l : g_lines)
        if (l.size() > k && l.compare(0, k, key) == 0 && l[k] == '=') return l.substr(k + 1);
    return def;
}

int conf_int(const char *key, int def) {
    std::string v = conf_get(key);
    if (v.empty()) return def;
    char *end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    return end && end != v.c_str() ? (int)n : def;
}

void conf_reload() {
    g_loaded = false;
    g_lines.clear();
}

void conf_set(const char *key, const std::string &value) {
    load();
    // anything another writer (idioma.cpp) wrote since it was read: re-read before saving
    g_loaded = false;
    g_lines.clear();
    load();
    size_t k = strlen(key);
    bool found = false;
    for (std::string &l : g_lines)
        if (l.size() > k && l.compare(0, k, key) == 0 && l[k] == '=') {
            l = std::string(key) + "=" + value;
            found = true;
        }
    if (!found) g_lines.push_back(std::string(key) + "=" + value);
    std::string out;
    for (const std::string &l : g_lines) out += l + "\r\n";
    std::wstring tmp = path() + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, out.data(), (DWORD)out.size(), &put, NULL);
    CloseHandle(f);
    if (put == out.size()) MoveFileExW(tmp.c_str(), path().c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}
