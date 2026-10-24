// shellops.cpp: see shellops.h.
#include "shellops.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <cstring>

namespace {

IContextMenu2 *g_cm2; // the menu open now (its extensions draw submenus through the owner window)
IContextMenu3 *g_cm3;

template <class T> void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

PIDLIST_ABSOLUTE pidl_of(const std::wstring &path) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), NULL, &p, 0, NULL))) return nullptr;
    return p;
}

// the items' parent folder and their child ids; only the items in the same folder as the first one
struct Children {
    IShellFolder *parent = nullptr;
    std::vector<PIDLIST_ABSOLUTE> abs;
    std::vector<PCUITEMID_CHILD> kids;
    ~Children() {
        release(parent);
        for (auto p : abs) CoTaskMemFree(p);
    }
    bool load(const std::vector<std::wstring> &items) {
        for (const std::wstring &s : items)
            if (PIDLIST_ABSOLUTE p = pidl_of(s)) abs.push_back(p);
        if (abs.empty()) return false;
        PCUITEMID_CHILD first = nullptr;
        if (FAILED(SHBindToParent(abs[0], IID_PPV_ARGS(&parent), &first))) return false;
        PIDLIST_ABSOLUTE folder = ILClone(abs[0]);
        ILRemoveLastID(folder);
        for (auto p : abs)
            if (ILIsParent(folder, p, TRUE)) kids.push_back(ILFindLastID(p));
        CoTaskMemFree(folder);
        return !kids.empty();
    }
};

template <class T> HRESULT ui_object(HWND wnd, const std::vector<std::wstring> &items, T **out) {
    Children c;
    if (!c.load(items)) return E_FAIL;
    return c.parent->GetUIObjectOf(wnd, (UINT)c.kids.size(), c.kids.data(), __uuidof(T), NULL, (void **)out);
}

// --- file operations on a thread of their own --------------------------------------------------------------

enum OpKind { OP_DELETE, OP_RENAME, OP_TRANSFER };
struct Op {
    OpKind kind;
    HWND wnd;
    std::vector<std::wstring> items;
    std::wstring to, name;
    bool permanent = false, move = false, confirm = true;
    IStream *data = nullptr; // the IDataObject, marshalled for that thread
};

DWORD WINAPI op_thread(void *p) {
    auto *op = (Op *)p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOperation *fo = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, NULL, CLSCTX_ALL, IID_PPV_ARGS(&fo)))) {
        fo->SetOwnerWindow(op->wnd);
        DWORD flags = FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT;
        if (op->kind == OP_DELETE) flags |= (op->permanent ? FOF_WANTNUKEWARNING : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE)) | (op->confirm ? 0 : FOF_NOCONFIRMATION);
        else flags |= FOF_ALLOWUNDO | FOFX_ADDUNDORECORD;
        fo->SetOperationFlags(flags);
        bool queued = false;
        if (op->kind == OP_TRANSFER) {
            IDataObject *data = nullptr;
            IShellItem *dest = nullptr;
            if (op->data && SUCCEEDED(CoGetInterfaceAndReleaseStream(op->data, IID_PPV_ARGS(&data)))) {
                op->data = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(op->to.c_str(), NULL, IID_PPV_ARGS(&dest))))
                    queued = SUCCEEDED(op->move ? fo->MoveItems(data, dest) : fo->CopyItems(data, dest));
            }
            release(dest);
            release(data);
        } else if (op->kind == OP_RENAME) {
            IShellItem *it = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(op->items[0].c_str(), NULL, IID_PPV_ARGS(&it))))
                queued = SUCCEEDED(fo->RenameItem(it, op->name.c_str(), NULL));
            release(it);
        } else {
            std::vector<PIDLIST_ABSOLUTE> pidls;
            for (const std::wstring &s : op->items)
                if (PIDLIST_ABSOLUTE pl = pidl_of(s)) pidls.push_back(pl);
            IShellItemArray *arr = nullptr;
            if (!pidls.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &arr)))
                queued = SUCCEEDED(fo->DeleteItems(arr));
            release(arr);
            for (auto pl : pidls) CoTaskMemFree(pl);
        }
        if (queued) fo->PerformOperations();
        release(fo);
    }
    if (op->data) op->data->Release();
    delete op;
    CoUninitialize();
    return 0;
}

void start(Op *op) {
    HANDLE t = CreateThread(NULL, 0, op_thread, op, 0, NULL);
    if (t) CloseHandle(t);
    else {
        if (op->data) op->data->Release();
        delete op;
    }
}

