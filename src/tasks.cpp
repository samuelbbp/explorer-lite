// tasks.cpp: see tasks.h. One NtQuerySystemInformation(SystemProcessInformation) per sample gives every process with
// its parent, private working set, CPU times, I/O bytes and threads without opening any of them; only a new process is
// opened once (its path, for the icon, the description and whether it is part of Windows). The rest (GPU, disks,
// network, the Performance page) comes from perf.cpp on the same thread.
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
#include "perf.h"
#include <cmath>

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
    SIZE_T quota_peak_paged, quota_paged, quota_peak_nonpaged, quota_nonpaged, pagefile, pagefile_peak, private_pages;
    LARGE_INTEGER read_ops, write_ops, other_ops, read_bytes, write_bytes, other_bytes;
};
static_assert(sizeof(SysProc) == 0x100, "SYSTEM_PROCESS_INFORMATION is 0x100 bytes on x64");
// SYSTEM_THREAD_INFORMATION: the process's threads follow it
struct SysThread {
    LARGE_INTEGER kernel, user, create;
    ULONG wait_time;
    void *start;
    HANDLE pid, tid;
    LONG priority, base_priority;
    ULONG switches, state, wait_reason;
};
static_assert(sizeof(SysThread) == 0x50, "SYSTEM_THREAD_INFORMATION is 0x50 bytes on x64");
typedef LONG(WINAPI *PQuery)(ULONG, void *, ULONG, ULONG *);

struct Proc {
    DWORD pid = 0, ppid = 0, session = 0;
    ULONGLONG create = 0;
    std::wstring name, path, desc;
    uint64_t priv = 0, ws = 0;
    double cpu = 0;  // % of the whole machine
    double disk = 0; // bytes/s read and written
    double gpu = 0;  // % of its busiest GPU engine
    bool suspended = false;
    int icon = -1;
};

struct Sample {
    std::vector<Proc> procs;
    double cpu = 0;
    uint64_t mem_used = 0, mem_total = 0;
    PerfSample perf;
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
    std::map<std::pair<DWORD, ULONGLONG>, ULONGLONG> prev_io;   // (pid, created) -> bytes read + written
    Perf *perf = perf_open();
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
        std::map<std::pair<DWORD, ULONGLONG>, ULONGLONG> times, ios;
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
                // disk: what it read and wrote (Task Manager's column counts only the disk; this also counts files on
                // the network and pipes, but not the device calls of OtherTransferCount)
                ULONGLONG io = (ULONGLONG)sp->read_bytes.QuadPart + (ULONGLONG)sp->write_bytes.QuadPart;
                ios[key] = io;
                auto pio = prev_io.find(key);
                if (pio != prev_io.end() && span > 0 && io >= pio->second) pr.disk = (double)(io - pio->second) * 1e7 * cpus / span;
                // suspended: every thread waiting because it was suspended (the store apps Windows parks)
                if (sp->threads && at + sizeof(SysProc) + sp->threads * sizeof(SysThread) <= buf.data() + got) {
                    auto *th = (const SysThread *)(at + sizeof(SysProc));
                    pr.suspended = true;
                    for (ULONG j = 0; j < sp->threads && pr.suspended; j++) pr.suspended = th[j].state == 5 && th[j].wait_reason == 5;
                }
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
        prev_io.swap(ios);
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
        if (perf) {
            perf_sample(perf, out.perf, out.cpu, out.mem_total, out.mem_used);
            for (Proc &pr : out.procs) {
                auto g = out.perf.gpu.find(pr.pid);
                if (g != out.perf.gpu.end()) pr.gpu = g->second;
            }
        }
        AcquireSRWLockExclusive(&s->lock);
        s->latest = std::move(out);
        s->fresh = true;
        ReleaseSRWLockExclusive(&s->lock);
        PostMessageW(s->wnd, WM_APP_TASKS, 0, 0);
        WaitForSingleObject(s->wake, 2000);
    }
    perf_close(perf);
    CoUninitialize();
    return 0;
}

// --- the model --------------------------------------------------------------------------------------

enum Section { SEC_APPS, SEC_AGENTS, SEC_WINDOWS, SEC_COUNT };
enum Page { PG_PROCESSES, PG_PERF, PG_STARTUP, PG_SERVICES, PG_COUNT }; // the order of Task Manager's
enum Col { TC_NAME, TC_STATUS, TC_CPU, TC_MEM, TC_DISK, TC_NET, TC_GPU, TC_N };

struct Group {
    std::wstring key, label;
    int section = SEC_APPS;
    std::vector<int> procs; // indexes into the sample
    double cpu = 0, disk = 0, gpu = 0;
    uint64_t mem = 0;
    int suspended = 0; // how many of its processes
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
    std::vector<StartupEntry> startup;
    D2D1_RECT_F list = {};
    std::vector<Hot> hots;
    bool collapsed[SEC_COUNT] = { false, false, false };
    std::wstring res_key = L"cpu"; // the resource shown on the Performance page
    float res_scroll = 0;
    // the page's own type sizes, made from the window's font on first use
    IDWriteTextFormat *f_total = nullptr, *f_title = nullptr, *f_model = nullptr, *f_stat = nullptr, *f_item = nullptr;
    ID2D1StrokeStyle *dash = nullptr;
};

