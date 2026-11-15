#include "desktop_namespace.h"
#include <shlobj.h>
#include <cstring>
#include <vector>

namespace {
constexpr wchar_t kOwner[] = L"DesktopBox.Native.Namespace.1";
constexpr wchar_t kClasses[] = L"Software\\Classes\\CLSID";
constexpr wchar_t kJunctions[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace";
constexpr wchar_t kHide[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\";
std::wstring Signature(const box::DesktopNamespace& object) {
    // Bump the schema prefix whenever the registration contract changes.
    return L"1|" + object.executable + L"|" + object.dataRoot + L"|" + object.boxRoot + L"|" + object.items + L"|" + object.icon;
}
std::wstring NameOverrideKey(const std::wstring& id) {
    return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CLSID\\" + id;
}
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
HRESULT Status(LSTATUS value) { return value == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(value); }
std::wstring Read(HKEY parent, const std::wstring& path, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(parent, path.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(parent, path.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
    return buffer.data();
}
bool Exists(HKEY parent, const std::wstring& path) {
    Key key; return RegOpenKeyExW(parent, path.c_str(), 0, KEY_READ, &key.value) == ERROR_SUCCESS;
}
HRESULT Write(HKEY parent, const std::wstring& path, const wchar_t* name, DWORD type,
              const void* value, DWORD bytes, bool& changed) {
    Key key;
    auto status = RegCreateKeyExW(parent, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key.value, nullptr);
    if (status != ERROR_SUCCESS) return Status(status);
    DWORD oldType = 0, oldBytes = 0;
    if (RegQueryValueExW(key.value, name, nullptr, &oldType, nullptr, &oldBytes) == ERROR_SUCCESS && oldType == type && oldBytes == bytes) {
        std::vector<BYTE> previous(bytes);
        if (RegQueryValueExW(key.value, name, nullptr, nullptr, previous.data(), &oldBytes) == ERROR_SUCCESS &&
            memcmp(previous.data(), value, bytes) == 0) return S_OK;
    }
    status = RegSetValueExW(key.value, name, 0, type, static_cast<const BYTE*>(value), bytes);
    if (status == ERROR_SUCCESS) changed = true;
    return Status(status);
}
HRESULT DeleteTree(HKEY root, const std::wstring& path, bool& changed) {
    const auto status = RegDeleteTreeW(root, path.c_str());
    if (status == ERROR_SUCCESS) changed = true;
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND ? S_OK : Status(status);
}
HRESULT DeleteValue(HKEY root, const std::wstring& path, const wchar_t* name, bool& changed) {
    Key key;
    auto status = RegOpenKeyExW(root, path.c_str(), 0, KEY_SET_VALUE, &key.value);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return S_OK;
    if (status != ERROR_SUCCESS) return Status(status);
    status = RegDeleteValueW(key.value, name);
    if (status == ERROR_SUCCESS) changed = true;
    return status == ERROR_FILE_NOT_FOUND ? S_OK : Status(status);
}
}

namespace box {
bool IsDesktopNamespaceId(const std::wstring& id) {
    if (id.size() != 38 || id.front() != L'{' || id.back() != L'}') return false;
    GUID value{}; return SUCCEEDED(CLSIDFromString(id.c_str(), &value)) && !IsEqualGUID(value, GUID_NULL);
}
std::wstring DesktopNamespaceClassKey(const std::wstring& id) { return std::wstring(kClasses) + L"\\" + id; }
std::wstring DesktopNamespaceJunctionKey(const std::wstring& id) { return std::wstring(kJunctions) + L"\\" + id; }
bool OwnsDesktopNamespace(HKEY root, const std::wstring& id, const std::wstring& boxRoot) {
    if (!root || !IsDesktopNamespaceId(id) || boxRoot.empty()) return false;
    const auto key = DesktopNamespaceClassKey(id);
    return Read(root, key, L"DesktopBoxOwner") == kOwner &&
           _wcsicmp(Read(root, key, L"DesktopBoxRoot").c_str(), boxRoot.c_str()) == 0;
}
bool DesktopNamespacePresent(HKEY root, const std::wstring& id, const std::wstring& boxRoot) {
    return OwnsDesktopNamespace(root, id, boxRoot) && Exists(root, DesktopNamespaceJunctionKey(id));
}
bool DesktopNamespaceConfigured(HKEY root, const DesktopNamespace& object) {
    const auto cls = DesktopNamespaceClassKey(object.id);
    return DesktopNamespacePresent(root, object.id, object.boxRoot) &&
           Read(root, cls, L"DesktopBoxSignature") == Signature(object) &&
           Read(root, cls + L"\\Instance\\InitPropertyBag", L"TargetFolderPath") == object.items &&
           Read(root, cls + L"\\DefaultIcon", nullptr) == object.icon;
}
std::wstring DesktopNamespaceName(HKEY root, const std::wstring& id) {
    if (!IsDesktopNamespaceId(id)) return {};
    // Native F2 uses Explorer's per-user name override rather than the class default.
    const auto renamed = Read(root, NameOverrideKey(id), nullptr);
    return renamed.empty() ? Read(root, DesktopNamespaceClassKey(id), nullptr) : renamed;
}
HRESULT RegisterDesktopNamespace(HKEY root, const DesktopNamespace& object, bool& changed) {
    changed = false;
    if (!root || !IsDesktopNamespaceId(object.id) || object.name.empty() || object.executable.empty() ||
        object.dataRoot.empty() || object.boxRoot.empty() || object.items.empty() || object.icon.empty()) return E_INVALIDARG;
    const auto cls = DesktopNamespaceClassKey(object.id), junction = DesktopNamespaceJunctionKey(object.id);
    const bool owned = OwnsDesktopNamespace(root, object.id, object.boxRoot);
    // Never claim another program's CLSID, namespace entry, or a different box's identity.
    if ((Exists(root, cls) && !owned) ||
        (!owned && Exists(root, NameOverrideKey(object.id)) && Read(root, NameOverrideKey(object.id), L"DesktopBoxOwner") != kOwner) ||
        (Exists(root, junction) && (!owned || Read(root, junction, L"DesktopBoxOwner") != kOwner)))
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    auto str = [&](const std::wstring& path, const wchar_t* name, const std::wstring& value) {
        return Write(root, path, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)), changed);
    };
    auto num = [&](const std::wstring& path, const wchar_t* name, DWORD value) {
        return Write(root, path, name, REG_DWORD, &value, sizeof(value), changed);
    };
    wchar_t system[32768]{};
    if (!GetSystemDirectoryW(system, 32768)) return HRESULT_FROM_WIN32(GetLastError());
    const auto shell = std::wstring(system) + L"\\shell32.dll";
    HRESULT hr = S_OK;
    if (FAILED(hr = str(cls, L"DesktopBoxOwner", kOwner)) ||
        FAILED(hr = str(cls, L"DesktopBoxRoot", object.boxRoot)) ||
        FAILED(hr = str(cls, L"DesktopBoxExecutable", object.executable)) ||
        FAILED(hr = str(cls, nullptr, object.name)) ||
        FAILED(hr = str(NameOverrideKey(object.id), L"DesktopBoxOwner", kOwner)) ||
        FAILED(hr = str(NameOverrideKey(object.id), nullptr, object.name)) ||
        FAILED(hr = str(cls, L"InfoTip", L"桌面收纳盒 · 双击打开文件，悬停展开收纳面板")) ||
        FAILED(hr = str(cls, L"HideAsDeletePerUser", L"")) ||
        FAILED(hr = num(cls, L"System.IsPinnedToNameSpaceTree", 0)) ||
        FAILED(hr = str(cls + L"\\DefaultIcon", nullptr, object.icon)) ||
        FAILED(hr = str(cls + L"\\InProcServer32", nullptr, shell)) ||
        FAILED(hr = str(cls + L"\\InProcServer32", L"ThreadingModel", L"Apartment")) ||
        // Windows' own FolderShortcut instance supplies folder browsing and drag/drop.
        FAILED(hr = str(cls + L"\\Instance", L"CLSID", L"{0E5AAE11-A475-4C5B-AB00-C66DE400274E}")) ||
        FAILED(hr = str(cls + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", object.items)) ||
        FAILED(hr = num(cls + L"\\Instance\\InitPropertyBag", L"Attributes", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) ||
        // The root is a virtual folder, never a link or a movable physical data directory.
        FAILED(hr = num(cls + L"\\ShellFolder", L"Attributes", SFGAO_FOLDER | SFGAO_HASSUBFOLDER |
            SFGAO_DROPTARGET | SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_STORAGEANCESTOR | SFGAO_FILESYSANCESTOR))) return hr;

    const auto launch = L"\"" + object.executable + L"\" --data-dir \"" + object.dataRoot + L"\" --open-box \"" + object.boxRoot + L"\"";
    auto verb = [&](const wchar_t* id, const wchar_t* label, const std::wstring& command) {
        const auto key = cls + L"\\shell\\" + id;
        HRESULT result = str(key, L"MUIVerb", label);
        if (SUCCEEDED(result)) result = str(key, L"MultiSelectModel", L"Single");
        if (SUCCEEDED(result)) result = str(key, L"Icon", object.icon);
        if (SUCCEEDED(result)) result = str(key + L"\\command", nullptr, command);
        return result;
    };
    if (FAILED(hr = verb(L"DesktopBox.Expand", L"展开收纳盒", launch)) ||
        FAILED(hr = verb(L"DesktopBox.Add", L"移入文件或文件夹…", launch + L" --shell-action add")) ||
        FAILED(hr = verb(L"DesktopBox.Settings", L"设置此收纳盒…", launch + L" --appearance")) ||
        FAILED(hr = verb(L"DesktopBox.New", L"新建收纳盒…", L"\"" + object.executable + L"\" --data-dir \"" + object.dataRoot + L"\" --new-box")) ||
        FAILED(hr = verb(L"DesktopBox.Refresh", L"刷新收纳内容", launch + L" --shell-action refresh")) ||
        FAILED(hr = verb(L"DesktopBox.Return", L"删除收纳盒并归还内容…", launch + L" --shell-action return"))) return hr;
    const auto directions = cls + L"\\shell\\DesktopBox.Direction";
    if (FAILED(hr = str(directions, L"MUIVerb", L"展开方向")) || FAILED(hr = str(directions, L"SubCommands", L""))) return hr;
    const wchar_t* ids[] = {L"auto", L"left", L"right", L"up", L"down"};
    const wchar_t* labels[] = {L"自动选择方向", L"优先向左展开", L"优先向右展开", L"优先向上展开", L"优先向下展开"};
    for (size_t i = 0; i < 5; ++i) {
        const auto key = directions + L"\\shell\\" + ids[i];
        if (FAILED(hr = str(key, L"MUIVerb", labels[i])) ||
            FAILED(hr = str(key + L"\\command", nullptr, launch + L" --shell-action direction-" + ids[i]))) return hr;
    }
    // Publish last: Explorer must never see an incomplete object registration.
    if (FAILED(hr = str(cls, L"DesktopBoxSignature", Signature(object))) ||
        FAILED(hr = str(junction, L"DesktopBoxOwner", kOwner)) ||
        FAILED(hr = str(junction, nullptr, object.name))) return hr;
    return S_OK;
}
HRESULT RevealDesktopNamespace(HKEY root, const std::wstring& id, const std::wstring& boxRoot, bool& changed) {
    changed = false;
    if (!OwnsDesktopNamespace(root, id, boxRoot)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (const auto* mode : {L"NewStartPanel", L"ClassicStartMenu"}) {
        const auto hr = DeleteValue(root, std::wstring(kHide) + mode, id.c_str(), changed);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}
HRESULT UnregisterDesktopNamespace(HKEY root, const std::wstring& id, const std::wstring& boxRoot,
                                   const std::wstring& executable, bool& changed) {
    changed = false;
    if (!root || !IsDesktopNamespaceId(id) || executable.empty()) return E_INVALIDARG;
    const auto cls = DesktopNamespaceClassKey(id), junction = DesktopNamespaceJunctionKey(id);
    if (!OwnsDesktopNamespace(root, id, boxRoot) ||
        _wcsicmp(Read(root, cls, L"DesktopBoxExecutable").c_str(), executable.c_str()) != 0) return S_FALSE;
    if (Read(root, junction, L"DesktopBoxOwner") == kOwner) {
        const auto hr = DeleteTree(root, junction, changed); if (FAILED(hr)) return hr;
    }
    bool revealed = false;
    auto hr = RevealDesktopNamespace(root, id, boxRoot, revealed); changed = changed || revealed;
    if (FAILED(hr)) return hr;
    hr = DeleteTree(root, NameOverrideKey(id), changed); if (FAILED(hr)) return hr;
    return DeleteTree(root, cls, changed);
}
HRESULT UnregisterDesktopNamespaces(HKEY root, const std::wstring& executable, bool& changed) {
    changed = false;
    if (!root || executable.empty()) return E_INVALIDARG;
    Key classes; const auto status = RegOpenKeyExW(root, kClasses, 0, KEY_READ, &classes.value);
    if (status == ERROR_FILE_NOT_FOUND) return S_OK;
    if (status != ERROR_SUCCESS) return Status(status);
    std::vector<std::wstring> ids;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{}; DWORD count = 256;
        const auto found = RegEnumKeyExW(classes.value, i, name, &count, nullptr, nullptr, nullptr, nullptr);
        if (found == ERROR_NO_MORE_ITEMS) break;
        if (found != ERROR_SUCCESS) return Status(found);
        const auto cls = DesktopNamespaceClassKey(name);
        if (Read(root, cls, L"DesktopBoxOwner") == kOwner &&
            _wcsicmp(Read(root, cls, L"DesktopBoxExecutable").c_str(), executable.c_str()) == 0) ids.emplace_back(name);
    }
    for (const auto& id : ids) {
        bool removed = false;
        const auto hr = UnregisterDesktopNamespace(root, id, Read(root, DesktopNamespaceClassKey(id), L"DesktopBoxRoot"), executable, removed);
        changed = changed || removed; if (FAILED(hr)) return hr;
    }
    return S_OK;
}
}
