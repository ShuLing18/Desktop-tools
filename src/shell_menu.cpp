#include "shell_menu.h"
#include <shellapi.h>
#include <climits>

namespace {
template<class T> struct Ptr { T* value = nullptr; ~Ptr() { if (value) value->Release(); } T** put() { return &value; } T* operator->() const { return value; } };
void Check(HRESULT result) { if (FAILED(result)) throw result; }
}
namespace box {
HRESULT ShellSelectionObject(HWND owner, const std::vector<std::wstring>& paths, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (paths.empty() || paths.size() > UINT_MAX) return E_INVALIDARG;
    struct Items {
        std::vector<PIDLIST_ABSOLUTE> values;
        ~Items() { for (auto item : values) CoTaskMemFree(item); }
    } items;
    std::vector<PCUITEMID_CHILD> children;
    Ptr<IShellFolder> folder;
    PIDLIST_ABSOLUTE common = nullptr;
    HRESULT hr = S_OK;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE absolute = ILCreateFromPathW(path.c_str());
        if (!absolute) { hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); break; }
        items.values.push_back(absolute);
        PIDLIST_ABSOLUTE parent = ILCloneFull(absolute);
        if (!parent) { hr = E_OUTOFMEMORY; break; }
        ILRemoveLastID(parent);
        if (!common) {
            common = parent;
            PCUITEMID_CHILD child = nullptr;
            hr = SHBindToParent(absolute, IID_IShellFolder, reinterpret_cast<void**>(folder.put()), &child);
        } else {
            const bool same = ILIsEqual(common, parent) != FALSE; CoTaskMemFree(parent);
            if (!same) { hr = E_INVALIDARG; break; }
        }
        if (FAILED(hr)) break;
        children.push_back(ILFindLastID(absolute));
    }
    CoTaskMemFree(common);
    if (FAILED(hr)) return hr;
    return folder->GetUIObjectOf(owner, static_cast<UINT>(children.size()), children.data(), iid, nullptr, result);
}
bool ShellMenu::Message(UINT message, WPARAM w, LPARAM l, LRESULT& result) {
    if (message != WM_INITMENUPOPUP && message != WM_DRAWITEM && message != WM_MEASUREITEM && message != WM_MENUCHAR) return false;
    if (menu3_) return SUCCEEDED(menu3_->HandleMenuMsg2(message, w, l, &result));
    if (menu2_) { result = 0; return SUCCEEDED(menu2_->HandleMenuMsg(message, w, l)); }
    return false;
}
void ShellMenu::Show(HWND owner, const std::vector<std::wstring>& paths, POINT point, const std::function<void()>& rename) {
    Ptr<IContextMenu> context;
    HRESULT hr = ShellSelectionObject(owner, paths, IID_IContextMenu, reinterpret_cast<void**>(context.put()));
    Check(hr);
    HMENU menu = CreatePopupMenu(); if (!menu) throw E_OUTOFMEMORY;
    hr = context->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE | (paths.size() == 1 ? CMF_CANRENAME : 0));
    if (FAILED(hr)) { DestroyMenu(menu); Check(hr); }
    Ptr<IContextMenu2> second; Ptr<IContextMenu3> third;
    context->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(third.put()));
    if (!third.value) context->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(second.put()));
    menu2_ = second.value; menu3_ = third.value;
    SetForegroundWindow(owner);
    const UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, owner, nullptr);
    menu2_ = nullptr; menu3_ = nullptr; DestroyMenu(menu); PostMessageW(owner, WM_NULL, 0, 0);
    if (!selected) return;
    wchar_t verb[128]{};
    context->GetCommandString(selected - 1, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 128);
    if (_wcsicmp(verb, L"rename") == 0 && rename) { rename(); return; }
    CMINVOKECOMMANDINFOEX command{}; command.cbSize = sizeof(command); command.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    command.hwnd = owner; command.lpVerb = MAKEINTRESOURCEA(selected - 1); command.lpVerbW = MAKEINTRESOURCEW(selected - 1);
    command.nShow = SW_SHOWNORMAL; command.ptInvoke = point;
    Check(context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command)));
}
}
