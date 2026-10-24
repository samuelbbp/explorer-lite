// perf.cpp: see perf.h. Performance counters (PDH) for CPU speed, disks and GPU engines, the interface table for
// network adapters, DXGI for the GPUs' names and memory, and what the system says once (CPU model, cores, SMBIOS).
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "perf.h"
#include <winioctl.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <algorithm>
#include <map>
#include <tuple>

namespace {

struct Hist {
    std::vector<float> a, b, c;
};

struct DiskInfo {
    std::wstring model, type;
    uint64_t capacity = 0;
};

struct NetPrev {
    uint64_t in = 0, out = 0;
    ULONGLONG t = 0;
    bool ips = false;
    std::wstring ipv4, ipv6;
};

struct Gpu {
    LUID luid;
    std::wstring name, driver;
    uint64_t ded = 0, sh = 0;
    UINT kmt = 0; // D3DKMT adapter handle, for the temperature
};

// D3DKMT (gdi32): the adapter's performance data, as Task Manager reads the temperature; declared here because
// the SDK headers zig carries do not have KMTQAITYPE_ADAPTERPERFDATA
struct KmtOpen {
    LUID luid;
    UINT adapter;
};
struct KmtQuery {
    UINT adapter;
    int type;
    void *data;
    UINT size;
};
struct KmtClose {
    UINT adapter;
};
struct KmtPerfData {
    UINT32 phys;
    ULONGLONG mem_freq, mem_freq_max, mem_freq_oc, mem_bw, pcie_bw;
    ULONG fan_rpm, power, temperature; // temperature in tenths of °C
    UCHAR power_override;
};
const int kKmtAdapterPerfData = 62;
typedef LONG(WINAPI *PKmt)(void *);

typedef BOOL(WINAPI *PPerfInfo)(PERFORMANCE_INFORMATION *, DWORD);
typedef wchar_t *(NTAPI *PIpText)(const void *, wchar_t *);

uint64_t luid64(LUID l) { return (uint64_t)(uint32_t)l.HighPart << 32 | l.LowPart; }

void push(std::vector<float> &h, double v) {
    h.push_back((float)v);
    if ((int)h.size() > kHistory) h.erase(h.begin());
}

std::wstring trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) i++;
    return s.substr(i);
}

} // namespace

struct Perf {
    HMODULE pdh = NULL, iph = NULL, dxgi = NULL;
    decltype(&PdhOpenQueryW) open = nullptr;
    decltype(&PdhAddEnglishCounterW) add = nullptr;
    decltype(&PdhCollectQueryData) collect = nullptr;
    decltype(&PdhGetFormattedCounterArrayW) array = nullptr;
    decltype(&PdhCloseQuery) close = nullptr;
    PDH_HQUERY q = NULL;
    PDH_HCOUNTER c_perf = NULL, c_freq = NULL, c_idle = NULL, c_read = NULL, c_write = NULL, c_resp = NULL, c_gpu = NULL, c_ded = NULL, c_sh = NULL;
    decltype(&GetIfTable2) iftable = nullptr;
    decltype(&FreeMibTable) freemib = nullptr;
    decltype(&GetAdaptersAddresses) addrs = nullptr;
    PKmt kmt_open = nullptr, kmt_query = nullptr, kmt_close = nullptr;
    PPerfInfo perfinfo = nullptr;
    std::map<std::wstring, Hist> hist;
    std::map<int, DiskInfo> disks;
    std::map<uint64_t, NetPrev> nets; // by interface LUID
    std::vector<Gpu> gpus;
    // asked once
    std::wstring cpu_name;
    double base_mhz = 0;
    int sockets = 0, cores = 0, logical = 0;
    uint64_t l1 = 0, l2 = 0, l3 = 0, installed = 0;
    bool virt = false;
    MemDevices mem;
};

