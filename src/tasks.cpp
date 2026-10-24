// tasks.cpp: see tasks.h. One NtQuerySystemInformation(SystemProcessInformation) per sample gives every process with
// its parent, private working set and CPU times without opening any of them; only a new process is opened once (its
// path, for the icon, the description and whether it is part of Windows).
#include "tasks.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winsvc.h>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include "app.h"
#include "gfx.h"
#include "idioma.h"

namespace {

// --- sampling (its own thread) ----------------------------------------------------------------------

// SYSTEM_PROCESS_INFORMATION as Windows lays it out (x64), up to the fields used here
struct SysProc {
    ULONG next, threads;
    LARGE_INTEGER ws_private;
    ULONG hard_faults, threads_peak;
    ULONGLONG cycles;
    LARGE_INTEGER create, user, kernel;
    USHORT name_len, name_max;
    wchar_t *name;
    LONG priority;
    HANDLE pid, ppid;
    ULONG handles, session;
    ULONG_PTR key;
    SIZE_T vm_peak, vm, faults_and_pad, ws_peak, ws;
};
typedef LONG(WINAPI *PQuery)(ULONG, void *, ULONG, ULONG *);

struct Proc {
    DWORD pid = 0, ppid = 0, session = 0;
    ULONGLONG create = 0;
    std::wstring name, path, desc;
    uint64_t priv = 0, ws = 0;
    double cpu = 0; // % of the whole machine
    int icon = -1;
};

struct Sample {
    std::vector<Proc> procs;
    double cpu = 0;
    uint64_t mem_used = 0, mem_total = 0;
};

struct Known { // what is asked once per process
    std::wstring path, desc;
    int icon = -1;
};

struct Sampler {
    HWND wnd;
    HANDLE thread = NULL, wake = NULL;
    volatile LONG stop = 0;
    SRWLOCK lock = SRWLOCK_INIT;
    Sample latest;
    bool fresh = false;
};

std::wstring describe(const std::wstring &path) {
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!n) return L"";
    std::vector<BYTE> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return L"";
    struct Lang {
        WORD lang, cp;
    } *langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void **)&langs, &len) || len < sizeof(Lang)) return L"";
    wchar_t key[64];
    swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t *val = nullptr;
    if (!VerQueryValueW(buf.data(), key, (void **)&val, &len) || !val) return L"";
    std::wstring d(val);
    while (!d.empty() && iswspace(d.back())) d.pop_back();
    return d;
}