namespace {

const float kRow = 28, kHead = 30, kHeadTotals = 54, kResH = 68, kResW = 236;
const ULONG kIdTab = 10000, kIdHead = 20000, kIdRes = 30000;

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

// a number with the user's decimal and thousands separators, as Task Manager writes them ("3,604.9")
std::wstring num(double v, int decimals) {
    static wchar_t dec[8], thou[8];
    if (!dec[0]) {
        if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8)) wcscpy(dec, L".");
        if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, thou, 8)) wcscpy(thou, L",");
    }
    wchar_t raw[64];
    swprintf(raw, 64, L"%.*f", decimals, v);
    std::wstring s = raw, out;
    size_t dot = s.find(L'.');
    std::wstring whole = s.substr(0, dot), frac = dot == std::wstring::npos ? L"" : s.substr(dot + 1);
    size_t lead = whole[0] == L'-' ? 1 : 0;
    for (size_t i = 0; i < whole.size(); i++) {
        if (i > lead && (whole.size() - i) % 3 == 0) out += thou;
        out += whole[i];
    }
    if (!frac.empty()) out += dec + frac;
    return out;
}

// the Processes columns: "25.2%", "0%"
std::wstring pct_cell(double c) { return c < 0.05 ? std::wstring(L"0%") : num(c, 1) + L"%"; }
// "3,604.9 MB": always MB, as the column
std::wstring mb_cell(uint64_t b) { return num(b / 1048576.0, 1) + L" MB"; }
std::wstring mbs_cell(double b) { return b < 0.05 * 1048576 ? std::wstring(L"0 MB/s") : num(b / 1048576.0, 1) + L" MB/s"; }

// sizes and speeds on the Performance page
std::wstring size_text(uint64_t b) {
    if (b >= (1ull << 40)) return num(b / 1099511627776.0, 1) + L" TB";
    if (b >= (1ull << 30)) return num(b / 1073741824.0, 1) + L" GB";
    if (b >= (1ull << 20)) return num(b / 1048576.0, 0) + L" MB";
    return num(b / 1024.0, 0) + L" KB";
}
std::wstring bytes_rate(double b) {
    if (b >= 1048576) return num(b / 1048576.0, 1) + L" MB/s";
    return num(b / 1024.0, 0) + L" KB/s";
}
std::wstring bits_rate(double b) {
    if (b >= 1e9) return num(b / 1e9, 1) + L" Gbps";
    if (b >= 1e6) return num(b / 1e6, 1) + L" Mbps";
    return num(b / 1e3, b >= 1e5 ? 0 : 1) + L" Kbps";
}
std::wstring link_text(uint64_t b) {
    if (b >= 1000000000) return num(b / 1e9, b % 1000000000 ? 1 : 0) + L" Gbps";
    return num(b / 1e6, 0) + L" Mbps";
}
std::wstring ghz(double mhz) { return num(mhz / 1000, 2) + L" GHz"; }

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
        g.disk += ps[i].disk;
        g.gpu += ps[i].gpu;
        g.suspended += ps[i].suspended ? 1 : 0;
    }
    t->groups.clear();
    for (auto &kv : groups) t->groups.push_back(std::move(kv.second));
    for (Group &g : t->groups) g.gpu = std::min(100.0, g.gpu);
    auto value = [&](const Group &g) -> double {
        switch (t->sort) {
        case TC_STATUS: return g.suspended == (int)g.procs.size() ? 1 : 0;
        case TC_CPU: return g.cpu;
        case TC_DISK: return g.disk;
        case TC_GPU: return g.gpu;
        default: return (double)g.mem; // memory; network has no figure per process
        }
    };
    auto cmp = [&](const Group &a, const Group &b) {
        if (t->sort == TC_NAME) {
            int c = _wcsicmp(a.label.c_str(), b.label.c_str());
            return t->desc ? c > 0 : c < 0;
        }
        double x = value(a), y = value(b);
        if (x == y) return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
        return t->desc ? x > y : x < y;
    };
    std::sort(t->groups.begin(), t->groups.end(), cmp);
    // a group's processes in the same order
    auto pv = [&](const Proc &p) -> double {
        switch (t->sort) {
        case TC_STATUS: return p.suspended ? 1 : 0;
        case TC_CPU: return p.cpu;
        case TC_DISK: return p.disk;
        case TC_GPU: return p.gpu;
        default: return (double)p.priv;
        }
    };
    for (Group &g : t->groups)
        std::sort(g.procs.begin(), g.procs.end(), [&](int a, int b) {
            if (t->sort == TC_NAME) {
                int c = _wcsicmp(ps[a].name.c_str(), ps[b].name.c_str());
                return t->desc ? c > 0 : c < 0;
            }
            double x = pv(ps[a]), y = pv(ps[b]);
            if (x == y) return ps[a].priv > ps[b].priv;
            return t->desc ? x > y : x < y;
        });
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

} // namespace