namespace {

// a wildcard counter's instances ("_Total" left out) with their values
std::vector<std::pair<std::wstring, double>> read_all(Perf *p, PDH_HCOUNTER c, bool total = false) {
    std::vector<std::pair<std::wstring, double>> out;
    if (!c || !p->array) return out;
    DWORD size = 0, count = 0;
    if (p->array(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, NULL) != (PDH_STATUS)PDH_MORE_DATA || !size) return out;
    std::vector<BYTE> buf(size);
    auto *items = (PDH_FMT_COUNTERVALUE_ITEM_W *)buf.data();
    if (p->array(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) return out;
    for (DWORD i = 0; i < count; i++) {
        if (!items[i].szName || (!total && !wcscmp(items[i].szName, L"_Total"))) continue;
        DWORD st = items[i].FmtValue.CStatus;
        if (st != PDH_CSTATUS_VALID_DATA && st != PDH_CSTATUS_NEW_DATA) continue;
        out.push_back({ items[i].szName, items[i].FmtValue.doubleValue });
    }
    return out;
}

double read_one(Perf *p, PDH_HCOUNTER c) {
    auto all = read_all(p, c, true);
    return all.empty() ? 0 : all[0].second;
}

void cpu_once(Perf *p) {
    wchar_t name[256];
    DWORD n = sizeof(name);
    const wchar_t *key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProcessorNameString", RRF_RT_REG_SZ, NULL, name, &n) == ERROR_SUCCESS) p->cpu_name = trim(name);
    DWORD mhz = 0;
    n = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &n) == ERROR_SUCCESS) p->base_mhz = mhz;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    std::vector<BYTE> buf(len);
    if (len && GetLogicalProcessorInformationEx(RelationAll, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf.data(), &len)) {
        for (DWORD at = 0; at + sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD) <= len;) {
            auto *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf.data() + at);
            if (!e->Size) break;
            if (e->Relationship == RelationProcessorPackage) p->sockets++;
            else if (e->Relationship == RelationProcessorCore) {
                p->cores++;
                for (WORD g = 0; g < e->Processor.GroupCount; g++)
                    for (KAFFINITY m = e->Processor.GroupMask[g].Mask; m; m &= m - 1) p->logical++;
            } else if (e->Relationship == RelationCache) {
                uint64_t s = e->Cache.CacheSize;
                if (e->Cache.Level == 1) p->l1 += s;
                else if (e->Cache.Level == 2) p->l2 += s;
                else if (e->Cache.Level == 3) p->l3 += s;
            }
            at += e->Size;
        }
    }
    p->virt = IsProcessorFeaturePresent(21 /* PF_VIRT_FIRMWARE_ENABLED */) != FALSE;
    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb)) p->installed = kb * 1024;
    UINT sz = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (sz > 8) {
        std::vector<BYTE> t(sz);
        if (GetSystemFirmwareTable('RSMB', 0, t.data(), sz) == sz) {
            DWORD body = *(DWORD *)(t.data() + 4); // RawSMBIOSData: 4 bytes of version, the length, the table
            p->mem = smbios_memory(t.data() + 8, std::min<size_t>(body, sz - 8));
        }
    }
}

void gpus_once(Perf *p) {
    p->dxgi = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    typedef HRESULT(WINAPI * PFactory)(REFIID, void **);
    auto make = p->dxgi ? (PFactory)(void *)GetProcAddress(p->dxgi, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1 *f = nullptr;
    if (!make || FAILED(make(__uuidof(IDXGIFactory1), (void **)&f)) || !f) return;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (gdi) {
        p->kmt_open = (PKmt)(void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
        p->kmt_query = (PKmt)(void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
        p->kmt_close = (PKmt)(void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    }
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (a && SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !(d.VendorId == 0x1414 && d.DeviceId == 0x8c)) {
            bool seen = false;
            for (const Gpu &g : p->gpus) seen |= luid64(g.luid) == luid64(d.AdapterLuid);
            if (!seen) {
                Gpu g;
                g.luid = d.AdapterLuid;
                g.name = trim(d.Description);
                g.ded = d.DedicatedVideoMemory;
                g.sh = d.SharedSystemMemory;
                LARGE_INTEGER v;
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) {
                    wchar_t s[48];
                    swprintf(s, 48, L"%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
                    g.driver = s;
                }
                KmtOpen o = { g.luid, 0 };
                if (p->kmt_open && p->kmt_open(&o) == 0) g.kmt = o.adapter;
                p->gpus.push_back(g);
            }
        }
        if (a) a->Release();
        a = nullptr;
    }
    f->Release();
}

DiskInfo disk_info(int index) {
    DiskInfo di;
    wchar_t path[32];
    swprintf(path, 32, L"\\\\.\\PhysicalDrive%d", index);
    // no access asked: enough for these queries, and no administrator needed
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return di;
    DWORD got = 0;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    int bus = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &got, NULL) && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        auto *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        bus = d->BusType;
        if (d->ProductIdOffset && d->ProductIdOffset < got) {
            std::string s((const char *)buf + d->ProductIdOffset, strnlen((const char *)buf + d->ProductIdOffset, got - d->ProductIdOffset));
            di.model = trim(wide(s));
        }
    }
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp = {};
    bool known = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), &got, NULL) && got >= sizeof(sp);
    if (bus == BusTypeUsb || bus == BusTypeSd || bus == BusTypeMmc) di.type = L"Removable";
    else if (known) di.type = sp.IncursSeekPenalty ? L"HDD" : bus == BusTypeNvme ? L"SSD (NVMe)" : L"SSD";
    DISK_GEOMETRY_EX g = {};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &g, sizeof(g), &got, NULL)) di.capacity = (uint64_t)g.DiskSize.QuadPart;
    CloseHandle(h);
    return di;
}