DWORD WINAPI sample_thread(void *p) {
    auto *s = (Sampler *)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto query = nt ? (PQuery)(void *)GetProcAddress(nt, "NtQuerySystemInformation") : nullptr;
    std::vector<BYTE> buf(1 << 20);
    std::map<std::pair<DWORD, ULONGLONG>, ULONGLONG> prev_time; // (pid, created) -> user + kernel
    std::map<std::pair<DWORD, ULONGLONG>, Known> known;
    ULONGLONG prev_wall = 0;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cpus = std::max<int>(1, (int)si.dwNumberOfProcessors);
    FILETIME pi, pk, pu;
    ULONGLONG prev_idle = 0, prev_busy = 0;
    while (!s->stop) {
        ULONG got = 0;
        LONG st = query ? query(5 /* SystemProcessInformation */, buf.data(), (ULONG)buf.size(), &got) : -1;
        if (st == (LONG)0xC0000004) { // STATUS_INFO_LENGTH_MISMATCH: bigger, and again
            buf.resize(std::max<size_t>(buf.size() * 2, got + (64 << 10)));
            continue;
        }
        Sample out;
        FILETIME now_ft;
        GetSystemTimeAsFileTime(&now_ft);
        ULONGLONG wall = (ULONGLONG)now_ft.dwHighDateTime << 32 | now_ft.dwLowDateTime;
        double span = prev_wall ? (double)(wall - prev_wall) * cpus : 0;
        std::map<std::pair<DWORD, ULONGLONG>, ULONGLONG> times;
        std::map<std::pair<DWORD, ULONGLONG>, Known> still;
        if (st >= 0) {
            for (BYTE *at = buf.data();;) {
                auto *sp = (SysProc *)at;
                Proc pr;
                pr.pid = (DWORD)(ULONG_PTR)sp->pid;
                pr.ppid = (DWORD)(ULONG_PTR)sp->ppid;
                pr.session = sp->session;
                pr.create = (ULONGLONG)sp->create.QuadPart;
                pr.name = sp->name ? std::wstring(sp->name, sp->name_len / sizeof(wchar_t)) : (pr.pid ? L"System" : L"System Idle Process");
                pr.priv = (uint64_t)sp->ws_private.QuadPart;
                pr.ws = sp->ws;
                ULONGLONG t = (ULONGLONG)sp->user.QuadPart + (ULONGLONG)sp->kernel.QuadPart;
                auto key = std::make_pair(pr.pid, pr.create);
                times[key] = t;
                auto pt = prev_time.find(key);
                if (pt != prev_time.end() && span > 0) pr.cpu = 100.0 * (double)(t - pt->second) / span;
                auto k = known.find(key);
                if (k == known.end()) {
                    Known kn;
                    HANDLE h = pr.pid > 4 ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pr.pid) : NULL;
                    if (h) {
                        wchar_t path[MAX_PATH * 2];
                        DWORD n = ARRAYSIZE(path);
                        if (QueryFullProcessImageNameW(h, 0, path, &n)) kn.path.assign(path, n);
                        CloseHandle(h);
                    }
                    if (!kn.path.empty()) {
                        kn.desc = describe(kn.path);
                        SHFILEINFOW fi = {};
                        if (SHGetFileInfoW(kn.path.c_str(), 0, &fi, sizeof(fi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) kn.icon = fi.iIcon;
                    }
                    k = known.emplace(key, kn).first;
                }
                pr.path = k->second.path;
                pr.desc = k->second.desc;
                pr.icon = k->second.icon;
                still[key] = k->second;
                if (pr.pid) out.procs.push_back(std::move(pr)); // not the Idle process
                if (!sp->next) break;
                at += sp->next;
            }
        }
        prev_time.swap(times);
        known.swap(still);
        prev_wall = wall;
        if (GetSystemTimes(&pi, &pk, &pu)) {
            ULONGLONG idle = (ULONGLONG)pi.dwHighDateTime << 32 | pi.dwLowDateTime;
            ULONGLONG busy = ((ULONGLONG)pk.dwHighDateTime << 32 | pk.dwLowDateTime) + ((ULONGLONG)pu.dwHighDateTime << 32 | pu.dwLowDateTime);
            if (prev_busy && busy > prev_busy) out.cpu = 100.0 * (1.0 - (double)(idle - prev_idle) / (double)(busy - prev_busy));
            prev_idle = idle;
            prev_busy = busy;
        }
        MEMORYSTATUSEX ms = { sizeof(ms) };
        if (GlobalMemoryStatusEx(&ms)) {
            out.mem_total = ms.ullTotalPhys;
            out.mem_used = ms.ullTotalPhys - ms.ullAvailPhys;
        }
        AcquireSRWLockExclusive(&s->lock);
        s->latest = std::move(out);
        s->fresh = true;
        ReleaseSRWLockExclusive(&s->lock);
        PostMessageW(s->wnd, WM_APP_TASKS, 0, 0);
        WaitForSingleObject(s->wake, 2000);
    }
    CoUninitialize();
    return 0;
}

// --- the model --------------------------------------------------------------------------------------

enum Section { SEC_APPS, SEC_AGENTS, SEC_WINDOWS, SEC_COUNT };
enum Page { PG_PROCESSES, PG_SERVICES, PG_STARTUP, PG_COUNT };
enum Col { TC_NAME, TC_COUNT, TC_CPU, TC_MEM, TC_N };

struct Group {
    std::wstring key, label;
    int section = SEC_APPS;
    std::vector<int> procs; // indexes into the sample
    double cpu = 0;
    uint64_t mem = 0;
    int icon = -1;
    std::wstring path;
};

struct Row {
    int kind; // 0 section header, 1 group, 2 process (of the group), 3 service, 4 startup entry
    int a, b;
};

struct Service {
    std::wstring name, display;
    DWORD state = 0, pid = 0, start = 0;
};

struct Startup {
    std::wstring name, command, where;
    bool enabled = true, editable = false;
    HKEY approved_root = NULL;
    std::wstring approved_key; // StartupApproved\Run or \StartupFolder
};

struct Hot { // what tasks_draw left clickable
    D2D1_RECT_F r;
    int id;
};

} // namespace

struct TasksView {
    HWND wnd;
    IconFn icon;
    void *ctx;
    Sampler *sampler = nullptr;
    Sample cur;
    std::vector<Group> groups;
    std::vector<Row> rows;
    std::set<std::wstring> open;   // expanded groups
    std::wstring sel_key;          // the selected row, by what it is (a sample moves positions)
    int sort = TC_MEM;
    bool desc = true;
    float scroll = 0;
    int page = PG_PROCESSES;
    std::vector<Service> services;
    std::vector<Startup> startup;
    D2D1_RECT_F list = {};
    std::vector<Hot> hots;
    bool collapsed[SEC_COUNT] = { false, false, false };
};

