#pragma once
#include <windows.h>
#include <string>

namespace box {
// classesRoot is HKCU\Software\Classes in production; tests use an isolated key.
std::wstring ShellNewCommand(const std::wstring& executable, const std::wstring& dataRoot);
std::wstring ShellNewIcon(const std::wstring& customIcon);
HRESULT RegisterShellNew(HKEY classesRoot, const std::wstring& executable,
                         const std::wstring& dataRoot, const std::wstring& icon, bool& changed);
HRESULT UnregisterShellNew(HKEY classesRoot, const std::wstring& executable, bool& changed);
HRESULT RegisterUserShellNew(const std::wstring& executable, const std::wstring& dataRoot,
                             const std::wstring& icon);
HRESULT UnregisterUserShellNew(const std::wstring& executable);
}
