// update.cpp: see update.h. WinHTTP and BCrypt are loaded by hand (no import libraries).
#include "update.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>
#include "conf.h"
#include "json.h"

UpdateInfo g_upd;

namespace {

const wchar_t kApiHost[] = L"api.github.com";
// releases/latest never returns a prerelease: the app only follows the stable channel (prereleases are
// developer builds, installed by hand)
const wchar_t kApiPath[] = L"/repos/justdiegocom/explorer-lite-releases/releases/latest";

HWND g_wnd;
UINT g_msg;
SRWLOCK g_lock = SRWLOCK_INIT;
UpdateInfo g_next;   // what the thread leaves for the UI
bool g_has_next;
volatile LONG g_busy;

// --- WinHTTP ---------------------------------------------------------------------------
typedef HINTERNET(WINAPI *POpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL(WINAPI *PSetTimeouts)(HINTERNET, int, int, int, int);
typedef HINTERNET(WINAPI *PConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *POpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *PSend)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *PReceive)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *PQueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *PRead)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *PQueryOption)(HINTERNET, DWORD, LPVOID, LPDWORD);
typedef BOOL(WINAPI *PClose)(HINTERNET);
typedef BOOL(WINAPI *PCrack)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
POpen h_open;
PSetTimeouts h_timeouts;
PConnect h_connect;
POpenRequest h_request;
PSend h_send;
PReceive h_receive;
PQueryHeaders h_headers;
PRead h_read;
PQueryOption h_option;
PClose h_close;
PCrack h_crack;

bool load_winhttp() {
    static int ok = -1;
    if (ok >= 0) return ok == 1;
    HMODULE m = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ok = 0;
    if (!m) return false;
#define X(v, n) v = (decltype(v))(void *)GetProcAddress(m, n); if (!v) return false;
    X(h_open, "WinHttpOpen") X(h_timeouts, "WinHttpSetTimeouts") X(h_connect, "WinHttpConnect") X(h_request, "WinHttpOpenRequest")
    X(h_send, "WinHttpSendRequest") X(h_receive, "WinHttpReceiveResponse") X(h_headers, "WinHttpQueryHeaders") X(h_read, "WinHttpReadData")
    X(h_option, "WinHttpQueryOption") X(h_close, "WinHttpCloseHandle") X(h_crack, "WinHttpCrackUrl")
#undef X
    ok = 1;
    return true;
}

// GitHub only: the API, github.com (where the asset points) and its download servers
bool allowed_host(const std::wstring &h) {
    auto ends = [&](const wchar_t *s) {
        size_t n = wcslen(s);
        return h.size() >= n && _wcsicmp(h.c_str() + h.size() - n, s) == 0;
    };
    return _wcsicmp(h.c_str(), L"api.github.com") == 0 || _wcsicmp(h.c_str(), L"github.com") == 0 || ends(L".githubusercontent.com");
}

