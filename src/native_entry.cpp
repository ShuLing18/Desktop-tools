#include "native_entry.h"
#include "desktop.h"
#include "model.h"
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
std::wstring Arguments(const box::Storage& storage) {
    return L"--data-dir \"" + box::ParentPath(storage.journalDirectory) + L"\" --open-box \"" + storage.root + L"\" --drop";
}
bool Owned(const std::wstring& path, const box::Storage& storage) {
    if (path.empty() || !Exists(path)) return false;
    Ptr<IShellLinkW> link; Ptr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.put()))) ||
        FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()))) || FAILED(file->Load(path.c_str(), STGM_READ))) return false;
    wchar_t arguments[32768]{};
    // Win32 paths are case-insensitive; launchers may normalize the executable/root spelling.
    return SUCCEEDED(link->GetArguments(arguments, 32768)) && _wcsicmp(Arguments(storage).c_str(), arguments) == 0;
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
NativeEntry::~NativeEntry() { Invalidate(); }
void NativeEntry::Invalidate() {
    if (view_) view_->Release(); view_ = nullptr;
    CoTaskMemFree(item_); item_ = nullptr;
}
void NativeEntry::Ensure(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                         const std::wstring& desktopOverride, bool renameExisting) {
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
    Ptr<IShellLinkW> link; Ptr<IPersistFile> file;
    Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.put())));
    Check(link->SetPath(executable.c_str())); Check(link->SetArguments(Arguments(storage).c_str()));
    Check(link->SetWorkingDirectory(ParentPath(executable).c_str())); Check(link->SetDescription(L"桌面收纳盒"));
    const auto icon = storage.SelectedIconPath();
    if (!icon.empty()) Check(link->SetIconLocation(icon.c_str(), 0));
    else {
        SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock);
        Check(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &stock)); Check(link->SetIconLocation(stock.szPath, stock.iIcon));
    }
    Check(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()))); Check(file->Save(desired.c_str(), TRUE));
    storage.Write(L"DesktopLink", desired); path_ = desired; Invalidate();
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, desired.c_str(), nullptr);
}
bool NativeEntry::RefreshName(const Storage& storage, std::wstring& name) {
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
        auto name = BaseName(path_);
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
}
