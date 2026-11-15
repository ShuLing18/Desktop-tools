#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "../src/data_location.h"
#include "fluent_setup.h"

namespace {
constexpr wchar_t kName[] = L"桌面收纳盒";
constexpr wchar_t kExe[] = L"DesktopBox.exe";
constexpr wchar_t kSetup[] = L"DesktopBox-Setup.exe";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DesktopBox.Native";
constexpr int kPathEdit = 101, kBrowse = 102, kInstall = 103, kLaunch = 104;
constexpr int kDataEdit = 105, kBrowseData = 106;

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
[[noreturn]] void Fail(const std::wstring& message, DWORD error = 0) {
    if (error) throw message + L"（错误 " + std::to_wstring(error) + L"）";
    throw message;
}
void Require(bool ok, const wchar_t* message) { if (!ok) Fail(message, GetLastError()); }
std::wstring ModulePath() {
    for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2) {
        std::wstring path(capacity, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, path.data(), capacity);
        if (!length) Fail(L"无法确定安装包位置", GetLastError());
        if (length < capacity) { path.resize(length); return path; }
    }
    Fail(L"安装包路径过长");
}
std::wstring Parent(const std::wstring& path) {
    auto position = path.find_last_of(L"\\/");
    if (position == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    return position == std::wstring::npos ? L"" : path.substr(0, position);
}
std::wstring Base(const std::wstring& path) {
    auto position = path.find_last_of(L"\\/");
    return position == std::wstring::npos ? path : path.substr(position + 1);
}
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    wchar_t* value = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &value);
    if (FAILED(result)) Fail(L"无法访问 Windows 用户文件夹", static_cast<DWORD>(result));
    std::wstring path(value); CoTaskMemFree(value); return path;
}
std::wstring FullPath(const std::wstring& source) {
    try { return box::AbsoluteDataDirectory(source); }
    catch (HRESULT) { Fail(L"路径无效，请选择完整的文件夹路径，例如 D:\\DesktopBox"); }
}
std::wstring InstallFolder(const std::wstring& parent) {
    std::wstring path = FullPath(parent);
    return _wcsicmp(Base(path).c_str(), L"DesktopBox") == 0 ? path : path + L"\\DesktopBox";
}
void EnsureDirectory(const std::wstring& path) {
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        Fail(L"无法创建安装文件夹", static_cast<DWORD>(result));
    const DWORD flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY)) Fail(L"安装位置不是文件夹");
}
std::wstring ReadRegistry(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
    return buffer.data();
}
void WriteRegistry(HKEY key, const wchar_t* name, const std::wstring& value) {
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    if (result != ERROR_SUCCESS) Fail(L"无法记录卸载信息", result);
}
bool SamePath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}
bool Within(const std::wstring& child, const std::wstring& parent) {
    return SamePath(child, parent) || (child.size() > parent.size() &&
        _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) == 0 &&
        (parent.back() == L'\\' || child[parent.size()] == L'\\'));
}
std::wstring SavedDataDirectory(const std::wstring& folder) {
    try {
        const auto configured = box::InstalledDataDirectory(folder);
        return configured.empty() ? folder + L"\\data" : configured;
    } catch (HRESULT) { Fail(L"无法读取原保存位置，请检查安装目录中的 storage.ini"); }
}
std::wstring ValidateDataDirectory(const std::wstring& folder, const std::wstring& chosen) {
    const auto directory = FullPath(chosen);
    if (Within(folder, directory) || Within(directory, folder + L"\\ico"))
        Fail(L"请为收纳文件选择单独的文件夹，例如 D:\\桌面收纳文件");
    const auto previous = SavedDataDirectory(folder);
    if (!SamePath(previous, directory) &&
        (GetFileAttributesW((previous + L"\\settings.ini").c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW((previous + L"\\items").c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW((previous + L"\\boxes").c_str()) != INVALID_FILE_ATTRIBUTES))
        Fail(L"此安装已有收纳数据，请保留原保存位置完成更新：\n" + previous +
             L"\n\n安装程序不会自动迁移已有文件。新安装可选择其他保存位置。");
    return directory;
}
void StopRunningApp() {
    HWND window = FindWindowW(L"DesktopBox.Controller.02", kName);
    if (!window) return;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    Handle process(pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
    Require(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, L"无法关闭正在运行的收纳盒");
    if (process.value && WaitForSingleObject(process.value, 10000) != WAIT_OBJECT_0)
        Fail(L"收纳盒仍在运行。请先退出后重试");
}
const void* Payload(DWORD& size) {
    HRSRC resource = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(201), RT_RCDATA);
    if (!resource) Fail(L"安装包缺少程序文件");
    HGLOBAL loaded = LoadResource(GetModuleHandleW(nullptr), resource);
    size = SizeofResource(GetModuleHandleW(nullptr), resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || size < 1024 || static_cast<const BYTE*>(bytes)[0] != 'M' || static_cast<const BYTE*>(bytes)[1] != 'Z')
        Fail(L"安装包内的程序文件无效");
    return bytes;
}
void Extract(const std::wstring& destination) {
    DWORD size = 0; const void* bytes = Payload(size);
    const std::wstring temporary = destination + L".new";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) Fail(L"无法写入程序文件", GetLastError());
    DWORD written = 0;
    if (!WriteFile(file.value, bytes, size, &written, nullptr) || written != size || !FlushFileBuffers(file.value)) {
        const DWORD error = GetLastError(); CloseHandle(file.value); file.value = nullptr;
        DeleteFileW(temporary.c_str()); Fail(L"写入程序文件失败", error);
    }
    CloseHandle(file.value); file.value = nullptr;
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError(); DeleteFileW(temporary.c_str()); Fail(L"无法替换程序文件", error);
    }
}
void MakePortable(const std::wstring& folder, const std::wstring& dataDirectory) {
    EnsureDirectory(dataDirectory);
    EnsureDirectory(folder + L"\\ico");
    for (const auto& directory : {dataDirectory, folder + L"\\ico"}) {
        const auto probe = directory + L"\\.write-check-" + std::to_wstring(GetCurrentProcessId());
        Handle check(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (check.value == INVALID_HANDLE_VALUE)
            Fail(L"所选位置不能保存收纳文件和图标，请选择可写入的文件夹", GetLastError());
    }
    const auto flag = folder + L"\\portable.flag";
    Handle file(CreateFileW(flag.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) Fail(L"无法创建便携模式标记", GetLastError());
    try { box::SaveDataDirectory(folder, dataDirectory); }
    catch (HRESULT error) { Fail(L"无法保存收纳文件的位置", static_cast<DWORD>(error)); }
}
void MakeShortcut(const std::wstring& link, const std::wstring& target) {
    IShellLinkW* shortcut = nullptr;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, reinterpret_cast<void**>(&shortcut));
    if (FAILED(result)) Fail(L"无法创建开始菜单快捷方式", result);
    result = shortcut->SetPath(target.c_str());
    if (SUCCEEDED(result)) result = shortcut->SetWorkingDirectory(Parent(target).c_str());
    IPersistFile* persist = nullptr;
    if (SUCCEEDED(result)) result = shortcut->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist));
    if (SUCCEEDED(result)) result = persist->Save(link.c_str(), TRUE);
    if (persist) persist->Release(); shortcut->Release();
    if (FAILED(result)) Fail(L"无法创建开始菜单快捷方式", result);
}
void RegisterUninstaller(const std::wstring& folder, const std::wstring& dataDirectory) {
    Key key;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                                     KEY_READ | KEY_WRITE, nullptr, &key.value, nullptr);
    if (result != ERROR_SUCCESS) Fail(L"无法注册卸载程序", result);
    WriteRegistry(key.value, L"DisplayName", kName);
    WriteRegistry(key.value, L"DisplayVersion", L"0.5.0");
    WriteRegistry(key.value, L"Publisher", L"DesktopBox");
    WriteRegistry(key.value, L"InstallLocation", folder);
    WriteRegistry(key.value, L"DataLocation", dataDirectory);
    WriteRegistry(key.value, L"DisplayIcon", folder + L"\\" + kExe);
    WriteRegistry(key.value, L"UninstallString", L"\"" + folder + L"\\" + kSetup + L"\" --uninstall");
    const DWORD noModify = 1;
    RegSetValueExW(key.value, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&noModify), sizeof(noModify));
}
void RunProgram(const std::wstring& path, const std::wstring& arguments = {}) {
    std::wstring command = L"\"" + path + L"\"" + (arguments.empty() ? L"" : L" " + arguments);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           Parent(path).c_str(), &startup, &process) != FALSE, L"无法启动程序");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
