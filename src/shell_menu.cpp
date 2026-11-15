#include "shell_menu.h"
#include <shellapi.h>
#include <commctrl.h>
#include <climits>

namespace {
template<class T> struct Ptr { T* value = nullptr; ~Ptr() { if (value) value->Release(); } T** put() { return &value; } T* operator->() const { return value; } };
void Check(HRESULT result) { if (FAILED(result)) throw result; }
struct ShellItems {
    std::vector<PIDLIST_ABSOLUTE> values;
    std::vector<PCUITEMID_CHILD> children;
    PIDLIST_ABSOLUTE common = nullptr;
    ~ShellItems() { for (auto item : values) CoTaskMemFree(item); CoTaskMemFree(common); }
    HRESULT Parse(const std::vector<std::wstring>& paths) {
        if (paths.empty() || paths.size() > UINT_MAX) return E_INVALIDARG;
        values.reserve(paths.size()); children.reserve(paths.size());
        for (const auto& path : paths) {
            PIDLIST_ABSOLUTE absolute = ILCreateFromPathW(path.c_str());
            if (!absolute) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            values.push_back(absolute);
            PIDLIST_ABSOLUTE parent = ILCloneFull(absolute);
            if (!parent) return E_OUTOFMEMORY;
            ILRemoveLastID(parent);
            if (!common) common = parent;
            else {
                const bool same = ILIsEqual(common, parent) != FALSE; CoTaskMemFree(parent);
                if (!same) return E_INVALIDARG;
            }
            children.push_back(ILFindLastID(absolute));
        }
        return S_OK;
    }
};
std::wstring CommandVerb(IContextMenu* context, UINT offset) {
    wchar_t wide[128]{};
    const HRESULT hr = context->GetCommandString(offset, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(wide), 128);
    wide[127] = 0;
    if (SUCCEEDED(hr)) return wide;
    char narrow[128]{};
    if (FAILED(context->GetCommandString(offset, GCS_VERBA, nullptr, narrow, 128))) return {};
    narrow[127] = 0;
    if (!MultiByteToWideChar(CP_ACP, 0, narrow, -1, wide, 128)) return {};
    return wide;
}
bool StartPinVerb(const std::wstring& verb) {
    return _wcsicmp(verb.c_str(), L"PinToStartScreen") == 0 || _wcsicmp(verb.c_str(), L"startpin") == 0 ||
           _wcsicmp(verb.c_str(), L"unpinfromstartscreen") == 0 || _wcsicmp(verb.c_str(), L"startunpin") == 0;
}
HRESULT ContinueStartPinInExplorer(HWND owner, const std::vector<std::wstring>& paths) {
    constexpr int openExplorer = 100;
    const TASKDIALOG_BUTTON buttons[] = {{openExplorer, L"在资源管理器中继续"}};
    TASKDIALOGCONFIG dialog{}; dialog.cbSize = sizeof(dialog); dialog.hwndParent = owner;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON; dialog.pszWindowTitle = L"桌面收纳盒";
    dialog.pszMainIcon = TD_INFORMATION_ICON; dialog.pszMainInstruction = L"请在文件资源管理器中完成固定";
    dialog.pszContent = L"Windows 拒绝了在收纳盒内执行此操作。\n\n继续后会打开并选中原项目，请右键该项目，选择固定到“开始”或取消固定。";
    dialog.cButtons = 1; dialog.pButtons = buttons; dialog.nDefaultButton = openExplorer;
    int selected = IDCANCEL;
    const HRESULT hr = TaskDialogIndirect(&dialog, &selected, nullptr, nullptr);
    if (FAILED(hr) || selected != openExplorer) return hr;
    // Select the original shortcut, preserving its arguments and working directory.
    return box::ShowShellSelectionInExplorer(paths);
}
}
namespace box {
HRESULT ShellSelectionObject(HWND owner, const std::vector<std::wstring>& paths, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    ShellItems items;
    HRESULT hr = items.Parse(paths);
    if (FAILED(hr)) return hr;
    Ptr<IShellFolder> folder; PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(items.values.front(), IID_IShellFolder, reinterpret_cast<void**>(folder.put()), &child);
    if (FAILED(hr)) return hr;
    return folder->GetUIObjectOf(owner, static_cast<UINT>(items.children.size()), items.children.data(), iid, nullptr, result);
}
HRESULT ShowShellSelectionInExplorer(const std::vector<std::wstring>& paths) {
    ShellItems items;
    const HRESULT hr = items.Parse(paths);
    if (FAILED(hr)) return hr;
    return SHOpenFolderAndSelectItems(items.common, static_cast<UINT>(items.children.size()), items.children.data(), 0);
}
HRESULT ShellMenu::Invoke(HWND owner, IContextMenu* context, UINT offset, POINT point,
                          const std::function<void()>& rename, const std::function<HRESULT()>& continueInExplorer) {
    if (!context) return E_POINTER;
    if (offset > USHRT_MAX) return E_INVALIDARG;
    const auto verb = CommandVerb(context, offset);
    if (_wcsicmp(verb.c_str(), L"rename") == 0 && rename) { rename(); return S_OK; }
    CMINVOKECOMMANDINFOEX command{}; command.cbSize = sizeof(command);
    command.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_FLAG_LOG_USAGE;
    command.hwnd = owner; command.lpVerb = MAKEINTRESOURCEA(offset); command.lpVerbW = MAKEINTRESOURCEW(offset);
    command.nShow = SW_SHOWNORMAL; command.ptInvoke = point;
    const HRESULT hr = context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&command));
    // Some Windows builds expose this verb in third-party menus but reject its invocation.
    // Do not retry a successful toggle or reinterpret errors from other file operations.
    if (hr == E_ACCESSDENIED && StartPinVerb(verb) && continueInExplorer) return continueInExplorer();
    return hr;
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
    Check(Invoke(owner, context.value, selected - 1, point, rename, [&] { return ContinueStartPinInExplorer(owner, paths); }));
}
}
