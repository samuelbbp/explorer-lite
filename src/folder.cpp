// folder.cpp: see folder.h.
#include "folder.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <unordered_map>
#include "app.h"
#include "idioma.h"

namespace {

// type name and icon per extension (SHGetFileInfo with USEFILEATTRIBUTES does not touch the disk); shared by the
// listing threads
struct TypeInfo {
    std::wstring type;
    int icon;
};
SRWLOCK g_types_lock = SRWLOCK_INIT;
SRWLOCK g_alive_lock = SRWLOCK_INIT;
std::vector<uint64_t> g_alive; // the listings whose icons are still wanted

bool alive(uint64_t gen) {
    AcquireSRWLockShared(&g_alive_lock);
    bool yes = std::find(g_alive.begin(), g_alive.end(), gen) != g_alive.end();
    ReleaseSRWLockShared(&g_alive_lock);
    return yes;
}
std::unordered_map<std::wstring, TypeInfo> g_types;

std::wstring lower(std::wstring s) {
    CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

TypeInfo type_of(const std::wstring &name, bool dir) {
    std::wstring key = dir ? L"<dir>" : lower(PathFindExtensionW(name.c_str()));
    // not an extension but part of a name (x.resources_31bf3856ad36, a hash, a version): a plain file, and one question
    // to the shell for all of them instead of one each (thousands in a search: a fifth of a second)
    if (key.size() > 9 || key.find_first_of(L"_- ") != std::wstring::npos) key.clear();
    AcquireSRWLockShared(&g_types_lock);
    auto it = g_types.find(key);
    bool found = it != g_types.end();
    TypeInfo t = found ? it->second : TypeInfo();
    ReleaseSRWLockShared(&g_types_lock);
    if (found) return t;
    SHFILEINFOW fi = {};
    std::wstring probe = dir ? L"folder" : L"file" + key;
    SHGetFileInfoW(probe.c_str(), dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                   SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    t.type = fi.szTypeName;
    t.icon = fi.iIcon;
    AcquireSRWLockExclusive(&g_types_lock);
    g_types[key] = t;
    ReleaseSRWLockExclusive(&g_types_lock);
    return t;
}

// files whose icon is their own, not their extension's: asked for one by one (on the listing thread)
bool own_icon(const std::wstring &name) {
    static const wchar_t *exts[] = { L".exe", L".lnk", L".ico", L".url", L".cpl", L".msc", L".scr", L".appref-ms" };
    const wchar_t *e = PathFindExtensionW(name.c_str());
    for (const wchar_t *x : exts)
        if (!_wcsicmp(e, x)) return true;
    return false;
}

std::wstring format_date(const FILETIME &ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t d[64] = L"", t[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 64, NULL);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, t, 64);
    return std::wstring(d) + L" " + t;
}

std::wstring format_bytes(uint64_t n) {
    wchar_t b[64] = L"";
    StrFormatByteSizeW((LONGLONG)n, b, 64);
    return b;
}

void list_home(Listing &l) {
    for (const Place &p : folder_places()) {
        if (p.path.empty()) continue; // Home itself
        Item it;
        it.name = p.label;
        it.target = p.path;
        it.dir = true;
        it.attr = FILE_ATTRIBUTE_DIRECTORY;
        TypeInfo t = type_of(L"", true);
        it.icon = t.icon;
        SHFILEINFOW fi = {};
        if (SHGetFileInfoW(p.path.c_str(), 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) it.icon = fi.iIcon;
        it.type = T(L"Folder");
        l.items.push_back(it);
    }
    for (const Place &p : folder_drives()) {
        Item it;
        it.name = p.label;
        it.target = p.path;
        it.dir = true;
        it.attr = FILE_ATTRIBUTE_DIRECTORY;
        SHFILEINFOW fi = {};
        if (SHGetFileInfoW(p.path.c_str(), 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME)) {
            it.icon = fi.iIcon;
            it.type = fi.szTypeName;
        }
        ULARGE_INTEGER free_bytes, total;
        if (GetDiskFreeSpaceExW(p.path.c_str(), &free_bytes, &total, NULL)) {
            it.bytes = total.QuadPart;
            it.size = Tf(L"{0} free", { format_bytes(free_bytes.QuadPart) });
        }
        l.items.push_back(it);
    }
}

// This PC: the drives, with their size and free space
void list_pc(Listing &l) {
    for (const Place &p : folder_drives()) {
        Item it;
        it.name = p.label;
        it.target = p.path;
        it.dir = true;
        it.attr = FILE_ATTRIBUTE_DIRECTORY;
        SHFILEINFOW fi = {};
        if (SHGetFileInfoW(p.path.c_str(), 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME)) {
            it.icon = fi.iIcon;
            it.type = fi.szTypeName;
        }
        ULARGE_INTEGER free_bytes, total;
        // only local disks: a network or removable drive can take seconds to answer
        if (GetDriveTypeW(p.path.c_str()) == DRIVE_FIXED && GetDiskFreeSpaceExW(p.path.c_str(), &free_bytes, &total, NULL)) {
            it.bytes = total.QuadPart;
            it.free_bytes = free_bytes.QuadPart;
            it.size = Tf(L"{0} free of {1}", { format_bytes(free_bytes.QuadPart), format_bytes(total.QuadPart) });
        }
        l.items.push_back(it);
    }
}

void list_dir(Listing &l) {
    std::wstring pattern = folder_join(l.path, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((L"\\\\?\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return; // empty (a drive root with nothing)
        wchar_t *msg = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, (wchar_t *)&msg, 0, NULL);
        l.error = msg ? msg : (L"error " + std::to_wstring(e));
        if (msg) LocalFree(msg);
        while (!l.error.empty() && (l.error.back() == L'\n' || l.error.back() == L'\r' || l.error.back() == L' ')) l.error.pop_back();
        return;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        // hidden and system files: hidden, as in Windows' default
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        Item it;
        it.name = fd.cFileName;
        it.target = folder_join(l.path, it.name);
        it.attr = fd.dwFileAttributes;
        it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.mtime = fd.ftLastWriteTime;
        it.bytes = it.dir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
        TypeInfo t = type_of(it.name, it.dir);
        it.type = t.type;
        it.icon = t.icon;
        it.date = format_date(it.mtime);
        if (!it.dir) {
            wchar_t kb[64] = L"";
            StrFormatKBSizeW((LONGLONG)it.bytes, kb, 64); // KB, like Windows' Details view
            it.size = kb;
        }
        l.items.push_back(std::move(it));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// after the listing: the icons of programs and shortcuts, one by one (reading each file), in batches
void own_icons(HWND wnd, uint64_t gen, const std::vector<std::wstring> &files) {
    Icons *batch = nullptr;
    DWORD sent = GetTickCount();
    for (size_t i = 0; i < files.size(); i++) {
        if ((i & 15) == 0 && !alive(gen)) break;
        SHFILEINFOW fi = {};
        if (!SHGetFileInfoW(files[i].c_str(), 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) continue;
        if (!batch) batch = new Icons{ gen, {} };
        batch->icons.push_back({ files[i], fi.iIcon });
        if (GetTickCount() - sent > 100) { // ten times a second at most: each batch repaints
            if (!PostMessageW(wnd, WM_APP_ICONS, 0, (LPARAM)batch)) delete batch;
            batch = nullptr;
            sent = GetTickCount();
        }
    }
    if (batch && !PostMessageW(wnd, WM_APP_ICONS, 0, (LPARAM)batch)) delete batch;
}

DWORD WINAPI list_thread(void *p) {
    auto *l = (Listing *)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    double t0 = now_ms();
    if (l->path.empty()) list_home(*l);
    else if (l->path == kThisPC) list_pc(*l);
    else list_dir(*l);
    l->ms = now_ms() - t0;
    std::vector<std::wstring> later;
    for (const Item &it : l->items)
        if (!it.dir && own_icon(it.name)) later.push_back(it.target);
    HWND wnd = l->wnd;
    uint64_t gen = l->gen;
    if (!PostMessageW(wnd, WM_APP_LIST, (WPARAM)gen, (LPARAM)l)) delete l;
    if (!later.empty()) own_icons(wnd, gen, later);
    CoUninitialize();
    return 0;
}

// --- Everything's IPC (everything_ipc.h of its SDK: no DLL needed) ---------------------------------------------

const DWORD kQuery2W = 18;                                          // EVERYTHING_IPC_COPYDATA_QUERY2W
const DWORD kReqName = 0x1, kReqPath = 0x2, kReqSize = 0x10, kReqModified = 0x40; // EVERYTHING_IPC_QUERY2_REQUEST_*
const DWORD kSortName = 1, kSortModifiedNewest = 14, kFolder = 0x1; // EVERYTHING_IPC_SORT_*

struct Query2 {
    DWORD reply_hwnd, reply_copydata_message, search_flags, offset, max_results, request_flags, sort_type;
};

// its window, on this desktop (Everything only answers windows on its own)
HWND everything() { return FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", NULL); }

} // namespace

bool folder_search(HWND wnd, DWORD id, const std::wstring &path, const std::wstring &text, DWORD max, bool recent_first) {
    HWND ev = everything();
    if (!ev) {
        log_line("everything: not running");
        return false;
    }
    // under a folder: its path as a term (Everything matches terms with a backslash against the full path)
    std::wstring q = path.empty() ? text : L"\"" + folder_join(path, L"") + L"\" " + text;
    std::vector<BYTE> buf(sizeof(Query2) + (q.size() + 1) * sizeof(wchar_t));
    auto *qs = (Query2 *)buf.data();
    qs->reply_hwnd = (DWORD)(DWORD_PTR)wnd;
    qs->reply_copydata_message = id;
    qs->max_results = max;
    qs->request_flags = kReqName | kReqPath | kReqSize | kReqModified;
    qs->sort_type = recent_first ? kSortModifiedNewest : kSortName;
    memcpy(qs + 1, q.c_str(), (q.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cds = { kQuery2W, (DWORD)buf.size(), buf.data() };
    DWORD_PTR res = 0;
    bool ok = SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)wnd, (LPARAM)&cds, SMTO_ABORTIFHUNG, 2000, &res) && res;
    if (!ok) log_line("everything: the query was not taken (" + std::to_string(GetLastError()) + ")");
    return ok;
}

void folder_search_items(const COPYDATASTRUCT *cds, std::vector<Item> &items) {
    // EVERYTHING_IPC_LIST2: totitems, numitems, offset, request_flags, sort_type, then {flags, data_offset} per item
    const BYTE *base = (const BYTE *)cds->lpData, *end = base + cds->cbData;
    if (cds->cbData < 20) return;
    const DWORD *head = (const DWORD *)base;
    DWORD n = head[1], req = head[3];
    const DWORD *entries = head + 5;
    // what comes from another process is checked as sizes, never as pointer sums that could wrap
    if (n > (cds->cbData - 20) / 8) return;
    items.reserve(n);
    for (DWORD i = 0; i < n; i++) {
        if (entries[2 * i + 1] >= cds->cbData) break;
        const BYTE *p = base + entries[2 * i + 1];
        Item it;
        it.dir = (entries[2 * i] & kFolder) != 0;
        std::wstring name, dir;
        // the fields come in the order of their request bits; text is a length then the characters and a null
        auto text = [&](std::wstring &out) {
            if ((size_t)(end - p) < 4) return false;
            DWORD len;
            memcpy(&len, p, 4);
            p += 4;
            if ((uint64_t)(end - p) < ((uint64_t)len + 1) * sizeof(wchar_t)) return false;
            out.assign((const wchar_t *)p, len);
            p += (len + 1) * sizeof(wchar_t);
            return true;
        };
        if ((req & kReqName) && !text(name)) break;
        if ((req & kReqPath) && !text(dir)) break;
        if (req & kReqSize) {
            if ((size_t)(end - p) < 8) break;
            memcpy(&it.bytes, p, 8);
            p += 8;
        }
        if (req & kReqModified) {
            if ((size_t)(end - p) < 8) break;
            memcpy(&it.mtime, p, 8);
            p += 8;
        }
        it.name = name;
        it.target = folder_join(dir, name);
        it.attr = it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        it.icon = type_of(name, it.dir).icon;
        it.type = dir;
        it.date = format_date(it.mtime);
        if (!it.dir) {
            wchar_t kb[64] = L"";
            StrFormatKBSizeW((LONGLONG)it.bytes, kb, 64);
            it.size = kb;
        }
        items.push_back(std::move(it));
    }
}

void folder_forget(uint64_t gen) {
    AcquireSRWLockExclusive(&g_alive_lock);
    g_alive.erase(std::remove(g_alive.begin(), g_alive.end(), gen), g_alive.end());
    ReleaseSRWLockExclusive(&g_alive_lock);
}

void folder_list(HWND wnd, uint64_t gen, const std::wstring &path) {
    AcquireSRWLockExclusive(&g_alive_lock);
    g_alive.push_back(gen);
    ReleaseSRWLockExclusive(&g_alive_lock);
    auto *l = new Listing{ wnd, gen, path };
    HANDLE t = CreateThread(NULL, 0, list_thread, l, 0, NULL);
    if (t) CloseHandle(t);
    else {
        l->error = L"CreateThread";
        PostMessageW(wnd, WM_APP_LIST, (WPARAM)gen, (LPARAM)l);
    }
}

void folder_sort(std::vector<Item> &items, int col, bool desc) {
    std::stable_sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
        if (a.dir != b.dir) return a.dir; // folders first, in either direction
        int c = 0;
        switch (col) {
        case COL_DATE: c = CompareFileTime(&a.mtime, &b.mtime); break;
        case COL_TYPE: c = _wcsicmp(a.type.c_str(), b.type.c_str()); break;
        case COL_SIZE: c = a.bytes < b.bytes ? -1 : a.bytes > b.bytes ? 1 : 0; break;
        }
        if (c == 0) c = StrCmpLogicalW(a.name.c_str(), b.name.c_str()); // "file2" before "file10", like Windows
        return desc ? c > 0 : c < 0;
    });
}

std::wstring folder_join(const std::wstring &dir, const std::wstring &name) {
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L"\\" + name;
}

const wchar_t kThisPC[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

bool folder_virtual(const std::wstring &path) { return path.empty() || path == kThisPC; }

std::wstring folder_parent(const std::wstring &path) {
    if (path == kThisPC) return L"";
    if (path.size() <= 3) return path.size() >= 2 && path[1] == L':' ? kThisPC : L""; // "C:\" -> This PC
    std::wstring p = path;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"";
    if (slash == 2 && p[1] == L':') return p.substr(0, 3); // "C:\x" -> "C:\"
    if (slash <= 1) return L"";                          // "\\server" -> Home
    std::wstring up = p.substr(0, slash);
    // "\\server\share" has no parent we can list
    if (up.size() > 2 && up[0] == L'\\' && up[1] == L'\\' && up.find(L'\\', 2) == std::wstring::npos) return L"";
    return up;
}

std::wstring folder_title(const std::wstring &path) {
    if (path.empty()) return T(L"Home");
    if (path == kThisPC) return T(L"This PC");
    for (const Place &p : folder_places())
        if (!p.path.empty() && !_wcsicmp(p.path.c_str(), path.c_str())) return p.label;
    if (path.size() <= 3 && path.size() >= 2 && path[1] == L':') return path.substr(0, 2);
    std::wstring p = path;
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

bool folder_exists(const std::wstring &path) {
    if (folder_virtual(path)) return true;
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring folder_normalize(const std::wstring &typed) {
    std::wstring s = typed;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    wchar_t buf[32768];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, ARRAYSIZE(buf));
    if (n && n <= ARRAYSIZE(buf)) s = buf;
    for (wchar_t &ch : s)
        if (ch == L'/') ch = L'\\';
    if (s.size() == 2 && s[1] == L':') s += L'\\'; // "C:" is the drive's root, not its current folder
    if (!_wcsicmp(s.c_str(), L"home") || !_wcsicmp(s.c_str(), T(L"Home"))) return L"";
    if (!_wcsicmp(s.c_str(), L"this pc") || !_wcsicmp(s.c_str(), T(L"This PC")) || !_wcsicmp(s.c_str(), kThisPC)) return kThisPC;
    wchar_t full[32768];
    if (!s.empty() && GetFullPathNameW(s.c_str(), ARRAYSIZE(full), full, NULL)) s = full;
    if (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::vector<Place> folder_places() {
    struct K {
        const KNOWNFOLDERID *id;
        const wchar_t *label, *glyph;
    };
    static const K known[] = {
        { &FOLDERID_Desktop, L"Desktop", L"\xE8FC" },     // translate
        { &FOLDERID_Downloads, L"Downloads", L"\xE896" }, // translate
        { &FOLDERID_Documents, L"Documents", L"\xE8A5" }, // translate
        { &FOLDERID_Pictures, L"Pictures", L"\xE91B" },   // translate
        { &FOLDERID_Music, L"Music", L"\xE8D6" },         // translate
        { &FOLDERID_Videos, L"Videos", L"\xE714" },       // translate
    };
    std::vector<Place> out;
    out.push_back({ T(L"Home"), L"", L"\xE80F" });
    for (const K &k : known) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*k.id, KF_FLAG_DONT_VERIFY, NULL, &p)) && p) out.push_back({ T(k.label), p, k.glyph });
        CoTaskMemFree(p);
    }
    return out;
}

std::vector<Place> folder_drives() {
    std::vector<Place> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_NO_ROOT_DIR) continue;
        wchar_t label[MAX_PATH + 1] = L"";
        // network and removable drives can take seconds to answer: only local disks are asked for their name
        if (type == DRIVE_FIXED) GetVolumeInformationW(root.c_str(), label, ARRAYSIZE(label), NULL, NULL, NULL, NULL, 0);
        std::wstring name = label[0] ? label : (type == DRIVE_REMOTE ? T(L"Network drive") : type == DRIVE_REMOVABLE ? T(L"USB drive") : T(L"Local Disk"));
        out.push_back({ name + L" (" + root.substr(0, 2) + L")", root, type == DRIVE_REMOTE ? L"\xE8CE" : type == DRIVE_REMOVABLE ? L"\xE88E" : L"\xEDA2" });
    }
    return out;
}
