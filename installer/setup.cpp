#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kName[] = L"桌面收纳盒";
constexpr wchar_t kExe[] = L"DesktopBox.exe";
constexpr wchar_t kSetup[] = L"DesktopBox-Setup.exe";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DesktopBox.Native";
constexpr int kPathEdit = 101, kBrowse = 102, kInstall = 103, kLaunch = 104;

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
    if (source.empty() || PathIsRelativeW(source.c_str())) Fail(L"请选择完整的安装路径");
    DWORD length = GetFullPathNameW(source.c_str(), 0, nullptr, nullptr);
    if (!length || length > 32768) Fail(L"安装路径无效", GetLastError());
    std::wstring path(length, L'\0');
    DWORD written = GetFullPathNameW(source.c_str(), length, path.data(), nullptr);
    if (!written || written >= length) Fail(L"安装路径无效", GetLastError());
    path.resize(written);
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
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
void MakePortable(const std::wstring& folder) {
    const auto flag = folder + L"\\portable.flag";
    Handle file(CreateFileW(flag.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) Fail(L"无法创建便携模式标记", GetLastError());
    EnsureDirectory(folder + L"\\data");
    EnsureDirectory(folder + L"\\ico");
    for (const auto* directory : {L"data", L"ico"}) {
        const auto probe = folder + L"\\" + directory + L"\\.write-check-" + std::to_wstring(GetCurrentProcessId());
        Handle check(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if (check.value == INVALID_HANDLE_VALUE)
            Fail(L"所选位置不能保存收纳文件和图标，请选择可写入的文件夹", GetLastError());
    }
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
void RegisterUninstaller(const std::wstring& folder) {
    Key key;
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                                     KEY_READ | KEY_WRITE, nullptr, &key.value, nullptr);
    if (result != ERROR_SUCCESS) Fail(L"无法注册卸载程序", result);
    WriteRegistry(key.value, L"DisplayName", kName);
    WriteRegistry(key.value, L"DisplayVersion", L"0.4.6");
    WriteRegistry(key.value, L"Publisher", L"DesktopBox");
    WriteRegistry(key.value, L"InstallLocation", folder);
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
void Install(const std::wstring& chosenParent, bool launch) {
    const std::wstring folder = InstallFolder(chosenParent);
    const auto previous = ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
    if (!previous.empty() && !SamePath(previous, folder))
        Fail(L"已安装在另一位置。请先从 Windows 设置中卸载旧程序，再选择新位置安装；收纳文件会保留");
    if (SamePath(ModulePath(), folder + L"\\" + kSetup))
        Fail(L"请从安装目录外运行安装包");
    EnsureDirectory(folder);
    MakePortable(folder);
    StopRunningApp();
    Extract(folder + L"\\" + kExe);
    Require(CopyFileW(ModulePath().c_str(), (folder + L"\\" + kSetup).c_str(), FALSE) != FALSE,
            L"无法复制卸载程序");
    MakeShortcut(KnownFolder(FOLDERID_Programs) + L"\\" + kName + L".lnk", folder + L"\\" + kExe);
    RegisterUninstaller(folder);
    if (launch) RunProgram(folder + L"\\" + kExe);
}
std::wstring LinkTarget(const std::wstring& path, std::wstring* arguments = nullptr) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link)))) return {};
    IPersistFile* file = nullptr; wchar_t target[32768]{}, parameters[32768]{};
    const bool valid = SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) &&
                       SUCCEEDED(file->Load(path.c_str(), STGM_READ)) &&
                       SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)) &&
                       (!arguments || SUCCEEDED(link->GetArguments(parameters, 32768)));
    if (file) file->Release(); link->Release();
    if (valid && arguments) *arguments = parameters;
    return valid ? std::wstring(target) : std::wstring();
}
void RemoveOwnedLinks(const std::wstring& folder) {
    const std::wstring target = folder + L"\\" + kExe;
    const auto start = KnownFolder(FOLDERID_Programs) + L"\\" + kName + L".lnk";
    if (SamePath(LinkTarget(start), target)) DeleteFileW(start.c_str());
    const auto desktop = KnownFolder(FOLDERID_Desktop);
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((desktop + L"\\*.lnk").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const auto path = desktop + L"\\" + found.cFileName;
        std::wstring arguments;
        if (SamePath(LinkTarget(path, &arguments), target) &&
            arguments.find(L"--open-box") != std::wstring::npos &&
            arguments.find(L"--data-dir") != std::wstring::npos) DeleteFileW(path.c_str());
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
    RunAndWait(program, L"--disable-startup --unregister-shell");
    RemoveOwnedLinks(folder);
    const LSTATUS removed = RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) Fail(L"无法移除卸载记录", removed);
    DeleteFileW(program.c_str());
    DeleteFileW((folder + L"\\portable.flag").c_str());
    DeleteFileW((folder + L"\\" + kSetup).c_str());
    RemoveDirectoryW((folder + L"\\ico").c_str());
    RemoveDirectoryW((folder + L"\\data").c_str());
    RemoveDirectoryW(folder.c_str());
    MoveFileExW(ModulePath().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    MessageBoxW(nullptr, (L"程序已卸载。收纳文件和图标仍保留在：\n" + folder).c_str(),
                kName, MB_OK | MB_ICONINFORMATION);
}
void BeginUninstall() {
    const auto folder = ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
    if (folder.empty()) Fail(L"没有找到本程序的安装记录");
    if (MessageBoxW(nullptr, L"卸载程序？收纳文件和自定义图标会保留在安装目录。",
                    kName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    wchar_t temp[MAX_PATH]{}; const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    if (!tempLength || tempLength >= MAX_PATH) Fail(L"无法访问临时目录", GetLastError());
    wchar_t reserved[MAX_PATH]{}; Require(GetTempFileNameW(temp, L"DBX", 0, reserved) != 0, L"无法创建卸载副本");
    DeleteFileW(reserved);
    const std::wstring copy = std::wstring(reserved) + L".exe";
    if (!CopyFileW(ModulePath().c_str(), copy.c_str(), TRUE)) Fail(L"无法启动卸载程序", GetLastError());
    RunProgram(copy, L"--uninstall-core \"" + folder + L"\" " + std::to_wstring(GetCurrentProcessId()));
}
std::wstring Browse(HWND owner) {
    IFileOpenDialog* picker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, reinterpret_cast<void**>(&picker)))) return {};
    DWORD options = 0; picker->GetOptions(&options);
    picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    picker->SetTitle(L"选择安装位置（会创建 DesktopBox 文件夹）");
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
    HWND hwnd = nullptr, edit = nullptr, check = nullptr;
    HFONT font = nullptr;
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); self->hwnd = window;
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        if (message == WM_COMMAND) {
            if (LOWORD(w) == kBrowse) {
                const auto path = Browse(window); if (!path.empty()) SetWindowTextW(self->edit, path.c_str());
            } else if (LOWORD(w) == kInstall) {
                const int count = GetWindowTextLengthW(self->edit);
                std::wstring path(count + 1, L'\0'); GetWindowTextW(self->edit, path.data(), count + 1); path.resize(count);
                EnableWindow(GetDlgItem(window, kInstall), FALSE);
                SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                try {
                    Install(path, SendMessageW(self->check, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    MessageBoxW(window, L"安装完成。程序、data 和 ico 文件夹位于所选位置。", kName, MB_OK | MB_ICONINFORMATION);
                    DestroyWindow(window);
                } catch (const std::wstring& error) {
                    MessageBoxW(window, error.c_str(), L"安装未完成", MB_OK | MB_ICONERROR);
                    EnableWindow(GetDlgItem(window, kInstall), TRUE);
                }
            }
            return 0;
        }
        if (message == WM_DESTROY) { if (self->font) DeleteObject(self->font); PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    void Show() {
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance; cls.lpfnWndProc = Proc;
        cls.lpszClassName = L"DesktopBox.Setup.01"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        Require(RegisterClassExW(&cls) != 0, L"无法创建安装窗口");
        const int dpi = GetDpiForSystem();
        auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        hwnd = CreateWindowExW(0, cls.lpszClassName, L"安装桌面收纳盒", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, px(560), px(245), nullptr, nullptr, instance, this);
        Require(hwnd != nullptr, L"无法打开安装窗口");
        font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto control = [&](const wchar_t* type, const wchar_t* label, DWORD style, int x, int y, int width, int height, int id = 0) {
            HWND child = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
                                         px(x), px(y), px(width), px(height), hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return child;
        };
        control(L"STATIC", L"安装桌面收纳盒", 0, 22, 18, 500, 26);
        control(L"STATIC", L"选择安装位置；将自动创建 DesktopBox 文件夹，收纳文件保存在其中的 data 文件夹。",
                0, 22, 52, 515, 34);
        const auto current = ReadRegistry(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation");
        const auto parent = current.empty() ? KnownFolder(FOLDERID_LocalAppData) + L"\\Programs" : Parent(current);
        edit = control(L"EDIT", parent.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 22, 94, 425, 27, kPathEdit);
        control(L"BUTTON", L"浏览…", WS_TABSTOP, 454, 94, 82, 27, kBrowse);
        check = control(L"BUTTON", L"安装完成后启动", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 136, 220, 26, kLaunch);
        SendMessageW(check, BM_SETCHECK, BST_CHECKED, 0);
        control(L"BUTTON", L"安装", BS_DEFPUSHBUTTON | WS_TABSTOP, 432, 157, 104, 32, kInstall);
        ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    int result = 0;
    try {
        int count = 0; wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) Fail(L"无法读取命令参数");
        if (count >= 2 && wcscmp(arguments[1], L"--extract-test") == 0 && count == 3) {
            const auto folder = InstallFolder(arguments[2]); EnsureDirectory(folder);
            Extract(folder + L"\\" + kExe); MakePortable(folder);
        } else if (count == 2 && wcscmp(arguments[1], L"--uninstall") == 0) BeginUninstall();
        else if (count == 4 && wcscmp(arguments[1], L"--uninstall-core") == 0)
            UninstallCore(FullPath(arguments[2]), wcstoul(arguments[3], nullptr, 10));
        else {
            Dialog dialog; dialog.instance = instance; dialog.Show();
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        LocalFree(arguments);
    } catch (const std::wstring& error) {
        MessageBoxW(nullptr, error.c_str(), L"桌面收纳盒安装程序", MB_OK | MB_ICONERROR); result = 1;
    } catch (...) {
        MessageBoxW(nullptr, L"操作未能完成。", L"桌面收纳盒安装程序", MB_OK | MB_ICONERROR); result = 1;
    }
    OleUninitialize(); return result;
}
