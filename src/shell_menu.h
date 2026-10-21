#pragma once
#include <windows.h>
#include <shlobj.h>
#include <functional>
#include <string>
#include <vector>

namespace box {
HRESULT ShellSelectionObject(HWND owner, const std::vector<std::wstring>& paths, REFIID iid, void** result);
class ShellMenu {
    IContextMenu2* menu2_ = nullptr;
    IContextMenu3* menu3_ = nullptr;
public:
    bool Message(UINT message, WPARAM w, LPARAM l, LRESULT& result);
    void Show(HWND owner, const std::vector<std::wstring>& paths, POINT point, const std::function<void()>& rename = {});
};
}