void net_ips(Perf *p, uint64_t luid, NetPrev &np) {
    np.ips = true;
    if (!p->addrs) return;
    ULONG len = 16 << 10;
    std::vector<BYTE> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.resize(len);
        r = p->addrs(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME | 0x0800 /* GAA_FLAG_SKIP_DNS_INFO */, NULL, (IP_ADAPTER_ADDRESSES *)buf.data(), &len);
    }
    if (r != NO_ERROR) return;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto v4 = nt ? (PIpText)(void *)GetProcAddress(nt, "RtlIpv4AddressToStringW") : nullptr;
    auto v6 = nt ? (PIpText)(void *)GetProcAddress(nt, "RtlIpv6AddressToStringW") : nullptr;
    for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
        if (a->Luid.Value != luid) continue;
        std::wstring link_local;
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            wchar_t s[64] = {};
            const sockaddr *sa = u->Address.lpSockaddr;
            if (!sa) continue;
            if (sa->sa_family == AF_INET && v4 && np.ipv4.empty()) {
                v4(&((const sockaddr_in *)sa)->sin_addr, s);
                np.ipv4 = s;
            } else if (sa->sa_family == AF_INET6 && v6) {
                const auto *a6 = &((const sockaddr_in6 *)sa)->sin6_addr;
                v6(a6, s);
                bool ll = a6->u.Byte[0] == 0xfe && (a6->u.Byte[1] & 0xc0) == 0x80;
                if (ll) {
                    if (link_local.empty()) link_local = s;
                } else if (np.ipv6.empty()) np.ipv6 = s;
            }
        }
        if (np.ipv6.empty()) np.ipv6 = link_local;
    }
}

} // namespace

MemDevices smbios_memory(const BYTE *raw, size_t n) {
    MemDevices m;
    size_t at = 0;
    while (raw && at + 4 <= n) {
        BYTE type = raw[at], len = raw[at + 1];
        if (len < 4 || at + len > n || type == 127) break; // 127: the end of the table
        if (type == 17) {                                     // a memory device: a slot
            const BYTE *s = raw + at;
            m.slots++;
            WORD size = len >= 0x0E ? (WORD)(s[0x0C] | s[0x0D] << 8) : 0;
            if (size != 0 && size != 0xFFFF) { // 0: empty, 0xFFFF: unknown
                m.used++;
                if (len > 0x0E && !m.form) m.form = s[0x0E];
                int speed = len >= 0x22 ? (s[0x20] | s[0x21] << 8) : 0; // configured speed (SMBIOS 2.7)
                if ((!speed || speed == 0xFFFF) && len >= 0x17) speed = s[0x15] | s[0x16] << 8;
                if (speed == 0xFFFF) speed = 0;
                m.speed = std::max(m.speed, speed);
            }
        }
        // the strings after the formatted part end with two zeros
        at += len;
        while (at + 1 < n && (raw[at] || raw[at + 1])) at++;
        at += 2;
    }
    return m;
}