std::vector<StartupEntry> startup_entries() {
    std::vector<StartupEntry> out;
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
            StartupEntry s;
            s.name = name;
            s.command = data;
            s.where = k.where;
            s.approved_root = k.root;
            s.approved_key = std::wstring(approved_base) + k.approved;
            s.enabled = approved(k.root, s.approved_key, s.name);
            s.editable = k.root == HKEY_CURRENT_USER; // the machine's need an administrator
            out.push_back(s);
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
            StartupEntry s;
            s.name = fd.cFileName;
            s.command = dir + L"\\" + fd.cFileName;
            s.folder = true;
            s.where = f.where;
            s.approved_root = f.root;
            s.approved_key = std::wstring(approved_base) + L"StartupFolder";
            s.enabled = approved(f.root, s.approved_key, s.name);
            s.editable = f.root == HKEY_CURRENT_USER;
            out.push_back(s);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

namespace {

void load_startup(TasksView *t) { t->startup = startup_entries(); }

void set_startup_enabled(StartupEntry &s, bool on) {
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
    if (t->page == PG_PERF) return; // no list: the resources are hot spots of their own
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
        const StartupEntry &s = t->startup[r.a];
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
    if (!f) return;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text(rt, br, s, r, f, rgb);
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void round_fill(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, D2D1_RECT_F r, uint32_t rgb, float a = 1) {
    br->SetColor(color(rgb, a));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br);
}

// a cell's shade, as Task Manager's heat map: every cell a little, the busier the stronger (the theme's accent)
float heat(double f) {
    f = std::min(1.0, std::max(0.0, f));
    float top = theme_light() ? 0.40f : 0.46f;
    return 0.07f + (top - 0.07f) * (float)std::sqrt(f);
}

IDWriteTextFormat *own_format(IDWriteTextFormat *like, float size, DWRITE_FONT_WEIGHT weight) {
    wchar_t family[64] = L"Segoe UI";
    if (like) like->GetFontFamilyName(family, 64);
    IDWriteTextFormat *f = nullptr;
    if (FAILED(g_dw->CreateTextFormat(family, NULL, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f))) return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteInlineObject *dots = nullptr;
    if (SUCCEEDED(g_dw->CreateEllipsisTrimmingSign(f, &dots))) {
        DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        f->SetTrimming(&tr, dots);
        dots->Release();
    }
    return f;
}

void make_formats(TasksView *t, IDWriteTextFormat *like) {
    if (t->f_total) return;
    t->f_total = own_format(like, 17, DWRITE_FONT_WEIGHT_NORMAL);
    t->f_title = own_format(like, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    t->f_model = own_format(like, 15, DWRITE_FONT_WEIGHT_NORMAL);
    t->f_stat = own_format(like, 19, DWRITE_FONT_WEIGHT_NORMAL);
    t->f_item = own_format(like, 14, DWRITE_FONT_WEIGHT_NORMAL);
}

// --- the Performance page ---------------------------------------------------------------------------

// each resource its colour, as Task Manager's graphs (CPU and GPU in the accent)
uint32_t res_color(int kind) {
    bool l = theme_light();
    switch (kind) {
    case RK_MEM: return l ? 0x8B12AE : 0xC37CF2;
    case RK_DISK: return l ? 0x4DA60C : 0x7CCB45;
    case RK_NET: return l ? 0xA74F01 : 0xE59A4B;
    default: return kAccent;
    }
}

// a round top for a graph's scale: 1, 2 or 5 times a power of ten of unit (1048576: in MB/s)
double nice_max(const std::vector<float> &a, const std::vector<float> &b, double floor, double unit = 1) {
    double v = floor;
    for (float x : a) v = std::max(v, (double)x);
    for (float x : b) v = std::max(v, (double)x);
    v /= unit;
    double p = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : { 1.0, 2.0, 5.0 })
        if (m * p >= v) return m * p * unit;
    return 10 * p * unit;
}

void series(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, D2D1_RECT_F g, const std::vector<float> &h, double max, uint32_t rgb, bool fill,
            bool dashed) {
    size_t n = h.size();
    if (n < 2 || max <= 0) return;
    float step = (g.right - g.left) / (kHistory - 1), ht = g.bottom - g.top;
    auto pt = [&](size_t i) {
        float v = (float)std::min(1.0, std::max(0.0, h[i] / max));
        return D2D1::Point2F(g.right - (float)(n - 1 - i) * step, g.bottom - v * ht);
    };
    for (int pass = fill ? 0 : 1; pass < 2; pass++) {
        ID2D1PathGeometry *pg = nullptr;
        ID2D1GeometrySink *sink = nullptr;
        if (FAILED(g_d2d->CreatePathGeometry(&pg)) || FAILED(pg->Open(&sink))) {
            if (pg) pg->Release();
            return;
        }
        if (pass == 0) { // the area under the line
            sink->BeginFigure(D2D1::Point2F(pt(0).x, g.bottom), D2D1_FIGURE_BEGIN_FILLED);
            for (size_t i = 0; i < n; i++) sink->AddLine(pt(i));
            sink->AddLine(D2D1::Point2F(pt(n - 1).x, g.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        } else {
            sink->BeginFigure(pt(0), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < n; i++) sink->AddLine(pt(i));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        sink->Close();
        sink->Release();
        if (pass == 0) {
            br->SetColor(color(rgb, 0.18f));
            rt->FillGeometry(pg, br);
        } else {
            br->SetColor(color(rgb));
            rt->DrawGeometry(pg, br, 1.4f, dashed ? t->dash : nullptr);
        }
        pg->Release();
    }
}

// a minute of history in a box with Task Manager's grid; b (dashed) is a second series on the same scale
void graph(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, D2D1_RECT_F g, uint32_t rgb, const std::vector<float> &a, double max,
           const std::vector<float> *b = nullptr, bool grid = true) {
    if (g.bottom - g.top < 8) return;
    if (grid) {
        br->SetColor(color(rgb, 0.14f));
        for (int i = 1; i < 10; i++) {
            float y = std::floor(g.top + (g.bottom - g.top) * i / 10) + 0.5f;
            rt->DrawLine(D2D1::Point2F(g.left, y), D2D1::Point2F(g.right, y), br, 1);
        }
        for (int i = 1; i < 20; i++) {
            float x = std::floor(g.left + (g.right - g.left) * i / 20) + 0.5f;
            rt->DrawLine(D2D1::Point2F(x, g.top), D2D1::Point2F(x, g.bottom), br, 1);
        }
    }
    rt->PushAxisAlignedClip(g, D2D1_ANTIALIAS_MODE_ALIASED);
    series(t, rt, br, g, a, max, rgb, true, false);
    if (b) series(t, rt, br, g, *b, max, rgb, false, true);
    rt->PopAxisAlignedClip();
    br->SetColor(color(rgb, 0.8f));
    rt->DrawRectangle(D2D1::RectF(g.left + 0.5f, g.top + 0.5f, g.right - 0.5f, g.bottom - 0.5f), br, 1);
}

std::wstring res_title(const PerfRes &r) {
    switch (r.kind) {
    case RK_CPU: return T(L"CPU");
    case RK_MEM: return T(L"Memory");
    case RK_DISK:
        return r.name.empty() ? Tf(L"Disk {0}", { std::to_wstring(r.index) }) : Tf(L"Disk {0} ({1})", { std::to_wstring(r.index), r.name });
    case RK_NET: return r.wifi ? T(L"Wi-Fi") : T(L"Ethernet");
    default: return Tf(L"GPU {0}", { std::to_wstring(r.index) });
    }
}

std::wstring used_of(uint64_t used, uint64_t total) {
    return num(used / 1073741824.0, 1) + L"/" + num(total / 1073741824.0, 1) + L" GB";
}

// the two lines under a resource's name in the list on the left
std::pair<std::wstring, std::wstring> res_lines(const PerfRes &r) {
    std::wstring pct = num(r.pct, 0) + L"%";
    switch (r.kind) {
    case RK_CPU: return { r.mhz > 0 ? pct + L"  " + ghz(r.mhz) : pct, L"" };
    case RK_MEM: return { used_of(r.used, r.total) + L" (" + pct + L")", L"" };
    case RK_DISK: return { !r.type.empty() ? r.type : r.model, pct };
    case RK_NET: return { r.name, Tf(L"S: {0}  R: {1}", { bits_rate(r.send), bits_rate(r.recv) }) };
    default: return { r.model, r.temp >= 0 ? pct + L" (" + num(r.temp, 0) + L" \x00B0" L"C)" : pct };
    }
}

double res_scale(const PerfRes &r) {
    return r.kind == RK_NET ? nice_max(r.h1, r.h2, 100e3) : 100;
}

std::wstring uptime_text(uint64_t ms) {
    uint64_t s = ms / 1000;
    wchar_t b[48];
    swprintf(b, 48, L"%llu:%02llu:%02llu:%02llu", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
    return b;
}

void draw_perf(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *small, D2D1_RECT_F r, int hover) {
    std::vector<PerfRes> &res = t->cur.perf.res;
    t->list = r;
    if (res.empty()) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(rt, br, T(L"Loading…"), D2D1::RectF(r.left, r.top + 20, r.right, r.top + 60), f, kDim);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    if (!t->dash) {
        float dashes[] = { 3, 2 };
        g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10,
                                                             D2D1_DASH_STYLE_CUSTOM, 0),
                                 dashes, 2, &t->dash);
    }
    int sel = 0;
    for (int i = 0; i < (int)res.size(); i++)
        if (res[i].key == t->res_key) sel = i;
    // the resources on the left
    D2D1_RECT_F side = D2D1::RectF(r.left, r.top, r.left + kResW, r.bottom);
    float max_scroll = std::max(0.f, res.size() * kResH + 16 - (side.bottom - side.top));
    t->res_scroll = std::min(std::max(0.f, t->res_scroll), max_scroll);
    rt->PushAxisAlignedClip(side, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < (int)res.size(); i++) {
        const PerfRes &p = res[i];
        float y = side.top + 8 + i * kResH - t->res_scroll;
        D2D1_RECT_F it = D2D1::RectF(side.left + 6, y, side.right - 6, y + kResH - 4);
        if (it.bottom < side.top || it.top > side.bottom) continue;
        if (i == sel) round_fill(rt, br, it, kSelect);
        else if (hover == (int)kIdRes + i) round_fill(rt, br, it, kHover);
        D2D1_RECT_F g = D2D1::RectF(it.left + 8, it.top + 10, it.left + 76, it.bottom - 10);
        br->SetColor(color(kBg));
        rt->FillRectangle(g, br);
        graph(t, rt, br, g, res_color(p.kind), p.h1, res_scale(p), p.kind == RK_NET ? &p.h2 : nullptr, false);
        float tx = g.right + 12;
        auto lines = res_lines(p);
        text(rt, br, res_title(p), D2D1::RectF(tx, it.top + 6, it.right - 6, it.top + 26), t->f_item, kText);
        text(rt, br, lines.first, D2D1::RectF(tx, it.top + 26, it.right - 6, it.top + 44), small, kDim);
        text(rt, br, lines.second, D2D1::RectF(tx, it.top + 43, it.right - 6, it.top + 61), small, kDim);
        t->hots.push_back({ it, (int)kIdRes + i });
    }
    rt->PopAxisAlignedClip();
    br->SetColor(color(kLine));
    rt->FillRectangle(D2D1::RectF(side.right, r.top, side.right + 1, r.bottom), br);

    // the one selected, big
    const PerfRes &p = res[sel];
    uint32_t col = res_color(p.kind);
    float x0 = side.right + 24, x1 = r.right - 24, y = r.top + 12;
    if (x1 - x0 < 200) return;
    text(rt, br, res_title(p), D2D1::RectF(x0, y, x0 + 260, y + 40), t->f_title, kText);
    text_right(rt, br, p.model, D2D1::RectF(x0 + 240, y, x1, y + 40), t->f_model, kText);
    y += 48;
    const float details = 186, label = 20;
    float avail = r.bottom - details - y;
    bool two = p.kind == RK_DISK || p.kind == RK_GPU;
    float main_h = two ? avail * 0.6f : avail;
    std::wstring l_left, l_right;
    double max = 100;
    const std::vector<float> *second = nullptr;
    switch (p.kind) {
    case RK_CPU: l_left = T(L"% Utilization"), l_right = L"100%"; break;
    case RK_MEM: l_left = T(L"Memory usage"), l_right = size_text(p.total); break;
    case RK_DISK: l_left = T(L"Active time"), l_right = L"100%"; break;
    case RK_NET:
        l_left = T(L"Throughput");
        max = res_scale(p);
        l_right = bits_rate(max);
        second = &p.h2;
        break;
    default: l_left = T(L"Utilization (busiest engine)"), l_right = L"100%";
    }
    text(rt, br, l_left, D2D1::RectF(x0, y, x1 - 120, y + label), small, kDim);
    text_right(rt, br, l_right, D2D1::RectF(x1 - 160, y, x1, y + label), small, kDim);
    D2D1_RECT_F g = D2D1::RectF(x0, y + label, x1, y + main_h - label);
    graph(t, rt, br, g, col, p.h1, max, second);
    text(rt, br, T(L"60 seconds"), D2D1::RectF(x0, g.bottom, x1 - 60, g.bottom + label), small, kDim);
    text_right(rt, br, L"0", D2D1::RectF(x1 - 60, g.bottom, x1, g.bottom + label), small, kDim);
    if (p.kind == RK_NET) {
        // which line is which: received solid, sent dashed
        text_right(rt, br, T(L"Receive: solid line   Send: dashed line"), D2D1::RectF(x0, g.bottom, x1 - 40, g.bottom + label), small, kDim);
    }
    y += main_h;
    if (p.kind == RK_DISK) {
        double m = nice_max(p.h2, p.h3, 1048576, 1048576);
        text(rt, br, T(L"Disk transfer rate (read solid, write dashed)"), D2D1::RectF(x0, y, x1 - 120, y + label), small, kDim);
        text_right(rt, br, bytes_rate(m), D2D1::RectF(x1 - 160, y, x1, y + label), small, kDim);
        graph(t, rt, br, D2D1::RectF(x0, y + label, x1, r.bottom - details - 8), col, p.h2, m, &p.h3);
    } else if (p.kind == RK_GPU) {
        float mid = (x0 + x1) / 2;
        text(rt, br, T(L"Dedicated GPU memory usage"), D2D1::RectF(x0, y, mid - 80, y + label), small, kDim);
        text_right(rt, br, size_text(p.ded_total), D2D1::RectF(mid - 90, y, mid - 8, y + label), small, kDim);
        graph(t, rt, br, D2D1::RectF(x0, y + label, mid - 8, r.bottom - details - 8), col, p.h3, (double)p.ded_total);
        text(rt, br, T(L"Shared GPU memory usage"), D2D1::RectF(mid + 8, y, x1 - 80, y + label), small, kDim);
        text_right(rt, br, size_text(p.sh_total), D2D1::RectF(x1 - 90, y, x1, y + label), small, kDim);
        graph(t, rt, br, D2D1::RectF(mid + 8, y + label, x1, r.bottom - details - 8), col, p.h2, (double)p.sh_total);
    }

    // the details: big figures on the left, the rest as a list on the right
    std::vector<std::pair<std::wstring, std::wstring>> big, info;
    auto add = [](std::vector<std::pair<std::wstring, std::wstring>> &v, const wchar_t *k, const std::wstring &val) {
        if (!val.empty()) v.push_back({ k, val });
    };
    std::wstring pct = num(p.pct, 0) + L"%";
    int cols = 2;
    switch (p.kind) {
    case RK_CPU:
        cols = 3;
        add(big, T(L"Utilization"), pct);
        add(big, T(L"Speed"), p.mhz > 0 ? ghz(p.mhz) : L"");
        add(big, T(L"Up time"), uptime_text(p.uptime_ms));
        add(big, T(L"Processes"), num(p.procs, 0));
        add(big, T(L"Threads"), num(p.threads, 0));
        add(big, T(L"Handles"), num(p.handles, 0));
        add(info, T(L"Base speed:"), p.base_mhz > 0 ? ghz(p.base_mhz) : L"");
        add(info, T(L"Sockets:"), p.sockets ? std::to_wstring(p.sockets) : L"");
        add(info, T(L"Cores:"), p.cores ? std::to_wstring(p.cores) : L"");
        add(info, T(L"Logical processors:"), p.logical ? std::to_wstring(p.logical) : L"");
        add(info, T(L"Virtualization:"), p.virt ? T(L"Enabled") : T(L"Disabled"));
        add(info, T(L"L1 cache:"), p.l1 ? size_text(p.l1) : L"");
        add(info, T(L"L2 cache:"), p.l2 ? size_text(p.l2) : L"");
        add(info, T(L"L3 cache:"), p.l3 ? size_text(p.l3) : L"");
        break;
    case RK_MEM:
        add(big, T(L"In use"), size_text(p.used));
        add(big, T(L"Available"), size_text(p.avail));
        add(big, T(L"Committed"), p.commit_limit ? used_of(p.commit, p.commit_limit) : L"");
        add(big, T(L"Cached"), p.cached ? size_text(p.cached) : L"");
        add(big, T(L"Paged pool"), p.paged ? size_text(p.paged) : L"");
        add(big, T(L"Non-paged pool"), p.nonpaged ? size_text(p.nonpaged) : L"");
        add(info, T(L"Total:"), size_text(p.total));
        add(info, T(L"Speed:"), p.speed ? num(p.speed, 0) + L" MT/s" : L"");
        add(info, T(L"Slots used:"), p.slots ? Tf(L"{0} of {1}", { std::to_wstring(p.slots_used), std::to_wstring(p.slots) }) : L"");
        add(info, T(L"Form factor:"), p.form);
        add(info, T(L"Hardware reserved:"), p.reserved ? size_text(p.reserved) : L"");
        break;
    case RK_DISK:
        add(big, T(L"Active time"), pct);
        add(big, T(L"Average response time"), num(p.resp_ms, 1) + L" ms");
        add(big, T(L"Read speed"), bytes_rate(p.read));
        add(big, T(L"Write speed"), bytes_rate(p.write));
        add(info, T(L"Capacity:"), p.capacity ? size_text(p.capacity) : L"");
        add(info, T(L"Type:"), p.type);
        add(info, T(L"Volumes:"), p.name);
        break;
    case RK_NET:
        add(big, T(L"Send"), bits_rate(p.send));
        add(big, T(L"Receive"), bits_rate(p.recv));
        add(info, T(L"Adapter name:"), p.name);
        add(info, T(L"Connection type:"), p.wifi ? T(L"Wi-Fi") : T(L"Ethernet"));
        add(info, T(L"Link speed:"), p.link ? link_text(p.link) : L"");
        add(info, T(L"IPv4 address:"), p.ipv4);
        add(info, T(L"IPv6 address:"), p.ipv6);
        break;
    default:
        add(big, T(L"Utilization"), pct);
        add(big, T(L"GPU memory"), used_of(p.ded_used + p.sh_used, p.ded_total + p.sh_total));
        add(big, T(L"Dedicated GPU memory"), used_of(p.ded_used, p.ded_total));
        add(big, T(L"Shared GPU memory"), used_of(p.sh_used, p.sh_total));
        add(big, T(L"GPU temperature"), p.temp >= 0 ? num(p.temp, 0) + L" \x00B0" L"C" : L"");
        add(info, T(L"Driver version:"), p.driver);
    }
    float dy = r.bottom - details + 8, cw = 150;
    for (int i = 0; i < (int)big.size(); i++) {
        float bx = x0 + (i % cols) * cw, by = dy + (i / cols) * 54;
        text(rt, br, big[i].first, D2D1::RectF(bx, by, bx + cw - 10, by + 18), small, kDim);
        text(rt, br, big[i].second, D2D1::RectF(bx, by + 18, bx + cw - 10, by + 44), t->f_stat, kText);
    }
    float ix = std::max(x0 + cols * cw + 20, x1 - 340);
    for (int i = 0; i < (int)info.size(); i++) {
        float iy = dy + i * 20;
        text(rt, br, info[i].first, D2D1::RectF(ix, iy, ix + 150, iy + 20), small, kDim);
        text(rt, br, info[i].second, D2D1::RectF(ix + 150, iy, x1, iy + 20), small, kText);
    }
}

// --- the Processes page -----------------------------------------------------------------------------

// the columns' left and right edges, from the right as Task Manager's: the name takes what is left
void proc_cols(D2D1_RECT_F r, float *l, float *rr) {
    static const float widths[TC_N] = { 0, 104, 74, 104, 84, 84, 66 };
    float x = r.right - 12;
    for (int i = TC_N - 1; i > TC_NAME; i--) {
        rr[i] = x;
        l[i] = x - widths[i];
        x = l[i];
    }
    l[TC_NAME] = r.left + 8;
    rr[TC_NAME] = std::max(l[TC_NAME] + 120, x);
}

void draw_processes(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *small, IDWriteTextFormat *icons,
                    D2D1_RECT_F r, int hover) {
    float cl[TC_N], cr[TC_N];
    proc_cols(r, cl, cr);
    float head = r.top, hb = head + kHeadTotals;
    const wchar_t *heads[TC_N] = { L"Name", L"Status", L"CPU", L"Memory", L"Disk", L"Network", L"GPU" }; // translate
    const PerfSample &ps = t->cur.perf;
    double totals[TC_N] = { 0, 0, t->cur.cpu, t->cur.mem_total ? 100.0 * t->cur.mem_used / t->cur.mem_total : 0, ps.disk, ps.net, ps.gpu_all };
    for (int i = 0; i < TC_N; i++) {
        D2D1_RECT_F c = D2D1::RectF(cl[i], head, cr[i], hb);
        if (hover == (int)kIdHead + i) round_fill(rt, br, D2D1::RectF(c.left + 1, c.top + 2, c.right - 1, c.bottom - 2), kHover);
        if (i > TC_NAME) {
            br->SetColor(color(kLine));
            rt->FillRectangle(D2D1::RectF(c.left, head + 6, c.left + 1, hb - 6), br);
        }
        if (i >= TC_CPU) {
            bool known = (i != TC_GPU || ps.has_gpu) && !t->cur.procs.empty();
            std::wstring total = known ? num(totals[i], 0) + L"%" : L"";
            text_right(rt, br, total, D2D1::RectF(c.left, head + 6, c.right - 8, head + 30), t->f_total, kText);
            text_right(rt, br, T(heads[i]), D2D1::RectF(c.left, head + 30, c.right - 8, hb - 4), small, kDim);
        } else text(rt, br, T(heads[i]), D2D1::RectF(c.left + 6, head + 30, c.right, hb - 4), small, kDim);
        if (t->sort == i) text(rt, br, t->desc ? L"\xE70D" : L"\xE70E", D2D1::RectF(c.left, head, c.right, head + 10), icons, kDim);
        t->hots.push_back({ c, (int)kIdHead + i });
    }
    br->SetColor(color(kLine));
    rt->FillRectangle(D2D1::RectF(r.left, hb - 1, r.right, hb), br);
    t->list = D2D1::RectF(r.left, hb, r.right, r.bottom);
    clamp(t);

    rt->PushAxisAlignedClip(t->list, D2D1_ANTIALIAS_MODE_ALIASED);
    int sel = selected_row(t);
    if (t->rows.empty()) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        text(rt, br, T(L"Loading…"), D2D1::RectF(t->list.left, t->list.top + 20, t->list.right, t->list.top + 60), f, kDim);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    uint64_t total = std::max<uint64_t>(1, t->cur.mem_total);
    int first = std::max(0, (int)(t->scroll / kRow)), last = std::min((int)t->rows.size(), (int)((t->scroll + (t->list.bottom - t->list.top)) / kRow) + 1);
    for (int i = first; i < last; i++) {
        const Row &row = t->rows[i];
        float y = t->list.top + i * kRow - t->scroll;
        D2D1_RECT_F rr = D2D1::RectF(r.left + 4, y + 1, r.right - 4, y + kRow - 1);
        auto cell = [&](int c) { return D2D1::RectF(cl[c], y, cr[c] - 8, y + kRow); };
        if (row.kind == 0) {
            static const wchar_t *names[SEC_COUNT] = { L"Apps", L"Agents", L"Windows" }; // translate
            int n = 0;
            uint64_t mem = 0;
            double cpu = 0, disk = 0, gpu = 0;
            for (const Group &g : t->groups)
                if (g.section == row.a) n++, mem += g.mem, cpu += g.cpu, disk += g.disk, gpu += g.gpu;
            if (hover == i) round_fill(rt, br, rr, kHover);
            text(rt, br, t->collapsed[row.a] ? L"\xE76C" : L"\xE70D", D2D1::RectF(rr.left + 4, y, rr.left + 22, y + kRow), icons, kDim);
            text(rt, br, std::wstring(T(names[row.a])) + L" (" + std::to_wstring(n) + L")", D2D1::RectF(rr.left + 26, y, cr[TC_STATUS], y + kRow), f, kAccent);
            text_right(rt, br, pct_cell(cpu), cell(TC_CPU), small, kDim);
            text_right(rt, br, mb_cell(mem), cell(TC_MEM), small, kDim);
            text_right(rt, br, mbs_cell(disk), cell(TC_DISK), small, kDim);
            text_right(rt, br, pct_cell(std::min(100.0, gpu)), cell(TC_GPU), small, kDim);
            continue;
        }
        const Group &g = t->groups[row.a];
        bool child = row.kind == 2;
        const Proc *p = child ? &t->cur.procs[row.b] : nullptr;
        double cpu = child ? p->cpu : g.cpu, disk = child ? p->disk : g.disk, gpu = child ? p->gpu : g.gpu;
        uint64_t mem = child ? p->priv : g.mem;
        if (i == sel) round_fill(rt, br, rr, kSelect);
        else {
            if (hover == i) round_fill(rt, br, rr, kHover);
            // the heat map: a cell per figure, touching its neighbours above and below as Task Manager's
            double level[TC_N] = { 0, 0, cpu / 50, (double)mem / total * 8, disk / (50.0 * 1048576), 0, gpu / 50 };
            for (int c = TC_CPU; c < TC_N; c++) {
                br->SetColor(color(kAccent, heat(level[c])));
                rt->FillRectangle(D2D1::RectF(cl[c] + 1, y, cr[c], y + kRow), br);
            }
        }
        float x0 = rr.left + (child ? 44 : 22);
        if (!child && g.procs.size() > 1)
            text(rt, br, t->open.count(g.key) ? L"\xE70D" : L"\xE76C", D2D1::RectF(rr.left + 4, y, rr.left + 20, y + kRow), icons, kDim);
        int ic = child ? p->icon : g.icon;
        if (ic >= 0) t->icon(t->ctx, ic, x0, y + (kRow - 16) / 2, 16);
        else text(rt, br, L"\xE7F8", D2D1::RectF(x0, y, x0 + 16, y + kRow), icons, kDim);
        std::wstring name = child ? p->name + L"  \x00B7  " + std::to_wstring(p->pid)
                                  : g.procs.size() > 1 ? g.label + L" (" + std::to_wstring(g.procs.size()) + L")" : g.label;
        text(rt, br, name, D2D1::RectF(x0 + 24, y, cr[TC_NAME] - 8, y + kRow), f, kText);
        bool susp = child ? p->suspended : g.suspended == (int)g.procs.size();
        if (susp) text(rt, br, T(L"Suspended"), D2D1::RectF(cl[TC_STATUS] + 8, y, cr[TC_STATUS] - 4, y + kRow), f, kDim);
        text_right(rt, br, pct_cell(cpu), cell(TC_CPU), f, kText);
        text_right(rt, br, mb_cell(mem), cell(TC_MEM), f, kText);
        text_right(rt, br, mbs_cell(disk), cell(TC_DISK), f, kText);
        // per process, Windows counts the network in a kernel trace only an administrator may start: not shown
        text_right(rt, br, L"\x2014", cell(TC_NET), f, kDim);
        text_right(rt, br, ps.has_gpu ? pct_cell(gpu) : L"\x2014", cell(TC_GPU), f, ps.has_gpu ? kText : kDim);
    }
    rt->PopAxisAlignedClip();
}

// --- Services and Startup apps ----------------------------------------------------------------------

void draw_lists(TasksView *t, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *br, IDWriteTextFormat *f, IDWriteTextFormat *small, IDWriteTextFormat *icons,
                D2D1_RECT_F r, int hover) {
    float right = r.right - 12;
    // columns from the right; the name takes the rest
    float x_mem = right - 110, x_cpu = x_mem - 90, x_cnt = x_cpu - 90;
    float head = r.top;
    const wchar_t *heads[4] = { L"Name", L"Status", L"PID", L"Memory" }; // translate
    D2D1_RECT_F cols[4] = { D2D1::RectF(r.left + 8, head, x_cnt, head + kHead), D2D1::RectF(x_cnt, head, x_cpu, head + kHead),
                            D2D1::RectF(x_cpu, head, x_mem, head + kHead), D2D1::RectF(x_mem, head, right, head + kHead) };
    if (t->page == PG_STARTUP) {
        heads[1] = L"Where";  // translate
        heads[2] = L"";
        heads[3] = L"Status"; // translate
        cols[1] = D2D1::RectF(x_cnt - 120, head, x_mem, head + kHead);
    }
    for (int i = 0; i < 4; i++) {
        if (!heads[i][0]) continue;
        D2D1_RECT_F c = cols[i];
        std::wstring h = T(heads[i]);
        if (i == 0) text(rt, br, h, D2D1::RectF(c.left + 4, c.top, c.right, c.bottom), small, kDim);
        else text_right(rt, br, h, D2D1::RectF(c.left, c.top, c.right - 6, c.bottom), small, kDim);
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
        if (i == sel) round_fill(rt, br, rr, kSelect);
        else if (hover == i) round_fill(rt, br, rr, kHover);
        if (row.kind == 3) {
            const Service &s = t->services[row.a];
            bool run = s.state == SERVICE_RUNNING;
            text(rt, br, run ? L"\xE768" : L"\xE71A", D2D1::RectF(rr.left + 4, y, rr.left + 22, y + kRow), icons, run ? kAccent : kDim);
            text(rt, br, s.display + L"  (" + s.name + L")", D2D1::RectF(rr.left + 26, y, x_cnt - 8, y + kRow), f, run ? kText : kDim);
            text_right(rt, br, run ? T(L"Running") : s.state == SERVICE_STOPPED ? T(L"Stopped") : T(L"Changing"), D2D1::RectF(x_cnt, y, x_cpu - 6, y + kRow), f,
                       kDim);
            if (s.pid) {
                text_right(rt, br, std::to_wstring(s.pid), D2D1::RectF(x_cpu, y, x_mem - 6, y + kRow), f, kDim);
                // a host shared with other services (svchost): its memory is theirs too, said so and dimmed
                int sharing = 0;
                for (const Service &o : t->services) sharing += o.pid == s.pid ? 1 : 0;
                for (const Proc &p : t->cur.procs)
                    if (p.pid == s.pid) {
                        std::wstring m = sharing > 1 ? Tf(L"{0} shared", { mem_text(p.priv) }) : mem_text(p.priv);
                        text_right(rt, br, m, D2D1::RectF(x_mem, y, right - 6, y + kRow), f, sharing > 1 ? kDim : kText);
                        break;
                    }
            }
        } else if (row.kind == 4) {
            const StartupEntry &s = t->startup[row.a];
            text(rt, br, s.enabled ? L"\xE73A" : L"\xE739", D2D1::RectF(rr.left + 4, y, rr.left + 22, y + kRow), icons, s.enabled ? kAccent : kDim);
            text(rt, br, s.name, D2D1::RectF(rr.left + 26, y, x_cnt - 128, y + kRow), f, s.enabled ? kText : kDim);
            text_right(rt, br, s.where, D2D1::RectF(x_cnt - 120, y, x_mem - 6, y + kRow), small, kDim);
            text_right(rt, br, s.enabled ? T(L"Enabled") : T(L"Disabled"), D2D1::RectF(x_mem, y, right - 6, y + kRow), f, s.enabled ? kText : kDim);
        }
    }
    rt->PopAxisAlignedClip();
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
    // a sample still running after 5 s (the first opens the counters, slow on a busy machine): the thread keeps its
    // Sampler, left behind rather than freed under it
    if (!s->thread || WaitForSingleObject(s->thread, 5000) == WAIT_OBJECT_0) {
        if (s->thread) CloseHandle(s->thread);
        CloseHandle(s->wake);
        delete s;
    }
    for (IDWriteTextFormat *f : { t->f_total, t->f_title, t->f_model, t->f_stat, t->f_item })
        if (f) f->Release();
    if (t->dash) t->dash->Release();
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
    const wchar_t *pages[PG_COUNT] = { L"Processes", L"Performance", L"Startup apps", L"Services" }; // translate
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
    make_formats(t, f);
    if (t->page == PG_PERF) draw_perf(t, rt, br, f, small, r, hover);
    else if (t->page == PG_PROCESSES) draw_processes(t, rt, br, f, small, icons, r, hover);
    else draw_lists(t, rt, br, f, small, icons, r, hover);
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
    if (id >= (int)kIdRes && id < (int)kIdRes + (int)t->cur.perf.res.size()) {
        t->res_key = t->cur.perf.res[id - kIdRes].key;
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
        StartupEntry &s = t->startup[r.a];
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
    if (t->page == PG_PERF) { // up and down go through the resources
        const std::vector<PerfRes> &res = t->cur.perf.res;
        int i = 0, n = (int)res.size();
        for (int j = 0; j < n; j++)
            if (res[j].key == t->res_key) i = j;
        if (key == VK_DOWN || key == VK_UP || key == VK_HOME || key == VK_END) {
            i = key == VK_HOME ? 0 : key == VK_END ? n - 1 : i + (key == VK_DOWN ? 1 : -1);
            if (n && i >= 0 && i < n) {
                t->res_key = res[i].key;
                float y = i * kResH, h = t->list.bottom - t->list.top - 16;
                if (y < t->res_scroll) t->res_scroll = y;
                else if (y + kResH > t->res_scroll + h) t->res_scroll = y + kResH - h;
                InvalidateRect(wnd, NULL, FALSE);
            }
            return true;
        }
        return false;
    }
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
    if (t->page == PG_PERF) {
        t->res_scroll -= delta / 120.f * kResH;
        return; // draw_perf keeps it in range
    }
    t->scroll -= delta / 120.f * kRow * 3;
    clamp(t);
}
