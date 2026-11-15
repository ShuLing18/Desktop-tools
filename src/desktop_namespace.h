#pragma once
#include <windows.h>
#include <string>

namespace box {
struct DesktopNamespace {
    std::wstring id, name, executable, dataRoot, boxRoot, items, icon;
};

// userRoot is HKCU in production. Tests supply an isolated registry tree.
bool IsDesktopNamespaceId(const std::wstring& id);
std::wstring DesktopNamespaceClassKey(const std::wstring& id);
std::wstring DesktopNamespaceJunctionKey(const std::wstring& id);
std::wstring DesktopNamespaceName(HKEY userRoot, const std::wstring& id);
bool OwnsDesktopNamespace(HKEY userRoot, const std::wstring& id, const std::wstring& boxRoot);
bool DesktopNamespacePresent(HKEY userRoot, const std::wstring& id, const std::wstring& boxRoot);
bool DesktopNamespaceConfigured(HKEY userRoot, const DesktopNamespace& object);
HRESULT RegisterDesktopNamespace(HKEY userRoot, const DesktopNamespace& object, bool& changed);
HRESULT RevealDesktopNamespace(HKEY userRoot, const std::wstring& id, const std::wstring& boxRoot, bool& changed);
HRESULT UnregisterDesktopNamespace(HKEY userRoot, const std::wstring& id,
                                   const std::wstring& boxRoot, const std::wstring& executable, bool& changed);
HRESULT UnregisterDesktopNamespaces(HKEY userRoot, const std::wstring& executable, bool& changed);
}
