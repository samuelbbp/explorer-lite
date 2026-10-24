// folder.h: what a folder holds, listed on a thread of its own so the window never waits for the disk.
#pragma once
#include "util.h"
#include <string>
#include <vector>

struct Item {
    std::wstring name;
    std::wstring target;   // the full path (Home's entries point anywhere; in a folder, folder + name)
    std::wstring date, type, size; // as shown
    uint64_t bytes = 0;
    FILETIME mtime = {};
    DWORD attr = 0;
    int icon = -1;         // index in the system image list
    bool dir = false;
};

enum SortCol { COL_NAME, COL_DATE, COL_TYPE, COL_SIZE, COL_COUNT };

struct Listing {
    HWND wnd;              // gets WM_APP_LIST with this Listing (it owns it then)
    uint64_t gen;          // which request this answers (newer ones replace older ones)
    std::wstring path;     // "" = Home
    std::vector<Item> items;
    std::wstring error;    // "" if it could be read
    double ms = 0;         // how long it took
};

// Starts listing `path` (Home if empty); WM_APP_LIST arrives with wParam = gen, lParam = the Listing. Then the files
// whose icon is their own (programs, shortcuts) get it on the same thread, and WM_APP_ICONS brings them in batches
// (lParam = an Icons) until folder_forget(gen): a folder left before that stops asking.
void folder_list(HWND wnd, uint64_t gen, const std::wstring &path);
void folder_forget(uint64_t gen);

struct Icons {
    uint64_t gen;
    std::vector<std::pair<std::wstring, int>> icons; // target, icon index
};
void folder_sort(std::vector<Item> &items, int col, bool desc);

// Search through Everything (voidtools) if it is running: what matches `text` under `path` (everywhere if ""),
// answered later with WM_COPYDATA to wnd (dwData = id). False if Everything is not there.
bool folder_search(HWND wnd, DWORD id, const std::wstring &path, const std::wstring &text);
// Everything's answer as items (their type column says the folder they are in)
void folder_search_items(const COPYDATASTRUCT *cds, std::vector<Item> &items);

std::wstring folder_title(const std::wstring &path);   // "Home", "Downloads", "C:", the last part of the path
std::wstring folder_parent(const std::wstring &path);  // "" (Home) above a drive
std::wstring folder_join(const std::wstring &dir, const std::wstring &name);
bool folder_exists(const std::wstring &path);
std::wstring folder_normalize(const std::wstring &typed); // what the user typed: quotes, env vars, slashes

// The known folders shown in the navigation pane and in Home: label, path and icon glyph.
struct Place {
    std::wstring label, path;
    const wchar_t *glyph;
};
std::vector<Place> folder_places(); // Home, Desktop, Downloads, Documents, Pictures, Music, Videos
std::vector<Place> folder_drives();
