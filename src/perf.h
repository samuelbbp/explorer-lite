// perf.h: the Task manager's Performance page and the totals above its columns: CPU, memory, each physical disk,
// each network adapter and each GPU, with a minute of history; also every process's GPU use. Read on the task
// manager's sampling thread (tasks.cpp), only while a Task manager tab is open: pdh.dll, iphlpapi.dll and dxgi.dll
// are loaded by perf_open and freed by perf_close, so the resident never has them. Nothing here needs an
// administrator. Per-process network use does (Windows reads it from a kernel ETW session), so it is not here.
#pragma once
#include "util.h"
#include <string>
#include <unordered_map>
#include <vector>

enum ResKind { RK_CPU, RK_MEM, RK_DISK, RK_NET, RK_GPU };

const int kHistory = 31; // a minute, one sample every 2 s

// One resource of the Performance page. Numbers are raw (bytes, bytes/s, %); the page formats them.
struct PerfRes {
    int kind = RK_CPU;
    std::wstring key;           // stable from sample to sample: "cpu", "mem", "disk0", "net<luid>", "gpu<luid>"
    int index = 0;              // Disk N, GPU N
    std::wstring name, model;   // disks: "C: D:"; adapters: alias and description; GPU, CPU: the model
    double pct = 0;             // the main graph's value, 0-100
    std::vector<float> h1, h2;  // history, oldest first: the main graph and a second series (write, send, shared)
    // CPU
    double mhz = 0, base_mhz = 0;
    int sockets = 0, cores = 0, logical = 0, procs = 0, threads = 0, handles = 0;
    uint64_t l1 = 0, l2 = 0, l3 = 0, uptime_ms = 0;
    bool virt = false;
    // memory
    uint64_t total = 0, used = 0, avail = 0, commit = 0, commit_limit = 0, cached = 0, paged = 0, nonpaged = 0, reserved = 0;
    int speed = 0, slots_used = 0, slots = 0;
    std::wstring form;          // "DIMM", "SODIMM"
    // disk
    double read = 0, write = 0, resp_ms = 0;
    uint64_t capacity = 0;
    std::wstring type;          // "SSD (NVMe)", "HDD", "SSD", "Removable"
    // network
    double send = 0, recv = 0;  // bits/s
    uint64_t link = 0;          // bits/s
    bool wifi = false;
    std::wstring ipv4, ipv6;
    // GPU
    uint64_t ded_used = 0, ded_total = 0, sh_used = 0, sh_total = 0;
    std::wstring driver;
    double temp = -1;           // °C, -1 unknown
    std::vector<float> h3;      // dedicated memory history (h2: shared)
};

struct PerfSample {
    std::vector<PerfRes> res;                 // CPU, memory, disks, adapters, GPUs, in that order
    std::unordered_map<DWORD, double> gpu;    // pid -> % (its busiest engine)
    double disk = 0, net = 0, gpu_all = 0;    // the column totals: busiest disk, adapter and GPU, %
    bool has_gpu = false;
};

struct Perf;
Perf *perf_open();
// one sample (every 2 s); cpu and memory are the ones tasks.cpp already read, so both pages agree
void perf_sample(Perf *p, PerfSample &out, double cpu, uint64_t mem_total, uint64_t mem_used);
void perf_close(Perf *p);

// SMBIOS memory devices (type 17) in a GetSystemFirmwareTable('RSMB') blob: configured speed (MT/s), slots
// used and in all, form factor. Bounds-checked: the table comes from the firmware.
struct MemDevices {
    int speed = 0, used = 0, slots = 0, form = 0;
};
MemDevices smbios_memory(const BYTE *raw, size_t n);
