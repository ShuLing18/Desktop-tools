#include "native_entry.h"
#include "desktop.h"
#include "model.h"
#include "desktop_namespace.h"
#include "shell_registration.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shlguid.h>
#include <algorithm>

namespace {
template<class T> struct Ptr {
    T* value = nullptr;
    ~Ptr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
};
void Check(HRESULT result) { if (FAILED(result)) throw result; }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring ParsingName(const std::wstring& path) { return path.rfind(L"::{", 0) == 0 ? path : box::BaseName(path); }
void NotifyObject(const std::wstring& path, LONG event) {
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
        SHChangeNotify(event, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, item, nullptr);
        CoTaskMemFree(item);
    }
}
std::wstring Arguments(const box::Storage& storage) {
    return L"--data-dir \"" + box::ParentPath(storage.journalDirectory) + L"\" --open-box \"" + storage.root + L"\" --drop";
}
bool Owned(const std::wstring& path, const box::Storage& storage) {
    if (path.empty() || !Exists(path)) return false;
    Ptr<IShellLinkW> link; Ptr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.put()))) ||
        FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()))) || FAILED(file->Load(path.c_str(), STGM_READ))) return false;
    wchar_t arguments[32768]{}, value[32768]{};
    if (SUCCEEDED(link->GetPath(value, 32768, nullptr, SLGP_RAWPATH)) && _wcsicmp(value, storage.items.c_str()) == 0 &&
        SUCCEEDED(link->GetWorkingDirectory(value, 32768)) && _wcsicmp(value, storage.root.c_str()) == 0) return true;
    // Win32 paths are case-insensitive; launchers may normalize the executable/root spelling.
    return SUCCEEDED(link->GetArguments(arguments, 32768)) && _wcsicmp(Arguments(storage).c_str(), arguments) == 0;
}
bool Configured(const std::wstring& path, const box::Storage& storage, const std::wstring& executable) {
    Ptr<IShellLinkW> link; Ptr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.put()))) ||
        FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()))) || FAILED(file->Load(path.c_str(), STGM_READ))) return false;
    wchar_t value[32768]{}; int iconIndex = 0;
    if (FAILED(link->GetPath(value, 32768, nullptr, SLGP_RAWPATH)) || _wcsicmp(value, storage.items.c_str()) != 0 ||
        FAILED(link->GetArguments(value, 32768)) || *value ||
        FAILED(link->GetWorkingDirectory(value, 32768)) || _wcsicmp(value, storage.root.c_str()) != 0 ||
        FAILED(link->GetIconLocation(value, 32768, &iconIndex))) return false;
    const auto icon = storage.SelectedIconPath();
    const auto& selected = !icon.empty() && Exists(icon) ? icon : executable;
    return iconIndex == 0 && _wcsicmp(value, selected.c_str()) == 0;
}
HRESULT DesktopView(IFolderView2** result) {
    Ptr<IShellWindows> windows; Ptr<IDispatch> dispatch; Ptr<IServiceProvider> services; Ptr<IShellBrowser> browser; Ptr<IShellView> view;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, reinterpret_cast<void**>(windows.put()));
    VARIANT empty{}; long desktop = 0;
    if (SUCCEEDED(hr)) hr = windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktop, SWFO_NEEDDISPATCH, dispatch.put());
    if (SUCCEEDED(hr) && !dispatch.value) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = dispatch->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(services.put()));
    if (SUCCEEDED(hr)) hr = services->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(browser.put()));
    if (SUCCEEDED(hr)) hr = browser->QueryActiveShellView(view.put());
    if (SUCCEEDED(hr)) hr = view->QueryInterface(IID_IFolderView2, reinterpret_cast<void**>(result));
    return hr;
}
}

