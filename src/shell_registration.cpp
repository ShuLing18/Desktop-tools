#include "shell_registration.h"
#include <shellapi.h>
#include <shlobj.h>
#include <vector>

namespace {
constexpr wchar_t kExtension[] = L".desktopbox";
constexpr wchar_t kProgId[] = L"DesktopBox.StorageBox.1";
constexpr wchar_t kOwner[] = L"DesktopBox.Native.ShellNew.1";
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
HRESULT Status(LSTATUS value) { return value == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(value); }
std::wstring Read(HKEY parent, const wchar_t* key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(parent, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(parent, key, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
    return buffer.data();
}
HRESULT Write(HKEY parent, const wchar_t* path, const wchar_t* name, const std::wstring& value, bool& changed) {
    Key key;
    LSTATUS result = RegCreateKeyExW(parent, path, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key.value, nullptr);
    if (result != ERROR_SUCCESS) return Status(result);
    DWORD bytes = 0;
    const bool exists = RegGetValueW(key.value, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
    if (exists && Read(key.value, nullptr, name) == value) return S_OK;
    result = RegSetValueExW(key.value, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (result == ERROR_SUCCESS) changed = true;
    return Status(result);
}
bool Exists(HKEY parent, const wchar_t* path) {
    Key key; return RegOpenKeyExW(parent, path, 0, KEY_READ, &key.value) == ERROR_SUCCESS;
}
bool Available(HKEY parent, const wchar_t* path) {
    return !Exists(parent, path) || Read(parent, path, L"DesktopBoxOwner") == kOwner;
}
}

namespace box {
std::wstring ShellNewCommand(const std::wstring& executable, const std::wstring& dataRoot) {
    // Command creates the native desktop link itself. No placeholder file is needed.
    return L"\"" + executable + L"\" --data-dir \"" + dataRoot + L"\" --new-box";
}
std::wstring ShellNewIcon(const std::wstring& customIcon) {
    if (!customIcon.empty() && GetFileAttributesW(customIcon.c_str()) != INVALID_FILE_ATTRIBUTES)
        return L"\"" + customIcon + L"\",0";
    SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock);
    const HRESULT result = SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICONLOCATION, &stock);
    if (FAILED(result)) throw result;
    return L"\"" + std::wstring(stock.szPath) + L"\"," + std::to_wstring(stock.iIcon);
}
HRESULT RegisterShellNew(HKEY classesRoot, const std::wstring& executable,
                         const std::wstring& dataRoot, const std::wstring& icon, bool& changed) {
    changed = false;
    if (!classesRoot || executable.empty() || dataRoot.empty()) return E_INVALIDARG;
    // Never take over a foreign file association using the same extension.
    if (!Available(classesRoot, kExtension) || !Available(classesRoot, kProgId))
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    auto write = [&](const wchar_t* path, const wchar_t* name, const std::wstring& value) {
        return Write(classesRoot, path, name, value, changed);
    };
    HRESULT hr = S_OK;
    for (const auto* path : {kExtension, kProgId}) {
        if (FAILED(hr = write(path, L"DesktopBoxOwner", kOwner)) ||
            FAILED(hr = write(path, L"DesktopBoxExecutable", executable))) return hr;
    }
    if (FAILED(hr = write(kExtension, nullptr, kProgId)) ||
        FAILED(hr = write(kProgId, nullptr, L"桌面收纳盒")) ||
        FAILED(hr = write(kProgId, L"FriendlyTypeName", L"桌面收纳盒")) ||
        FAILED(hr = write(L"DesktopBox.StorageBox.1\\DefaultIcon", nullptr, icon)) ||
        FAILED(hr = write(L"DesktopBox.StorageBox.1\\shell\\open\\command", nullptr,
                           L"\"" + executable + L"\" --data-dir \"" + dataRoot + L"\"")) ||
        FAILED(hr = write(L".desktopbox\\ShellNew", L"Command", ShellNewCommand(executable, dataRoot))) ||
        FAILED(hr = write(L".desktopbox\\ShellNew", L"ItemName", L"桌面收纳盒")) ||
        FAILED(hr = write(L".desktopbox\\ShellNew", L"IconPath", icon))) return hr;
    return S_OK;
}
HRESULT UnregisterShellNew(HKEY classesRoot, const std::wstring& executable, bool& changed) {
    changed = false;
    if (!classesRoot || executable.empty()) return E_INVALIDARG;
    for (const auto* path : {kExtension, kProgId}) {
        // An old uninstaller must not remove a newer installation's registration.
        if (Read(classesRoot, path, L"DesktopBoxOwner") != kOwner ||
            _wcsicmp(Read(classesRoot, path, L"DesktopBoxExecutable").c_str(), executable.c_str()) != 0) continue;
        const LSTATUS result = RegDeleteTreeW(classesRoot, path);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return Status(result);
        if (result == ERROR_SUCCESS) changed = true;
    }
    return S_OK;
}
HRESULT RegisterUserShellNew(const std::wstring& executable, const std::wstring& dataRoot, const std::wstring& icon) {
    Key classes; const LSTATUS opened = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0,
        KEY_READ | KEY_WRITE, nullptr, &classes.value, nullptr);
    if (opened != ERROR_SUCCESS) return Status(opened);
    bool changed = false; const HRESULT result = RegisterShellNew(classes.value, executable, dataRoot, icon, changed);
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return result;
}
HRESULT UnregisterUserShellNew(const std::wstring& executable) {
    Key classes; const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_READ | KEY_WRITE, &classes.value);
    if (opened == ERROR_FILE_NOT_FOUND) return S_OK;
    if (opened != ERROR_SUCCESS) return Status(opened);
    bool changed = false; const HRESULT result = UnregisterShellNew(classes.value, executable, changed);
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return result;
}
}