std::wstring Install(const std::wstring& chosenParent, const std::wstring& chosenData, bool launch) {
    const std::wstring folder = InstallFolder(chosenParent);
    const auto previous = ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
    if (!previous.empty() && !SamePath(previous, folder))
        Fail(L"已安装在另一位置。请先从 Windows 设置中卸载旧程序，再选择新位置安装；收纳文件会保留");
    if (SamePath(ModulePath(), folder + L"\\" + kSetup))
        Fail(L"请从安装目录外运行安装包");
    const auto dataDirectory = ValidateDataDirectory(folder, chosenData);
    EnsureDirectory(folder);
    StopRunningApp();
    MakePortable(folder, dataDirectory);
    Extract(folder + L"\\" + kExe);
    Require(CopyFileW(ModulePath().c_str(), (folder + L"\\" + kSetup).c_str(), FALSE) != FALSE,
            L"无法复制卸载程序");
    MakeShortcut(KnownFolder(FOLDERID_Programs) + L"\\" + kName + L".lnk", folder + L"\\" + kExe);
    RegisterUninstaller(folder, dataDirectory);
    if (launch) RunProgram(folder + L"\\" + kExe);
    return dataDirectory;
}
std::wstring LinkTarget(const std::wstring& path, std::wstring* arguments = nullptr, std::wstring* workingDirectory = nullptr) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return {};
    IPersistFile* file = nullptr; wchar_t target[32768]{}, parameters[32768]{}, working[32768]{};
    const bool valid = SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) &&
                       SUCCEEDED(file->Load(path.c_str(), STGM_READ)) &&
                       SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)) &&
                       (!arguments || SUCCEEDED(link->GetArguments(parameters, 32768))) &&
                       (!workingDirectory || SUCCEEDED(link->GetWorkingDirectory(working, 32768)));
    if (file) file->Release(); link->Release();
    if (valid && arguments) *arguments = parameters;
    if (valid && workingDirectory) *workingDirectory = working;
    return valid ? std::wstring(target) : std::wstring();
}
void RemoveOwnedLinks(const std::wstring& folder, const std::wstring& dataRoot) {
    const std::wstring target = folder + L"\\" + kExe;
    const auto start = KnownFolder(FOLDERID_Programs) + L"\\" + kName + L".lnk";
    if (SamePath(LinkTarget(start), target)) DeleteFileW(start.c_str());
    const auto desktop = KnownFolder(FOLDERID_Desktop);
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((desktop + L"\\*.lnk").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const auto path = desktop + L"\\" + found.cFileName;
        std::wstring arguments, working;
        const auto linked = LinkTarget(path, &arguments, &working);
        const bool withinData = Within(working, dataRoot);
        bool ownedFolder = false;
        if (withinData && SamePath(linked, working + L"\\items")) {
            wchar_t owned[32768]{};
            GetPrivateProfileStringW(L"DesktopBox", L"DesktopLink", L"", owned, 32768, (working + L"\\settings.ini").c_str());
            ownedFolder = SamePath(owned, path);
        }
        if (ownedFolder || (SamePath(linked, target) &&
            arguments.find(L"--open-box") != std::wstring::npos &&
            arguments.find(L"--data-dir") != std::wstring::npos)) DeleteFileW(path.c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
}
void RunAndWait(const std::wstring& path, const wchar_t* arguments) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring command = L"\"" + path + L"\" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           Parent(path).c_str(), &startup, &process) != FALSE,
            L"无法执行程序的卸载清理操作");
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) {
        CloseHandle(process.hProcess); Fail(L"无法结束后台注册操作");
    }
    DWORD exitCode = 1; GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (exitCode != 0) Fail(L"无法移除本程序的启动项或系统菜单，请检查后重试");
}
void UninstallCore(const std::wstring& folder, DWORD parentPid) {
    Handle parent(parentPid ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr);
    if (parent.value) WaitForSingleObject(parent.value, 10000);
    if (!SamePath(ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation"), folder))
        Fail(L"无法确认此安装位置，卸载已停止");
    StopRunningApp();
    const auto program = folder + L"\\" + kExe;
    const auto dataDirectory = SavedDataDirectory(folder);
    RunAndWait(program, L"--disable-startup --unregister-shell");
    RemoveOwnedLinks(folder, dataDirectory);
    const LSTATUS removed = RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) Fail(L"无法移除卸载记录", removed);
    DeleteFileW(program.c_str());
    DeleteFileW((folder + L"\\portable.flag").c_str());
    DeleteFileW((folder + L"\\" + kSetup).c_str());
    RemoveDirectoryW((folder + L"\\ico").c_str());
    RemoveDirectoryW((folder + L"\\data").c_str());
    RemoveDirectoryW(folder.c_str());
    MoveFileExW(ModulePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    MessageBoxW(nullptr, (L"程序已卸载，个人文件已保留。\n\n收纳文件：" + dataDirectory +
                         L"\n图标库：" + folder + L"\\ico").c_str(),
                kName, MB_OK | MB_ICONINFORMATION);
}
void BeginUninstall() {
    const auto folder = ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
    if (folder.empty()) Fail(L"没有找到本程序的安装记录");
    const auto dataDirectory = SavedDataDirectory(folder);
    if (MessageBoxW(nullptr, (L"卸载程序？个人文件会保留。\n\n收纳文件：" + dataDirectory +
                             L"\n图标库：" + folder + L"\\ico").c_str(),
                    kName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    wchar_t temp[MAX_PATH]{}; const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    if (!tempLength || tempLength >= MAX_PATH) Fail(L"无法访问临时目录", GetLastError());
    wchar_t reserved[MAX_PATH]{}; Require(GetTempFileNameW(temp, L"DBX", 0, reserved) != 0, L"无法创建卸载副本");
    DeleteFileW(reserved);
    const std::wstring copy = std::wstring(reserved) + L".exe";
    if (!CopyFileW(ModulePath().c_str(), copy.c_str(), TRUE)) Fail(L"无法启动卸载程序", GetLastError());
    RunProgram(copy, L"--uninstall-core \"" + folder + L"\" " + std::to_wstring(GetCurrentProcessId()));
}
std::wstring Browse(HWND owner, const wchar_t* title, const std::wstring& initial) {
    IFileOpenDialog* picker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, reinterpret_cast<void**>(&picker)))) return {};
    DWORD options = 0; picker->GetOptions(&options);
    picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    picker->SetTitle(title);
    IShellItem* initialFolder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&initialFolder)))) {
        picker->SetFolder(initialFolder); initialFolder->Release();
    }
    std::wstring selected;
    if (SUCCEEDED(picker->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(picker->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = path; CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    picker->Release(); return selected;
}
struct Dialog {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr, edit = nullptr, dataEdit = nullptr, check = nullptr;
    setupui::FluentSetup visual;
    bool followInstall = true, updatingData = false, ready = false;
    static std::wstring Text(HWND control) {
        const int count = GetWindowTextLengthW(control);
        std::wstring result(count + 1, L'\0');
        GetWindowTextW(control, result.data(), count + 1); result.resize(count); return result;
    }
    void UpdateDataPath(const std::wstring& path) {
        updatingData = true; SetWindowTextW(dataEdit, path.c_str()); updatingData = false;
    }
    void Layout(UINT dpi, const RECT* suggested = nullptr) {
        visual.SetDpi(dpi);
        struct Placement { int id, x, y, width, height; };
        for (const auto& item : {
            Placement{setupui::kTitle, 100, 23, 438, 36}, {setupui::kSubtitle, 100, 65, 484, 20},
            {setupui::kVersion, 546, 34, 66, 18}, {setupui::kProgramLabel, 44, 126, 548, 22},
            {kPathEdit, 56, 167, 408, 20}, {kBrowse, 488, 158, 108, 38},
            {setupui::kProgramHint, 44, 210, 548, 22}, {setupui::kDataLabel, 44, 278, 548, 22},
            {kDataEdit, 56, 319, 408, 20}, {kBrowseData, 488, 310, 108, 38},
            {setupui::kDataHint, 44, 362, 548, 22}, {kLaunch, 28, 450, 250, 24},
            {IDCANCEL, 382, 446, 92, 40}, {kInstall, 486, 446, 126, 40}}) {
            const RECT rect = visual.Rect(item.x, item.y, item.width, item.height);
            SetWindowPos(GetDlgItem(hwnd, item.id), nullptr, rect.left, rect.top, rect.right - rect.left,
                rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT bounds = visual.Rect(0, 0, 640, 510);
        AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, 0, dpi);
        RECT current{}; GetWindowRect(hwnd, &current);
        const int x = suggested ? suggested->left : current.left, y = suggested ? suggested->top : current.top;
        SetWindowPos(hwnd, nullptr, x, y, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->hwnd = window;
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        LRESULT visualResult = 0;
        if (self->ready && self->visual.Message(message, w, l, visualResult)) return visualResult;
        if (message == WM_DPICHANGED && self->ready) {
            self->Layout(HIWORD(w), reinterpret_cast<const RECT*>(l)); return 0;
        }
        if (message == DM_GETDEFID) return MAKELONG(kInstall, DC_HASDEFID);
        if (message == WM_COMMAND) {
            if (LOWORD(w) == kBrowse) {
                const auto path = Browse(window, L"选择安装位置（会创建 DesktopBox 文件夹）", Text(self->edit));
                if (!path.empty()) SetWindowTextW(self->edit, path.c_str());
            } else if (LOWORD(w) == kBrowseData) {
                const auto path = Browse(window, L"选择收纳文件保存位置", Text(self->dataEdit));
                if (!path.empty()) { self->followInstall = false; self->UpdateDataPath(path); }
            } else if (LOWORD(w) == kPathEdit && HIWORD(w) == EN_CHANGE && self->followInstall && self->dataEdit) {
                try { self->UpdateDataPath(SavedDataDirectory(InstallFolder(Text(self->edit)))); }
                catch (const std::wstring&) {} // The path can be incomplete while typing.
            } else if (LOWORD(w) == kDataEdit && HIWORD(w) == EN_CHANGE && self->ready && !self->updatingData) {
                self->followInstall = false;
            } else if (LOWORD(w) == kInstall) {
                EnableWindow(GetDlgItem(window, kInstall), FALSE);
                SetDlgItemTextW(window, kInstall, L"正在安装…"); UpdateWindow(GetDlgItem(window, kInstall));
                SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                try {
                    const auto data = Install(Text(self->edit), Text(self->dataEdit),
                                              SendMessageW(self->check, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    MessageBoxW(window, (L"安装完成。\n\n程序位置：" + InstallFolder(Text(self->edit)) +
                                         L"\n收纳文件保存位置：" + data).c_str(), kName, MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(window);
                } catch (const std::wstring& error) {
                    MessageBoxW(window, error.c_str(), L"安装未完成", MB_OK | MB_ICONERROR);
                    EnableWindow(GetDlgItem(window, kInstall), TRUE);
                    SetDlgItemTextW(window, kInstall, L"开始安装");
                }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (LOWORD(w) == IDCANCEL) {
                DestroyWindow(window);
            }
            return 0;
        }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    void Show() {
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance; cls.lpfnWndProc = Proc;
        cls.lpszClassName = L"DesktopBox.Setup.01"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        cls.hIconSm = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        cls.hbrBackground = nullptr;
        Require(RegisterClassExW(&cls) != 0, L"无法创建安装窗口");
        const int dpi = GetDpiForSystem();
        auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        RECT bounds{0, 0, px(640), px(510)};
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, dpi);
        hwnd = CreateWindowExW(0, cls.lpszClassName, L"安装桌面收纳盒", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                               nullptr, nullptr, instance, this);
        Require(hwnd != nullptr, L"无法打开安装窗口");
        auto control = [&](const wchar_t* type, const wchar_t* label, DWORD controlStyle, int id) {
            HWND child = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | controlStyle,
                                         0, 0, 1, 1, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            return child;
        };
        control(L"STATIC", L"安装桌面收纳盒", 0, setupui::kTitle);
        control(L"STATIC", L"让桌面井然有序，从选择保存位置开始。", 0, setupui::kSubtitle);
        control(L"STATIC", L"0.5.0", SS_RIGHT, setupui::kVersion);
        const auto current = ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
        const auto parent = current.empty() ? KnownFolder(FOLDERID_LocalAppData) + L"\\Programs" : Parent(current);
        followInstall = current.empty();
        control(L"STATIC", L"程序安装位置", 0, setupui::kProgramLabel);
        edit = control(L"EDIT", parent.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, kPathEdit);
        SendMessageW(edit, EM_SETLIMITTEXT, 32760, 0);
        control(L"BUTTON", L"浏览…", WS_TABSTOP, kBrowse);
        control(L"STATIC", L"自动创建 DesktopBox 文件夹，用于存放程序和图标库。", 0, setupui::kProgramHint);
        control(L"STATIC", L"收纳文件保存位置", 0, setupui::kDataLabel);
        const auto data = SavedDataDirectory(current.empty() ? InstallFolder(parent) : current);
        dataEdit = control(L"EDIT", data.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, kDataEdit);
        SendMessageW(dataEdit, EM_SETLIMITTEXT, 32760, 0);
        control(L"BUTTON", L"浏览…", WS_TABSTOP, kBrowseData);
        control(L"STATIC", L"拖入的文件与文件夹保存在这里，可选择其他磁盘。", 0, setupui::kDataHint);
        check = control(L"BUTTON", L"安装完成后启动", BS_AUTOCHECKBOX | WS_TABSTOP, kLaunch);
        SendMessageW(check, BM_SETCHECK, BST_CHECKED, 0);
        control(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
        control(L"BUTTON", L"开始安装", BS_DEFPUSHBUTTON | WS_TABSTOP, kInstall);
        visual.Initialize(hwnd, kPathEdit, kDataEdit, kBrowse, kBrowseData, kInstall, kLaunch, IDCANCEL);
        ready = true; Layout(GetDpiForWindow(hwnd));
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT window{}; GetWindowRect(hwnd, &window);
        SetWindowPos(hwnd, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - window.right + window.left) / 2,
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - window.bottom + window.top) / 2,
            0, 0, SWP_NOSIZE | SWP_NOZORDER);
        SetFocus(edit);
        ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    int result = 0;
    try {
        int count = 0; wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) Fail(L"无法读取命令参数");
        if (count >= 2 && wcscmp(arguments[1], L"--extract-test") == 0 && (count == 3 || count == 4)) {
            const auto folder = InstallFolder(arguments[2]); EnsureDirectory(folder);
            const auto data = ValidateDataDirectory(folder, count == 4 ? arguments[3] : SavedDataDirectory(folder));
            Extract(folder + L"\\" + kExe); MakePortable(folder, data);
        } else if (count == 2 && wcscmp(arguments[1], L"--uninstall") == 0) BeginUninstall();
        else if (count == 4 && wcscmp(arguments[1], L"--uninstall-core") == 0)
            UninstallCore(FullPath(arguments[2]), wcstoul(arguments[3], nullptr, 10));
        else {
            Dialog dialog; dialog.instance = instance; dialog.Show();
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(dialog.hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
        LocalFree(arguments);
    } catch (const std::wstring& error) {
        MessageBoxW(nullptr, error.c_str(), L"桌面收纳盒安装程序", MB_OK | MB_ICONERROR); result = 1;
    } catch (...) {
        MessageBoxW(nullptr, L"操作未能完成。", L"桌面收纳盒安装程序", MB_OK | MB_ICONERROR); result = 1;
    }
    OleUninitialize(); return result;
}