namespace box {
std::vector<NativeBounds> ReadNativeBounds(const std::vector<std::wstring>& paths, HWND desktop) {
    std::vector<NativeBounds> result;
    result.reserve(paths.size());
    for (const auto& path : paths) result.push_back({path, {}, {}, false});
    if (!IsWindow(desktop) || paths.empty()) return result;
    Ptr<IFolderView2> view; Ptr<IShellFolder> folder;
    if (FAILED(DesktopView(view.put())) ||
        FAILED(view->GetFolder(IID_IShellFolder, reinterpret_cast<void**>(folder.put())))) return result;
    POINT spacing{}; FOLDERVIEWMODE mode{}; int nominal = 48;
    if (FAILED(view->GetSpacing(&spacing)) || FAILED(view->GetViewModeAndIconSize(&mode, &nominal)) ||
        spacing.x <= 0 || spacing.y <= 0 || nominal < 16 || nominal > 512) return result;
    const int pixels = MulDiv(nominal, GetDpiForWindow(desktop), 96);
    for (auto& item : result) {
        auto name = ParsingName(item.path); ULONG parsed = 0, attributes = 0;
        ITEMIDLIST* id = nullptr;
        if (name.empty() || FAILED(folder->ParseDisplayName(nullptr, nullptr, name.data(), &parsed, &id, &attributes))) continue;
        POINT position{}; const HRESULT found = view->GetItemPosition(id, &position); CoTaskMemFree(id);
        if (FAILED(found) || !IsWindow(desktop) || !ClientToScreen(desktop, &position)) continue;
        item.image = {position.x, position.y, position.x + pixels, position.y + pixels};
        item.cell = {position.x - (spacing.x - pixels) / 2, position.y - 2, 0, 0};
        item.cell.right = item.cell.left + spacing.x; item.cell.bottom = item.cell.top + spacing.y;
        item.located = true;
    }
    return result;
}
NativeEntry::~NativeEntry() { Invalidate(); }
void NativeEntry::Invalidate() {
    if (view_) view_->Release(); view_ = nullptr;
    CoTaskMemFree(item_); item_ = nullptr;
}
void NativeEntry::Ensure(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                         const std::wstring& desktopOverride, bool renameExisting, bool onlyIfChanged) {
    if (desktopOverride.empty() && storage.Read(L"DesktopEntryMode") != L"shortcut") {
        EnsureNamespace(storage, name, executable, renameExisting, onlyIfChanged); return;
    }
    std::wstring shortcutName = name;
    const auto id = storage.Read(L"DesktopNamespace");
    if (desktopOverride.empty() && OwnsDesktopNamespace(HKEY_CURRENT_USER, id, storage.root)) {
        path_ = L"::" + id;
        if (!renameExisting) RefreshName(storage, shortcutName);
        const auto bounds = ReadNativeBounds({path_}, DesktopSurface());
        if (!bounds.empty() && bounds.front().located) {
            storage.Write(L"NativePositionX", std::to_wstring(bounds.front().image.left));
            storage.Write(L"NativePositionY", std::to_wstring(bounds.front().image.top));
            storage.Write(L"NativePositionPending", L"1");
        }
    }
    EnsureShortcut(storage, shortcutName, executable, desktopOverride, renameExisting, onlyIfChanged);
    if (desktopOverride.empty() && IsDesktopNamespaceId(id)) {
        bool changed = false;
        Check(UnregisterDesktopNamespace(HKEY_CURRENT_USER, id, storage.root, executable, changed));
        if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}
void NativeEntry::EnsureNamespace(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                                 bool renameExisting, bool onlyIfChanged) {
    storage.EnsureMoveDropTarget();
    auto id = storage.Read(L"DesktopNamespace");
    if (!IsDesktopNamespaceId(id)) {
        GUID guid{}; Check(CoCreateGuid(&guid)); wchar_t text[64]{}; StringFromGUID2(guid, text, 64); id = text;
        storage.Write(L"DesktopNamespace", id);
    }
    const bool registered = OwnsDesktopNamespace(HKEY_CURRENT_USER, id, storage.root);
    auto previous = storage.Read(L"DesktopLink");
    std::wstring actualName = name;
    path_ = registered ? L"::" + id : previous;
    if (!renameExisting) RefreshName(storage, actualName);
    previous = storage.Read(L"DesktopLink");
    const bool legacyOwned = Owned(previous, storage);
    if (actualName.empty()) actualName = L"桌面收纳盒";
    // Capture the old Explorer image origin before replacing its link identity.
    if (legacyOwned && storage.ReadInt(L"NativePositionPending", 0) == 0) {
        const auto bounds = ReadNativeBounds({previous}, DesktopSurface());
        if (!bounds.empty() && bounds.front().located) {
            storage.Write(L"NativePositionX", std::to_wstring(bounds.front().image.left));
            storage.Write(L"NativePositionY", std::to_wstring(bounds.front().image.top));
            storage.Write(L"NativePositionPending", L"1");
        }
    }
    DesktopNamespace object{id, actualName, executable, ParentPath(storage.journalDirectory), storage.root,
                            storage.items, ShellNewIcon(storage.SelectedIconPath(), executable)};
    if (onlyIfChanged && !renameExisting && !legacyOwned && DesktopNamespaceConfigured(HKEY_CURRENT_USER, object)) {
        path_ = L"::" + id; return;
    }
    bool changed = false;
    Check(RegisterDesktopNamespace(HKEY_CURRENT_USER, object, changed));
    path_ = L"::" + id; Invalidate();
    if (!registered) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    // Validate the replacement with the actual Shell before removing any old shortcut.
    PIDLIST_ABSOLUTE item = nullptr;
    Ptr<IShellFolder> desktop, folder;
    HRESULT valid = SHParseDisplayName(path_.c_str(), nullptr, &item, 0, nullptr);
    if (SUCCEEDED(valid)) valid = SHGetDesktopFolder(desktop.put());
    if (SUCCEEDED(valid)) valid = desktop->BindToObject(item, nullptr, IID_IShellFolder, reinterpret_cast<void**>(folder.put()));
    CoTaskMemFree(item);
    if (FAILED(valid)) {
        if (!registered) { bool removed = false; UnregisterDesktopNamespace(HKEY_CURRENT_USER, id, storage.root, executable, removed); }
        Check(valid);
    }
    storage.Write(L"Name", actualName);
    if (legacyOwned) {
        if (!DeleteFileW(previous.c_str())) Check(HRESULT_FROM_WIN32(GetLastError()));
        storage.Write(L"LegacyDesktopLink", previous);
        storage.Write(L"DesktopLink", L"");
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, previous.c_str(), nullptr);
    }
    if (changed) NotifyObject(path_, registered ? SHCNE_UPDATEITEM : SHCNE_MKDIR);
}
void NativeEntry::EnsureShortcut(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                                const std::wstring& desktopOverride, bool renameExisting, bool onlyIfChanged) {
    const bool dropChanged = storage.EnsureMoveDropTarget();
    std::wstring desktop = desktopOverride;
    if (desktop.empty()) {
        wchar_t* folder = nullptr; Check(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_CREATE, nullptr, &folder));
        desktop = folder; CoTaskMemFree(folder);
    }
    path_ = storage.Read(L"DesktopLink");
    std::wstring actualName = name;
    RefreshName(storage, actualName);
    const auto previous = storage.Read(L"DesktopLink");
    const bool owned = Owned(previous, storage);
    std::wstring desired = desktop + L"\\" + SafeName(actualName) + L".lnk";
    // Explorer may have assigned a numeric suffix because another box has the same
    // display name. Refreshing its icon or restarting must reuse that exact item.
    if (owned && !renameExisting && _wcsicmp(ParentPath(previous).c_str(), desktop.c_str()) == 0) desired = previous;
    if (_wcsicmp(previous.c_str(), desired.c_str()) != 0 || !owned) {
        if (Exists(desired)) desired = UniquePath(desktop, SafeName(actualName), L".lnk");
        if (owned && !MoveFileExW(previous.c_str(), desired.c_str(), MOVEFILE_WRITE_THROUGH)) throw HRESULT_FROM_WIN32(GetLastError());
        if (owned) SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, previous.c_str(), desired.c_str());
    } else desired = previous;
    if (onlyIfChanged && owned && _wcsicmp(previous.c_str(), desired.c_str()) == 0 && Configured(desired, storage, executable)) {
        path_ = desired;
        if (dropChanged) SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, desired.c_str(), nullptr);
        return;
    }
    Ptr<IShellLinkW> link; Ptr<IPersistFile> file;
    Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.put())));
    Check(link->SetPath(storage.items.c_str())); Check(link->SetArguments(L""));
    Check(link->SetWorkingDirectory(storage.root.c_str())); Check(link->SetDescription(L"桌面收纳盒"));
    const auto icon = storage.SelectedIconPath();
    Check(link->SetIconLocation(!icon.empty() && Exists(icon) ? icon.c_str() : executable.c_str(), 0));
    Check(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()))); Check(file->Save(desired.c_str(), TRUE));
    storage.Write(L"DesktopLink", desired); path_ = desired; Invalidate();
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, desired.c_str(), nullptr);
}
bool NativeEntry::RefreshName(const Storage& storage, std::wstring& name) {
    if (NamespaceMode()) {
        const auto id = path_.substr(2);
        if (!OwnsDesktopNamespace(HKEY_CURRENT_USER, id, storage.root)) return false;
        const auto actual = DesktopNamespaceName(HKEY_CURRENT_USER, id);
        if (actual.empty() || actual == storage.Read(L"Name")) return false;
        name = actual; storage.Write(L"Name", name); return true;
    }
    if (path_.empty() || Exists(path_)) return false;
    const auto directory = ParentPath(path_);
    WIN32_FIND_DATAW found{}; HANDLE search = FindFirstFileW((directory + L"\\*.lnk").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool renamed = false;
    do {
        const auto candidate = directory + L"\\" + found.cFileName;
        if (Owned(candidate, storage)) {
            path_ = candidate; name = BaseName(candidate); name.resize(name.size() - 4);
            storage.Write(L"DesktopLink", path_); storage.Write(L"Name", name); Invalidate(); renamed = true; break;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search); return renamed;
}
void NativeEntry::Remove(const Storage& storage) {
    const auto id = storage.Read(L"DesktopNamespace");
    if (IsDesktopNamespaceId(id)) {
        // Resolve a PIDL while the class still exists so Explorer gets a precise removal.
        PIDLIST_ABSOLUTE item = nullptr; SHParseDisplayName((L"::" + id).c_str(), nullptr, &item, 0, nullptr);
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        bool changed = false;
        const auto hr = UnregisterDesktopNamespace(HKEY_CURRENT_USER, id, storage.root, executable, changed);
        if (changed && item) SHChangeNotify(SHCNE_RMDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, item, nullptr);
        CoTaskMemFree(item); Check(hr);
        if (changed) storage.Write(L"DesktopNamespace", L"");
    }
    const auto path = storage.Read(L"DesktopLink");
    if (Owned(path, storage)) {
        if (!DeleteFileW(path.c_str())) throw HRESULT_FROM_WIN32(GetLastError());
        SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, path.c_str(), nullptr);
    }
    storage.Write(L"DesktopLink", L""); path_.clear(); Invalidate();
}
bool NativeEntry::Bounds(RECT& cell, RECT& image) {
    HWND desktop = DesktopSurface();
    if (!desktop || path_.empty()) return false;
    if (!view_ && FAILED(DesktopView(&view_))) return false;
    if (!item_) {
        Ptr<IShellFolder> folder; ULONG parsed = 0, attributes = 0;
        auto name = ParsingName(path_);
        if (FAILED(view_->GetFolder(IID_IShellFolder, reinterpret_cast<void**>(folder.put()))) ||
            FAILED(folder->ParseDisplayName(nullptr, nullptr, name.data(), &parsed, &item_, &attributes))) return false;
    }
    POINT position{}, spacing{}; FOLDERVIEWMODE mode{}; int nominal = 48;
    if (FAILED(view_->GetItemPosition(item_, &position)) || FAILED(view_->GetSpacing(&spacing))) { Invalidate(); return false; }
    view_->GetViewModeAndIconSize(&mode, &nominal);
    ClientToScreen(desktop, &position);
    const int pixels = MulDiv(nominal, GetDpiForWindow(desktop), 96);
    image = {position.x, position.y, position.x + pixels, position.y + pixels};
    cell.left = position.x - (spacing.x - pixels) / 2; cell.top = position.y - 2;
    cell.right = cell.left + spacing.x; cell.bottom = cell.top + spacing.y;
    return true;
}
bool NativeEntry::BeginRename() {
    RECT cell{}, image{}; if (!Bounds(cell, image)) return false;
    Ptr<IShellView> shellView;
    if (FAILED(view_->QueryInterface(IID_IShellView, reinterpret_cast<void**>(shellView.put())))) return false;
    SetForegroundWindow(GetAncestor(DesktopSurface(), GA_ROOT));
    return SUCCEEDED(shellView->SelectItem(item_, SVSI_EDIT | SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED | SVSI_ENSUREVISIBLE));
}
bool NativeEntry::RestorePosition(const Storage& storage) {
    if (path_.empty() || !storage.ReadInt(L"NativePositionPending", 0)) return false;
    RECT cell{}, image{}; if (!Bounds(cell, image)) return false;
    DWORD flags = 0;
    if (FAILED(view_->GetCurrentFolderFlags(&flags))) return false;
    if (flags & FWF_AUTOARRANGE) { storage.Write(L"NativePositionPending", L"0"); return false; }
    POINT point{storage.ReadInt(L"NativePositionX", image.left), storage.ReadInt(L"NativePositionY", image.top)};
    if (!ScreenToClient(DesktopSurface(), &point)) return false;
    PCUITEMID_CHILD item = item_;
    if (FAILED(view_->SelectAndPositionItems(1, &item, &point, SVSI_POSITIONITEM | SVSI_NOSTATECHANGE))) return false;
    storage.Write(L"NativePositionPending", L"0"); return true;
}
void NativeEntry::Reveal(const Storage& storage, const std::wstring& name, const std::wstring& executable) {
    if (NamespaceMode()) {
        const auto id = path_.substr(2);
        if (!DesktopNamespacePresent(HKEY_CURRENT_USER, id, storage.root)) Ensure(storage, name, executable);
        bool changed = false;
        Check(RevealDesktopNamespace(HKEY_CURRENT_USER, id, storage.root, changed));
        if (changed) NotifyObject(path_, SHCNE_MKDIR);
    } else if (!Exists(path_)) Ensure(storage, name, executable);
}
}
