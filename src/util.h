// util.h: small things used all over explorer-lite (COM pointers, clock, UTF-8 <-> UTF-16 text, data folder, logging).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdint>
#include <string>

// A COM pointer that releases itself (like Zas's).
template <class T> class Com {
    T *p = nullptr;
public:
    Com() {}
    ~Com() { reset(); }
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    Com(Com &&o) noexcept : p(o.p) { o.p = nullptr; }
    Com &operator=(Com &&o) noexcept {
        if (this != &o) {
            reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T **put() {
        reset();
        return &p;
    }
    T *get() const { return p; }
    T *operator->() const { return p; }
    operator T *() const { return p; }
};

double now_ms();                         // high-resolution clock
double since_process_start_ms();         // since Windows created the process
std::string utf8(const std::wstring &w);
std::wstring wide(const std::string &s);
std::wstring exe_dir();
std::wstring exe_path();
// The instance: "" for the real one; tests run their own with --instance <name>, which gets its own data
// folder, single-instance mutex and window classes, so it never touches the installed one.
void set_instance(const std::wstring &name);
const std::wstring &instance();
std::wstring instance_suffix();          // "" or "-<name>": appended to mutex and class names
std::wstring data_dir();                 // %LOCALAPPDATA%\explorer-lite[-<name>], created on first use
void log_line(const std::string &line);  // data_dir()\explorer-lite.log
// Where a test shot goes: data_dir()\shots\<name> (folder created). "" unless `name` is a bare file name ending in
// .png: a --cmd never writes anywhere else.
std::wstring shot_path(const std::wstring &name);