DWORD preferred_effect(IDataObject *data) {
    static CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    FORMATETC fe = { cf, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm = {};
    DWORD effect = DROPEFFECT_COPY;
    if (SUCCEEDED(data->GetData(&fe, &sm))) {
        if (DWORD *d = (DWORD *)GlobalLock(sm.hGlobal)) {
            effect = *d;
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
    }
    return effect;
}

// --- dropping into a window: the shell's own drop target of the folder under the point does the work -----------

class Dropper final : public IDropTarget {
    LONG ref_ = 1;
    HWND wnd_;
    DropHandler *h_;
    IDataObject *data_ = nullptr;
    IDropTarget *inner_ = nullptr;
    IDropTargetHelper *helper_ = nullptr;
    std::wstring inner_path_;

    // the folder under the point changed: leave the old one's target, enter the new one's
    void retarget(POINT pt, DWORD keys, DWORD *effect) {
        std::wstring path = h_->drop_folder(pt);
        if (path != inner_path_) {
            if (inner_) inner_->DragLeave();
            release(inner_);
            inner_path_ = path;
            if (!path.empty() && ui_object(wnd_, { path }, &inner_) == S_OK) {
                DWORD e = *effect;
                if (FAILED(inner_->DragEnter(data_, keys, { pt.x, pt.y }, &e))) release(inner_);
            }
        }
        if (inner_) inner_->DragOver(keys, { pt.x, pt.y }, effect);
        else *effect = DROPEFFECT_NONE;
    }

public:
    Dropper(HWND w, DropHandler *h) : wnd_(w), h_(h) { CoCreateInstance(CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_)); }
    ~Dropper() {
        release(inner_);
        release(data_);
        release(helper_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&ref_);
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *d, DWORD keys, POINTL p, DWORD *effect) override {
        release(data_);
        data_ = d;
        data_->AddRef();
        inner_path_.clear();
        POINT pt = { p.x, p.y };
        if (helper_) helper_->DragEnter(wnd_, d, &pt, *effect);
        retarget(pt, keys, effect);
        h_->drop_hover(pt, true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL p, DWORD *effect) override {
        POINT pt = { p.x, p.y };
        retarget(pt, keys, effect);
        if (helper_) helper_->DragOver(&pt, *effect);
        h_->drop_hover(pt, true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        if (inner_) inner_->DragLeave();
        release(inner_);
        release(data_);
        inner_path_.clear();
        h_->drop_hover({ 0, 0 }, false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *d, DWORD keys, POINTL p, DWORD *effect) override {
        POINT pt = { p.x, p.y };
        retarget(pt, keys, effect);
        if (helper_) helper_->Drop(d, &pt, *effect);
        if (inner_) inner_->Drop(d, keys, p, effect);
        else *effect = DROPEFFECT_NONE;
        release(inner_);
        release(data_);
        inner_path_.clear();
        h_->drop_hover(pt, false);
        return S_OK;
    }
};

} // namespace

std::string shell_menu(HWND wnd, const std::wstring &folder, const std::vector<std::wstring> &items, POINT pt) {
    IContextMenu *cm = nullptr;
    if (items.empty()) {
        if (folder.empty()) return ""; // Home's background has nothing of its own
        IShellFolder *sf = nullptr;
        PIDLIST_ABSOLUTE p = pidl_of(folder);
        if (p && SUCCEEDED(SHBindToObject(NULL, p, NULL, IID_PPV_ARGS(&sf)))) sf->CreateViewObject(wnd, IID_PPV_ARGS(&cm));
        release(sf);
        CoTaskMemFree(p);
    } else {
        ui_object(wnd, items, &cm);
    }
    if (!cm) return "";
    std::string verb;
    HMENU m = CreatePopupMenu();
    UINT flags = CMF_NORMAL | (items.empty() ? 0 : CMF_CANRENAME) | (GetKeyState(VK_SHIFT) < 0 ? CMF_EXTENDEDVERBS : 0);
    if (m && SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7FFF, flags))) {
        cm->QueryInterface(IID_PPV_ARGS(&g_cm3));
        cm->QueryInterface(IID_PPV_ARGS(&g_cm2));
        UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, wnd, NULL);
        release(g_cm3);
        release(g_cm2);
        if (cmd) {
            char v[64] = "";
            if (FAILED(cm->GetCommandString(cmd - 1, GCS_VERBA, NULL, v, sizeof(v)))) v[0] = 0;
            verb = v;
            if (verb == "rename") {
                // inline, in our own list
            } else {
                CMINVOKECOMMANDINFOEX ci = { sizeof(ci) };
                ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
                if (GetKeyState(VK_CONTROL) < 0) ci.fMask |= CMIC_MASK_CONTROL_DOWN;
                if (GetKeyState(VK_SHIFT) < 0) ci.fMask |= CMIC_MASK_SHIFT_DOWN;
                ci.hwnd = wnd;
                ci.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                ci.nShow = SW_SHOWNORMAL;
                ci.ptInvoke = pt;
                cm->InvokeCommand((CMINVOKECOMMANDINFO *)&ci);
                verb.clear();
            }
        }
    }
    if (m) DestroyMenu(m);
    cm->Release();
    return verb;
}

