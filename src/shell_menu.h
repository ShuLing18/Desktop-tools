#pragma once
#include <windows.h>
#include <shlobj.h>
#include <functional>
#include <string>
#include <vector>

namespace box {
HRESULT ShellSelectionObject(HWND owner, const std::vector<std::wstring>& paths, REFIID iid, void** result);
HRESULT ShowShellSelectionInExplorer(const std::vector<std::wstring>& paths);
class ShellMenu {
    IContextMenu2* menu2_ = nullptr;
    IContextMenu3* menu3_ = nullptr;
public:
    bool Message(UINT message, WPARAM w, LPARAM l, LRESULT& result);
    static HRESULT Invoke(HWND owner, IContextMenu* context, UINT offset, POINT point,
                          const std::function<void()>& rename = {}, const std::function<HRESULT()>& continueInExplorer = {});
    void Show(HWND owner, const std::vector<std::wstring>& paths, POINT point, const std::function<void()>& rename = {});
};
}
