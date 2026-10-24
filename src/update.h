// update.h: look for and install new versions (copied from wa-lite). The latest published version is checked in
// the public repo justdiegocom/explorer-lite-releases (WinHTTP, no curl or telemetry: just that
// request), 30 s after startup and then every 4 hours, or by hand from the tray menu. If
// there is a new one it is downloaded (only from github.com / *.githubusercontent.com over HTTPS), its SHA256 is
// checked against the release's .sha256 (if it does not match or is missing, it is not installed) and launched
// with /S; explorer-lite saves its open windows and exits, and the installer reopens it with --updated.
#pragma once
#include <string>
#include "util.h"

#ifndef EXPLORER_LITE_VERSION
#define EXPLORER_LITE_VERSION "0.1.0" // just build <version> sets the real one
#endif

enum UpdateState { UP_IDLE, UP_CHECKING, UP_AVAILABLE, UP_DOWNLOADING, UP_READY, UP_FAILED, UP_LATEST };

struct UpdateInfo {
    int state = UP_IDLE;
    std::string version;   // the new one (without the v)
    std::wstring notes;    // the release's text (simple markdown)
    std::string url, sha_url;
    int64_t size = 0, done = 0;
    std::wstring file;     // the downloaded installer
    std::wstring error;
    bool manual = false;   // the search was requested by the user: they are told even if there is nothing
    bool notes_open = false;
};
extern UpdateInfo g_upd;

void update_init(HWND wnd, UINT msg); // whom to notify (PostMessage) when there is something
void update_check(bool manual);       // on another thread
void update_download();               // the new one, on another thread (and installed when done if requested)
bool update_install();                // launches the downloaded installer with /S (and then it must exit)
bool update_pump();                   // UI thread, on notification: true if something changed
bool newer_version(const std::string &a, const std::string &b); // a > b (semver, with or without v)
std::string sha256_published(const std::string &body);           // the hash in a .sha256, lowercase ("" if short or empty)
bool sha256_matches(const std::wstring &path, const std::string &want); // the file has exactly that hash
bool update_auto();                   // the "Update automatically" setting (default: on)
void update_set_auto(bool on);