Perf *perf_open() {
    auto *p = new Perf;
    cpu_once(p);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    p->perfinfo = k ? (PPerfInfo)(void *)GetProcAddress(k, "K32GetPerformanceInfo") : nullptr;
    p->pdh = LoadLibraryExW(L"pdh.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (p->pdh) {
        p->open = (decltype(p->open))(void *)GetProcAddress(p->pdh, "PdhOpenQueryW");
        p->add = (decltype(p->add))(void *)GetProcAddress(p->pdh, "PdhAddEnglishCounterW");
        p->collect = (decltype(p->collect))(void *)GetProcAddress(p->pdh, "PdhCollectQueryData");
        p->array = (decltype(p->array))(void *)GetProcAddress(p->pdh, "PdhGetFormattedCounterArrayW");
        p->close = (decltype(p->close))(void *)GetProcAddress(p->pdh, "PdhCloseQuery");
    }
    if (p->open && p->add && p->collect && p->array && p->close && p->open(NULL, 0, &p->q) == ERROR_SUCCESS) {
        // English paths: the localized names change with the system's language
        struct {
            const wchar_t *path;
            PDH_HCOUNTER *c;
        } cs[] = {
            { L"\\Processor Information(_Total)\\% Processor Performance", &p->c_perf },
            { L"\\Processor Information(_Total)\\Processor Frequency", &p->c_freq },
            { L"\\PhysicalDisk(*)\\% Idle Time", &p->c_idle },
            { L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", &p->c_read },
            { L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", &p->c_write },
            { L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer", &p->c_resp },
            { L"\\GPU Engine(*)\\Utilization Percentage", &p->c_gpu },
            { L"\\GPU Adapter Memory(*)\\Dedicated Usage", &p->c_ded },
            { L"\\GPU Adapter Memory(*)\\Shared Usage", &p->c_sh },
        };
        for (auto &c : cs)
            if (p->add(p->q, c.path, 0, c.c) != ERROR_SUCCESS) *c.c = NULL;
        p->collect(p->q); // rates need two samples: the first one now
    } else p->q = NULL;
    p->iph = LoadLibraryExW(L"iphlpapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (p->iph) {
        p->iftable = (decltype(p->iftable))(void *)GetProcAddress(p->iph, "GetIfTable2");
        p->freemib = (decltype(p->freemib))(void *)GetProcAddress(p->iph, "FreeMibTable");
        p->addrs = (decltype(p->addrs))(void *)GetProcAddress(p->iph, "GetAdaptersAddresses");
    }
    gpus_once(p);
    return p;
}

void perf_close(Perf *p) {
    if (!p) return;
    if (p->q) p->close(p->q);
    for (Gpu &g : p->gpus)
        if (g.kmt && p->kmt_close) {
            KmtClose c = { g.kmt };
            p->kmt_close(&c);
        }
    if (p->pdh) FreeLibrary(p->pdh);
    if (p->iph) FreeLibrary(p->iph);
    if (p->dxgi) FreeLibrary(p->dxgi);
    delete p;
}

void perf_sample(Perf *p, PerfSample &out, double cpu, uint64_t mem_total, uint64_t mem_used) {
    if (p->q) p->collect(p->q);
    auto hist = [&](PerfRes &r, double a, double b = 0, double c = 0) {
        Hist &h = p->hist[r.key];
        push(h.a, a);
        push(h.b, b);
        push(h.c, c);
        r.h1 = h.a;
        r.h2 = h.b;
        r.h3 = h.c;
    };
    PERFORMANCE_INFORMATION pi = { sizeof(pi) };
    bool have_pi = p->perfinfo && p->perfinfo(&pi, sizeof(pi));

    PerfRes c;
    c.kind = RK_CPU;
    c.key = L"cpu";
    c.model = p->cpu_name;
    c.pct = std::min(100.0, std::max(0.0, cpu));
    c.base_mhz = p->base_mhz;
    double freq = read_one(p, p->c_freq), perf = read_one(p, p->c_perf);
    if (!freq) freq = p->base_mhz;
    c.mhz = perf > 0 ? freq * perf / 100 : 0;
    c.sockets = p->sockets, c.cores = p->cores, c.logical = p->logical;
    c.l1 = p->l1, c.l2 = p->l2, c.l3 = p->l3;
    c.virt = p->virt;
    c.uptime_ms = GetTickCount64();
    if (have_pi) c.procs = (int)pi.ProcessCount, c.threads = (int)pi.ThreadCount, c.handles = (int)pi.HandleCount;
    hist(c, c.pct);
    out.res.push_back(std::move(c));

    PerfRes m;
    m.kind = RK_MEM;
    m.key = L"mem";
    m.total = mem_total;
    m.used = mem_used;
    m.avail = mem_total - mem_used;
    m.pct = mem_total ? 100.0 * mem_used / mem_total : 0;
    if (have_pi) {
        uint64_t pg = pi.PageSize;
        m.commit = pi.CommitTotal * pg, m.commit_limit = pi.CommitLimit * pg, m.cached = pi.SystemCache * pg;
        m.paged = pi.KernelPaged * pg, m.nonpaged = pi.KernelNonpaged * pg;
    }
    if (p->installed > mem_total) m.reserved = p->installed - mem_total;
    m.speed = p->mem.speed, m.slots_used = p->mem.used, m.slots = p->mem.slots;
    m.form = p->mem.form == 0x09 ? L"DIMM" : p->mem.form == 0x0D ? L"SODIMM" : L"";
    hist(m, m.pct);
    out.res.push_back(std::move(m));

    // disks: "0 C: D:" is disk 0 with those volumes
    auto idle = read_all(p, p->c_idle), rd = read_all(p, p->c_read), wr = read_all(p, p->c_write), resp = read_all(p, p->c_resp);
    auto find = [](const std::vector<std::pair<std::wstring, double>> &v, const std::wstring &n) {
        for (auto &x : v)
            if (x.first == n) return x.second;
        return 0.0;
    };
    std::vector<PerfRes> disks;
    for (auto &d : idle) {
        int index = -1;
        if (swscanf(d.first.c_str(), L"%d", &index) != 1 || index < 0) continue;
        PerfRes r;
        r.kind = RK_DISK;
        r.index = index;
        r.key = L"disk" + std::to_wstring(index);
        size_t sp = d.first.find(L' ');
        r.name = sp == std::wstring::npos ? L"" : trim(d.first.substr(sp + 1));
        auto it = p->disks.find(index);
        if (it == p->disks.end()) it = p->disks.emplace(index, disk_info(index)).first;
        r.model = it->second.model;
        r.type = it->second.type;
        r.capacity = it->second.capacity;
        r.pct = std::min(100.0, std::max(0.0, 100.0 - d.second));
        r.read = find(rd, d.first);
        r.write = find(wr, d.first);
        r.resp_ms = find(resp, d.first) * 1000;
        hist(r, r.pct, r.read, r.write);
        out.disk = std::max(out.disk, r.pct);
        disks.push_back(std::move(r));
    }
    std::sort(disks.begin(), disks.end(), [](const PerfRes &a, const PerfRes &b) { return a.index < b.index; });
    for (PerfRes &r : disks) out.res.push_back(std::move(r));

    // network adapters: the hardware ones that are up (the table also has a row per filter driver on each)
    MIB_IF_TABLE2 *t = nullptr;
    if (p->iftable && p->freemib && p->iftable(&t) == NO_ERROR && t) {
        ULONGLONG now = GetTickCount64();
        std::map<uint64_t, NetPrev> still;
        for (ULONG i = 0; i < t->NumEntries; i++) {
            const MIB_IF_ROW2 &row = t->Table[i];
            if (!row.InterfaceAndOperStatusFlags.HardwareInterface || row.InterfaceAndOperStatusFlags.FilterInterface) continue;
            if (row.OperStatus != IfOperStatusUp || (row.Type != IF_TYPE_ETHERNET_CSMACD && row.Type != IF_TYPE_IEEE80211)) continue;
            uint64_t luid = row.InterfaceLuid.Value;
            NetPrev np;
            auto it = p->nets.find(luid);
            if (it != p->nets.end()) np = it->second;
            PerfRes r;
            r.kind = RK_NET;
            r.key = L"net" + std::to_wstring(luid);
            r.name = row.Alias;
            r.model = row.Description;
            r.wifi = row.Type == IF_TYPE_IEEE80211;
            if (np.t && now > np.t) {
                double dt = (now - np.t) / 1000.0;
                r.recv = row.InOctets >= np.in ? (row.InOctets - np.in) * 8 / dt : 0;
                r.send = row.OutOctets >= np.out ? (row.OutOctets - np.out) * 8 / dt : 0;
            }
            uint64_t link = std::max(row.ReceiveLinkSpeed, row.TransmitLinkSpeed);
            r.link = link == ~0ull ? 0 : link;
            r.pct = r.link ? std::min(100.0, 100.0 * (r.send + r.recv) / r.link) : 0;
            np.in = row.InOctets, np.out = row.OutOctets, np.t = now;
            if (!np.ips) net_ips(p, luid, np);
            r.ipv4 = np.ipv4, r.ipv6 = np.ipv6;
            still[luid] = np;
            hist(r, r.recv, r.send);
            out.net = std::max(out.net, r.pct);
            out.res.push_back(std::move(r));
        }
        p->freemib(t);
        p->nets.swap(still);
    }

    // GPUs: each engine's use summed over the processes; a GPU's use and a process's are their busiest engine
    std::map<uint64_t, std::map<std::pair<unsigned, unsigned>, double>> by_gpu;
    std::map<DWORD, std::map<std::tuple<uint64_t, unsigned, unsigned>, double>> by_pid;
    for (auto &e : read_all(p, p->c_gpu)) {
        unsigned pid = 0, hi = 0, lo = 0, phys = 0, eng = 0;
        if (swscanf(e.first.c_str(), L"pid_%u_luid_0x%x_0x%x_phys_%u_eng_%u", &pid, &hi, &lo, &phys, &eng) != 5) continue;
        uint64_t luid = (uint64_t)hi << 32 | lo;
        by_gpu[luid][{ phys, eng }] += e.second;
        by_pid[pid][std::make_tuple(luid, phys, eng)] += e.second;
    }
    for (auto &kv : by_pid) {
        double mx = 0;
        for (auto &e : kv.second) mx = std::max(mx, e.second);
        out.gpu[kv.first] = std::min(100.0, mx);
    }
    std::map<uint64_t, double> ded, sh;
    for (auto &e : read_all(p, p->c_ded)) {
        unsigned hi = 0, lo = 0;
        if (swscanf(e.first.c_str(), L"luid_0x%x_0x%x", &hi, &lo) == 2) ded[(uint64_t)hi << 32 | lo] += e.second;
    }
    for (auto &e : read_all(p, p->c_sh)) {
        unsigned hi = 0, lo = 0;
        if (swscanf(e.first.c_str(), L"luid_0x%x_0x%x", &hi, &lo) == 2) sh[(uint64_t)hi << 32 | lo] += e.second;
    }
    for (int i = 0; i < (int)p->gpus.size(); i++) {
        const Gpu &g = p->gpus[i];
        uint64_t l = luid64(g.luid);
        PerfRes r;
        r.kind = RK_GPU;
        r.index = i;
        r.key = L"gpu" + std::to_wstring(l);
        r.model = g.name;
        r.driver = g.driver;
        double mx = 0;
        for (auto &e : by_gpu[l]) mx = std::max(mx, e.second);
        r.pct = std::min(100.0, mx);
        r.ded_used = (uint64_t)ded[l], r.sh_used = (uint64_t)sh[l];
        r.ded_total = g.ded, r.sh_total = g.sh;
        if (g.kmt && p->kmt_query) {
            KmtPerfData d = {};
            KmtQuery q = { g.kmt, kKmtAdapterPerfData, &d, sizeof(d) };
            if (p->kmt_query(&q) == 0 && d.temperature > 0 && d.temperature < 2000) r.temp = d.temperature / 10.0;
        }
        hist(r, r.pct, (double)r.sh_used, (double)r.ded_used);
        out.gpu_all = std::max(out.gpu_all, r.pct);
        out.has_gpu = true;
        out.res.push_back(std::move(r));
    }
}