namespace {

const float kRow = 28, kHead = 30;
const ULONG kIdTab = 10000, kIdHead = 20000;

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

bool is_agent_name(const std::wstring &n) {
    std::wstring l = lower(n);
    return l == L"claude.exe" || l == L"codex.exe";
}

std::wstring g_windir;

bool is_windows(const Proc &p) {
    if (g_windir.empty()) {
        wchar_t d[MAX_PATH];
        UINT n = GetWindowsDirectoryW(d, MAX_PATH);
        g_windir = lower(std::wstring(d, n)) + L"\\";
    }
    if (p.path.empty()) return true; // System, Registry, protected processes: none of them is an app
    return lower(p.path).compare(0, g_windir.size(), g_windir) == 0;
}

std::wstring mem_text(uint64_t b) {
    wchar_t s[32];
    if (b >= (1ull << 30)) swprintf(s, 32, L"%.1f GB", b / 1073741824.0);
    else swprintf(s, 32, L"%.1f MB", b / 1048576.0);
    return s;
}

std::wstring cpu_text(double c) {
    wchar_t s[16];
    swprintf(s, 16, L"%.1f %%", c < 0.05 ? 0.0 : c);
    return s;
}

void build_groups(TasksView *t) {
    std::vector<Proc> &ps = t->cur.procs;
    std::unordered_map<DWORD, int> by_pid;
    for (int i = 0; i < (int)ps.size(); i++) by_pid[ps[i].pid] = i;
    // a parent counts only if it was created before the child (PIDs are reused)
    auto parent = [&](int i) -> int {
        auto it = by_pid.find(ps[i].ppid);
        if (it == by_pid.end() || it->second == i || ps[it->second].create > ps[i].create) return -1;
        return it->second;
    };
    std::map<std::wstring, Group> groups;
    for (int i = 0; i < (int)ps.size(); i++) {
        // agents: the topmost claude.exe / codex.exe above it, so each session is one group with all it runs
        int agent = -1;
        for (int j = i, guard = 0; j >= 0 && guard < 64; j = parent(j), guard++)
            if (is_agent_name(ps[j].name)) agent = j;
        int section;
        int root = i;
        if (agent >= 0) {
            section = SEC_AGENTS;
            root = agent;
        } else if (is_windows(ps[i])) {
            section = SEC_WINDOWS;
        } else {
            section = SEC_APPS;
            // an app and the apps it started (Chrome's renderers, VS Code's helpers): up to the first non-app parent
            for (int j = parent(i), guard = 0; j >= 0 && guard < 64 && !is_windows(ps[j]); j = parent(j), guard++) root = j;
        }
        std::wstring key = section == SEC_AGENTS ? L"a" + std::to_wstring(ps[root].pid) + L"." + std::to_wstring(ps[root].create)
                                                 : (section == SEC_APPS ? L"p" : L"w") + lower(ps[root].name);
        Group &g = groups[key];
        if (g.key.empty()) {
            g.key = key;
            g.section = section;
            const Proc &r = ps[root];
            g.label = section == SEC_AGENTS ? (lower(r.name) == L"codex.exe" ? L"Codex" : L"Claude Code") + std::wstring(L"  ·  ") + std::to_wstring(r.pid)
                                            : (!r.desc.empty() ? r.desc : r.name);
            g.icon = r.icon;
            g.path = r.path;
        }
        g.procs.push_back(i);
        g.cpu += ps[i].cpu;
        g.mem += ps[i].priv;
    }
    t->groups.clear();
    for (auto &kv : groups) t->groups.push_back(std::move(kv.second));
    auto cmp = [&](const Group &a, const Group &b) {
        double x = 0, y = 0;
        switch (t->sort) {
        case TC_NAME: {
            int c = _wcsicmp(a.label.c_str(), b.label.c_str());
            return t->desc ? c > 0 : c < 0;
        }
        case TC_COUNT: x = (double)a.procs.size(), y = (double)b.procs.size(); break;
        case TC_CPU: x = a.cpu, y = b.cpu; break;
        default: x = (double)a.mem, y = (double)b.mem;
        }
        if (x == y) return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
        return t->desc ? x > y : x < y;
    };
    std::sort(t->groups.begin(), t->groups.end(), cmp);
    for (Group &g : t->groups)
        std::sort(g.procs.begin(), g.procs.end(), [&](int a, int b) { return ps[a].priv > ps[b].priv; });
}

void load_services(TasksView *t) {
    t->services.clear();
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    DWORD need = 0, count = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &count, &resume, NULL);
    std::vector<BYTE> buf(need + 4096);
    resume = 0;
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(), (DWORD)buf.size(), &need, &count, &resume, NULL)) {
        auto *s = (ENUM_SERVICE_STATUS_PROCESSW *)buf.data();
        for (DWORD i = 0; i < count; i++) {
            Service sv;
            sv.name = s[i].lpServiceName;
            sv.display = s[i].lpDisplayName;
            sv.state = s[i].ServiceStatusProcess.dwCurrentState;
            sv.pid = s[i].ServiceStatusProcess.dwProcessId;
            t->services.push_back(sv);
        }
    }
    CloseServiceHandle(scm);
    std::sort(t->services.begin(), t->services.end(), [](const Service &a, const Service &b) {
        bool ra = a.state == SERVICE_RUNNING, rb = b.state == SERVICE_RUNNING;
        if (ra != rb) return ra;
        return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0;
    });
}

// StartupApproved: the first byte even (02, 06) is enabled, odd (03, 07) disabled; no value is enabled
bool approved(HKEY root, const std::wstring &key, const std::wstring &name) {
    BYTE v[12] = {};
    DWORD n = sizeof(v);
    if (RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_REG_BINARY, NULL, v, &n) != ERROR_SUCCESS || !n) return true;
    return (v[0] & 1) == 0;
}