// GET over HTTPS; `out` (memory, at most max_mem) or `file`; prog: what was downloaded and the total
bool http_get(const std::wstring &url, std::string *out, HANDLE file, size_t max_mem, void (*prog)(int64_t, int64_t), std::wstring &err) {
    if (!load_winhttp()) {
        err = L"WinHTTP";
        return false;
    }
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!h_crack(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || !allowed_host(host)) {
        err = L"URL not allowed"; // translate
        return false;
    }
    std::wstring agent = L"explorer-lite/" + wide(EXPLORER_LITE_VERSION);
    HINTERNET s = h_open(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = h_open(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        err = L"WinHTTP";
        return false;
    }
    h_timeouts(s, 15000, 15000, 15000, file ? 60000 : 15000);
    bool ok = false;
    HINTERNET c = h_connect(s, host, uc.nPort, 0);
    HINTERNET r = c ? h_request(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && h_send(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        h_receive(r, NULL)) {
        // after redirects (github.com -> its download servers): still GitHub and HTTPS
        wchar_t final_url[2048] = L"";
        DWORD fl = sizeof(final_url);
        URL_COMPONENTS fc = { sizeof(fc) };
        wchar_t fhost[256] = L"";
        fc.lpszHostName = fhost;
        fc.dwHostNameLength = 256;
        bool safe = h_option(r, WINHTTP_OPTION_URL, final_url, &fl) && h_crack(final_url, 0, 0, &fc) && fc.nScheme == INTERNET_SCHEME_HTTPS && allowed_host(fhost);
        DWORD status = 0, sl = sizeof(status);
        h_headers(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
        wchar_t lenbuf[32] = L"";
        DWORD ll = sizeof(lenbuf);
        int64_t total = 0;
        if (h_headers(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenbuf, &ll, WINHTTP_NO_HEADER_INDEX)) total = _wtoi64(lenbuf);
        if (!safe) err = L"redirected outside GitHub"; // translate
        else if (status != 200) err = L"HTTP " + std::to_wstring(status);
        else {
            ok = true;
            std::vector<char> buf(64 << 10);
            int64_t done = 0;
            for (;;) {
                DWORD got = 0;
                if (!h_read(r, buf.data(), (DWORD)buf.size(), &got)) {
                    ok = false;
                    err = L"read error"; // translate
                    break;
                }
                if (!got) break;
                done += got;
                if (out) {
                    if (out->size() + got > max_mem) {
                        ok = false;
                        err = L"response too large"; // translate
                        break;
                    }
                    out->append(buf.data(), got);
                } else {
                    DWORD put = 0;
                    if (!WriteFile(file, buf.data(), got, &put, NULL) || put != got) {
                        ok = false;
                        err = L"disk error"; // translate
                        break;
                    }
                }
                if (prog) prog(done, total);
            }
        }
    } else if (err.empty()) {
        err = L"no connection"; // translate
    }
    if (r) h_close(r);
    if (c) h_close(c);
    h_close(s);
    return ok;
}

// --- SHA256 of the file with BCrypt ------------------------------------------------------
std::string sha256_file(const std::wstring &path) {
    typedef NTSTATUS(WINAPI * POpenAlg)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
    typedef NTSTATUS(WINAPI * PCreate)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    typedef NTSTATUS(WINAPI * PData)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    typedef NTSTATUS(WINAPI * PFinish)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    typedef NTSTATUS(WINAPI * PDestroy)(BCRYPT_HASH_HANDLE);
    typedef NTSTATUS(WINAPI * PCloseAlg)(BCRYPT_ALG_HANDLE, ULONG);
    HMODULE m = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return "";
    POpenAlg open_alg = (POpenAlg)(void *)GetProcAddress(m, "BCryptOpenAlgorithmProvider");
    PCreate create = (PCreate)(void *)GetProcAddress(m, "BCryptCreateHash");
    PData data = (PData)(void *)GetProcAddress(m, "BCryptHashData");
    PFinish finish = (PFinish)(void *)GetProcAddress(m, "BCryptFinishHash");
    PDestroy destroy = (PDestroy)(void *)GetProcAddress(m, "BCryptDestroyHash");
    PCloseAlg close_alg = (PCloseAlg)(void *)GetProcAddress(m, "BCryptCloseAlgorithmProvider");
    if (!open_alg || !create || !data || !finish || !destroy || !close_alg) return "";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    if (open_alg(&alg, L"SHA256", NULL, 0) != 0) return "";
    std::string hex;
    if (create(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<unsigned char> buf(1 << 20);
            DWORD got = 0;
            bool ok = true;
            while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, NULL) && got) ok &= data(h, buf.data(), got, 0) == 0;
            CloseHandle(f);
            unsigned char dig[32];
            if (ok && finish(h, dig, 32, 0) == 0) {
                char b[3];
                for (unsigned char d : dig) {
                    sprintf(b, "%02x", d);
                    hex += b;
                }
            }
        }
        destroy(h);
    }
    close_alg(alg, 0);
    return hex;
}

void publish(const UpdateInfo &u) {
    AcquireSRWLockExclusive(&g_lock);
    g_next = u;
    g_has_next = true;
    ReleaseSRWLockExclusive(&g_lock);
    if (g_wnd) PostMessageW(g_wnd, g_msg, 0, 0);
}

UpdateInfo snapshot() {
    AcquireSRWLockShared(&g_lock);
    UpdateInfo u = g_has_next ? g_next : g_upd;
    ReleaseSRWLockShared(&g_lock);
    return u;
}

DWORD WINAPI check_thread(void *p) {
    bool manual = p != nullptr;
    UpdateInfo u = snapshot();
    u.manual = manual;
    std::string body;
    std::wstring err;
    if (!http_get(std::wstring(L"https://") + kApiHost + kApiPath, &body, NULL, 2 << 20, nullptr, err)) {
        u.state = manual ? UP_FAILED : UP_IDLE;
        u.error = err;
        publish(u);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    Json j;
    std::string tag;
    if (Json::parse(body.data(), body.size(), j) && j.type == Json::Obj) tag = j["tag_name"].str();
    if (tag.empty() || !newer_version(tag, EXPLORER_LITE_VERSION)) {
        u.state = manual ? UP_LATEST : UP_IDLE;
        publish(u);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    u.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    u.notes = wide(j["body"].str());
    u.url.clear();
    u.sha_url.clear();
    std::string setup = "explorer-lite-Setup-" + u.version + ".exe";
    const Json &as = j["assets"];
    for (size_t i = 0; i < as.size(); i++) {
        std::string name = as[i]["name"].str();
        if (name == setup) {
            u.url = as[i]["browser_download_url"].str();
            u.size = as[i]["size"].i64();
        } else if (name == setup + ".sha256") {
            u.sha_url = as[i]["browser_download_url"].str();
        }
    }
    u.state = u.url.empty() ? (manual ? UP_FAILED : UP_IDLE) : UP_AVAILABLE;
    if (u.url.empty()) u.error = L"the release has no installer"; // translate
    publish(u);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

void on_progress(int64_t done, int64_t total) {
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 200 && done < total) return; // the bar, five times per second
    last = now;
    UpdateInfo u = snapshot();
    u.done = done;
    if (total > 0) u.size = total;
    publish(u);
}

DWORD WINAPI download_thread(void *) {
    UpdateInfo u = snapshot();
    std::wstring err;
    // the checksum first: without a .sha256 nothing is installed
    std::string body;
    if (!u.sha_url.empty() && !http_get(wide(u.sha_url), &body, NULL, 4096, nullptr, err)) body.clear();
    std::string sha = sha256_published(body);
    if (sha.empty()) {
        u.state = UP_FAILED;
        u.error = L"the installer SHA256 sum is missing"; // translate
        publish(u);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    // data_dir()\\tmp: the installer knows it was launched from here (installer/explorer-lite.nsi) and reopens the app
    std::wstring dir = data_dir() + L"\\tmp";
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring path = dir + L"\\explorer-lite-Setup-" + wide(u.version) + L".exe";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = f != INVALID_HANDLE_VALUE && http_get(wide(u.url), nullptr, f, 0, on_progress, err);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    u = snapshot();
    if (ok && !sha256_matches(path, sha)) {
        ok = false;
        err = L"the SHA256 sum does not match"; // translate
    }
    if (!ok) {
        DeleteFileW(path.c_str());
        u.state = UP_FAILED;
        u.error = err;
    } else {
        u.state = UP_READY;
        u.file = path;
    }
    publish(u);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

} // namespace

// The release's .sha256 is "<64 hex>  <file>" (empaquetar.sh); a short or empty one counts as missing.
std::string sha256_published(const std::string &body) {
    if (body.size() < 64) return "";
    std::string sha = body.substr(0, 64);
    for (char &ch : sha) ch = (char)tolower((unsigned char)ch);
    return sha;
}

// sha256_file gives 64 lowercase hex digits or "" on any error, so a malformed `want` never matches
// and "" never matches an unreadable file.
bool sha256_matches(const std::wstring &path, const std::string &want) { return want.size() == 64 && sha256_file(path) == want; }

// Semver precedence (semver.org 11): developer builds carry a prerelease suffix
// ("0.2.8-dcbot-x-30.1"), and the release with the same numbers must still count as newer.
// Build metadata (+...) is ignored; missing numbers count as 0 ("0.1" == "0.1.0").
bool newer_version(const std::string &a, const std::string &b) {
    struct Ver {
        std::vector<int> num;
        std::vector<std::string> pre;
    };
    auto parse = [](std::string s) {
        Ver v;
        size_t at = !s.empty() && (s[0] == 'v' || s[0] == 'V') ? 1 : 0;
        size_t plus = s.find('+', at);
        if (plus != std::string::npos) s.resize(plus);
        size_t dash = s.find('-', at);
        int cur = -1;
        for (size_t i = at; i < (dash == std::string::npos ? s.size() : dash); i++) {
            char ch = s[i];
            if (ch >= '0' && ch <= '9') cur = std::min(100000000, (cur < 0 ? 0 : cur) * 10 + (ch - '0'));
            else if (cur >= 0) {
                v.num.push_back(cur);
                cur = -1;
            }
        }
        if (cur >= 0) v.num.push_back(cur);
        if (dash != std::string::npos) {
            size_t p = dash + 1;
            for (;;) {
                size_t q = s.find('.', p);
                v.pre.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        return v;
    };
    auto numeric = [](const std::string &s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; };
    Ver x = parse(a), y = parse(b);
    for (size_t i = 0; i < std::max(x.num.size(), y.num.size()); i++) {
        int p = i < x.num.size() ? x.num[i] : 0, q = i < y.num.size() ? y.num[i] : 0;
        if (p != q) return p > q;
    }
    // same numbers: a release beats any of its prereleases
    if (x.pre.empty() || y.pre.empty()) return x.pre.empty() && !y.pre.empty();
    for (size_t i = 0; i < std::min(x.pre.size(), y.pre.size()); i++) {
        const std::string &p = x.pre[i], &q = y.pre[i];
        if (p == q) continue;
        bool pn = numeric(p), qn = numeric(q);
        if (pn && qn) {
            std::string ps = p.substr(std::min(p.find_first_not_of('0'), p.size())), qs = q.substr(std::min(q.find_first_not_of('0'), q.size()));
            if (ps.size() != qs.size()) return ps.size() > qs.size();
            return ps > qs;
        }
        if (pn != qn) return qn; // numbers sort before words
        return p > q;
    }
    return x.pre.size() > y.pre.size(); // "a.1" > "a"
}

void update_init(HWND wnd, UINT msg) {
    g_wnd = wnd;
    g_msg = msg;
}

void update_check(bool manual) {
    if (InterlockedExchange(&g_busy, 1)) return;
    if (manual) {
        g_upd.state = UP_CHECKING;
        g_upd.manual = true;
    }
    HANDLE t = CreateThread(NULL, 0, check_thread, manual ? (void *)1 : nullptr, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_busy, 0);
}

void update_download() {
    if (g_upd.state != UP_AVAILABLE && g_upd.state != UP_FAILED) return;
    if (g_upd.url.empty() || InterlockedExchange(&g_busy, 1)) return;
    g_upd.state = UP_DOWNLOADING;
    g_upd.done = 0;
    publish(g_upd);
    HANDLE t = CreateThread(NULL, 0, download_thread, nullptr, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_busy, 0);
}

bool update_install() {
    if (g_upd.state != UP_READY || g_upd.file.empty()) return false;
    std::wstring cmd = L"\"" + g_upd.file + L"\" /S";
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    // detached: the installer closes this explorer-lite, puts the new one in and reopens it
    if (!CreateProcessW(g_upd.file.c_str(), line.data(), NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool update_pump() {
    AcquireSRWLockExclusive(&g_lock);
    bool has = g_has_next;
    if (has) {
        bool open = g_upd.notes_open;
        g_upd = g_next;
        g_upd.notes_open = open;
        g_has_next = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return has;
}

// on unless the user turned it off (an explicit auto_update=0 is kept)
bool update_auto() { return conf_int("auto_update", 1) != 0; }
void update_set_auto(bool on) { conf_set("auto_update", on ? "1" : "0"); }