bool shell_menu_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *res) {
    if (g_cm3) return SUCCEEDED(g_cm3->HandleMenuMsg2(msg, wp, lp, res));
    if (g_cm2 && msg != WM_MENUCHAR) {
        *res = 0;
        return SUCCEEDED(g_cm2->HandleMenuMsg(msg, wp, lp));
    }
    return false;
}

void shell_clipboard(HWND wnd, const std::vector<std::wstring> &items, bool cut) {
    IDataObject *data = nullptr;
    if (items.empty() || FAILED(ui_object(wnd, items, &data))) return;
    static CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (g) {
        *(DWORD *)GlobalLock(g) = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY | DROPEFFECT_LINK;
        GlobalUnlock(g);
        FORMATETC fe = { cf, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm = { TYMED_HGLOBAL };
        sm.hGlobal = g;
        if (FAILED(data->SetData(&fe, &sm, TRUE))) GlobalFree(g);
    }
    if (SUCCEEDED(OleSetClipboard(data))) OleFlushClipboard(); // it stays there after explorer-lite exits
    data->Release();
}

bool shell_can_paste() { return IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)); }

void shell_paste(HWND wnd, const std::wstring &folder) {
    if (folder.empty()) return;
    IDataObject *data = nullptr;
    if (FAILED(OleGetClipboard(&data))) return;
    DWORD e = preferred_effect(data);
    shell_transfer(wnd, data, folder, (e & DROPEFFECT_MOVE) && !(e & DROPEFFECT_COPY));
    data->Release();
    if (e & DROPEFFECT_MOVE) OleSetClipboard(NULL); // a cut is pasted once, as in Windows
}

void shell_transfer(HWND wnd, IDataObject *data, const std::wstring &to, bool move) {
    auto *op = new Op{ OP_TRANSFER, wnd };
    op->to = to;
    op->move = move;
    if (FAILED(CoMarshalInterThreadInterfaceInStream(IID_IDataObject, data, &op->data))) {
        delete op;
        return;
    }
    start(op);
}

void shell_copy_to(HWND wnd, const std::vector<std::wstring> &items, const std::wstring &to, bool move) {
    IDataObject *data = nullptr;
    if (items.empty() || FAILED(ui_object(wnd, items, &data))) return;
    shell_transfer(wnd, data, to, move);
    data->Release();
}

void shell_delete(HWND wnd, const std::vector<std::wstring> &items, bool permanent, bool confirm) {
    if (items.empty()) return;
    auto *op = new Op{ OP_DELETE, wnd, items };
    op->permanent = permanent;
    op->confirm = confirm;
    start(op);
}

void shell_rename(HWND wnd, const std::wstring &item, const std::wstring &name) {
    if (name.empty()) return;
    auto *op = new Op{ OP_RENAME, wnd, { item } };
    op->name = name;
    start(op);
}

std::wstring shell_new_folder(const std::wstring &folder, const std::wstring &base) {
    if (folder.empty()) return L"";
    for (int i = 1; i < 1000; i++) {
        std::wstring name = i == 1 ? base : base + L" (" + std::to_wstring(i) + L")";
        std::wstring path = folder + (folder.back() == L'\\' ? L"" : L"\\") + name;
        if (CreateDirectoryW(path.c_str(), NULL)) {
            SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), NULL);
            return name;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    }
    return L"";
}

void shell_properties(HWND wnd, const std::wstring &item) { SHObjectProperties(wnd, SHOP_FILEPATH, item.c_str(), NULL); }

void shell_drag(HWND wnd, const std::vector<std::wstring> &items) {
    IDataObject *data = nullptr;
    if (items.empty() || FAILED(ui_object(wnd, items, &data))) return;
    DWORD effect = 0;
    SHDoDragDrop(wnd, data, NULL, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    data->Release();
}

void shell_register_drop(HWND wnd, DropHandler *h) {
    auto *d = new Dropper(wnd, h);
    RegisterDragDrop(wnd, d);
    d->Release(); // RegisterDragDrop holds it
}

void shell_revoke_drop(HWND wnd) { RevokeDragDrop(wnd); }

ULONG shell_watch(HWND wnd, UINT msg, const std::wstring &folder) {
    if (folder.empty()) return 0;
    PIDLIST_ABSOLUTE p = pidl_of(folder);
    if (!p) return 0;
    SHChangeNotifyEntry e = { p, FALSE };
    ULONG id = SHChangeNotifyRegister(wnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                      SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
                                          SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES,
                                      msg, 1, &e);
    CoTaskMemFree(p);
    return id;
}

void shell_unwatch(ULONG id) {
    if (id) SHChangeNotifyDeregister(id);
}
