// stub/explorer.cpp: C:\Windows\explorer.exe in Windows Lite. Windows' explorer is gone; whatever still starts
// explorer.exe by name (apps' "show in folder", scripts, Run, Task Manager's "run new task") gets explorer-lite:
//   explorer.exe                       a window on Home, or the shell when none is running
//   explorer.exe <folder>              that folder (also /e,<folder> /n,<folder> /root,<folder>)
//   explorer.exe /select,<file>        the file's folder
//   explorer.exe shell:<name>|::{guid}  the folder it names when it has a path on disk, Home otherwise
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>

// the user's own install first (the updater keeps it current), the machine-wide copy otherwise
static std::wstring target() {
    wchar_t buf[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Programs\\explorer-lite\\explorer-lite.exe", buf, MAX_PATH);
    if (GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) return buf;
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\explorer-lite\\explorer-lite.exe", buf, MAX_PATH);
    return buf;
}

// shell:Downloads, ::{20D04FE0-...} and the like: their path on disk, if they have one
static std::wstring resolve(const std::wstring &name) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHParseDisplayName(name.c_str(), nullptr, &pidl, 0, nullptr))) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, p)) out = p;
        CoTaskMemFree(pidl);
    }
    return out;
}

static std::wstring trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'"')) i++;
    return s.substr(i);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring all;
    for (int i = 1; i < argc; i++) all += (all.empty() ? L"" : L" ") + std::wstring(argv[i]);

    // explorer's switches come comma-separated: /e,C:\x  /select,"C:\x\y.txt"
    std::wstring path;
    bool select = false;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t c = all.find(L',', pos);
        std::wstring part = trim(all.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos));
        pos = c == std::wstring::npos ? all.size() + 1 : c + 1;
        if (part.empty()) continue;
        if (_wcsicmp(part.c_str(), L"/select") == 0) select = true;
        else if (part[0] == L'/') continue; // /e /n /root /separate
        else path = part;
    }
    if (!path.empty() && (path.rfind(L"shell:", 0) == 0 || path.rfind(L"::", 0) == 0)) path = resolve(path);
    if (select && !path.empty()) {
        size_t s = path.find_last_of(L"\\/");
        if (s != std::wstring::npos) path = path.substr(0, s);
    }

    std::wstring args;
    if (path.empty() && !all.size() && !FindWindowW(L"Shell_TrayWnd", nullptr)) args = L"--shell";
    else if (!path.empty()) args = L"\"" + path + L"\"";
    std::wstring exe = target();
    ShellExecuteW(nullptr, nullptr, exe.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
    return 0;
}
