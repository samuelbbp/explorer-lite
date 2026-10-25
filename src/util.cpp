// util.cpp: see util.h.
#include "util.h"
#include <cstdio>
#include <cwctype>

double now_ms() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

double since_process_start_ms() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = created.dwLowDateTime;
    a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    return b.QuadPart > a.QuadPart ? (b.QuadPart - a.QuadPart) / 10000.0 : 0;
}

std::string utf8(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring exe_dir() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

std::wstring exe_path() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    return std::wstring(exe, n);
}

namespace {
std::wstring g_instance;
}

void set_instance(const std::wstring &name) {
    g_instance.clear();
    for (wchar_t ch : name) // a name for paths and kernel objects: letters, digits, - and _
        if (iswalnum(ch) || ch == L'-' || ch == L'_') g_instance += ch;
}

const std::wstring &instance() { return g_instance; }

std::wstring instance_suffix() { return g_instance.empty() ? L"" : L"-" + g_instance; }

std::wstring data_dir() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t la[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        dir = (n && n < MAX_PATH ? std::wstring(la, n) : exe_dir()) + L"\\explorer-lite" + instance_suffix();
        CreateDirectoryW(dir.c_str(), NULL);
    }
    return dir;
}

std::wstring shot_path(const std::wstring &name) {
    if (name.size() < 5 || name.size() > 100 || _wcsicmp(name.c_str() + name.size() - 4, L".png") != 0) return L"";
    for (wchar_t ch : name)
        if (!(iswalnum(ch) || ch == L'-' || ch == L'_' || ch == L'.')) return L"";
    if (name.find(L"..") != std::wstring::npos) return L"";
    std::wstring dir = data_dir() + L"\\shots";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir + L"\\" + name;
}

void log_line(const std::string &line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE f = CreateFileW((data_dir() + L"\\explorer-lite.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string s = stamp + line + "\r\n";
    DWORD n = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &n, NULL);
    CloseHandle(f);
}