void load_startup(TasksView *t) {
    t->startup.clear();
    const wchar_t *approved_base = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
    struct RunKey {
        HKEY root;
        const wchar_t *key, *where, *approved;
        REGSAM view;
    } keys[] = {
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKCU Run", L"Run", 0 },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKLM Run", L"Run", KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKLM Run (32-bit)", L"Run32", KEY_WOW64_32KEY },
    };
    for (const RunKey &k : keys) {
        HKEY h;
        if (RegOpenKeyExW(k.root, k.key, 0, KEY_READ | k.view, &h) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i++) {
            wchar_t name[512];
            DWORD nl = ARRAYSIZE(name), type = 0, dl = 0;
            if (RegEnumValueW(h, i, name, &nl, NULL, &type, NULL, &dl) != ERROR_SUCCESS) break;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            std::wstring data(dl / sizeof(wchar_t) + 1, 0);
            DWORD dl2 = (DWORD)(data.size() * sizeof(wchar_t));
            nl = ARRAYSIZE(name);
            if (RegEnumValueW(h, i, name, &nl, NULL, &type, (BYTE *)&data[0], &dl2) != ERROR_SUCCESS) continue;
            data.resize(wcsnlen(data.c_str(), data.size()));
            Startup s;
            s.name = name;
            s.command = data;
            s.where = k.where;
            s.approved_root = k.root;
            s.approved_key = std::wstring(approved_base) + k.approved;
            s.enabled = approved(k.root, s.approved_key, s.name);
            s.editable = k.root == HKEY_CURRENT_USER; // the machine's need an administrator
            t->startup.push_back(s);
        }
        RegCloseKey(h);
    }
    struct Folder {
        const KNOWNFOLDERID *id;
        HKEY root;
        const wchar_t *where;
    } folders[] = { { &FOLDERID_Startup, HKEY_CURRENT_USER, L"Startup folder" }, { &FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE, L"Startup folder (all users)" } };
    for (const Folder &f : folders) {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(*f.id, 0, NULL, &p)) || !p) continue;
        std::wstring dir = p;
        CoTaskMemFree(p);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
            Startup s;
            s.name = fd.cFileName;
            s.command = dir + L"\\" + fd.cFileName;
            s.where = f.where;
            s.approved_root = f.root;
            s.approved_key = std::wstring(approved_base) + L"StartupFolder";
            s.enabled = approved(f.root, s.approved_key, s.name);
            s.editable = f.root == HKEY_CURRENT_USER;
            t->startup.push_back(s);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void set_startup_enabled(Startup &s, bool on) {
    if (!s.editable) return;
    HKEY h;
    if (RegCreateKeyExW(s.approved_root, s.approved_key.c_str(), 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS) return;
    BYTE v[12] = { (BYTE)(on ? 2 : 3) };
    if (!on) { // when it was disabled, as Task Manager writes it
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        memcpy(v + 4, &ft, sizeof(ft));
    }
    RegSetValueExW(h, s.name.c_str(), 0, REG_BINARY, v, sizeof(v));
    RegCloseKey(h);
    s.enabled = on;
}

void build_rows(TasksView *t) {
    t->rows.clear();
    if (t->page == PG_SERVICES) {
        for (int i = 0; i < (int)t->services.size(); i++) t->rows.push_back({ 3, i, 0 });
        return;
    }
    if (t->page == PG_STARTUP) {
        for (int i = 0; i < (int)t->startup.size(); i++) t->rows.push_back({ 4, i, 0 });
        return;
    }
    for (int s = 0; s < SEC_COUNT; s++) {
        bool any = false;
        for (const Group &g : t->groups) any |= g.section == s;
        if (!any) continue;
        t->rows.push_back({ 0, s, 0 });
        if (t->collapsed[s]) continue;
        for (int gi = 0; gi < (int)t->groups.size(); gi++) {
            const Group &g = t->groups[gi];
            if (g.section != s) continue;
            t->rows.push_back({ 1, gi, 0 });
            if (t->open.count(g.key) && g.procs.size() > 1)
                for (int p : g.procs) t->rows.push_back({ 2, gi, p });
        }
    }
}

std::wstring row_key(TasksView *t, const Row &r) {
    switch (r.kind) {
    case 0: return L"s" + std::to_wstring(r.a);
    case 1: return t->groups[r.a].key;
    case 2: return t->groups[r.a].key + L"/" + std::to_wstring(t->cur.procs[r.b].pid);
    case 3: return L"v" + t->services[r.a].name;
    case 4: return L"r" + t->startup[r.a].where + L"/" + t->startup[r.a].name;
    }
    return L"";
}

int selected_row(TasksView *t) {
    for (int i = 0; i < (int)t->rows.size(); i++)
        if (row_key(t, t->rows[i]) == t->sel_key) return i;
    return -1;
}

void clamp(TasksView *t) {
    float h = t->list.bottom - t->list.top;
    float max = std::max(0.f, t->rows.size() * kRow - h);
    t->scroll = std::min(std::max(0.f, t->scroll), max);
}

// critical to Windows: ending one logs you off or blue-screens; never offered
bool critical(const std::wstring &name) {
    static const wchar_t *names[] = { L"system", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
                                      L"lsass.exe", L"lsaiso.exe", L"dwm.exe", L"memory compression", L"secure system", L"fontdrvhost.exe" };
    std::wstring l = lower(name);
    for (const wchar_t *n : names)
        if (l == n) return true;
    return false;
}

void end_pids(const std::vector<DWORD> &pids) {
    for (DWORD pid : pids) {
        if (pid == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) continue;
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

// End task on the selected row: a group ends its whole tree, a process row only that process
void end_selected(TasksView *t, HWND wnd) {
    int i = selected_row(t);
    if (i < 0) return;
    const Row &r = t->rows[i];
    if (r.kind != 1 && r.kind != 2) return;
    const Group &g = t->groups[r.a];
    std::vector<DWORD> pids;
    std::wstring names;
    if (r.kind == 2) {
        if (critical(t->cur.procs[r.b].name)) return;
        pids.push_back(t->cur.procs[r.b].pid);
    } else {
        for (int p : g.procs)
            if (!critical(t->cur.procs[p].name)) pids.push_back(t->cur.procs[p].pid);
    }
    if (pids.empty()) return;
    // Windows' own processes: asked first (ending one can restart Windows features or lose work)
    if (g.section == SEC_WINDOWS) {
        std::wstring q = Tf(L"End {0}? It is part of Windows: ending it can close windows or stop Windows features until you sign in again.", { g.label });
        if (MessageBoxW(wnd, q.c_str(), L"explorer-lite", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    end_pids(pids);
    tasks_refresh(t);
}

void open_location(const std::wstring &path) {
    if (path.empty()) return;
    // our own window would be nicer, but this is the one place that should select the file: Windows' does
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
    }
}

void service_control(const Service &s, const wchar_t *verb) {
    // starting and stopping services needs an administrator: sc.exe elevated (Windows asks)
    std::wstring args = std::wstring(verb) + L" \"" + s.name + L"\"";
    ShellExecuteW(NULL, L"runas", L"sc.exe", args.c_str(), NULL, SW_HIDE);
}

enum MenuCmd { MC_END = 1, MC_LOCATION, MC_EXPAND, MC_START, MC_STOP, MC_RESTART, MC_ENABLE, MC_DISABLE, MC_COPY };

void copy_text(HWND wnd, const std::wstring &s) {
    if (!OpenClipboard(wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) {
        memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void context_menu(TasksView *t, HWND wnd) {
    int i = selected_row(t);
    if (i < 0) return;
    const Row r = t->rows[i];
    HMENU m = CreatePopupMenu();
    MENUINFO mi = { sizeof(mi) };
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(m, &mi);
    std::vector<HBITMAP> bmps;
    int px = MulDiv(16, (int)GetDpiForWindow(wnd), 96);
    auto add = [&](UINT id, const wchar_t *glyph, const wchar_t *label, bool enabled = true) {
        HBITMAP b = glyph_bitmap(glyph, px, enabled ? kText : kDim);
        if (b) bmps.push_back(b);
        MENUITEMINFOW it = { sizeof(it) };
        it.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_BITMAP | MIIM_FTYPE;
        it.fType = MFT_STRING;
        it.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
        it.wID = id;
        it.dwTypeData = (wchar_t *)label;
        it.hbmpItem = b;
        InsertMenuItemW(m, (UINT)GetMenuItemCount(m), TRUE, &it);
    };
    std::wstring path, copy;
    if (r.kind == 1 || r.kind == 2) {
        const Group &g = t->groups[r.a];
        path = r.kind == 2 ? t->cur.procs[r.b].path : g.path;
        copy = r.kind == 2 ? t->cur.procs[r.b].name + L" " + std::to_wstring(t->cur.procs[r.b].pid) : g.label;
        add(MC_END, L"\xE711", r.kind == 1 && g.procs.size() > 1 ? T(L"End task (whole tree)") : T(L"End task"));
        if (r.kind == 1 && g.procs.size() > 1) add(MC_EXPAND, L"\xE8C4", t->open.count(g.key) ? T(L"Collapse") : T(L"Expand"));
        add(MC_LOCATION, L"\xE838", T(L"Open file location"), !path.empty());
    } else if (r.kind == 3) {
        const Service &s = t->services[r.a];
        copy = s.name;
        add(MC_START, L"\xE768", T(L"Start"), s.state == SERVICE_STOPPED);
        add(MC_STOP, L"\xE71A", T(L"Stop"), s.state == SERVICE_RUNNING);
        add(MC_RESTART, L"\xE72C", T(L"Restart"), s.state == SERVICE_RUNNING);
    } else if (r.kind == 4) {
        const Startup &s = t->startup[r.a];
        copy = s.command;
        add(MC_ENABLE, L"\xE73E", T(L"Enable"), s.editable && !s.enabled);
        add(MC_DISABLE, L"\xE711", T(L"Disable"), s.editable && s.enabled);
    } else {
        DestroyMenu(m);
        return;
    }
    add(MC_COPY, L"\xE8C8", T(L"Copy"));
    POINT pt;
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, NULL);
    DestroyMenu(m);
    for (HBITMAP b : bmps) DeleteObject(b);
    switch (cmd) {
    case MC_END: end_selected(t, wnd); break;
    case MC_EXPAND: {
        const std::wstring &k = t->groups[r.a].key;
        if (!t->open.erase(k)) t->open.insert(k);
        build_rows(t);
        break;
    }
    case MC_LOCATION: open_location(path); break;
    case MC_START: service_control(t->services[r.a], L"start"); break;
    case MC_STOP: service_control(t->services[r.a], L"stop"); break;
    case MC_RESTART: {
        // stop, then start, in one elevated cmd (one consent prompt)
        std::wstring args = L"/c sc stop \"" + t->services[r.a].name + L"\" & timeout /t 2 >nul & sc start \"" + t->services[r.a].name + L"\"";
        ShellExecuteW(NULL, L"runas", L"cmd.exe", args.c_str(), NULL, SW_HIDE);
        break;
    }
    case MC_ENABLE: set_startup_enabled(t->startup[r.a], true); break;
    case MC_DISABLE: set_startup_enabled(t->startup[r.a], false); break;
    case MC_COPY: copy_text(wnd, copy); break;
    }
    if (cmd >= MC_START && cmd <= MC_RESTART) tasks_refresh(t);
    InvalidateRect(wnd, NULL, FALSE);
}

void text(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, const std::wstring &s, D2D1_RECT_F r, IDWriteTextFormat *f, uint32_t rgb, float a = 1) {
    if (!f || s.empty() || r.right <= r.left) return;
    br->SetColor(color(rgb, a));
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void text_right(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, const std::wstring &s, D2D1_RECT_F r, IDWriteTextFormat *f, uint32_t rgb) {
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text(rt, br, s, r, f, rgb);
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void round_fill(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, D2D1_RECT_F r, uint32_t rgb, float a = 1) {
    br->SetColor(color(rgb, a));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br);
}

// memory as a heat colour: the bigger, the more it stands out (as Task Manager's column)
float heat(uint64_t mem, uint64_t total) {
    if (!total) return 0;
    double f = (double)mem / (double)total;
    return (float)std::min(0.55, f * 12);
}

} // namespace

TasksView *tasks_create(HWND wnd, IconFn icon, void *ctx) {
    auto *t = new TasksView;
    t->wnd = wnd;
    t->icon = icon;
    t->ctx = ctx;
    t->sampler = new Sampler;
    t->sampler->wnd = wnd;
    t->sampler->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    t->sampler->thread = CreateThread(NULL, 0, sample_thread, t->sampler, 0, NULL);
    return t;
}

void tasks_destroy(TasksView *t) {
    if (!t) return;
    Sampler *s = t->sampler;
    InterlockedExchange(&s->stop, 1);
    SetEvent(s->wake);
    if (s->thread) {
        WaitForSingleObject(s->thread, 5000);
        CloseHandle(s->thread);
    }
    CloseHandle(s->wake);
    delete s;
    delete t;
}

void tasks_refresh(TasksView *t) {
    if (!t) return;
    if (t->page == PG_SERVICES) load_services(t);
    if (t->page == PG_STARTUP) load_startup(t);
    build_rows(t);
    SetEvent(t->sampler->wake);
}

void tasks_on_sample(TasksView *t) {
    AcquireSRWLockExclusive(&t->sampler->lock);
    bool fresh = t->sampler->fresh;
    if (fresh) t->cur = std::move(t->sampler->latest);
    t->sampler->fresh = false;
    ReleaseSRWLockExclusive(&t->sampler->lock);
    if (!fresh) return;
    build_groups(t);
    if (t->page == PG_SERVICES && t->services.empty()) load_services(t);
    build_rows(t);
    clamp(t);
}

std::wstring tasks_status(TasksView *t) {
    if (!t) return L"";
    return Tf(L"{0} processes   CPU {1}   Memory {2} of {3}", { std::to_wstring(t->cur.procs.size()), cpu_text(t->cur.cpu), mem_text(t->cur.mem_used),
                                                               mem_text(t->cur.mem_total) });
}

void tasks_toolbar(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *, D2D1_RECT_F r) {
    if (!t) return;
    t->hots.erase(std::remove_if(t->hots.begin(), t->hots.end(), [](const Hot &h) { return h.id >= (int)kIdTab && h.id < (int)kIdHead; }), t->hots.end());
    const wchar_t *pages[PG_COUNT] = { L"Processes", L"Services", L"Startup apps" }; // translate
    float x = r.left;
    for (int i = 0; i < PG_COUNT; i++) {
        IDWriteTextLayout *l = nullptr;
        std::wstring label = T(pages[i]);
        float w = 80;
        if (SUCCEEDED(g_dw->CreateTextLayout(label.c_str(), (UINT32)label.size(), f, 1000, 40, &l))) {
            DWRITE_TEXT_METRICS m;
            l->GetMetrics(&m);
            w = m.width + 24;
            l->Release();
        }
        D2D1_RECT_F b = D2D1::RectF(x, r.top + 6, x + w, r.bottom - 6);
        if (i == t->page) round_fill(rt, br, b, kTabOn);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(rt, br, label, b, f, i == t->page ? kText : kDim);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (i == t->page) {
            br->SetColor(color(kAccent));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 12, b.bottom - 3, b.right - 12, b.bottom - 1), 1, 1), br);
        }
        t->hots.push_back({ b, (int)kIdTab + i });
        x += w + 4;
    }
}

void tasks_draw(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *small, IDWriteTextFormat *icons,
                D2D1_RECT_F r, int hover) {
    if (!t) return;
    t->hots.erase(std::remove_if(t->hots.begin(), t->hots.end(), [](const Hot &h) { return h.id >= (int)kIdHead; }), t->hots.end());
    float right = r.right - 12;
    // columns from the right: memory, CPU, processes; the name takes the rest
    float x_mem = right - 110, x_cpu = x_mem - 90, x_cnt = x_cpu - 90;
    float head = r.top;
    const wchar_t *heads[TC_N] = { L"Name", L"Processes", L"CPU", L"Memory" }; // translate
    D2D1_RECT_F cols[TC_N] = { D2D1::RectF(r.left + 8, head, x_cnt, head + kHead), D2D1::RectF(x_cnt, head, x_cpu, head + kHead),
                               D2D1::RectF(x_cpu, head, x_mem, head + kHead), D2D1::RectF(x_mem, head, right, head + kHead) };
    if (t->page == PG_SERVICES) {
        heads[TC_COUNT] = L"Status"; // translate
        heads[TC_CPU] = L"PID";      // translate
        heads[TC_MEM] = L"Memory";   // translate
    } else if (t->page == PG_STARTUP) {
        heads[TC_COUNT] = L"Where";  // translate
        heads[TC_CPU] = L"";
        heads[TC_MEM] = L"Status";   // translate
        cols[TC_COUNT] = D2D1::RectF(x_cnt - 120, head, x_mem, head + kHead);
    }
    for (int i = 0; i < TC_N; i++) {
        if (!heads[i][0]) continue;
        D2D1_RECT_F c = cols[i];
        bool hot = hover == (int)kIdHead + i;
        if (hot && t->page == PG_PROCESSES) round_fill(rt, br, D2D1::RectF(c.left - 4, c.top + 2, c.right, c.bottom - 2), kHover);
        std::wstring h = T(heads[i]);
        if (t->page == PG_PROCESSES && t->sort == i) h = (t->desc ? L"\x2193 " : L"\x2191 ") + h;
        if (i == TC_NAME) text(rt, br, h, D2D1::RectF(c.left + 4, c.top, c.right, c.bottom), small, kDim);
        else text_right(rt, br, h, D2D1::RectF(c.left, c.top, c.right - 6, c.bottom), small, kDim);
        if (t->page == PG_PROCESSES) t->hots.push_back({ c, (int)kIdHead + i });
    }
    br->SetColor(color(kLine));
    rt->FillRectangle(D2D1::RectF(r.left, head + kHead - 1, r.right, head + kHead), br);
    t->list = D2D1::RectF(r.left, head + kHead, r.right, r.bottom);
    clamp(t);

    rt->PushAxisAlignedClip(t->list, D2D1_ANTIALIAS_MODE_ALIASED);
    int sel = selected_row(t);
    if (t->rows.empty()) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(rt, br, T(L"Loading…"), D2D1::RectF(t->list.left, t->list.top + 20, t->list.right, t->list.top + 60), f, kDim);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    int first = std::max(0, (int)(t->scroll / kRow)), last = std::min((int)t->rows.size(), (int)((t->scroll + (t->list.bottom - t->list.top)) / kRow) + 1);
    for (int i = first; i < last; i++) {
        const Row &row = t->rows[i];
        float y = t->list.top + i * kRow - t->scroll;
        D2D1_RECT_F rr = D2D1::RectF(r.left + 4, y + 1, r.right - 4, y + kRow - 1);
        if (row.kind == 0) {
            static const wchar_t *names[SEC_COUNT] = { L"Apps", L"Agents", L"Windows" }; // translate
            int n = 0;
            uint64_t mem = 0;
            double cpu = 0;
            for (const Group &g : t->groups)
                if (g.section == row.a) n++, mem += g.mem, cpu += g.cpu;
            if (hover == i) round_fill(rt, br, rr, kHover);
            text(rt, br, t->collapsed[row.a] ? L"\xE76C" : L"\xE70D", D2D1::RectF(rr.left + 4, y, rr.left + 22, y + kRow), icons, kDim);
            text(rt, br, std::wstring(T(names[row.a])) + L" (" + std::to_wstring(n) + L")", D2D1::RectF(rr.left + 26, y, x_cnt, y + kRow), f, kAccent);
            text_right(rt, br, cpu_text(cpu), D2D1::RectF(x_cpu, y, x_mem - 6, y + kRow), small, kDim);
            text_right(rt, br, mem_text(mem), D2D1::RectF(x_mem, y, right - 6, y + kRow), small, kDim);
            continue;
        }
        if (i == sel) round_fill(rt, br, rr, kSelect);
        else if (hover == i) round_fill(rt, br, rr, kHover);
        if (row.kind == 1 || row.kind == 2) {
            const Group &g = t->groups[row.a];
            bool child = row.kind == 2;
            const Proc *p = child ? &t->cur.procs[row.b] : nullptr;
            float x0 = rr.left + (child ? 44 : 22);
            if (!child && g.procs.size() > 1)
                text(rt, br, t->open.count(g.key) ? L"\xE70D" : L"\xE76C", D2D1::RectF(rr.left + 4, y, rr.left + 20, y + kRow), icons, kDim);
            int ic = child ? p->icon : g.icon;
            if (ic >= 0) t->icon(t->ctx, ic, x0, y + (kRow - 16) / 2, 16);
            else text(rt, br, L"\xE7F8", D2D1::RectF(x0, y, x0 + 16, y + kRow), icons, kDim);
            std::wstring name = child ? p->name + L"  ·  " + std::to_wstring(p->pid) : g.label;
            text(rt, br, name, D2D1::RectF(x0 + 24, y, x_cnt - 8, y + kRow), f, kText);
            if (!child) text_right(rt, br, std::to_wstring(g.procs.size()), D2D1::RectF(x_cnt, y, x_cpu - 6, y + kRow), f, kDim);
            double cpu = child ? p->cpu : g.cpu;
            uint64_t mem = child ? p->priv : g.mem;
            float hm = heat(mem, t->cur.mem_total);
            if (hm > 0.02f) round_fill(rt, br, D2D1::RectF(x_mem + 4, rr.top + 2, right, rr.bottom - 2), 0xF2A33A, hm * 0.5f);
            float hc = (float)std::min(0.55, cpu / 100.0 * 3);
            if (hc > 0.02f) round_fill(rt, br, D2D1::RectF(x_cpu + 4, rr.top + 2, x_mem - 2, rr.bottom - 2), 0xF2A33A, hc * 0.5f);
            text_right(rt, br, cpu_text(cpu), D2D1::RectF(x_cpu, y, x_mem - 6, y + kRow), f, kText);
            text_right(rt, br, mem_text(mem), D2D1::RectF(x_mem, y, right - 6, y + kRow), f, kText);
        } else if (row.kind == 3) {
            const Service &s = t->services[row.a];
            bool run = s.state == SERVICE_RUNNING;
            text(rt, br, run ? L"\xE768" : L"\xE71A", D2D1::RectF(rr.left + 4, y, rr.left + 22, y + kRow), icons, run ? kAccent : kDim);
            text(rt, br, s.display + L"  (" + s.name + L")", D2D1::RectF(rr.left + 26, y, x_cnt - 8, y + kRow), f, run ? kText : kDim);
            text_right(rt, br, run ? T(L"Running") : s.state == SERVICE_STOPPED ? T(L"Stopped") : T(L"Changing"), D2D1::RectF(x_cnt, y, x_cpu - 6, y + kRow), f,
                       kDim);
            if (s.pid) {
                text_right(rt, br, std::to_wstring(s.pid), D2D1::RectF(x_cpu, y, x_mem - 6, y + kRow), f, kDim);
                for (const Proc &p : t->cur.procs)
                    if (p.pid == s.pid) {
                        text_right(rt, br, mem_text(p.priv), D2D1::RectF(x_mem, y, right - 6, y + kRow), f, kText);
                        break;
                    }
            }
        } else if (row.kind == 4) {
            const Startup &s = t->startup[row.a];
            text(rt, br, s.enabled ? L"\xE73A" : L"\xE739", D2D1::RectF(rr.left + 4, y, rr.left + 22, y + kRow), icons, s.enabled ? kAccent : kDim);
            text(rt, br, s.name, D2D1::RectF(rr.left + 26, y, x_cnt - 128, y + kRow), f, s.enabled ? kText : kDim);
            text_right(rt, br, s.where, D2D1::RectF(x_cnt - 120, y, x_mem - 6, y + kRow), small, kDim);
            text_right(rt, br, s.enabled ? T(L"Enabled") : T(L"Disabled"), D2D1::RectF(x_mem, y, right - 6, y + kRow), f, s.enabled ? kText : kDim);
        }
    }
    rt->PopAxisAlignedClip();
}

int tasks_row_at(TasksView *t, float x, float y) {
    if (!t) return -1;
    for (const Hot &h : t->hots)
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return h.id;
    if (y < t->list.top || y >= t->list.bottom || x < t->list.left || x >= t->list.right) return -1;
    int i = (int)((y - t->list.top + t->scroll) / kRow);
    return i >= 0 && i < (int)t->rows.size() ? i : -1;
}

void tasks_click(TasksView *t, HWND wnd, float x, float y, bool dbl, bool right) {
    if (!t) return;
    int id = tasks_row_at(t, x, y);
    if (id >= (int)kIdTab && id < (int)kIdTab + PG_COUNT) {
        t->page = id - kIdTab;
        t->scroll = 0;
        t->sel_key.clear();
        if (t->page == PG_SERVICES) load_services(t);
        if (t->page == PG_STARTUP) load_startup(t);
        build_rows(t);
        return;
    }
    if (id >= (int)kIdHead && id < (int)kIdHead + TC_N) {
        int c = id - kIdHead;
        if (t->sort == c) t->desc = !t->desc;
        else t->sort = c, t->desc = c != TC_NAME;
        build_groups(t);
        build_rows(t);
        return;
    }
    if (id < 0 || id >= (int)t->rows.size()) return;
    const Row r = t->rows[id];
    if (r.kind == 0) {
        t->collapsed[r.a] = !t->collapsed[r.a];
        build_rows(t);
        return;
    }
    t->sel_key = row_key(t, r);
    // the chevron, or a double click on a group: expand
    if (r.kind == 1 && (dbl || x < t->list.left + 24) && t->groups[r.a].procs.size() > 1) {
        const std::wstring &k = t->groups[r.a].key;
        if (!t->open.erase(k)) t->open.insert(k);
        build_rows(t);
        return;
    }
    if (r.kind == 4 && x < t->list.left + 26) { // the box: enable or disable
        Startup &s = t->startup[r.a];
        set_startup_enabled(s, !s.enabled);
        return;
    }
    if (right) {
        InvalidateRect(wnd, NULL, FALSE);
        UpdateWindow(wnd);
        context_menu(t, wnd);
    }
}

bool tasks_key(TasksView *t, HWND wnd, WPARAM key) {
    if (!t) return false;
    int sel = selected_row(t);
    int n = (int)t->rows.size();
    auto pick = [&](int i) {
        i = std::max(0, std::min(n - 1, i));
        if (i < 0) return;
        t->sel_key = row_key(t, t->rows[i]);
        float y = i * kRow, h = t->list.bottom - t->list.top;
        if (y < t->scroll) t->scroll = y;
        else if (y + kRow > t->scroll + h) t->scroll = y + kRow - h;
        clamp(t);
        InvalidateRect(wnd, NULL, FALSE);
    };
    switch (key) {
    case VK_DOWN: pick(sel + 1); return true;
    case VK_UP: pick(sel < 0 ? 0 : sel - 1); return true;
    case VK_HOME: pick(0); return true;
    case VK_END: pick(n - 1); return true;
    case VK_DELETE: end_selected(t, wnd); return true;
    case VK_APPS: context_menu(t, wnd); return true;
    case VK_RETURN:
    case VK_RIGHT:
    case VK_LEFT:
        if (sel >= 0 && t->rows[sel].kind == 1) {
            const std::wstring &k = t->groups[t->rows[sel].a].key;
            if (key == VK_LEFT) t->open.erase(k);
            else if (key == VK_RIGHT) t->open.insert(k);
            else if (!t->open.erase(k)) t->open.insert(k);
            build_rows(t);
            InvalidateRect(wnd, NULL, FALSE);
        }
        return true;
    }
    return false;
}

void tasks_wheel(TasksView *t, int delta) {
    if (!t) return;
    t->scroll -= delta / 120.f * kRow * 3;
    clamp(t);
}
