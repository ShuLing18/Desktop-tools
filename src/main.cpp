#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <functional>
#include "storage.h"
#include <initguid.h>
#include <taskschd.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>
#include "model.h"
#include "selection.h"
#include "desktop.h"
#include "desktop_mouse.h"
#include "native_entry.h"
#include "backdrop.h"
#include "backdrop_loader.h"
#include "render_cache.h"
#include "shell_menu.h"
#include "shell_registration.h"
#include "fluent_settings.h"
#ifdef DESKTOPBOX_TESTS
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace {
constexpr wchar_t kTitle[] = L"桌面收纳盒";
constexpr wchar_t kHostClass[] = L"DesktopBox.Host.01";
constexpr wchar_t kPanelClass[] = L"DesktopBox.Panel.01";
constexpr wchar_t kControllerClass[] = L"DesktopBox.Controller.02";
constexpr UINT kRemoveBoxMsg = WM_APP + 8, kFilesChanged = WM_APP + 9;
constexpr UINT kTray = WM_APP + 1, kIconReady = WM_APP + 2, kWake = WM_APP + 3, kDesktopVisible = WM_APP + 4;
constexpr UINT kRenderAnchor = WM_APP + 5;
constexpr UINT kNativeMouse = WM_APP + 6;
constexpr UINT kNativeChanged = WM_APP + 10;
constexpr UINT kNativeContext = WM_APP + 11, kShellIconChanged = WM_APP + 12, kCreateShellBox = WM_APP + 13;
constexpr UINT kBackdropReady = WM_APP + 14;
constexpr UINT kCompleteShellSetup = WM_APP + 15;
constexpr UINT kCompleteBackdropHide = WM_APP + 16;
constexpr UINT kWarmIcons = WM_APP + 17;
bool backdropNeedsFlush = false; // Accessed only by this UI thread.
void FlushBackdropHide() {
    if (backdropNeedsFlush) { DwmFlush(); backdropNeedsFlush = false; }
}
void NoteBackdropHide(HWND receiver) {
    backdropNeedsFlush = true;
    if (receiver) PostMessageW(receiver, kCompleteBackdropHide, 0, 0);
}
constexpr UINT_PTR kCloseTimer = 1, kDesktopTimer = 2, kRefreshTimer = 3, kNativeRefreshTimer = 4, kSelectionScrollTimer = 5;
constexpr UINT_PTR kLayoutTimer = 6, kPointerSettleTimer = 7;
constexpr UINT_PTR kBackdropCacheTimer = 8;
constexpr UINT_PTR kTextCacheTimer = 9;
constexpr UINT kPanelCloseDelay = 120;
constexpr int kSearchControl = 1201;
enum : UINT { kAddItems = 100, kOpenData,
    kDirectionAuto, kDirectionLeft, kDirectionRight, kDirectionUp, kDirectionDown, kStartup, kRefresh,
    kExit, kRemove, kOpen, kReveal,
    kNewBox, kRenameBox, kDeleteBox, kAppearance, kSortManual, kSortModified, kSortName };
struct FlagGuard {
    bool& flag;
    bool previous;
    bool active = true;
    explicit FlagGuard(bool& target) : flag(target), previous(target) { flag = true; }
    void Restore() { if (active) { flag = previous; active = false; } }
    ~FlagGuard() { Restore(); }
};

class RecentBackdrop {
    box::Backdrop value_;
    HWND owner_ = nullptr;
    ULONGLONG until_ = 0;
public:
    void Clear() {
        if (owner_) KillTimer(owner_, kBackdropCacheTimer);
        owner_ = nullptr; until_ = 0; value_.Clear();
    }
    bool Retain(box::Backdrop value, HWND owner) {
        Clear();
        if (!owner || value.MemoryBytes() > 4 * 1024 * 1024 || !value.MemoryBytes()) return false;
        value_ = std::move(value); owner_ = owner; until_ = GetTickCount64() + 500;
        if (!SetTimer(owner_, kBackdropCacheTimer, 500, nullptr)) { Clear(); return false; }
        return true;
    }
    bool Take(RECT screen, box::Backdrop& output) {
        if (!until_ || GetTickCount64() >= until_ || !value_.Matches(screen)) { Clear(); return false; }
        output = std::move(value_); Clear(); return true;
    }
    void Expire(HWND receiver, ULONGLONG now = GetTickCount64()) {
        KillTimer(receiver, kBackdropCacheTimer);
        if (receiver != owner_ || !until_) return;
        if (now < until_) { if (!SetTimer(receiver, kBackdropCacheTimer, static_cast<UINT>(until_ - now), nullptr)) Clear(); }
        else Clear();
    }
    void ForgetOwner(HWND owner) { if (owner_ == owner) Clear(); }
    size_t Bytes() const { return value_.MemoryBytes(); }
    ULONGLONG Until() const { return until_; }
};
RecentBackdrop recentBackdrop;

template<class T> class ComPtr {
    T* value_ = nullptr;
public:
    ~ComPtr() { if (value_) value_->Release(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
};
struct BStr {
    BSTR value;
    explicit BStr(const wchar_t* text) : value(SysAllocString(text)) {}
    explicit BStr(const std::wstring& text) : BStr(text.c_str()) {}
    ~BStr() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
struct Variant {
    VARIANT value;
    Variant() { VariantInit(&value); }
    explicit Variant(const std::wstring& text) : Variant() { value.vt = VT_BSTR; value.bstrVal = SysAllocString(text.c_str()); }
    ~Variant() { VariantClear(&value); }
};
struct KernelHandle {
    HANDLE value = nullptr;
    explicit KernelHandle(HANDLE handle) : value(handle) {}
    ~KernelHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct MediumGuard {
    STGMEDIUM& medium;
    ~MediumGuard() { ReleaseStgMedium(&medium); }
};
struct Icon {
    HICON value = nullptr;
    Icon() = default;
    explicit Icon(HICON h) : value(h) {}
    ~Icon() { if (value) DestroyIcon(value); }
    Icon(Icon&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Icon& operator=(Icon&& other) noexcept {
        if (this != &other) { if (value) DestroyIcon(value); value = std::exchange(other.value, nullptr); }
        return *this;
    }
    Icon(const Icon&) = delete;
    Icon& operator=(const Icon&) = delete;
};

std::wstring ErrorText(HRESULT error) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"操作失败";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    wchar_t code[32]; swprintf_s(code, L" (0x%08lX)", static_cast<unsigned long>(error));
    return result + code;
}
void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }
std::wstring ExePath() {
    wchar_t small[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, small, MAX_PATH);
    if (!length) throw HRESULT_FROM_WIN32(GetLastError());
    if (length < MAX_PATH) return {small, length};
    for (DWORD size = 512; size <= 32768; size *= 2) {
        std::wstring path(size, L'\0');
        length = GetModuleFileNameW(nullptr, path.data(), size);
        if (!length) throw HRESULT_FROM_WIN32(GetLastError());
        if (length < size) { path.resize(length); return path; }
    }
    throw HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
}
std::wstring AbsolutePath(const std::wstring& path) {
    return box::FullPath(path);
}
void EnsureDirectory(const std::wstring& path) {
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        throw HRESULT_FROM_WIN32(result);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw HRESULT_FROM_WIN32(ERROR_DIRECTORY);
}
bool WriteBytes(const std::wstring& path, const void* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file); return ok;
}
#ifdef DESKTOPBOX_TESTS
std::string ReadSmallFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "";
    char data[4096]; DWORD read = 0; ReadFile(file, data, sizeof(data), &read, nullptr); CloseHandle(file);
    return std::string(data, read);
}
#endif

using box::Storage;

struct AppearanceSettings {
    int background = 0, transparency = 55, blur = 18;
    std::wstring icon;
    static AppearanceSettings Read(const Storage& storage) {
        return {std::clamp(storage.ReadInt(L"BackgroundMode", 0), 0, 3),
                std::clamp(storage.ReadInt(L"GlassTransparency", 55), 0, 100),
                std::clamp(storage.ReadInt(L"GlassBlur", 18), 0, 40), storage.Read(L"IconFile")};
    }
    static AppearanceSettings NewBoxDefaults(const Storage& storage) {
        return {std::clamp(storage.ReadInt(L"NewBoxBackgroundMode", 0), 0, 3),
                std::clamp(storage.ReadInt(L"NewBoxGlassTransparency", 55), 0, 100),
                std::clamp(storage.ReadInt(L"NewBoxGlassBlur", 18), 0, 40),
                storage.ReadInt(L"HasNewBoxAppearance", 0) ? storage.Read(L"NewBoxIconFile") :
                    storage.Read(L"NewBoxIconFile", storage.Read(L"IconFile").c_str())};
    }
    void Save(const Storage& storage) const {
        storage.Write(L"BackgroundMode", std::to_wstring(background));
        storage.Write(L"GlassTransparency", std::to_wstring(transparency));
        storage.Write(L"GlassBlur", std::to_wstring(blur));
        storage.Write(L"IconFile", icon);
    }
    void SaveNewBoxDefaults(const Storage& storage) const {
        storage.Write(L"NewBoxBackgroundMode", std::to_wstring(background));
        storage.Write(L"NewBoxGlassTransparency", std::to_wstring(transparency));
        storage.Write(L"NewBoxGlassBlur", std::to_wstring(blur));
        storage.Write(L"NewBoxIconFile", icon);
        storage.Write(L"HasNewBoxAppearance", L"1");
    }
    bool operator==(const AppearanceSettings& other) const {
        return background == other.background && transparency == other.transparency && blur == other.blur && icon == other.icon;
    }
};
using AppearanceCommit = std::function<void(const AppearanceSettings&)>;

std::wstring UserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw HRESULT_FROM_WIN32(GetLastError());
    DWORD bytes = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes);
    DWORD error = GetLastError(); CloseHandle(token);
    if (!ok) throw HRESULT_FROM_WIN32(error);
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) throw HRESULT_FROM_WIN32(GetLastError());
    std::wstring result(text); LocalFree(text); return result;
}
std::wstring XmlEscape(const std::wstring& text) {
    std::wstring result;
    for (wchar_t c : text) {
        switch (c) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'\"': result += L"&quot;"; break;
        case L'\'': result += L"&apos;"; break;
        default: result += c;
        }
    }
    return result;
}
std::wstring StartupXml(const std::wstring& sid, const std::wstring& executable, const std::wstring& dataRoot = L"") {
    return L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>登录后启动桌面文件盒</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEscape(sid) + L"</UserId><Delay>PT0S</Delay></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"User\"><UserId>" + XmlEscape(sid) + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable><IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>"
        L"<WakeToRun>false</WakeToRun><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>6</Priority></Settings>"
        L"<Actions Context=\"User\"><Exec><Command>" + XmlEscape(executable) + L"</Command>" +
        (dataRoot.empty() ? L"" : L"<Arguments>" + XmlEscape(L"--data-dir \"" + dataRoot + L"\"") + L"</Arguments>") +
        L"<WorkingDirectory>" + XmlEscape(box::ParentPath(executable)) + L"</WorkingDirectory></Exec></Actions></Task>";
}
// Called by the user's startup switch or by an installer, always at normal desktop permissions.
class StartupTask {
    ComPtr<ITaskService> service_;
    ComPtr<ITaskFolder> folder_;
    std::wstring sid_, name_;
public:
    StartupTask() : sid_(UserSid()), name_(L"DesktopBox-" + sid_) {
        Check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(service_.put())));
        Variant empty;
        Check(service_->Connect(empty.value, empty.value, empty.value, empty.value));
        Check(service_->GetFolder(BStr(L"\\"), folder_.put()));
    }
    bool Enabled() {
        ComPtr<IRegisteredTask> task;
        if (FAILED(folder_->GetTask(BStr(name_), task.put()))) return false;
        VARIANT_BOOL enabled = VARIANT_FALSE;
        return SUCCEEDED(task->get_Enabled(&enabled)) && enabled == VARIANT_TRUE;
    }
    bool Validate(const std::wstring& executable) {
        // Parsing and inspection only; no task is registered by this method.
        ComPtr<ITaskDefinition> definition;
        Check(service_->NewTask(0, definition.put()));
        Check(definition->put_XmlText(BStr(StartupXml(sid_, executable))));
        ComPtr<ITaskSettings> settings; Check(definition->get_Settings(settings.put()));
        int priority = 0; VARIANT_BOOL battery = VARIANT_TRUE; TASK_INSTANCES_POLICY policy;
        Check(settings->get_Priority(&priority)); Check(settings->get_DisallowStartIfOnBatteries(&battery));
        Check(settings->get_MultipleInstances(&policy));
        ComPtr<IPrincipal> principal; Check(definition->get_Principal(principal.put()));
        TASK_RUNLEVEL_TYPE level; TASK_LOGON_TYPE logon;
        Check(principal->get_RunLevel(&level)); Check(principal->get_LogonType(&logon));
        return priority == 6 && battery == VARIANT_FALSE && policy == TASK_INSTANCES_IGNORE_NEW &&
               level == TASK_RUNLEVEL_LUA && logon == TASK_LOGON_INTERACTIVE_TOKEN;
    }
    void Set(bool enabled, const std::wstring& dataRoot = L"") {
        if (!enabled) {
            const HRESULT result = folder_->DeleteTask(BStr(name_), 0);
            if (result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) Check(result);
            return;
        }
        ComPtr<IRegisteredTask> registered;
        Variant user(sid_), empty;
        Check(folder_->RegisterTask(BStr(name_), BStr(StartupXml(sid_, ExePath(), dataRoot)), TASK_CREATE_OR_UPDATE,
              user.value, empty.value, TASK_LOGON_INTERACTIVE_TOKEN, empty.value, registered.put()));
    }
};

struct Item {
    std::wstring path, label;
    Icon icon;
    ULONGLONG modified = 0;
    int manualOrder = INT_MAX;
};
struct IconResult { unsigned generation; size_t index; Icon icon; };
class IconLoader {
#ifdef DESKTOPBOX_TESTS
    friend int SelfTest(const std::wstring& directory);
#endif
    struct Request { HWND destination; ULONGLONG client; unsigned generation; size_t index; std::wstring path; int size; };
    class Worker {
    public:
        std::mutex mutex;
        std::condition_variable event;
        std::deque<Request> requests;
        std::vector<ULONGLONG> clients;
        ULONGLONG nextClient = 0;
        bool stop = false;
        std::thread thread;
        Worker() : thread([this] { Run(); }) {}
        ~Worker() {
            { std::lock_guard<std::mutex> lock(mutex); stop = true; requests.clear(); }
            event.notify_one();
            if (thread.joinable()) thread.join();
        }
        void Run() {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (;;) {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    event.wait(lock, [this] { return stop || !requests.empty(); });
                    if (stop) break;
                    request = std::move(requests.front()); requests.pop_front();
                }
                SHFILEINFOW info{};
                SHGetFileInfoW(request.path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON);
                auto result = std::make_unique<IconResult>();
                result->generation = request.generation; result->index = request.index;
                if (info.hIcon) {
                    result->icon = Icon(reinterpret_cast<HICON>(CopyImage(info.hIcon, IMAGE_ICON, request.size, request.size, 0)));
                    DestroyIcon(info.hIcon);
                }
                // Unsubscribing and delivery share this lock: a destroyed box can never
                // receive an in-flight result, even if Windows reuses its HWND.
                std::lock_guard<std::mutex> lock(mutex);
                if (std::find(clients.begin(), clients.end(), request.client) != clients.end() &&
                    PostMessageW(request.destination, kIconReady, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
            }
            if (SUCCEEDED(initialized)) CoUninitialize();
        }
    };
    inline static std::weak_ptr<Worker> sharedWorker_;
    std::shared_ptr<Worker> worker_;
    HWND destination_ = nullptr;
    ULONGLONG client_ = 0;
public:
    void Start(HWND destination) {
        if (worker_ && destination_ == destination) return;
        Stop();
        auto worker = sharedWorker_.lock();
        if (!worker) { worker = std::make_shared<Worker>(); sharedWorker_ = worker; }
        { std::lock_guard<std::mutex> lock(worker->mutex); client_ = ++worker->nextClient; worker->clients.push_back(client_); }
        worker_ = std::move(worker);
        destination_ = destination;
    }
    void Clear() {
        if (!worker_) return;
        std::lock_guard<std::mutex> lock(worker_->mutex);
        auto& requests = worker_->requests;
        requests.erase(std::remove_if(requests.begin(), requests.end(), [this](const Request& request) { return request.client == client_; }), requests.end());
    }
    void RequestIcon(unsigned generation, size_t index, const std::wstring& path, int size) {
        if (!worker_) return;
        { std::lock_guard<std::mutex> lock(worker_->mutex); worker_->requests.push_back({destination_, client_, generation, index, path, size}); }
        worker_->event.notify_one();
    }
    void Prioritize() {
        if (!worker_) return;
        std::lock_guard<std::mutex> lock(worker_->mutex);
        std::stable_partition(worker_->requests.begin(), worker_->requests.end(),
            [this](const Request& request) { return request.client == client_; });
    }
    void Stop() {
        Clear();
        if (worker_) {
            std::lock_guard<std::mutex> lock(worker_->mutex);
            auto& clients = worker_->clients;
            clients.erase(std::remove(clients.begin(), clients.end(), client_), clients.end());
        }
        MSG message{};
        while (destination_ && PeekMessageW(&message, destination_, kIconReady, kIconReady, PM_REMOVE))
            delete reinterpret_cast<IconResult*>(message.lParam);
        destination_ = nullptr; client_ = 0; worker_.reset();
    }
    ~IconLoader() { Stop(); }
};


struct NameInput { std::wstring value; const wchar_t* title = L"收纳盒名称"; int limit = 48; };
INT_PTR CALLBACK NameProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_INITDIALOG) {
        auto* input = reinterpret_cast<NameInput*>(l);
        SetWindowLongPtrW(dialog, DWLP_USER, l);
        SetWindowTextW(dialog, input->title);
        SetDlgItemTextW(dialog, 1001, input->value.c_str());
        SendDlgItemMessageW(dialog, 1001, EM_SETLIMITTEXT, input->limit, 0);
        SendDlgItemMessageW(dialog, 1001, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, 1001)); return FALSE;
    }
    if (message == WM_COMMAND && LOWORD(w) == IDOK) {
        wchar_t value[512]{}; GetDlgItemTextW(dialog, 1001, value, 512);
        std::wstring text = value;
        const auto first = text.find_first_not_of(L" \t");
        if (first == std::wstring::npos) { SetFocus(GetDlgItem(dialog, 1001)); return TRUE; }
        text = text.substr(first, text.find_last_not_of(L" \t") - first + 1);
        auto* input = reinterpret_cast<NameInput*>(GetWindowLongPtrW(dialog, DWLP_USER)); input->value = text;
        EndDialog(dialog, IDOK); return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(w) == IDCANCEL) || message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
std::wstring AskName(HINSTANCE instance, HWND owner, const std::wstring& initial, const wchar_t* title = L"收纳盒名称", int limit = 48) {
    NameInput input{initial, title, limit};
    if (DialogBoxParamW(instance, MAKEINTRESOURCEW(101), owner, NameProc, reinterpret_cast<LPARAM>(&input)) == IDOK) return input.value;
    return L"";
}
std::wstring PickFolder(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    Check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(dialog.put())));
    DWORD options = 0; Check(dialog->GetOptions(&options));
    Check(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT));
    Check(dialog->SetTitle(title));
    if (FAILED(dialog->Show(owner))) return L"";
    ComPtr<IShellItem> item; Check(dialog->GetResult(item.put()));
    wchar_t* path = nullptr; Check(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
    std::wstring result(path); CoTaskMemFree(path); return result;
}

std::vector<std::wstring> ShellItemPaths(IShellItemArray* items) {
    std::vector<std::wstring> result;
    DWORD count = 0; Check(items->GetCount(&count));
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item; Check(items->GetItemAt(index, item.put()));
        wchar_t* path = nullptr; Check(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        result.emplace_back(path); CoTaskMemFree(path);
    }
    return result;
}

class ItemPickerEvents final : public IFileDialogEvents, public IFileDialogControlEvents {
    LONG references_ = 1;
    IFileOpenDialog* dialog_;
public:
    std::vector<std::wstring> selected;
    explicit ItemPickerEvents(IFileOpenDialog* dialog) : dialog_(dialog) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IFileDialogEvents)) *result = static_cast<IFileDialogEvents*>(this);
        else if (IsEqualIID(iid, IID_IFileDialogControlEvents)) *result = static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override { const LONG count = InterlockedDecrement(&references_); if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize*, DWORD control) override {
        if (control != 100) return S_OK;
        try {
            ComPtr<IShellItemArray> items; Check(dialog_->GetSelectedItems(items.put()));
            auto paths = ShellItemPaths(items.get());
            if (!paths.empty()) { selected = std::move(paths); return dialog_->Close(S_OK); }
        } catch (HRESULT error) { return error; }
        catch (...) { return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override { *response = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override { *response = FDEOR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }
};

std::vector<std::wstring> PickItems(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    Check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(dialog.put())));
    DWORD options = 0; Check(dialog->GetOptions(&options));
    Check(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR | FOS_NODEREFERENCELINKS));
    Check(dialog->SetTitle(L"选择要移入的文件或文件夹"));
    Check(dialog->SetOkButtonLabel(L"移入"));
    ComPtr<IFileDialogCustomize> controls;
    Check(dialog->QueryInterface(IID_IFileDialogCustomize, reinterpret_cast<void**>(controls.put())));
    // The standard Open button navigates into folders. This button accepts the current selection, including folders.
    Check(controls->AddPushButton(100, L"移入选中项（含文件夹）"));
    auto* events = new ItemPickerEvents(dialog.get());
    DWORD cookie = 0; const HRESULT advised = dialog->Advise(events, &cookie);
    if (FAILED(advised)) { events->Release(); Check(advised); }
    const HRESULT shown = dialog->Show(owner);
    dialog->Unadvise(cookie);
    auto selected = std::move(events->selected); events->Release();
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    Check(shown);
    if (selected.empty()) { ComPtr<IShellItemArray> items; Check(dialog->GetResults(items.put())); selected = ShellItemPaths(items.get()); }
    return selected;
}

class App;
class DragSource final : public IDropSource {
    LONG references_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER; *result = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDropSource)) return E_NOINTERFACE;
        *result = static_cast<IDropSource*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = InterlockedDecrement(&references_); if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override { return escape ? DRAGDROP_S_CANCEL : !(keys & MK_LBUTTON) ? DRAGDROP_S_DROP : S_OK; }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
class DirectoryWatch {
    HANDLE change_ = INVALID_HANDLE_VALUE, wait_ = nullptr;
    HWND window_ = nullptr;
    static void CALLBACK Changed(void* context, BOOLEAN) {
        auto* watcher = static_cast<DirectoryWatch*>(context);
        PostMessageW(watcher->window_, kFilesChanged, 0, 0);
    }
public:
    void Start(HWND window, const std::wstring& directory) {
        window_ = window;
        change_ = FindFirstChangeNotificationW(directory.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (change_ != INVALID_HANDLE_VALUE)
            RegisterWaitForSingleObject(&wait_, change_, Changed, this, INFINITE, WT_EXECUTEONLYONCE);
    }
    void Rearm() {
        if (wait_) { UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE); wait_ = nullptr; }
        if (change_ != INVALID_HANDLE_VALUE && FindNextChangeNotification(change_))
            RegisterWaitForSingleObject(&wait_, change_, Changed, this, INFINITE, WT_EXECUTEONLYONCE);
    }
    void Stop() {
        if (wait_) { UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE); wait_ = nullptr; }
        if (change_ != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(change_); change_ = INVALID_HANDLE_VALUE; }
    }
    ~DirectoryWatch() { Stop(); }
};
class DropTarget final : public IDropTarget {
    LONG references_ = 1;
    App* app_;
    DWORD acceptedEffect_ = DROPEFFECT_NONE;
public:
    explicit DropTarget(App* app) : app_(app) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) {
            *result = static_cast<IDropTarget*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG result = InterlockedDecrement(&references_); if (!result) delete this; return result; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD*) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD*) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD*) override;
};

class App {
#ifdef DESKTOPBOX_TESTS
    friend int SelfTest(const std::wstring& directory);
    friend int PerformanceTest(const std::wstring& directory);
#endif
public:
    Storage storage;
    HWND host = nullptr, panel = nullptr;
    bool dragIncoming = false;
private:
    HINSTANCE instance_;
    std::wstring name_;
    std::function<void(const std::wstring&, POINT)> createBox_;
    std::function<void(App*)> deleteBox_;
    std::function<bool(App*)> activatePanel_;
    HWND owner_ = nullptr, desktopParent_ = nullptr;
    SIZE cell_{};
    int desktopIconSize_ = 0;
    bool anchorShown_ = false, removing_ = false;
    bool nativeMode_ = false, nativeLocated_ = false, nativePointerInside_ = false;
    box::NativeEntry nativeEntry_;
    RECT nativeCell_{}, nativeImage_{};
    int backgroundMode_ = 0, glassTransparency_ = 55, glassBlur_ = 18;
    bool customGlass_ = false;
    box::Backdrop backdrop_;
    box::BackdropLoader backdropLoader_;
    ULONGLONG backdropGeneration_ = 0;
    bool backdropPending_ = false;
    int pendingBlur_ = -1;
    box::DibSurface panelSurface_;
    mutable box::TextMasks textMasks_;
    ULONGLONG textCacheUntil_ = 0;
    int roundedWidth_ = 0, roundedHeight_ = 0, roundedDark_ = -1;
    UINT roundedDpi_ = 0;
    DWORD roundedBackdrop_ = 0;
    box::ShellMenu shellMenu_;
    std::unique_ptr<box::FluentSettings> settings_;
    std::wstring previewIconFile_;
    bool globalSettings_ = false;
    bool acrylic_ = false, iconThreadStarted_ = false, anchorPaintPending_ = false, renderingAnchor_ = false;
    mutable DWORD* paintPixels_ = nullptr;
    mutable int paintWidth_ = 0, paintHeight_ = 0;
    Icon folderIcon_, placeholder_;
    HFONT titleFont_ = nullptr, bodyFont_ = nullptr, smallFont_ = nullptr, desktopFont_ = nullptr;
    std::vector<Item> items_;
    std::vector<Item> hiddenItems_;
    size_t totalItems_ = 0;
    std::wstring searchQuery_;
    HWND searchEdit_ = nullptr;
    HBRUSH searchBrush_ = nullptr;
    bool searchTracking_ = false, searchComposing_ = false;
    struct GhostCard { std::wstring path, label; Icon icon; RECT rect{}; bool selected = false; double alpha = 1.; };
    struct CardMotion { int index; RECT from{}, to{}; double alphaFrom = 1.; };
    std::vector<CardMotion> cardMotion_;
    std::vector<GhostCard> ghostCards_;
    ULONGLONG motionStarted_ = 0;
    bool motionAllowed_ = true;
    mutable std::vector<DWORD> blendScratch_;
    IconLoader icons_;
    DirectoryWatch watcher_;
    std::vector<bool> requestedIcons_;
    unsigned generation_ = 0;
    UINT dpi_ = 96, taskbarCreated_ = 0;
    box::Direction direction_ = box::Direction::Automatic;
    box::SortMode sortMode_ = box::SortMode::Manual;
    box::Selection selection_;
    bool dark_ = false, trackingHost_ = false, trackingPanel_ = false;
    bool pressed_ = false, moving_ = false, inMenu_ = false, inDialog_ = false;
    POINT pressPoint_{}, initialPoint_{};
    int hovered_ = -1, firstRow_ = 0, wheelRemainder_ = 0;
    int contextItem_ = -1;
    bool panelVisible_ = false, openingPanel_ = false;
    ULONGLONG closeDeadline_ = 0;
    bool itemPressed_ = false;
    int pressedItem_ = -1;
    POINT itemPressPoint_{};
    std::vector<std::wstring> draggingPaths_;
    bool collapseOnRelease_ = false, toggleOnRelease_ = false;
    bool addPressed_ = false, sortPressed_ = false, sortHovered_ = false;
    bool marquee_ = false, marqueeMoved_ = false, marqueeControl_ = false, marqueeShift_ = false, marqueeScroll_ = false;
    POINT marqueeStart_{}, marqueePoint_{};
    RECT marqueeRect_{};
    box::Selection::Mask marqueeBaseline_;
    int marqueeFocus_ = -1, marqueeAnchor_ = -1;
    HWND tooltip_ = nullptr;
    int tooltipCount_ = 0;
    DropTarget* hostDrop_ = nullptr;
    DropTarget* panelDrop_ = nullptr;
    inline static HWINEVENTHOOK foregroundHook_ = nullptr;
    inline static std::vector<HWND> eventHosts_;
    static void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD, HWND foreground, LONG, LONG, DWORD, DWORD) {
        if (!foreground) return;
        wchar_t name[64]{}; GetClassNameW(foreground, name, 64);
        if (foreground == GetShellWindow() || wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0)
            for (HWND window : eventHosts_) PostMessageW(window, kDesktopVisible, 0, 0);
    }
    static LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) { return Dispatch(hwnd, message, w, l, false); }
    static LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) { return Dispatch(hwnd, message, w, l, true); }
    static LRESULT CALLBACK SearchProc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR context) {
        auto& app = *reinterpret_cast<App*>(context);
        if (message == WM_IME_STARTCOMPOSITION) app.searchComposing_ = true;
        if (message == WM_IME_ENDCOMPOSITION) app.searchComposing_ = false;
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(app.panel, nullptr, FALSE);
        if (message == WM_MOUSEMOVE) {
            app.CancelClose();
            if (!app.searchTracking_) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track); app.searchTracking_ = true; }
        }
        if (message == WM_MOUSELEAVE) { app.searchTracking_ = false; app.ScheduleClose(); }
        if (message == WM_KEYDOWN && !app.searchComposing_) {
            if (w == VK_ESCAPE) {
                if (!app.searchQuery_.empty()) SetWindowTextW(hwnd, L"");
                else SetFocus(app.panel);
                return 0;
            }
            if (w == VK_TAB || w == VK_DOWN || w == VK_RETURN) {
                SetFocus(app.panel);
                if (w == VK_DOWN && !app.items_.empty()) { app.selection_.Click(0); app.SelectionChanged(); }
                if (w == VK_RETURN && !app.items_.empty()) SendMessageW(app.panel, WM_KEYDOWN, VK_RETURN, 0);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && w == 'F') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        }
        if (message == WM_CHAR && (w == VK_RETURN || w == VK_TAB || w == VK_ESCAPE) && !app.searchComposing_) return 0;
        return DefSubclassProc(hwnd, message, w, l);
    }
    static LRESULT Dispatch(HWND hwnd, UINT message, WPARAM w, LPARAM l, bool isPanel) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = reinterpret_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            if (isPanel) app->panel = hwnd; else app->host = hwnd;
        }
        if (!app) return DefWindowProcW(hwnd, message, w, l);
        try { return app->Handle(hwnd, message, w, l, isPanel); }
        catch (HRESULT error) { app->Report(L"无法完成操作", error); return 0; }
        catch (...) { app->Report(L"操作失败，请重试", E_FAIL); return 0; }
    }
    int Px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND OwnerWindow() const { return panel ? panel : owner_; }
    RECT AnchorIconRect() const {
        const int x = (cell_.cx - desktopIconSize_) / 2;
        return {x, 2, x + desktopIconSize_, 2 + desktopIconSize_};
    }
    RECT AnchorLabelRect() const { return {Px(2), 2 + Px(4) + desktopIconSize_, cell_.cx - Px(2), cell_.cy - Px(3)}; }
    void MoveAnchor(POINT screen, bool resize = false) {
        if (desktopParent_) ScreenToClient(desktopParent_, &screen);
        SetWindowPos(host, nullptr, screen.x, screen.y, cell_.cx, cell_.cy,
                     SWP_NOACTIVATE | SWP_NOZORDER | (resize ? 0 : SWP_NOSIZE));
    }
    POINT SnapPosition(POINT point, const box::DesktopGrid& grid) const {
        auto occupied = grid.occupied;
        for (HWND other : eventHosts_) if (other != host && IsWindow(other)) {
            RECT bounds{}; GetWindowRect(other, &bounds); occupied.push_back(bounds);
        }
        MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
        return box::SnapAnchor(point, cell_, monitor.rcWork, occupied);
    }
    void RequestAnchorPaint() {
        // Layered windows need their bitmap submitted explicitly, including before their first WM_PAINT.
        if (!nativeMode_ && host && !anchorPaintPending_) anchorPaintPending_ = PostMessageW(host, kRenderAnchor, 0, 0) != FALSE;
    }
    COLORREF Background() const { return dark_ ? RGB(30, 33, 39) : RGB(250, 251, 254); }
    COLORREF Foreground() const { return dark_ ? RGB(239, 242, 248) : RGB(32, 42, 58); }
    COLORREF Muted() const { return dark_ ? RGB(159, 170, 187) : RGB(110, 123, 143); }
    COLORREF Border() const { return dark_ ? RGB(59, 65, 76) : RGB(221, 228, 238); }
    COLORREF SearchFill() const { return dark_ ? RGB(41, 47, 57) : RGB(246, 248, 251); }
    RECT WorkArea(HWND window) const {
        MONITORINFO info{}; info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
        return info.rcWork;
    }
    void Round(HWND window) {
        if (!window) return;
        if (window == host) { SetWindowRgn(host, nullptr, TRUE); return; }
        if (!roundedBackdrop_) {
            const DWORD preference = 2;
            DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(33), &preference, sizeof(preference));
        }
        const BOOL dark = dark_;
        if (roundedDark_ != dark) {
            DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark)); roundedDark_ = dark;
        }
        const DWORD backdrop = backgroundMode_ == 0 && !customGlass_ ? 3 : 1;
        if (roundedBackdrop_ != backdrop) {
            const HRESULT changed = DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(38), &backdrop, sizeof(backdrop));
            acrylic_ = backdrop == 3 && SUCCEEDED(changed); roundedBackdrop_ = backdrop;
            MARGINS margins = acrylic_ ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
            DwmExtendFrameIntoClientArea(window, &margins);
        }
        RECT rect{}; GetClientRect(window, &rect);
        if (roundedWidth_ != rect.right || roundedHeight_ != rect.bottom || roundedDpi_ != dpi_) {
            SetWindowRgn(window, CreateRoundRectRgn(0, 0, rect.right + 1, rect.bottom + 1, Px(18), Px(18)), TRUE);
            roundedWidth_ = rect.right; roundedHeight_ = rect.bottom; roundedDpi_ = dpi_;
        }
    }
    void DropTextCache() {
        if (host) KillTimer(host, kTextCacheTimer);
        textCacheUntil_ = 0; textMasks_.Clear();
    }
    void ReleaseTextCache(ULONGLONG now) {
        if (!textCacheUntil_ || panelVisible_) { if (host) KillTimer(host, kTextCacheTimer); textCacheUntil_ = 0; return; }
        if (now < textCacheUntil_) {
            if (!SetTimer(host, kTextCacheTimer, static_cast<UINT>(textCacheUntil_ - now), nullptr)) DropTextCache();
        } else DropTextCache();
    }
    void LoadAppearance() {
        DropTextCache();
        DWORD value = 1, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
        dark_ = backgroundMode_ == 1 || (backgroundMode_ != 2 && value == 0);
        for (HFONT font : {titleFont_, bodyFont_, smallFont_, desktopFont_}) if (font) DeleteObject(font);
        LOGFONTW iconFont{};
        if (!SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(iconFont), &iconFont, 0, dpi_)) {
            iconFont.lfHeight = -Px(12); iconFont.lfWeight = FW_NORMAL;
            wcscpy_s(iconFont.lfFaceName, L"Microsoft YaHei UI");
        }
        // Grayscale coverage is valid on a transparent surface; subpixel ClearType assumes an opaque background.
        iconFont.lfQuality = ANTIALIASED_QUALITY;
        desktopFont_ = CreateFontIndirectW(&iconFont);
        titleFont_ = CreateFontW(-Px(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        bodyFont_ = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        smallFont_ = CreateFontW(-Px(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (searchBrush_) DeleteObject(searchBrush_);
        searchBrush_ = CreateSolidBrush(SearchFill());
        if (searchEdit_) { SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE); LayoutSearch(); }
        folderIcon_ = Icon();
        const auto chosenIcon = settings_ ? (previewIconFile_.empty() ? L"" : storage.iconDirectory + L"\\" + previewIconFile_) : storage.SelectedIconPath();
        if (!chosenIcon.empty())
            folderIcon_ = Icon(reinterpret_cast<HICON>(LoadImageW(nullptr, chosenIcon.c_str(), IMAGE_ICON, std::max(Px(48), desktopIconSize_), std::max(Px(48), desktopIconSize_), LR_LOADFROMFILE)));
        if (!folderIcon_.value) {
            SHSTOCKICONINFO info{}; info.cbSize = sizeof(info);
            if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_LARGEICON, &info))) folderIcon_ = Icon(info.hIcon);
        }
        SHSTOCKICONINFO placeholder{}; placeholder.cbSize = sizeof(placeholder);
        if (SUCCEEDED(SHGetStockIconInfo(SIID_DOCNOASSOC, SHGSI_ICON | SHGSI_LARGEICON, &placeholder))) placeholder_ = Icon(placeholder.hIcon);
        if (host) {
            Round(host); Round(panel);
            SendMessageW(host, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(folderIcon_.value));
            SendMessageW(host, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(folderIcon_.value));
            RequestAnchorPaint(); if (panel) InvalidateRect(panel, nullptr, FALSE);
        }
    }
    int Rows() const { return static_cast<int>((items_.size() + 3) / 4); }
    int VisibleRows() const {
        RECT rect{}; GetClientRect(panel, &rect);
        return std::max(1, static_cast<int>((rect.bottom - Px(88)) / Px(110)));
    }
    int MaxRow() const { return std::max(0, Rows() - VisibleRows()); }
    RECT ItemRect(int index) const {
        const int row = index / 4 - firstRow_, col = index % 4;
        return {Px(12 + col * 98), Px(76 + row * 110), Px(106 + col * 98), Px(184 + row * 110)};
    }
    int HitItem(POINT point) const {
        const int start = firstRow_ * 4, end = std::min(static_cast<int>(items_.size()), start + VisibleRows() * 4);
        const RECT viewport = ContentRect(); if (!PtInRect(&viewport, point)) return -1;
        for (int i = end - 1; i >= start; --i) { RECT rect = DisplayRect(i); if (PtInRect(&rect, point)) return i; }
        return -1;
    }
    RECT AddRect() const {
        RECT rect{}; GetClientRect(panel, &rect);
        return {rect.right - Px(75), Px(22), rect.right - Px(18), Px(49)};
    }
    RECT SortRect() const {
        RECT rect{}; GetClientRect(panel, &rect);
        return {rect.right - Px(157), Px(22), rect.right - Px(84), Px(49)};
    }
    RECT SearchRect() const { const RECT sort = SortRect(); return {Px(150), Px(22), std::max<LONG>(Px(170), sort.left - Px(9)), Px(49)}; }
    void LayoutSearch() {
        if (!searchEdit_) return;
        const RECT rect = SearchRect();
        SetWindowPos(searchEdit_, nullptr, rect.left + Px(8), rect.top + Px(6),
                     std::max(1L, rect.right - rect.left - Px(30)), Px(18), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    double MotionProgress() const { return box::LayoutEase(GetTickCount64() - motionStarted_); }
    RECT DisplayRect(int index) const {
        const double progress = MotionProgress();
        for (const auto& motion : cardMotion_) if (motion.index == index) return box::InterpolateRect(motion.from, motion.to, progress);
        return ItemRect(index);
    }
    void StopMotion() {
        if (host) KillTimer(host, kLayoutTimer);
        cardMotion_.clear(); ghostCards_.clear();
        if (panel) InvalidateRect(panel, nullptr, FALSE);
    }
    bool MotionEnabled() const {
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        return panelVisible_ && motionAllowed_ && enabled;
    }
    std::vector<GhostCard> CaptureCards() const {
        std::vector<GhostCard> cards;
        if (!MotionEnabled()) return cards;
        const int start = firstRow_ * 4, end = std::min(static_cast<int>(items_.size()), start + VisibleRows() * 4);
        const double progress = MotionProgress();
        for (int index = start; index < end; ++index) {
            const auto& item = items_[index]; GhostCard card;
            card.path = item.path; card.label = item.label; card.rect = DisplayRect(index); card.selected = selection_.Contains(index);
            card.icon = Icon(CopyIcon(item.icon.value ? item.icon.value : placeholder_.value));
            for (const auto& motion : cardMotion_) if (motion.index == index) card.alpha = motion.alphaFrom + (1. - motion.alphaFrom) * progress;
            cards.push_back(std::move(card));
        }
        for (const auto& ghost : ghostCards_) if (ghost.alpha * (1. - progress) > .01) {
            GhostCard card; card.path = ghost.path; card.label = ghost.label; card.selected = ghost.selected;
            card.rect = ghost.rect; OffsetRect(&card.rect, 0, -static_cast<int>(Px(8) * progress));
            card.alpha = ghost.alpha * (1. - progress); card.icon = Icon(CopyIcon(ghost.icon.value));
            cards.push_back(std::move(card));
        }
        return cards;
    }
    void StartMotion(std::vector<GhostCard> previous) {
        StopMotion();
        if (!MotionEnabled()) return;
        const int start = firstRow_ * 4, end = std::min(static_cast<int>(items_.size()), start + VisibleRows() * 4);
        std::vector<bool> retained(previous.size(), false);
        for (int index = start; index < end; ++index) {
            const RECT target = ItemRect(index); RECT from = target; double alpha = 0.;
            OffsetRect(&from, 0, Px(18));
            for (size_t i = 0; i < previous.size(); ++i) if (previous[i].path == items_[index].path) {
                from = previous[i].rect; alpha = previous[i].alpha; retained[i] = true; break;
            }
            if (!EqualRect(&from, &target) || alpha < 1.) cardMotion_.push_back({index, from, target, alpha});
        }
        for (size_t i = 0; i < previous.size() && ghostCards_.size() < 12; ++i) if (!retained[i]) ghostCards_.push_back(std::move(previous[i]));
        if (cardMotion_.empty() && ghostCards_.empty()) return;
        motionStarted_ = GetTickCount64(); ClearTooltips();
        if (!SetTimer(host, kLayoutTimer, 16, nullptr)) StopMotion();
    }
    RECT ContentRect() const {
        RECT rect{}; GetClientRect(panel, &rect);
        return {Px(10), Px(72), rect.right - Px(10), rect.bottom - Px(10)};
    }
    std::vector<std::wstring> SelectedPaths() const {
        std::vector<std::wstring> paths; paths.reserve(static_cast<size_t>(selection_.Count()));
        for (int i : selection_.Indices()) if (i < static_cast<int>(items_.size())) paths.push_back(items_[i].path);
        return paths;
    }
    std::wstring FocusPath(int index) const {
        return index >= 0 && index < static_cast<int>(items_.size()) ? items_[index].path : L"";
    }
    void SelectionChanged() {
        if (!panel) return;
        const auto caption = selection_.Count() ? name_ + L" · 已选 " + std::to_wstring(selection_.Count()) + L" / " +
                             std::to_wstring(items_.size()) + L" 个项目" : name_ + L" · 内容";
        SetWindowTextW(panel, caption.c_str()); InvalidateRect(panel, nullptr, FALSE);
    }
    void RestoreSelection(const std::vector<std::wstring>& paths, const std::wstring& focus = L"", const std::wstring& anchor = L"") {
        std::vector<std::wstring_view> sorted; sorted.reserve(paths.size());
        for (const auto& path : paths) sorted.emplace_back(path);
        std::sort(sorted.begin(), sorted.end());
        selection_.Reset(items_.size()); box::Selection::Mask mask(items_.size(), 0);
        int focused = -1, anchored = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
            mask[i] = std::binary_search(sorted.begin(), sorted.end(), std::wstring_view(items_[i].path));
            if (items_[i].path == focus) focused = static_cast<int>(i);
            if (items_[i].path == anchor) anchored = static_cast<int>(i);
        }
        selection_.Restore(mask, focused, anchored); SelectionChanged();
    }
    void EndMarquee(bool restore) {
        if (!marquee_) return;
        marquee_ = marqueeMoved_ = false;
        KillTimer(host, kSelectionScrollTimer); marqueeScroll_ = false;
        if (restore) selection_.Restore(marqueeBaseline_, marqueeFocus_, marqueeAnchor_);
        marqueeBaseline_.clear();
        if (GetCapture() == panel) ReleaseCapture();
        UpdateTooltips(); SelectionChanged(); ScheduleClose();
    }
    void MoveMarquee(POINT point) {
        if (!marquee_) return;
        marqueePoint_ = point;
        const RECT viewport = ContentRect();
        const POINT current{std::clamp(point.x, viewport.left, viewport.right),
                            std::clamp(point.y, viewport.top, viewport.bottom) + firstRow_ * Px(110)};
        if (!marqueeMoved_ && (abs(current.x - marqueeStart_.x) >= GetSystemMetrics(SM_CXDRAG) ||
                              abs(current.y - marqueeStart_.y) >= GetSystemMetrics(SM_CYDRAG))) marqueeMoved_ = true;
        if (marqueeMoved_) {
            marqueeRect_ = {std::min(current.x, marqueeStart_.x), std::min(current.y, marqueeStart_.y),
                            std::max(current.x, marqueeStart_.x), std::max(current.y, marqueeStart_.y)};
            std::vector<int> hits;
            for (size_t i = 0; i < items_.size(); ++i) {
                RECT item = ItemRect(static_cast<int>(i)), intersection{};
                OffsetRect(&item, 0, firstRow_ * Px(110));
                if (IntersectRect(&intersection, &marqueeRect_, &item)) hits.push_back(static_cast<int>(i));
            }
            selection_.Box(hits, marqueeBaseline_, marqueeControl_, marqueeShift_); SelectionChanged();
        }
        const bool scroll = marqueeMoved_ && ((point.y < viewport.top + Px(14) && firstRow_ > 0) ||
                                              (point.y > viewport.bottom - Px(14) && firstRow_ < MaxRow()));
        if (scroll && !marqueeScroll_) { SetTimer(host, kSelectionScrollTimer, 80, nullptr); marqueeScroll_ = true; }
        else if (!scroll && marqueeScroll_) { KillTimer(host, kSelectionScrollTimer); marqueeScroll_ = false; }
    }
    void BeginMarquee(POINT point, bool control, bool shift) {
        StopMotion();
        marqueeBaseline_ = selection_.Values(); marqueeFocus_ = selection_.Focus(); marqueeAnchor_ = selection_.Anchor();
        marqueeControl_ = control; marqueeShift_ = shift; marquee_ = true; marqueeMoved_ = false;
        marqueePoint_ = point; marqueeStart_ = {point.x, point.y + firstRow_ * Px(110)};
        if (!control && !shift) selection_.Clear();
        ClearTooltips(); SetCapture(panel); SelectionChanged();
    }
    void QueueVisibleIcons(bool warm = false) {
        const size_t start = static_cast<size_t>(firstRow_ * 4);
        const size_t end = std::min(items_.size(), start + static_cast<size_t>((warm ? 3 : VisibleRows()) * 4));
        if (start == end) return;
        if (!iconThreadStarted_) { icons_.Start(host); iconThreadStarted_ = true; }
        for (size_t i = start; i < end; ++i) if (!requestedIcons_[i]) {
            requestedIcons_[i] = true;
            icons_.RequestIcon(generation_, i, items_[i].path, Px(36));
        }
        if (!warm) icons_.Prioritize();
        // Keep at most 64 loaded icons. Evicted rows are loaded again only when visited.
        if (items_.size() > 64) {
            const size_t keepStart = start > 24 ? start - 24 : 0;
            const size_t keepEnd = std::min(items_.size(), keepStart + 64);
            for (size_t i = 0; i < items_.size(); ++i) if ((i < keepStart || i >= keepEnd) && items_[i].icon.value) {
                items_[i].icon = Icon(); requestedIcons_[i] = false;
            }
        }
    }
    bool ItemLess(const Item& a, const Item& b) const {
        if (sortMode_ == box::SortMode::Manual && a.manualOrder != b.manualOrder) return a.manualOrder < b.manualOrder;
        if (sortMode_ == box::SortMode::Modified && a.modified != b.modified) return a.modified > b.modified;
        const int compared = StrCmpLogicalW(a.label.c_str(), b.label.c_str());
        return compared != 0 ? compared < 0 : StrCmpLogicalW(a.path.c_str(), b.path.c_str()) < 0;
    }
    void Reload(std::vector<GhostCard> previous = {}) { RebuildView(true, std::move(previous)); }
    void RebuildView(bool rescan, std::vector<GhostCard> previous = {}, bool resetScroll = false) {
        if (previous.empty()) previous = CaptureCards();
        EndMarquee(false);
        itemPressed_ = addPressed_ = sortPressed_ = collapseOnRelease_ = toggleOnRelease_ = false;
        if (GetCapture() == panel) ReleaseCapture();
        const auto selected = SelectedPaths();
        const auto focus = FocusPath(selection_.Focus()), anchor = FocusPath(selection_.Anchor());
        ClearTooltips();
        icons_.Clear(); ++generation_; requestedIcons_.clear();
        std::vector<Item> pool = std::move(items_); items_.clear();
        for (auto& item : hiddenItems_) pool.push_back(std::move(item)); hiddenItems_.clear();
        if (rescan) {
            storage.ReconcileOrigins();
            std::unordered_map<std::wstring, size_t> cached;
            for (size_t i = 0; i < pool.size(); ++i) cached.emplace(pool[i].path, i);
            std::vector<Item> refreshed;
            for (const auto& path : storage.Entries()) {
                Item item; item.path = path; item.label = box::BaseName(path);
                if (box::IsLink(path)) item.label.resize(item.label.find_last_of(L'.'));
                item.manualOrder = GetPrivateProfileIntW(L"Order", box::BaseName(path).c_str(), INT_MAX, storage.ini.c_str());
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
                    item.modified = (static_cast<ULONGLONG>(attributes.ftLastWriteTime.dwHighDateTime) << 32) | attributes.ftLastWriteTime.dwLowDateTime;
                const auto found = cached.find(path); if (found != cached.end()) item.icon = std::move(pool[found->second].icon);
                refreshed.push_back(std::move(item));
            }
            pool = std::move(refreshed);
        }
        sortMode_ = static_cast<box::SortMode>(std::clamp(storage.ReadInt(L"SortMode", 0), 0, 2));
        std::sort(pool.begin(), pool.end(), [this](const Item& a, const Item& b) { return ItemLess(a, b); });
        totalItems_ = pool.size();
        for (auto& item : pool) {
            if (box::MatchesSearch(box::BaseName(item.path), searchQuery_)) items_.push_back(std::move(item));
            else { item.icon = Icon(); hiddenItems_.push_back(std::move(item)); }
        }
        for (const auto& item : items_) requestedIcons_.push_back(item.icon.value != nullptr);
        hovered_ = -1; RestoreSelection(selected, focus, anchor);
        if (resetScroll) firstRow_ = 0;
        firstRow_ = std::clamp(firstRow_, 0, MaxRow());
        RequestAnchorPaint();
        if (panelVisible_) { PositionPanel(); QueueVisibleIcons(); StartMotion(std::move(previous)); InvalidateRect(panel, nullptr, FALSE); }
        else StopMotion();
    }
    void SaveOrder() {
        std::vector<Item*> order;
        for (auto& item : items_) order.push_back(&item);
        for (auto& item : hiddenItems_) order.push_back(&item);
        std::sort(order.begin(), order.end(), [this](const Item* a, const Item* b) { return ItemLess(*a, *b); });
        size_t visible = 0;
        for (auto& item : order) if (box::MatchesSearch(box::BaseName(item->path), searchQuery_)) item = &items_[visible++];
        for (size_t i = 0; i < order.size(); ++i) {
            order[i]->manualOrder = static_cast<int>(i);
            if (!WritePrivateProfileStringW(L"Order", box::BaseName(order[i]->path).c_str(), std::to_wstring(i).c_str(), storage.ini.c_str()))
                throw HRESULT_FROM_WIN32(GetLastError());
        }
    }
    void SavePosition() {
        RECT rect{}; GetWindowRect(host, &rect);
        storage.Write(L"X", std::to_wstring(rect.left)); storage.Write(L"Y", std::to_wstring(rect.top));
    }
    void PositionPanel() {
        RECT anchor{}; GetWindowRect(host, &anchor);
        const auto work = WorkArea(host);
        const int desiredRows = searchQuery_.empty() ? Rows() : static_cast<int>((totalItems_ + 3) / 4);
        const int desiredHeight = Px(88 + std::clamp(desiredRows, 2, 3) * 110);
        const auto placement = box::PlacePanel(anchor, work, Px(416), desiredHeight, Px(10), direction_);
        SetWindowPos(panel, HWND_TOPMOST, placement.rect.left, placement.rect.top,
                     placement.rect.right - placement.rect.left, placement.rect.bottom - placement.rect.top, SWP_NOACTIVATE);
        firstRow_ = std::clamp(firstRow_, 0, MaxRow());
        UpdateTooltips();
        if (panelVisible_) PrepareBackdrop();
    }
    void PrepareBackdrop() {
        if (backgroundMode_ != 0 || !panel || !glassTransparency_) {
            backdropLoader_.Stop(); ++backdropGeneration_; backdropPending_ = false; pendingBlur_ = -1;
            recentBackdrop.Clear();
            backdrop_.Clear(); customGlass_ = panel && backgroundMode_ == 0;
            Round(panel); return;
        }
        RECT rect{}; GetWindowRect(panel, &rect);
        if (!backdrop_.Matches(rect)) {
            backdropLoader_.Cancel(); ++backdropGeneration_; backdropPending_ = false; pendingBlur_ = -1;
            if (!recentBackdrop.Take(rect, backdrop_)) {
                const bool visible = IsWindowVisible(panel) != FALSE;
                if (visible) { ShowWindow(panel, SW_HIDE); NoteBackdropHide(owner_ ? owner_ : host); }
                // Clear the previous composed frame before a genuinely new capture.
                FlushBackdropHide();
                backdrop_.Clear(); backdrop_.Capture(rect);
                if (visible) ShowWindow(panel, SW_SHOWNOACTIVATE);
            }
        }
        const int strength = std::clamp(Px(glassBlur_), 0, 120);
        customGlass_ = backdrop_.HasPixels();
        if (backdrop_.Matches(rect) && !backdrop_.Ready(strength) && (!backdropPending_ || pendingBlur_ != strength)) {
            backdropLoader_.Start(host, kBackdropReady);
            backdropPending_ = true; pendingBlur_ = strength; ++backdropGeneration_;
            backdropLoader_.RequestBlur(backdrop_, strength, backdropGeneration_);
        }
        Round(panel);
    }
    void ApplyAppearance() {
        DWORD light = 1, bytes = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
        dark_ = backgroundMode_ == 1 || (backgroundMode_ != 2 && light == 0);
        if (panelVisible_) PrepareBackdrop();
        Round(panel); InvalidateRect(panel, nullptr, FALSE);
    }
    void UpdateAppearanceDialog(HWND dialog) {
        wchar_t value[32]{}; swprintf_s(value, L"%d%%", glassTransparency_); SetDlgItemTextW(dialog, 1104, value);
        swprintf_s(value, L"%d", glassBlur_); SetDlgItemTextW(dialog, 1105, value);
        settings_->SetGlassEnabled(backgroundMode_ == 0);
        settings_->SetIcon(folderIcon_.value, previewIconFile_.empty() ? L"Windows 文件夹" : previewIconFile_);
    }
    static INT_PTR CALLBACK AppearanceProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            app = reinterpret_cast<App*>(l); SetWindowLongPtrW(dialog, DWLP_USER, l);
            app->settings_->Initialize(dialog);
            SetWindowTextW(dialog, app->globalSettings_ ? L"全局设置" : L"收纳盒设置");
            SetDlgItemTextW(dialog, 1120, app->globalSettings_ ? L"所有收纳盒外观" : L"当前收纳盒外观");
            SetDlgItemTextW(dialog, 1121, app->globalSettings_ ? L"确定后应用于全部收纳盒，新建时沿用" : L"自定义当前收纳盒的背景和图标");
            SetDlgItemTextW(dialog, 1132, app->globalSettings_ ? L"所有收纳盒图标" : L"当前收纳盒图标");
            for (const auto* label : {L"毛玻璃", L"暗黑色", L"白色", L"跟随系统"}) SendDlgItemMessageW(dialog, 1101, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendDlgItemMessageW(dialog, 1101, CB_SETCURSEL, app->backgroundMode_, 0);
            SendDlgItemMessageW(dialog, 1102, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendDlgItemMessageW(dialog, 1103, TBM_SETRANGE, TRUE, MAKELPARAM(0, 40));
            SendDlgItemMessageW(dialog, 1102, TBM_SETPOS, TRUE, app->glassTransparency_);
            SendDlgItemMessageW(dialog, 1103, TBM_SETPOS, TRUE, app->glassBlur_);
            app->UpdateAppearanceDialog(dialog); return TRUE;
        }
        if (!app) return FALSE;
        INT_PTR presentation = FALSE;
        if (app->settings_->Message(message, w, l, presentation)) return presentation;
        if (message == WM_HSCROLL || (message == WM_COMMAND && LOWORD(w) == 1101 && HIWORD(w) == CBN_SELCHANGE)) {
            app->backgroundMode_ = static_cast<int>(SendDlgItemMessageW(dialog, 1101, CB_GETCURSEL, 0, 0));
            app->glassTransparency_ = static_cast<int>(SendDlgItemMessageW(dialog, 1102, TBM_GETPOS, 0, 0));
            app->glassBlur_ = static_cast<int>(SendDlgItemMessageW(dialog, 1103, TBM_GETPOS, 0, 0));
            app->UpdateAppearanceDialog(dialog);
            try { app->ApplyAppearance(); } catch (...) {}
            return TRUE;
        }
        if (message == WM_COMMAND && HIWORD(w) == BN_CLICKED) {
            try {
                switch (LOWORD(w)) {
                case 1140: app->ChooseIcon(dialog); app->UpdateAppearanceDialog(dialog); return TRUE;
                case 1141:
                    app->previewIconFile_.clear(); app->LoadAppearance(); app->UpdateAppearanceDialog(dialog); return TRUE;
                case 1142:
                    ShellExecuteW(dialog, L"open", app->storage.iconDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return TRUE;
                }
            } catch (HRESULT error) {
                MessageBoxW(dialog, ErrorText(error).c_str(), L"无法设置图标", MB_OK | MB_ICONERROR); return TRUE;
            } catch (...) {
                MessageBoxW(dialog, L"无法设置图标，请重试。", kTitle, MB_OK | MB_ICONERROR); return TRUE;
            }
        }
        if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) { EndDialog(dialog, LOWORD(w)); return TRUE; }
        if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }
    void ReloadAppearance() {
        const auto saved = AppearanceSettings::Read(storage);
        backgroundMode_ = saved.background; glassTransparency_ = saved.transparency; glassBlur_ = saved.blur;
        LoadAppearance(); ApplyAppearance();
        if (nativeMode_) nativeEntry_.Ensure(storage, name_, ExePath());
    }
    void CommitAppearance(const AppearanceSettings& value, const AppearanceCommit& globalCommit = {}) {
        if (globalCommit) { globalCommit(value); return; }
        value.Save(storage); ReloadAppearance();
        if (owner_) PostMessageW(owner_, kShellIconChanged, 0, reinterpret_cast<LPARAM>(this));
    }
    void ChooseAppearance(const AppearanceCommit& globalCommit = {}, const AppearanceSettings* initial = nullptr) {
        if (settings_) return;
        if (!ShowPanel()) return;
        FlagGuard dialogGuard(inDialog_);
        FlagGuard globalGuard(globalSettings_); globalSettings_ = static_cast<bool>(globalCommit);
        const int mode = backgroundMode_, transparency = glassTransparency_, blur = glassBlur_;
        if (initial) {
            backgroundMode_ = initial->background; glassTransparency_ = initial->transparency; glassBlur_ = initial->blur;
        }
        previewIconFile_ = initial ? initial->icon : storage.Read(L"IconFile");
        settings_ = std::make_unique<box::FluentSettings>();
        LoadAppearance(); ApplyAppearance();
        const INT_PTR choice = DialogBoxParamW(instance_, MAKEINTRESOURCEW(102), OwnerWindow(), AppearanceProc, reinterpret_cast<LPARAM>(this));
        const DWORD creationError = choice == -1 ? GetLastError() : ERROR_SUCCESS;
        settings_.reset();
        try {
            if (choice == IDOK) CommitAppearance({backgroundMode_, glassTransparency_, glassBlur_, previewIconFile_}, globalCommit);
            else { backgroundMode_ = mode; glassTransparency_ = transparency; glassBlur_ = blur; }
        } catch (...) {
            previewIconFile_.clear();
            const auto saved = AppearanceSettings::Read(storage);
            backgroundMode_ = saved.background; glassTransparency_ = saved.transparency; glassBlur_ = saved.blur;
            LoadAppearance(); ApplyAppearance(); dialogGuard.Restore(); ScheduleClose(); throw;
        }
        previewIconFile_.clear(); LoadAppearance(); ApplyAppearance();
        dialogGuard.Restore(); ScheduleClose();
        if (choice == -1) throw HRESULT_FROM_WIN32(creationError ? creationError : ERROR_INVALID_WINDOW_HANDLE);
    }
    void ClearTooltips() {
        if (!tooltip_) return;
        SendMessageW(tooltip_, TTM_POP, 0, 0);
        for (int index = 0; index < tooltipCount_; ++index) {
            TOOLINFOW tool{}; tool.cbSize = TTTOOLINFOW_V2_SIZE; tool.hwnd = panel; tool.uId = index + 1;
            SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        tooltipCount_ = 0;
    }
    void UpdateTooltips() {
        ClearTooltips(); if (!tooltip_) return;
        const int start = firstRow_ * 4, end = std::min(static_cast<int>(items_.size()), start + VisibleRows() * 4);
        for (int index = start; index < end; ++index) {
            TOOLINFOW tool{}; tool.cbSize = TTTOOLINFOW_V2_SIZE; tool.hwnd = panel; tool.uId = ++tooltipCount_;
            tool.uFlags = TTF_SUBCLASS; tool.rect = ItemRect(index); tool.lpszText = items_[index].label.data();
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    void Reorder(int from, int to) {
        if (from < 0 || to < 0 || from >= static_cast<int>(items_.size()) || to >= static_cast<int>(items_.size()) || from == to) return;
        selection_.Click(from); ReorderSelected(to);
    }
    void ReorderSelected(int target) {
        auto previous = CaptureCards();
        const auto order = box::ReorderSelection(items_.size(), selection_.Indices(), target);
        bool changed = false;
        for (size_t i = 0; i < order.size(); ++i) if (order[i] != static_cast<int>(i)) { changed = true; break; }
        if (!changed) return;
        const auto selected = SelectedPaths();
        const auto focus = FocusPath(selection_.Focus()), anchor = FocusPath(selection_.Anchor());
        std::vector<Item> arranged; arranged.reserve(items_.size());
        for (int index : order) arranged.push_back(std::move(items_[index]));
        items_ = std::move(arranged); RestoreSelection(selected, focus, anchor);
        SaveOrder(); sortMode_ = box::SortMode::Manual; storage.Write(L"SortMode", L"0"); Reload(std::move(previous));
    }
    void SortMenu() {
        FlagGuard guard(inMenu_); CancelClose();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (sortMode_ == box::SortMode::Manual ? MF_CHECKED : 0), kSortManual, L"手动排序");
        AppendMenuW(menu, MF_STRING | (sortMode_ == box::SortMode::Modified ? MF_CHECKED : 0), kSortModified, L"按修改时间（最新在前）");
        AppendMenuW(menu, MF_STRING | (sortMode_ == box::SortMode::Name ? MF_CHECKED : 0), kSortName, L"按名称（A–Z）");
        const RECT button = SortRect(); POINT point{button.left, button.bottom + Px(5)}; ClientToScreen(panel, &point);
        SetForegroundWindow(panel);
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, panel, nullptr);
        DestroyMenu(menu); PostMessageW(panel, WM_NULL, 0, 0);
        if (choice >= kSortManual && choice <= kSortName) {
            Command(choice);
        }
        guard.Restore(); ScheduleClose();
    }
    void BeginItemDrag(int index) {
        if (index < 0 || index >= static_cast<int>(items_.size())) return;
        if (!selection_.Contains(index)) selection_.Click(index);
        const auto paths = SelectedPaths(); itemPressed_ = false; collapseOnRelease_ = toggleOnRelease_ = false;
        ReleaseCapture(); ClearTooltips();
        FlagGuard dialogGuard(inDialog_); CancelClose(); draggingPaths_ = paths;
        ComPtr<IDataObject> data;
        const HRESULT result = box::ShellSelectionObject(OwnerWindow(), paths, IID_IDataObject, reinterpret_cast<void**>(data.put()));
        if (FAILED(result)) { draggingPaths_.clear(); Check(result); }
        auto* source = new DragSource; DWORD effect = 0;
        DoDragDrop(data.get(), source, DROPEFFECT_MOVE, &effect); source->Release();
        draggingPaths_.clear(); for (const auto& path : paths) storage.ForgetMovedOut(path); Reload();
        dialogGuard.Restore(); ScheduleClose();
    }
    void MakeOpaque(RECT rect, bool flush = true) const {
        if (!paintPixels_) return;
        if (flush) GdiFlush();
        for (int y = std::max(0L, rect.top); y < std::min(static_cast<LONG>(paintHeight_), rect.bottom); ++y)
            for (int x = std::max(0L, rect.left); x < std::min(static_cast<LONG>(paintWidth_), rect.right); ++x) {
                DWORD& pixel = paintPixels_[y * paintWidth_ + x];
                if ((pixel & 0x00FFFFFF) && (pixel >> 24) <= 1) pixel |= 0xFF000000;
            }
    }
    void FillRound(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) const {
        HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
        auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Px(radius), Px(radius));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen); DeleteObject(brush);
        MakeOpaque(rect);
    }
    void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
              UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, bool shadow = false) const {
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0 || !paintPixels_ || text.empty()) return;
        const auto* glyph = textMasks_.Get(dc, text, font, width, height, flags);
        if (!glyph) return;
        auto coverage = [&](int x, int y) { return glyph->At(x, y); };
        auto blend = [&](int x, int y, unsigned alpha, COLORREF ink) {
            x += rect.left; y += rect.top;
            if (!alpha || x < 0 || x >= paintWidth_ || y < 0 || y >= paintHeight_) return;
            DWORD& pixel = paintPixels_[y * paintWidth_ + x];
            const unsigned inverse = 255 - alpha;
            const unsigned a = alpha + ((pixel >> 24) * inverse + 127) / 255;
            const unsigned r = (GetRValue(ink) * alpha + ((pixel >> 16) & 255) * inverse + 127) / 255;
            const unsigned g = (GetGValue(ink) * alpha + ((pixel >> 8) & 255) * inverse + 127) / 255;
            const unsigned b = (GetBValue(ink) * alpha + (pixel & 255) * inverse + 127) / 255;
            pixel = (a << 24) | (r << 16) | (g << 8) | b;
        };
        if (shadow) {
            // A light, soft shadow from the same glyph mask, rather than a second offset text drawing.
            const int offset = std::max(1, Px(1));
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                unsigned sum = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                    sum += coverage(x + dx, y + dy - offset) * (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1);
                blend(x, y, sum * 96 / (16 * 255), RGB(0, 0, 0));
            }
        }
        for (LONG y = glyph->ink.top; y < glyph->ink.bottom; ++y)
            for (LONG x = glyph->ink.left; x < glyph->ink.right; ++x) blend(x, y, coverage(x, y), color);
        textMasks_.ReleaseTransient();
    }
    void DrawItemIcon(HDC dc, RECT rect, HICON icon) const {
        DrawIconEx(dc, rect.left, rect.top, icon, rect.right - rect.left, rect.bottom - rect.top, 0, nullptr, DI_NORMAL);
        MakeOpaque(rect); // Legacy icons may have no alpha; never run this over antialiased text.
    }
    void DrawCard(HDC dc, const std::wstring& label, HICON icon, RECT tile, bool selected, bool hovered, bool focus, double opacity = 1.) const {
        if (opacity <= .001) return;
        RECT blend{}; const RECT viewport = ContentRect();
        const bool fade = opacity < .999 && IntersectRect(&blend, &tile, &viewport);
        if (fade) {
            GdiFlush(); blendScratch_.resize(static_cast<size_t>(blend.right - blend.left) * (blend.bottom - blend.top));
            size_t offset = 0;
            for (LONG y = blend.top; y < blend.bottom; ++y) for (LONG x = blend.left; x < blend.right; ++x)
                blendScratch_[offset++] = paintPixels_[y * paintWidth_ + x];
        }
        if (selected || hovered) {
            const COLORREF fill = dark_ ? (selected ? RGB(49, 75, 111) : RGB(47, 58, 77)) :
                                          (selected ? RGB(218, 233, 255) : RGB(236, 242, 250));
            FillRound(dc, tile, fill, selected ? RGB(109, 163, 237) : fill, 12);
        }
        const int iconX = tile.left + (tile.right - tile.left - Px(36)) / 2;
        DrawItemIcon(dc, {iconX, tile.top + Px(9), iconX + Px(36), tile.top + Px(45)}, icon ? icon : placeholder_.value);
        Text(dc, label, {tile.left + Px(3), tile.top + Px(50), tile.right - Px(3), tile.bottom - Px(6)}, bodyFont_, Foreground(), DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        if (selected) {
            const RECT check{tile.left + Px(4), tile.top + Px(4), tile.left + Px(17), tile.top + Px(17)};
            FillRound(dc, check, RGB(38, 119, 232), RGB(38, 119, 232), 4);
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(1)), RGB(255, 255, 255)); auto oldPen = SelectObject(dc, pen);
            MoveToEx(dc, check.left + Px(3), check.top + Px(6), nullptr);
            LineTo(dc, check.left + Px(5), check.top + Px(9)); LineTo(dc, check.left + Px(10), check.top + Px(3));
            SelectObject(dc, oldPen); DeleteObject(pen); MakeOpaque(check);
        }
        if (focus) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(69, 132, 220)); auto oldPen = SelectObject(dc, pen);
            auto oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            RoundRect(dc, tile.left, tile.top, tile.right, tile.bottom, Px(12), Px(12));
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen); MakeOpaque(tile);
        }
        if (fade) {
            GdiFlush(); size_t offset = 0; const unsigned alpha = static_cast<unsigned>(opacity * 255.);
            for (LONG y = blend.top; y < blend.bottom; ++y) for (LONG x = blend.left; x < blend.right; ++x) {
                DWORD& pixel = paintPixels_[y * paintWidth_ + x]; const DWORD below = blendScratch_[offset++];
                const unsigned r = (((pixel >> 16) & 255) * alpha + ((below >> 16) & 255) * (255 - alpha)) / 255;
                const unsigned g = (((pixel >> 8) & 255) * alpha + ((below >> 8) & 255) * (255 - alpha)) / 255;
                const unsigned b = ((pixel & 255) * alpha + (below & 255) * (255 - alpha)) / 255;
                pixel = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    }
    HRESULT RenderSurface(HWND hwnd, bool isPanel, HDC target, std::vector<DWORD>* anchorSnapshot = nullptr) {
        RECT rect{}; GetClientRect(hwnd, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return E_INVALIDARG;
        box::DibSurface anchorSurface;
        auto& surface = isPanel ? panelSurface_ : anchorSurface;
        if (!surface.Ensure(target, rect.right, rect.bottom)) return E_OUTOFMEMORY;
        HDC dc = surface.DC();
        paintPixels_ = surface.Pixels(); paintWidth_ = rect.right; paintHeight_ = rect.bottom;
        if (!isPanel) {
            std::fill_n(paintPixels_, static_cast<size_t>(rect.right) * rect.bottom, 0x01000000u);
            DrawItemIcon(dc, AnchorIconRect(), folderIcon_.value);
            Text(dc, name_, AnchorLabelRect(), desktopFont_, RGB(255, 255, 255), DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS, true);
            if (totalItems_) {
                RECT icon = AnchorIconRect(); RECT badge{icon.right - Px(12), Px(2), icon.right + Px(10), Px(21)};
                FillRound(dc, badge, RGB(47, 111, 235), RGB(47, 111, 235), 12);
                Text(dc, totalItems_ > 99 ? L"99+" : std::to_wstring(totalItems_), badge, smallFont_, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        } else {
            if (!(backgroundMode_ == 0 && customGlass_ && backdrop_.Draw(paintPixels_, rect.right, rect.bottom, glassTransparency_, Background()))) {
                const COLORREF background = Background();
                const DWORD pixel = acrylic_ ? 0 : 0xFF000000 | (static_cast<DWORD>(GetRValue(background)) << 16) |
                    (static_cast<DWORD>(GetGValue(background)) << 8) | GetBValue(background);
                std::fill_n(paintPixels_, static_cast<size_t>(rect.right) * rect.bottom, pixel);
            }
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(HOLLOW_BRUSH)));
            HPEN border = CreatePen(PS_SOLID, 1, dragIncoming ? RGB(51, 120, 242) : Border()); HPEN oldPen = static_cast<HPEN>(SelectObject(dc, border));
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Px(18), Px(18));
            SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(border); GdiFlush();
            const LONG edge = Px(18);
            MakeOpaque({0, 0, rect.right, edge}, false); MakeOpaque({0, rect.bottom - edge, rect.right, rect.bottom}, false);
            MakeOpaque({0, edge, edge, rect.bottom - edge}, false); MakeOpaque({rect.right - edge, edge, rect.right, rect.bottom - edge}, false);
            DrawItemIcon(dc, {Px(19), Px(20), Px(47), Px(48)}, folderIcon_.value);
            const RECT search = SearchRect();
            Text(dc, name_, {Px(58), Px(16), search.left - Px(8), Px(39)}, titleFont_, Foreground());
            const auto count = selection_.Count() ? L"选 " + std::to_wstring(selection_.Count()) + L" / " + std::to_wstring(items_.size()) :
                               searchQuery_.empty() ? std::to_wstring(totalItems_) + L" 个项目" :
                               std::to_wstring(items_.size()) + L" / " + std::to_wstring(totalItems_);
            Text(dc, count, {Px(58), Px(39), search.left - Px(8), Px(60)}, smallFont_, Muted());
            FillRound(dc, search, SearchFill(), GetFocus() == searchEdit_ ? RGB(38, 119, 232) : Border(), 8);
            Text(dc, searchQuery_.empty() ? L"搜索" : searchQuery_, {search.left + Px(8), search.top, search.right - Px(23), search.bottom},
                 bodyFont_, searchQuery_.empty() ? Muted() : Foreground());
            if (!searchQuery_.empty()) {
                HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(1)), Muted()); auto oldPen = SelectObject(dc, pen);
                MoveToEx(dc, search.right - Px(15), search.top + Px(10), nullptr); LineTo(dc, search.right - Px(9), search.top + Px(16));
                MoveToEx(dc, search.right - Px(9), search.top + Px(10), nullptr); LineTo(dc, search.right - Px(15), search.top + Px(16));
                SelectObject(dc, oldPen); DeleteObject(pen); MakeOpaque(search);
            }
            const RECT sort = SortRect();
            const COLORREF sortFill = dark_ ? (sortHovered_ || sortPressed_ ? RGB(52, 64, 82) : RGB(42, 50, 65)) :
                                             (sortHovered_ || sortPressed_ ? RGB(224, 234, 247) : RGB(240, 244, 248));
            FillRound(dc, sort, sortFill, sortFill, 10);
            Text(dc, sortMode_ == box::SortMode::Modified ? L"时间 ▾" : sortMode_ == box::SortMode::Name ? L"A–Z ▾" : L"排序 ▾",
                 sort, smallFont_, Foreground(), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            const RECT add = AddRect();
            FillRound(dc, add, dark_ ? RGB(40, 58, 85) : RGB(232, 240, 255), dark_ ? RGB(40, 58, 85) : RGB(232, 240, 255), 10);
            Text(dc, L"＋ 添加", add, smallFont_, dark_ ? RGB(146, 183, 255) : RGB(36, 97, 211), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            if (items_.empty() && ghostCards_.empty()) {
                const int center = (rect.bottom + Px(55)) / 2;
                DrawItemIcon(dc, {(rect.right - Px(48)) / 2, center - Px(58), (rect.right + Px(48)) / 2, center - Px(10)}, folderIcon_.value);
                Text(dc, searchQuery_.empty() ? L"把常用项目拖到这里" : L"没有找到匹配的项目", {Px(15), center, rect.right - Px(15), center + Px(25)}, bodyFont_, Foreground(), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                Text(dc, searchQuery_.empty() ? L"文件 · 文件夹 · 快捷方式" : L"换个关键词，或清空搜索", {Px(15), center + Px(28), rect.right - Px(15), center + Px(47)}, smallFont_, Muted(), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            } else {
                const int start = firstRow_ * 4, end = std::min(static_cast<int>(items_.size()), start + VisibleRows() * 4);
                const RECT viewport = ContentRect(); const int saved = SaveDC(dc);
                IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
                const double progress = MotionProgress();
                for (const auto& ghost : ghostCards_) {
                    RECT tile = ghost.rect; OffsetRect(&tile, 0, -static_cast<int>(Px(8) * progress));
                    DrawCard(dc, ghost.label, ghost.icon.value, tile, ghost.selected, false, false, ghost.alpha * (1. - progress));
                }
                for (int i = start; i < end; ++i) {
                    double alpha = 1.;
                    for (const auto& motion : cardMotion_) if (motion.index == i) alpha = motion.alphaFrom + (1. - motion.alphaFrom) * progress;
                    DrawCard(dc, items_[i].label, items_[i].icon.value, DisplayRect(i), selection_.Contains(i),
                             i == hovered_, GetFocus() == panel && selection_.Focus() == i, alpha);
                }
                RestoreDC(dc, saved);
            }
            if (marquee_ && marqueeMoved_) {
                RECT outline = marqueeRect_, fill{}, viewport = ContentRect(); OffsetRect(&outline, 0, -firstRow_ * Px(110));
                if (IntersectRect(&fill, &outline, &viewport)) {
                    GdiFlush();
                    for (LONG y = fill.top; y < fill.bottom; ++y) for (LONG x = fill.left; x < fill.right; ++x) {
                        DWORD& pixel = paintPixels_[y * paintWidth_ + x];
                        const unsigned r = (((pixel >> 16) & 255) * 219 + 38 * 36) / 255;
                        const unsigned g = (((pixel >> 8) & 255) * 219 + 119 * 36) / 255;
                        const unsigned b = ((pixel & 255) * 219 + 232 * 36) / 255;
                        pixel = 0xFF000000 | (r << 16) | (g << 8) | b;
                    }
                    const int saved = SaveDC(dc); IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
                    HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(1)), RGB(38, 119, 232)); SelectObject(dc, pen);
                    SelectObject(dc, GetStockObject(HOLLOW_BRUSH)); Rectangle(dc, outline.left, outline.top, outline.right, outline.bottom);
                    RestoreDC(dc, saved); DeleteObject(pen); MakeOpaque(fill);
                }
            }
        }
        GdiFlush();
        HRESULT result = S_OK;
        if (anchorSnapshot) anchorSnapshot->assign(paintPixels_, paintPixels_ + static_cast<size_t>(rect.right) * rect.bottom);
        if (isPanel) {
            if (!BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY)) result = E_FAIL;
        }
        else {
            POINT origin{}; SIZE size{rect.right, rect.bottom};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            if (!UpdateLayeredWindow(hwnd, target, nullptr, &size, dc, &origin, 0, &blend, ULW_ALPHA)) {
                const DWORD error = GetLastError(); result = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
        }
        paintPixels_ = nullptr;
        return result;
    }
    HRESULT RenderAnchor(std::vector<DWORD>* snapshot = nullptr) {
        if (!host || renderingAnchor_) return S_FALSE;
        FlagGuard rendering(renderingAnchor_);
        HDC screen = GetDC(nullptr);
        if (!screen) return E_FAIL;
        const HRESULT result = RenderSurface(host, false, screen, snapshot);
        ReleaseDC(nullptr, screen);
        return result;
    }
    void Paint(HWND hwnd, bool isPanel) {
        PAINTSTRUCT paint{}; HDC target = BeginPaint(hwnd, &paint);
        if (isPanel) RenderSurface(hwnd, true, target);
        EndPaint(hwnd, &paint);
        if (!isPanel) RequestAnchorPaint();
    }
    void OpenItem(int index) {
        if (index < 0 || index >= static_cast<int>(items_.size())) return;
        OpenPaths({items_[index].path});
    }
    void OpenPaths(const std::vector<std::wstring>& paths) {
        if (paths.empty()) return;
        FlagGuard guard(inDialog_); HidePanel();
        for (const auto& path : paths) {
            SHELLEXECUTEINFOW execute{}; execute.cbSize = sizeof(execute); execute.hwnd = OwnerWindow();
            execute.fMask = SEE_MASK_FLAG_NO_UI; execute.lpVerb = L"open"; execute.lpFile = path.c_str(); execute.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&execute)) Report(L"无法打开项目，请检查目标文件是否仍然存在", HRESULT_FROM_WIN32(GetLastError()));
        }
    }
    void ChooseItems() {
        FlagGuard dialogGuard(inDialog_); HidePanel();
        const auto selected = PickItems(OwnerWindow());
        if (!selected.empty()) AddPaths(selected);
        dialogGuard.Restore(); ShowPanel();
    }
    void MoveSelectedOut() {
        auto paths = SelectedPaths();
        if (paths.empty() && contextItem_ >= 0 && contextItem_ < static_cast<int>(items_.size())) paths.push_back(items_[contextItem_].path);
        if (paths.empty()) return;
        FlagGuard dialogGuard(inDialog_); HidePanel();
        const auto selected = PickFolder(OwnerWindow(), L"选择移出后的目标文件夹");
        if (!selected.empty()) {
            try { for (const auto& path : paths) if (!storage.MoveOut(path, selected, OwnerWindow()).complete) break; }
            catch (...) { Reload(); throw; }
            Reload();
        }
    }
    void Rename() {
        FlagGuard dialogGuard(inDialog_); HidePanel();
        if (nativeMode_ && nativeEntry_.BeginRename()) return;
        auto value = AskName(instance_, OwnerWindow(), name_);
        if (!value.empty()) {
            storage.Write(L"Name", value); name_ = value;
            SetWindowTextW(host, (name_ + L" · 桌面文件盒").c_str());
            SetWindowTextW(panel, (name_ + L" · 内容").c_str());
            RequestAnchorPaint();
            if (nativeMode_) { nativeEntry_.Ensure(storage, name_, ExePath(), L"", true); UpdateNativeBounds(); }
        }
    }
    void CreateSibling() {
        FlagGuard dialogGuard(inDialog_); HidePanel();
        auto value = AskName(instance_, OwnerWindow(), L"新收纳盒");
        if (!value.empty() && createBox_) {
            RECT rect{}; GetWindowRect(host, &rect); RECT work = WorkArea(host);
            POINT point{rect.left, rect.bottom + Px(18)};
            if (point.y + cell_.cy > work.bottom) point = {rect.right + Px(18), work.top + Px(36)};
            createBox_(value, point);
        }
    }
    void DeleteCurrent() {
        FlagGuard dialogGuard(inDialog_); HidePanel();
        const auto entries = storage.Entries();
        auto prompt = L"删除“" + name_ + L"”？\n\n将先把 " + std::to_wstring(entries.size()) + L" 个项目移回拖入前的位置。\n全部归还成功后才会删除收纳盒，同名文件保留两份。";
        if (MessageBoxW(OwnerWindow(), prompt.c_str(), L"删除收纳盒并归还内容", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
        bool unknown = false;
        for (const auto& path : entries) if (storage.OriginalPath(path).empty()) { unknown = true; break; }
        std::wstring fallback;
        if (unknown) {
            MessageBoxW(OwnerWindow(), L"部分项目没有原位置记录（例如从资源管理器直接放入的项目）。请选择这些项目的归还文件夹，其余项目仍会回到各自原处。", kTitle, MB_OK | MB_ICONINFORMATION);
            fallback = PickFolder(OwnerWindow(), L"选择无原位置记录项目的归还文件夹");
            if (fallback.empty()) return;
        }
        box::MoveResult restored;
        try { restored = storage.RestoreAll(OwnerWindow(), fallback); }
        catch (...) { Reload(); throw; }
        Reload();
        if (!restored.complete) {
            MessageBoxW(OwnerWindow(), L"部分项目未能归还，收纳盒已保留。请处理文件占用或权限问题后重试。", kTitle, MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (nativeMode_) nativeEntry_.Remove(storage);
        storage.Write(L"Deleted", L"1");
        removing_ = true; HidePanel(); ShowWindow(host, SW_HIDE);
        if (deleteBox_) deleteBox_(this);
    }
    void ChooseIcon(HWND settingsDialog) {
        FlagGuard dialogGuard(inDialog_);
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = settingsDialog;
        dialog.lpstrFilter = L"Windows 图标 (*.ico)\0*.ico\0"; dialog.lpstrFile = path; dialog.nMaxFile = 32768;
        dialog.lpstrTitle = L"选择 ICO 图标（外部图标会存入图标库）";
        dialog.lpstrInitialDir = storage.iconDirectory.c_str();
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
        if (GetOpenFileNameW(&dialog)) { previewIconFile_ = storage.ImportIcon(path); LoadAppearance(); }
    }
    void RenameFile(const std::wstring& path) {
        FlagGuard guard(inDialog_);
        const auto name = AskName(instance_, OwnerWindow(), box::BaseName(path), L"重命名", 255);
        if (name.empty() || name == box::BaseName(path)) return;
        ComPtr<IFileOperation> operation; ComPtr<IShellItem> item;
        Check(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOperation, reinterpret_cast<void**>(operation.put())));
        Check(operation->SetOwnerWindow(OwnerWindow())); Check(operation->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD));
        Check(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.put())));
        Check(operation->RenameItem(item.get(), name.c_str(), nullptr)); Check(operation->PerformOperations());
        Reload();
    }
    void ContextMenu(POINT screen, int item = -1) {
        FlagGuard menuGuard(inMenu_); CancelClose(); contextItem_ = item;
        EndMarquee(false);
        if (item >= 0 && item < static_cast<int>(items_.size())) {
            SetFocus(panel);
            if (!selection_.Contains(item)) selection_.Click(item);
            SelectionChanged(); const auto paths = SelectedPaths();
            std::function<void()> rename;
            if (paths.size() == 1) rename = [this, path = paths.front()] { RenameFile(path); };
            shellMenu_.Show(panel, paths, screen, rename);
            Reload(); menuGuard.Restore(); ScheduleClose(); return;
        }
        HMENU menu = CreatePopupMenu();
        if (item >= 0) {
            AppendMenuW(menu, MF_STRING, kOpen, L"打开");
            AppendMenuW(menu, MF_STRING, kReveal, L"打开项目所在位置");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kRemove, L"移出到指定文件夹…");
        } else {
            AppendMenuW(menu, MF_STRING, kNewBox, L"新建收纳盒…");
            AppendMenuW(menu, MF_STRING, kRenameBox, L"重命名收纳盒…");
            AppendMenuW(menu, MF_STRING, kDeleteBox, L"删除当前收纳盒并归还内容…");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kAddItems, L"移入文件或文件夹…");
            AppendMenuW(menu, MF_STRING, kOpenData, L"打开收纳目录");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kAppearance, L"设置（当前收纳盒）…");
            HMENU directions = CreatePopupMenu();
            AppendMenuW(directions, MF_STRING | (direction_ == box::Direction::Automatic ? MF_CHECKED : 0), kDirectionAuto, L"自动选择方向");
            AppendMenuW(directions, MF_STRING | (direction_ == box::Direction::Left ? MF_CHECKED : 0), kDirectionLeft, L"优先向左展开");
            AppendMenuW(directions, MF_STRING | (direction_ == box::Direction::Right ? MF_CHECKED : 0), kDirectionRight, L"优先向右展开");
            AppendMenuW(directions, MF_STRING | (direction_ == box::Direction::Up ? MF_CHECKED : 0), kDirectionUp, L"优先向上展开");
            AppendMenuW(directions, MF_STRING | (direction_ == box::Direction::Down ? MF_CHECKED : 0), kDirectionDown, L"优先向下展开");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(directions), L"展开方向");
            bool startup = false; try { StartupTask task; startup = task.Enabled(); } catch (...) {}
            AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : 0), kStartup, L"登录后自动启动");
            AppendMenuW(menu, MF_STRING, kRefresh, L"刷新内容");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kExit, L"退出");
        }
        const HWND menuOwner = panelVisible_ ? panel : owner_ ? owner_ : host;
        SetForegroundWindow(menuOwner);
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, menuOwner, nullptr);
        DestroyMenu(menu); PostMessageW(menuOwner, WM_NULL, 0, 0);
        if (choice) Command(choice);
        menuGuard.Restore(); ScheduleClose();
    }
    void Command(UINT command) {
        switch (command) {
        case kAddItems: ChooseItems(); break;
        case kOpenData: ShellExecuteW(host, L"open", storage.items.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kAppearance: ChooseAppearance(); break;
        case kSortManual: case kSortModified: case kSortName:
            storage.Write(L"SortMode", std::to_wstring(command - kSortManual)); RebuildView(true, {}, true); break;
        case kNewBox: CreateSibling(); break;
        case kRenameBox: Rename(); break;
        case kDeleteBox: DeleteCurrent(); break;
        case kDirectionAuto: case kDirectionLeft: case kDirectionRight: case kDirectionUp: case kDirectionDown:
            SetDirection(static_cast<box::Direction>(command - kDirectionAuto)); break;
        case kStartup: { StartupTask task; task.Set(!task.Enabled(), box::ParentPath(storage.journalDirectory)); break; }
        case kRefresh: Reload(); break;
        case kExit: PostQuitMessage(0); break;
        case kOpen: OpenItem(contextItem_); break;
        case kReveal:
            if (contextItem_ >= 0 && contextItem_ < static_cast<int>(items_.size())) {
                const auto arguments = L"/select,\"" + items_[contextItem_].path + L"\"";
                ShellExecuteW(host, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        case kRemove: MoveSelectedOut(); break;
        }
    }
    bool PointerInside(POINT point) const {
        RECT anchor = nativeImage_; if (!nativeMode_) GetWindowRect(host, &anchor);
        if (PtInRect(&anchor, point)) return true;
        if (panelVisible_) {
            RECT rect{}; GetWindowRect(panel, &rect);
            if (PtInRect(&rect, point) || box::InPanelTransit(point, anchor, rect)) return true;
        }
        return false;
    }
    void CancelClose() { if (host) KillTimer(host, kCloseTimer); closeDeadline_ = 0; }
    void CloseTimer(POINT point, ULONGLONG now) {
        if (!closeDeadline_ || !panelVisible_) { CancelClose(); return; }
        if (now < closeDeadline_) { SetTimer(host, kCloseTimer, static_cast<UINT>(closeDeadline_ - now), nullptr); return; }
        CancelClose();
        if (!inMenu_ && !inDialog_ && !dragIncoming && GetCapture() != panel && !PointerInside(point)) HidePanel();
    }
    LRESULT Handle(HWND hwnd, UINT message, WPARAM w, LPARAM l, bool isPanel) {
        LRESULT menuResult = 0; if (isPanel && shellMenu_.Message(message, w, l, menuResult)) return menuResult;
        if (!isPanel && taskbarCreated_ && message == taskbarCreated_) { SetTimer(host, kDesktopTimer, 160, nullptr); return 0; }
        switch (message) {
        case kCompleteBackdropHide: FlushBackdropHide(); return 0;
        case kRenderAnchor:
            anchorPaintPending_ = false; Check(RenderAnchor()); return 0;
        case WM_PAINT: Paint(hwnd, isPanel); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: if (isPanel) LayoutSearch(); return 0;
        case WM_CTLCOLOREDIT:
            if (isPanel && reinterpret_cast<HWND>(l) == searchEdit_) {
                SetTextColor(reinterpret_cast<HDC>(w), Foreground()); SetBkColor(reinterpret_cast<HDC>(w), SearchFill());
                return reinterpret_cast<LRESULT>(searchBrush_);
            }
            break;
        case WM_COMMAND:
            if (isPanel && LOWORD(w) == kSearchControl && HIWORD(w) == EN_CHANGE) {
                const int length = GetWindowTextLengthW(searchEdit_);
                std::wstring query(static_cast<size_t>(length) + 1, L'\0');
                GetWindowTextW(searchEdit_, query.data(), length + 1); query.resize(length);
                if (query != searchQuery_) { searchQuery_ = std::move(query); RebuildView(false, {}, true); }
                return 0;
            }
            break;
        case WM_MOUSEACTIVATE: return isPanel ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            if (isPanel && marquee_) { MoveMarquee({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); return 0; }
            if (isPanel && itemPressed_) {
                POINT point{}; GetCursorPos(&point);
                if (abs(point.x - itemPressPoint_.x) >= GetSystemMetrics(SM_CXDRAG) || abs(point.y - itemPressPoint_.y) >= GetSystemMetrics(SM_CYDRAG)) { BeginItemDrag(pressedItem_); return 0; }
            }
            if (!isPanel && pressed_) {
                POINT point{}; GetCursorPos(&point);
                if (!moving_ && (abs(point.x - pressPoint_.x) >= GetSystemMetrics(SM_CXDRAG) || abs(point.y - pressPoint_.y) >= GetSystemMetrics(SM_CYDRAG))) {
                    moving_ = true; HidePanel();
                }
                if (moving_) MoveAnchor({initialPoint_.x + point.x - pressPoint_.x, initialPoint_.y + point.y - pressPoint_.y});
                return 0;
            }
            CancelClose();
            bool& tracking = isPanel ? trackingPanel_ : trackingHost_;
            if (!tracking) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track); tracking = true;
            }
            if (!isPanel && !panelVisible_ && !inMenu_ && !inDialog_) ShowPanel();
            if (isPanel) {
                const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; const RECT sort = SortRect();
                const bool overSort = PtInRect(&sort, point) != FALSE;
                if (overSort != sortHovered_) { sortHovered_ = overSort; InvalidateRect(panel, nullptr, FALSE); }
                const int index = HitItem(point);
                if (index != hovered_) {
                    hovered_ = index; InvalidateRect(panel, nullptr, FALSE);
                    if (!selection_.Count()) SetWindowTextW(panel, index >= 0 ? (name_ + L" · " + items_[index].label).c_str() : (name_ + L" · 内容").c_str());
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (isPanel) { trackingPanel_ = false; hovered_ = -1; sortHovered_ = false; InvalidateRect(panel, nullptr, FALSE); }
            else trackingHost_ = false;
            ScheduleClose(); return 0;
        case WM_LBUTTONDOWN:
            if (isPanel) {
                const RECT search = SearchRect(); const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (PtInRect(&search, point)) {
                    if (point.x >= search.right - Px(23)) SetWindowTextW(searchEdit_, L"");
                    SetFocus(searchEdit_); return 0;
                }
            }
            if (!isPanel) {
                pressed_ = true; moving_ = false; GetCursorPos(&pressPoint_);
                RECT rect{}; GetWindowRect(host, &rect); initialPoint_ = {rect.left, rect.top}; SetCapture(host);
            } else {
                SetFocus(panel); CancelClose();
                const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                const RECT add = AddRect(), sort = SortRect(), content = ContentRect();
                const bool control = (w & MK_CONTROL) != 0, shift = (w & MK_SHIFT) != 0;
                addPressed_ = PtInRect(&add, point) != FALSE; sortPressed_ = PtInRect(&sort, point) != FALSE;
                collapseOnRelease_ = toggleOnRelease_ = false;
                pressedItem_ = HitItem(point); itemPressed_ = pressedItem_ >= 0;
                if (itemPressed_) {
                    collapseOnRelease_ = !control && !shift && selection_.Contains(pressedItem_) && selection_.Count() > 1;
                    toggleOnRelease_ = control && !shift && selection_.Contains(pressedItem_);
                    if (!collapseOnRelease_ && !toggleOnRelease_) selection_.Click(pressedItem_, control, shift);
                    else selection_.Focus(pressedItem_);
                    GetCursorPos(&itemPressPoint_); SetCapture(panel); ClearTooltips(); SelectionChanged();
                } else if (addPressed_ || sortPressed_) { SetCapture(panel); InvalidateRect(panel, nullptr, FALSE); }
                else if (PtInRect(&content, point)) BeginMarquee(point, control, shift);
            }
            return 0;
        case WM_LBUTTONUP:
            if (!isPanel) {
                if (!pressed_) return 0;
                const bool wasMoving = moving_; pressed_ = moving_ = false; ReleaseCapture();
                if (wasMoving) {
                    RECT rect{}; GetWindowRect(host, &rect);
                    const auto grid = box::ReadDesktopGrid(dpi_);
                    MoveAnchor(SnapPosition({rect.left, rect.top}, grid)); SavePosition();
                } else ShowPanel();
            } else {
                const POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (marquee_) { MoveMarquee(point); EndMarquee(false); return 0; }
                const bool click = itemPressed_, addClick = addPressed_, sortClick = sortPressed_;
                const bool collapse = collapseOnRelease_, toggle = toggleOnRelease_; const int item = pressedItem_;
                itemPressed_ = addPressed_ = sortPressed_ = collapseOnRelease_ = toggleOnRelease_ = false;
                if (GetCapture() == panel) ReleaseCapture();
                const RECT add = AddRect(), sort = SortRect();
                if (addClick && PtInRect(&add, point)) ChooseItems();
                else if (sortClick && PtInRect(&sort, point)) SortMenu();
                else if (click && HitItem(point) == item) {
                    if (collapse) selection_.Click(item);
                    else if (toggle) selection_.Click(item, true);
                    SelectionChanged(); UpdateTooltips();
                }
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (isPanel && !(w & (MK_CONTROL | MK_SHIFT))) {
                const int item = HitItem({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
                if (item >= 0) { selection_.Click(item); SelectionChanged(); OpenItem(item); }
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (isPanel) { itemPressed_ = addPressed_ = sortPressed_ = collapseOnRelease_ = toggleOnRelease_ = false; EndMarquee(false); }
            else pressed_ = moving_ = false;
            return 0;
        case WM_CANCELMODE:
            if (isPanel) { EndMarquee(true); itemPressed_ = addPressed_ = sortPressed_ = false; if (GetCapture() == panel) ReleaseCapture(); }
            return 0;
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (point.x == -1 && point.y == -1) {
                if (isPanel && selection_.Focus() >= 0) {
                    const int row = selection_.Focus() / 4;
                    if (row < firstRow_) firstRow_ = row;
                    if (row >= firstRow_ + VisibleRows()) firstRow_ = row - VisibleRows() + 1;
                    QueueVisibleIcons(); UpdateTooltips();
                    const RECT item = ItemRect(selection_.Focus()); point = {item.left + Px(20), item.top + Px(20)}; ClientToScreen(panel, &point);
                } else { RECT rect{}; GetWindowRect(hwnd, &rect); point = {rect.left + Px(20), rect.top + Px(20)}; }
            }
            POINT local = point; ScreenToClient(hwnd, &local);
            ContextMenu(point, isPanel ? HitItem(local) : -1); return 0;
        }
        case WM_MOUSEWHEEL:
            if (isPanel) {
                StopMotion();
                wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(w);
                const int rows = wheelRemainder_ / WHEEL_DELTA; wheelRemainder_ %= WHEEL_DELTA;
                firstRow_ = std::clamp(firstRow_ - rows, 0, MaxRow()); hovered_ = -1;
                QueueVisibleIcons();
                if (marquee_) MoveMarquee(marqueePoint_); else UpdateTooltips();
                InvalidateRect(panel, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (isPanel && (w == VK_TAB || ((GetKeyState(VK_CONTROL) & 0x8000) && w == 'F'))) {
                SetFocus(searchEdit_); SendMessageW(searchEdit_, EM_SETSEL, 0, -1); return 0;
            }
            if (w == VK_ESCAPE) {
                if (isPanel && marquee_) EndMarquee(true);
                else if (isPanel && !searchQuery_.empty()) SetWindowTextW(searchEdit_, L"");
                else if (isPanel && selection_.Count()) { selection_.Clear(); SelectionChanged(); }
                else HidePanel();
                return 0;
            }
            if (w == VK_F5) { Reload(); return 0; }
            if (isPanel && !items_.empty()) {
                const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (control && w == 'A') { selection_.All(); SelectionChanged(); return 0; }
                int next = selection_.Focus() < 0 ? firstRow_ * 4 : selection_.Focus();
                if (w == VK_RETURN) { if (selection_.Count()) OpenPaths(SelectedPaths()); else OpenItem(next); return 0; }
                if (w == VK_SPACE) { selection_.Click(next, control, shift); SelectionChanged(); return 0; }
                if (w == VK_F2 && selection_.Count() == 1) { RenameFile(SelectedPaths().front()); return 0; }
                if (w == VK_RIGHT) ++next; else if (w == VK_LEFT) --next;
                else if (w == VK_DOWN) next += 4; else if (w == VK_UP) next -= 4;
                else if (w == VK_HOME) next = 0; else if (w == VK_END) next = static_cast<int>(items_.size()) - 1; else break;
                next = std::clamp(next, 0, static_cast<int>(items_.size()) - 1);
                if (control && !shift) selection_.Focus(next); else selection_.Click(next, control, shift);
                if (next / 4 < firstRow_) firstRow_ = next / 4;
                if (next / 4 >= firstRow_ + VisibleRows()) firstRow_ = next / 4 - VisibleRows() + 1;
                QueueVisibleIcons(); UpdateTooltips(); SelectionChanged(); return 0;
            }
            break;
        case WM_ACTIVATE:
            if (isPanel && LOWORD(w) == WA_INACTIVE) ScheduleClose(); return 0;
        case WM_TIMER:
            if (w == kBackdropCacheTimer) { recentBackdrop.Expire(hwnd); return 0; }
            if (w == kTextCacheTimer) { ReleaseTextCache(GetTickCount64()); return 0; }
            if (w == kLayoutTimer) {
                if (MotionProgress() >= 1. || !panelVisible_) { StopMotion(); UpdateTooltips(); }
                else { const RECT content = ContentRect(); InvalidateRect(panel, &content, FALSE); }
                return 0;
            }
            if (w == kSelectionScrollTimer) {
                if (marquee_) {
                    const RECT viewport = ContentRect();
                    const int delta = marqueePoint_.y < viewport.top + Px(14) ? -1 : marqueePoint_.y > viewport.bottom - Px(14) ? 1 : 0;
                    firstRow_ = std::clamp(firstRow_ + delta, 0, MaxRow()); QueueVisibleIcons(); MoveMarquee(marqueePoint_);
                } else { KillTimer(host, kSelectionScrollTimer); marqueeScroll_ = false; }
                return 0;
            }
            if (w == kRefreshTimer) {
                KillTimer(host, kRefreshTimer); watcher_.Rearm(); Reload(); return 0;
            }
            if (w == kDesktopTimer) {
                KillTimer(host, kDesktopTimer);
                Reveal();
                return 0;
            }
            if (w == kCloseTimer) {
                POINT point{}; GetCursorPos(&point); CloseTimer(point, GetTickCount64());
            }
            return 0;
        case kIconReady: {
            std::unique_ptr<IconResult> result(reinterpret_cast<IconResult*>(l));
            if (result && result->generation == generation_ && result->index < items_.size()) {
                const size_t start = static_cast<size_t>(firstRow_ * 4);
                const size_t keepStart = start > 24 ? start - 24 : 0;
                if (items_.size() <= 64 || (result->index >= keepStart && result->index < keepStart + 64))
                    items_[result->index].icon = std::move(result->icon);
                else requestedIcons_[result->index] = false;
                InvalidateRect(panel, nullptr, FALSE);
            }
            return 0;
        }
        case kBackdropReady: {
            std::unique_ptr<box::BackdropResult> result(reinterpret_cast<box::BackdropResult*>(l));
            if (!result && w == backdropGeneration_) { backdropPending_ = false; pendingBlur_ = -1; }
            if (result && result->generation == backdropGeneration_) {
                backdropPending_ = false; pendingBlur_ = -1;
                RECT rect{}; GetWindowRect(panel, &rect);
                if (panelVisible_ && backgroundMode_ == 0 && glassTransparency_ && result->value.Matches(rect) &&
                    result->value.Ready(std::clamp(Px(glassBlur_), 0, 120))) {
                    backdrop_ = std::move(result->value); customGlass_ = true;
                    Round(panel); InvalidateRect(panel, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_DPICHANGED:
            if (!isPanel) {
                dpi_ = HIWORD(w); const auto* suggested = reinterpret_cast<RECT*>(l);
                const auto grid = box::ReadDesktopGrid(dpi_, !nativeMode_); cell_ = grid.cell; desktopIconSize_ = grid.iconSize;
                MoveAnchor(SnapPosition({suggested->left, suggested->top}, grid), true);
                LoadAppearance(); Reload();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            if (!isPanel) {
                RECT rect{}; GetWindowRect(host, &rect);
                const auto grid = box::ReadDesktopGrid(dpi_, !nativeMode_); cell_ = grid.cell; desktopIconSize_ = grid.iconSize;
                MoveAnchor(SnapPosition({rect.left, rect.top}, grid), true); SavePosition();
                if (panelVisible_) PositionPanel();
            }
            return 0;
        case WM_SETTINGCHANGE: if (!isPanel) LoadAppearance(); return 0;
        case WM_DWMCOMPOSITIONCHANGED: if (isPanel) { roundedBackdrop_ = 0; roundedDark_ = -1; Round(panel); InvalidateRect(panel, nullptr, FALSE); } return 0;
        case kWake: Reveal(); ShowPanel(); return 0;
        case kFilesChanged: SetTimer(host, kRefreshTimer, 100, nullptr); return 0;
        case kDesktopVisible: {
            POINT cursor{}; GetCursorPos(&cursor);
            if (!nativeMode_ || !NativeIconContains(cursor)) HidePanel();
            SetTimer(host, kDesktopTimer, 160, nullptr);
            if (owner_) PostMessageW(owner_, kDesktopVisible, 0, 0);
            return 0;
        }
        case kTray:
            if (LOWORD(l) == WM_CONTEXTMENU) { POINT point{}; GetCursorPos(&point); ContextMenu(point); }
            else if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) { Reveal(); ShowPanel(); }
            return 0;
        case WM_SYSCOMMAND:
            if (!isPanel && ((w & 0xFFF0) == SC_MINIMIZE || (w & 0xFFF0) == SC_CLOSE)) { HidePanel(); return 0; }
            break;
        case WM_WINDOWPOSCHANGING:
            if (!isPanel && anchorShown_ && !removing_) {
                auto* position = reinterpret_cast<WINDOWPOS*>(l);
                if (position->flags & SWP_HIDEWINDOW) position->flags = (position->flags & ~SWP_HIDEWINDOW) | SWP_SHOWWINDOW;
            }
            break;
        case WM_CLOSE: HidePanel(); if (!isPanel && !removing_) Reveal(); return 0;
        case WM_DESTROY:
            if (!isPanel) {
                watcher_.Stop();
                eventHosts_.erase(std::remove(eventHosts_.begin(), eventHosts_.end(), host), eventHosts_.end());
                if (eventHosts_.empty() && foregroundHook_) { UnhookWinEvent(foregroundHook_); foregroundHook_ = nullptr; }
                if (hostDrop_) { RevokeDragDrop(host); hostDrop_->Release(); hostDrop_ = nullptr; }
                if (panelDrop_) { RevokeDragDrop(panel); panelDrop_->Release(); panelDrop_ = nullptr; }
                if (IsWindow(panel)) DestroyWindow(panel);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
public:
    App(HINSTANCE instance, const std::wstring& root, const std::wstring& icons, const std::wstring& journal,
        std::function<void(const std::wstring&, POINT)> create, std::function<void(App*)> remove, HWND owner = nullptr,
        std::function<bool(App*)> activatePanel = {})
        : storage(root, icons, journal), instance_(instance), name_(storage.Read(L"Name", L"常用")), createBox_(std::move(create)), deleteBox_(std::move(remove)),
          activatePanel_(std::move(activatePanel)), owner_(owner) {}
    ~App() {
        removing_ = true;
        if (panelVisible_) HidePanel();
        recentBackdrop.ForgetOwner(host);
        watcher_.Stop(); icons_.Stop(); backdropLoader_.Stop();
        if (IsWindow(host)) DestroyWindow(host);
        if (IsWindow(panel)) DestroyWindow(panel);
        for (HFONT font : {titleFont_, bodyFont_, smallFont_, desktopFont_}) if (font) DeleteObject(font);
        if (searchBrush_) DeleteObject(searchBrush_);
    }
    void Report(const std::wstring& title, HRESULT hr) {
        const bool previous = inDialog_; inDialog_ = true;
        MessageBoxW(OwnerWindow(), (title + L"\n\n" + ErrorText(hr)).c_str(), kTitle, MB_OK | MB_ICONWARNING);
        inDialog_ = previous;
    }
    void Initialize(bool show = true, bool attachDesktop = true) {
        direction_ = static_cast<box::Direction>(std::clamp(storage.ReadInt(L"Direction", 0), 0, 4));
        backgroundMode_ = std::clamp(storage.ReadInt(L"BackgroundMode", 0), 0, 3);
        glassTransparency_ = std::clamp(storage.ReadInt(L"GlassTransparency", 55), 0, 100);
        glassBlur_ = std::clamp(storage.ReadInt(L"GlassBlur", 18), 0, 40);
        dpi_ = GetDpiForSystem();
        const auto grid = box::ReadDesktopGrid(dpi_, !(show && attachDesktop)); cell_ = grid.cell; desktopIconSize_ = grid.iconSize;
        const UINT fontDpi = dpi_;
        LoadAppearance();
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance_; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.style = CS_DBLCLKS; cls.lpfnWndProc = HostProc; cls.lpszClassName = kHostClass; cls.hIcon = folderIcon_.value;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw HRESULT_FROM_WIN32(GetLastError());
        cls.lpfnWndProc = PanelProc; cls.lpszClassName = kPanelClass;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw HRESULT_FROM_WIN32(GetLastError());
        POINT point{storage.ReadInt(L"X", 80), storage.ReadInt(L"Y", 180)};
        point = SnapPosition(point, grid);
        nativeMode_ = show && attachDesktop;
        desktopParent_ = nullptr;
        POINT local = point;
        if (desktopParent_) ScreenToClient(desktopParent_, &local);
        {
            box::WindowDpiScope matchingParent(desktopParent_);
            if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kHostClass, (name_ + L" · 桌面文件盒").c_str(),
                                 desktopParent_ ? WS_CHILD : WS_POPUP, local.x, local.y, cell_.cx, cell_.cy, desktopParent_, nullptr, instance_, this))
                throw HRESULT_FROM_WIN32(GetLastError());
        }
        dpi_ = GetDpiForWindow(host);
        MoveAnchor(point, true);
        if (!CreateWindowExW(WS_EX_TOOLWINDOW, kPanelClass, (name_ + L" · 内容").c_str(), WS_POPUP | WS_CLIPCHILDREN,
                             point.x + cell_.cx + Px(10), point.y, Px(416), Px(308), owner_, nullptr, instance_, this))
            throw HRESULT_FROM_WIN32(GetLastError());
        searchEdit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchControl)), instance_, nullptr);
        if (!searchEdit_ || !SetWindowSubclass(searchEdit_, SearchProc, 1, reinterpret_cast<DWORD_PTR>(this))) throw E_FAIL;
        SendMessageW(searchEdit_, EM_SETLIMITTEXT, 256, 0);
        SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索"));
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, panel, nullptr, instance_, nullptr);
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, Px(500));
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
        if (fontDpi != dpi_) LoadAppearance();
        else { SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE); LayoutSearch(); }
        Round(host); Round(panel);
        Reload();
        if (!nativeMode_) Check(RenderAnchor());
        hostDrop_ = new DropTarget(this); Check(RegisterDragDrop(host, hostDrop_));
        panelDrop_ = new DropTarget(this); Check(RegisterDragDrop(panel, panelDrop_));
        watcher_.Start(host, storage.items);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        eventHosts_.push_back(host);
        if (!foregroundHook_) foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundEvent,
                                        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (nativeMode_) {
            nativeEntry_.Ensure(storage, name_, ExePath(), L"", false, true);
            name_ = storage.Read(L"Name", name_.c_str());
            UpdateNativeBounds();
        } else if (show) {
            anchorShown_ = true;
            SetWindowPos(host, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(host); SavePosition();
        }
    }
    bool ShowPanel(bool locatedFromHover = false) {
        if (moving_ || inDialog_ || openingPanel_) return false;
        if (panelVisible_) { CancelClose(); return true; }
        FlagGuard opening(openingPanel_);
        if (activatePanel_ && !activatePanel_(this)) return false;
        CancelClose();
        KillTimer(host, kTextCacheTimer); textCacheUntil_ = 0;
        if (nativeMode_) {
            if (!locatedFromHover || !nativeLocated_) UpdateNativeBounds();
        } else if (!IsWindowVisible(host)) ShowWindow(host, SW_SHOWNOACTIVATE);
        PositionPanel(); panelVisible_ = true; QueueVisibleIcons();
        PrepareBackdrop();
        ShowWindow(panel, SW_SHOWNOACTIVATE); InvalidateRect(panel, nullptr, FALSE);
        UpdateWindow(panel);
        if (owner_) PostMessageW(owner_, kNativeMouse, 0, 0); // Recheck the latest pointer after Shell/backdrop work.
        return true;
    }
    void HidePanel() {
        if (!panel) return;
        StopMotion(); std::vector<DWORD>().swap(blendScratch_);
        EndMarquee(false); itemPressed_ = addPressed_ = sortPressed_ = false;
        if (GetCapture() == panel) ReleaseCapture();
        if (panelVisible_) NoteBackdropHide(owner_ ? owner_ : host);
        panelVisible_ = false; nativePointerInside_ = false; hovered_ = -1; ShowWindow(panel, SW_HIDE); CancelClose();
        ++backdropGeneration_; backdropPending_ = false; pendingBlur_ = -1; backdropLoader_.Stop();
        RECT screen{}; GetWindowRect(panel, &screen);
        if (!removing_ && backgroundMode_ == 0 && glassTransparency_ && backdrop_.Matches(screen))
            recentBackdrop.Retain(std::move(backdrop_), owner_ ? owner_ : host);
        backdrop_.Clear(); customGlass_ = false;
        panelSurface_.Clear();
        if (!removing_ && textMasks_.Count()) {
            textCacheUntil_ = GetTickCount64() + 500;
            if (!SetTimer(host, kTextCacheTimer, 500, nullptr)) DropTextCache();
        } else DropTextCache();
        icons_.Clear(); ++generation_;
        const size_t first = static_cast<size_t>(firstRow_ * 4), last = first + static_cast<size_t>(VisibleRows() * 4);
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i < first || i >= last) items_[i].icon = Icon();
            requestedIcons_[i] = items_[i].icon.value != nullptr;
        }
    }
    void ScheduleClose() {
        if (panelVisible_ && !closeDeadline_ && !inMenu_ && !inDialog_ && !dragIncoming && GetCapture() != panel) {
            closeDeadline_ = GetTickCount64() + kPanelCloseDelay;
            if (!SetTimer(host, kCloseTimer, kPanelCloseDelay, nullptr)) closeDeadline_ = 0;
        }
    }
    bool CanYieldPanel() const {
        return !openingPanel_ && !inMenu_ && (!inDialog_ || !draggingPaths_.empty()) && !dragIncoming && GetCapture() != panel;
    }
    bool PanelVisible() const { return panelVisible_; }
    void SetDropHover(bool enabled) {
        dragIncoming = enabled;
        if (enabled) { CancelClose(); ShowPanel(); }
        else ScheduleClose();
        RequestAnchorPaint(); InvalidateRect(panel, nullptr, FALSE);
    }
    bool AddPaths(const std::vector<std::wstring>& paths) {
        FlagGuard guard(inDialog_);
        box::MoveResult result;
        try { result = storage.MoveInto(paths, OwnerWindow()); }
        catch (...) { Reload(); throw; }
        Reload();
        return result.moved > 0;
    }
    bool ReorderDrop(POINT point) {
        if (draggingPaths_.empty()) return false;
        POINT local = point; ScreenToClient(panel, &local);
        int target = HitItem(local);
        if (target < 0) target = static_cast<int>(items_.size());
        RestoreSelection(draggingPaths_); ReorderSelected(target);
        return true;
    }
    bool AttachedToDesktop() const { return IsWindow(host) && nativeMode_; }
    bool NativeReady() const { return AttachedToDesktop() && nativeLocated_; }
    bool NativeIconContains(POINT point) const { return nativeMode_ && nativeLocated_ && PtInRect(&nativeImage_, point); }
    RECT NativeContextBounds() const { return nativeMode_ && nativeLocated_ ? nativeCell_ : RECT{}; }
    void UpdateNativeBounds(bool invalidate = false) {
        if (!nativeMode_) return;
        if (nativeEntry_.RefreshName(storage, name_)) {
            SetWindowTextW(host, (name_ + L" · 桌面文件盒").c_str());
            SetWindowTextW(panel, (name_ + L" · 内容").c_str());
            if (panelVisible_) InvalidateRect(panel, nullptr, FALSE);
        }
        if (invalidate) nativeEntry_.Invalidate();
        const RECT previous = nativeCell_;
        const bool wasLocated = nativeLocated_;
        if (nativeEntry_.Bounds(nativeCell_, nativeImage_)) {
            nativeLocated_ = true;
            cell_ = {nativeCell_.right - nativeCell_.left, nativeCell_.bottom - nativeCell_.top};
            desktopIconSize_ = nativeImage_.right - nativeImage_.left;
            if (!wasLocated || !EqualRect(&previous, &nativeCell_)) MoveAnchor({nativeCell_.left, nativeCell_.top}, true);
        } else nativeLocated_ = false;
    }
    void NativeUnavailable() { nativeLocated_ = false; nativeEntry_.Invalidate(); }
    void NativeMouse(POINT point, bool desktop, bool buttonDown) {
        const bool overIcon = desktop && NativeIconContains(point);
        RECT panelRect{}; if (panelVisible_) GetWindowRect(panel, &panelRect);
        const bool inside = overIcon || (panelVisible_ && (PtInRect(&panelRect, point) || (desktop && box::InPanelTransit(point, nativeImage_, panelRect))));
        if (inside) CancelClose();
        else if (nativePointerInside_) ScheduleClose();
        nativePointerInside_ = inside;
        if (overIcon && !buttonDown && !panelVisible_ && !inMenu_ && !inDialog_) ShowPanel(true);
    }
    void Reveal() {
        if (!IsWindow(host) || removing_) return;
        if (nativeMode_) {
            if (GetFileAttributesW(nativeEntry_.Path().c_str()) == INVALID_FILE_ATTRIBUTES) nativeEntry_.Ensure(storage, name_, ExePath());
            UpdateNativeBounds(true); return;
        }
        anchorShown_ = true; Check(RenderAnchor()); SetWindowPos(host, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    void Menu(POINT point) { ContextMenu(point); }
    box::Direction Direction() const { return direction_; }
    void SetDirection(box::Direction value) {
        storage.Write(L"Direction", std::to_wstring(static_cast<int>(value)));
        direction_ = value;
        if (panelVisible_) PositionPanel();
    }
    void Refresh() { Reload(); }
    void WarmIcons() { if (!panelVisible_) QueueVisibleIcons(true); }
    void RefreshAppearance() { ReloadAppearance(); }
    void Appearance(const AppearanceCommit& commit = {}, const AppearanceSettings* initial = nullptr) { ChooseAppearance(commit, initial); }
    void AppendHoverDiagnostics(std::wstring& text) const {
        text += L"box located=" + std::to_wstring(nativeLocated_) + L" visible=" + std::to_wstring(panelVisible_) +
                L" menu=" + std::to_wstring(inMenu_) + L" dialog=" + std::to_wstring(inDialog_) +
                L" moving=" + std::to_wstring(moving_) + L" opening=" + std::to_wstring(openingPanel_) +
                L" drag=" + std::to_wstring(dragIncoming) + L" item_pressed=" + std::to_wstring(itemPressed_) +
                L" marquee=" + std::to_wstring(marquee_) +
                L" image=" + std::to_wstring(nativeImage_.left) + L"," + std::to_wstring(nativeImage_.top) + L"," +
                std::to_wstring(nativeImage_.right) + L"," + std::to_wstring(nativeImage_.bottom) + L"\r\n";
    }
};

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) {
    if (!data || !effect) return E_POINTER;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    acceptedEffect_ = SUCCEEDED(data->QueryGetData(&format)) && (*effect & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
    *effect = acceptedEffect_;
    app_->SetDropHover(acceptedEffect_ != DROPEFFECT_NONE); return S_OK;
}
HRESULT DropTarget::DragOver(DWORD, POINTL, DWORD* effect) {
    if (!effect) return E_POINTER;
    *effect = acceptedEffect_; return S_OK;
}
HRESULT DropTarget::DragLeave() { acceptedEffect_ = DROPEFFECT_NONE; app_->SetDropHover(false); return S_OK; }
HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) {
    if (!data || !effect) return E_POINTER;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
    *effect = DROPEFFECT_NONE;
    try {
        if (acceptedEffect_ != DROPEFFECT_NONE && SUCCEEDED(data->GetData(&format, &medium))) {
            MediumGuard release{medium};
            std::vector<std::wstring> paths;
            HDROP drop = static_cast<HDROP>(medium.hGlobal);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length + 1, L'\0'); DragQueryFileW(drop, i, path.data(), length + 1); path.resize(length);
                paths.push_back(std::move(path));
            }
            if (app_->ReorderDrop({point.x, point.y}) || (!paths.empty() && app_->AddPaths(paths))) {
                *effect = DROPEFFECT_MOVE;
                // The target already moved the files. Tell the source not to delete them a second time.
                for (const auto& formatName : {CFSTR_PERFORMEDDROPEFFECT, CFSTR_LOGICALPERFORMEDDROPEFFECT}) {
                    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
                    if (!memory) continue;
                    auto* value = static_cast<DWORD*>(GlobalLock(memory));
                    if (!value) { GlobalFree(memory); continue; }
                    *value = wcscmp(formatName, CFSTR_PERFORMEDDROPEFFECT) == 0 ? DROPEFFECT_NONE : DROPEFFECT_MOVE;
                    GlobalUnlock(memory);
                    FORMATETC feedback{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(formatName)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                    STGMEDIUM transferred{}; transferred.tymed = TYMED_HGLOBAL; transferred.hGlobal = memory;
                    if (FAILED(data->SetData(&feedback, &transferred, TRUE))) GlobalFree(memory);
                }
            }
        }
    } catch (...) {
        acceptedEffect_ = DROPEFFECT_NONE; app_->SetDropHover(false); return E_FAIL;
    }
    acceptedEffect_ = DROPEFFECT_NONE; app_->SetDropHover(false); return S_OK;
}


class Manager {
#ifdef DESKTOPBOX_TESTS
    friend int SelfTest(const std::wstring& directory);
    friend int PerformanceTest(const std::wstring& directory);
#endif
    HINSTANCE instance_;
    std::wstring root_, icons_, journal_;
    HWND controller_ = nullptr;
    std::vector<std::unique_ptr<App>> boxes_;
    Icon trayIcon_;
    NOTIFYICONDATAW tray_{};
    bool trayAdded_ = false;
    bool trayCustomIcon_ = false;
    UINT taskbarCreated_ = 0;
    int desktopRetries_ = 0;
    ULONGLONG desktopRetryStarted_ = 0, desktopRetryDeadline_ = 0;
    ULONGLONG mouseReadyAt_ = 0, desktopReadyAt_ = 0, createdAt_ = GetTickCount64();
    ULONG desktopNotifications_ = 0;
    box::DesktopMouse mouse_;
    ULONGLONG lastNativeRefresh_ = 0, lastSurfaceRefresh_ = 0;
    HWND desktopSurface_ = nullptr;
    bool contextMenuOpen_ = false;
    bool globalSettingsOpen_ = false;
    bool integrateUser_ = true;
    ULONGLONG pointerChecks_ = 0, lastPointerCheck_ = 0;
    ULONGLONG pointerSettleDue_ = 0;
    bool pointerSettlePending_ = false;
    bool ActivatePanel(App* requested) {
        // Every opening path (hover, tray, launch, settings and drag/drop) comes
        // through here before preparing the new panel's backdrop.
        for (const auto& box : boxes_) if (box.get() != requested && !box->CanYieldPanel()) return false;
        for (const auto& box : boxes_) if (box.get() != requested && box->PanelVisible()) box->HidePanel();
        return true;
    }
    void BeginDesktopRetry() {
        desktopRetries_ = 0; desktopRetryStarted_ = GetTickCount64(); desktopReadyAt_ = 0;
        desktopRetryDeadline_ = desktopRetryStarted_ + 60000;
        SetTimer(controller_, kDesktopTimer, 100, nullptr);
    }
    bool DesktopReady() const {
        return IsWindow(desktopSurface_) && std::all_of(boxes_.begin(), boxes_.end(), [](const auto& box) { return box->NativeReady(); });
    }
    void RetryDesktop() {
        RestoreDesktop(); RefreshNative(true); NativePointer(false);
        const ULONGLONG now = GetTickCount64();
        if (DesktopReady() || !desktopRetryDeadline_ || now >= desktopRetryDeadline_) {
            KillTimer(controller_, kDesktopTimer); desktopRetryDeadline_ = 0;
            if (DesktopReady()) desktopReadyAt_ = now;
        } else {
            ++desktopRetries_;
            const ULONGLONG elapsed = now - desktopRetryStarted_;
            SetTimer(controller_, kDesktopTimer, elapsed < 2000 ? 100 : elapsed < 10000 ? 250 : 1000, nullptr);
        }
    }
    void PublishContextTargets() {
        std::vector<box::DesktopContextTarget> targets;
        for (const auto& box : boxes_) {
            const RECT bounds = box->NativeContextBounds();
            if (!IsRectEmpty(&bounds)) targets.push_back({bounds, reinterpret_cast<UINT_PTR>(box.get())});
        }
        mouse_.SetContextTargets(desktopSurface_, std::move(targets));
    }
    void UpdateShellIntegration() {
        Storage defaults(root_, icons_, journal_);
        const auto selected = AppearanceSettings::NewBoxDefaults(defaults).icon;
        const auto icon = selected.empty() ? L"" : icons_ + L"\\" + box::BaseName(selected);
        Check(box::RegisterUserShellNew(ExePath(), root_, box::ShellNewIcon(icon)));
        RefreshTrayIcon();
    }
    void RefreshTrayIcon() {
        Storage defaults(root_, icons_, journal_);
        const auto selected = AppearanceSettings::NewBoxDefaults(defaults).icon;
        Icon replacement;
        if (!selected.empty()) {
            const auto path = icons_ + L"\\" + box::BaseName(selected);
            const UINT dpi = GetDpiForWindow(controller_);
            const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
            replacement = Icon(reinterpret_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                size, size, LR_LOADFROMFILE)));
        }
        const bool customIconLoaded = replacement.value != nullptr;
        if (!replacement.value) {
            SHSTOCKICONINFO stock{}; stock.cbSize = sizeof(stock);
            Check(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_SMALLICON, &stock));
            replacement = Icon(stock.hIcon);
        }
        trayCustomIcon_ = customIconLoaded;
        // Explorer copies the icon on NIM_MODIFY, so the previous handle can be released afterward.
        tray_.hIcon = replacement.value;
        if (trayAdded_) {
            tray_.uFlags = NIF_ICON;
            Shell_NotifyIconW(NIM_MODIFY, &tray_);
            tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        }
        trayIcon_ = std::move(replacement);
    }
    bool OnDesktop(POINT point) const {
        HWND surface = desktopSurface_, under = WindowFromPoint(point);
        if (!surface || (under != surface && !IsChild(surface, under))) return false;
        wchar_t type[32]{}; if (under != surface) GetClassNameW(under, type, 32);
        if (_wcsicmp(type, L"Edit") == 0) return false;
        GUITHREADINFO gui{}; gui.cbSize = sizeof(gui);
        return !GetGUIThreadInfo(GetWindowThreadProcessId(surface, nullptr), &gui) || !(gui.flags & GUI_INMENUMODE);
    }
    void RefreshNative(bool invalidate = false) {
        desktopSurface_ = box::DesktopSurface();
        for (auto& box : boxes_) {
            if (desktopSurface_) box->UpdateNativeBounds(invalidate);
            else box->NativeUnavailable();
        }
        PublishContextTargets();
        lastSurfaceRefresh_ = lastNativeRefresh_ = GetTickCount64();
    }
    void SchedulePointerSettle() {
        pointerSettleDue_ = GetTickCount64() + 80;
        if (!pointerSettlePending_) pointerSettlePending_ = SetTimer(controller_, kPointerSettleTimer, 80, nullptr) != 0;
    }
    void SettlePointer() {
        if (!pointerSettlePending_) return;
        KillTimer(controller_, kPointerSettleTimer); pointerSettlePending_ = false;
        const auto now = GetTickCount64();
        if (now < pointerSettleDue_) {
            pointerSettlePending_ = SetTimer(controller_, kPointerSettleTimer, static_cast<UINT>(pointerSettleDue_ - now), nullptr) != 0;
            return;
        }
        pointerSettleDue_ = 0;
        if (contextMenuOpen_) return;
        // One final snapshot after input settles also covers a stationary pointer
        // while Explorer finishes a move, DPI change or desktop-view replacement.
        const HWND surface = box::DesktopSurface();
        POINT point{}; GetCursorPos(&point);
        if (surface != desktopSurface_) RefreshNative(true);
        else if (OnDesktop(point)) RefreshNative();
        NativePointer(false);
    }
    void NativePointer(bool settle = true) {
        if (contextMenuOpen_) { mouse_.SetPointerContext({}, false, nullptr); return; }
        if (settle && integrateUser_) SchedulePointerSettle();
        ++pointerChecks_; lastPointerCheck_ = GetTickCount64();
        POINT point{}; GetCursorPos(&point);
        const ULONGLONG now = GetTickCount64();
        // A stale/null cached surface must not prevent the refresh that would recover it.
        // This lookup only runs on mouse activity, never as an idle polling timer.
        if (!IsWindow(desktopSurface_) || now - lastSurfaceRefresh_ > 500) {
            const HWND surface = box::DesktopSurface();
            lastSurfaceRefresh_ = now;
            if (surface != desktopSurface_) RefreshNative(true);
        }
        const bool desktop = OnDesktop(point);
        if (desktop && now - lastNativeRefresh_ > 500) RefreshNative();
        mouse_.SetPointerContext(point, desktop, GetForegroundWindow());
        const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        for (auto& box : boxes_) box->NativeMouse(point, desktop, buttonDown);
    }
    void ApplyGlobalAppearance(const AppearanceSettings& value) {
        Storage defaults(root_, icons_, journal_);
        std::vector<std::pair<App*, AppearanceSettings>> previous;
        for (const auto& box : boxes_) previous.emplace_back(box.get(), AppearanceSettings::Read(box->storage));
        std::vector<std::pair<const wchar_t*, std::wstring>> savedDefaults;
        for (const auto* key : {L"NewBoxBackgroundMode", L"NewBoxGlassTransparency", L"NewBoxGlassBlur", L"NewBoxIconFile", L"HasNewBoxAppearance"})
            savedDefaults.emplace_back(key, defaults.Read(key));
        try {
            value.SaveNewBoxDefaults(defaults);
            for (const auto& box : boxes_) value.Save(box->storage);
            for (const auto& box : boxes_) box->RefreshAppearance();
            if (integrateUser_) UpdateShellIntegration();
            else RefreshTrayIcon();
        } catch (...) {
            // Restore both individual settings and defaults if any part of the batch cannot be applied.
            for (const auto& saved : previous) { try { saved.second.Save(saved.first->storage); } catch (...) {} }
            for (const auto& saved : savedDefaults) { try { defaults.Write(saved.first, saved.second); } catch (...) {} }
            for (const auto& saved : previous) { try { saved.first->RefreshAppearance(); } catch (...) {} }
            if (integrateUser_) { try { UpdateShellIntegration(); } catch (...) {} }
            else { try { RefreshTrayIcon(); } catch (...) {} }
            throw;
        }
    }
    void GlobalAppearance() {
        if (globalSettingsOpen_) return;
        FlagGuard settingsGuard(globalSettingsOpen_);
        Storage defaults(root_, icons_, journal_);
        const auto initial = defaults.Read(L"NewBoxBackgroundMode").empty() ? AppearanceSettings::Read(defaults) : AppearanceSettings::NewBoxDefaults(defaults);
        const AppearanceCommit commit = [this](const AppearanceSettings& value) { ApplyGlobalAppearance(value); };
        if (!boxes_.empty()) boxes_.front()->Appearance(commit, &initial);
        else {
            // Global defaults remain editable after the last box is removed, without creating a new box or desktop icon.
            App editor(instance_, root_, icons_, journal_, {}, {}, controller_);
            editor.Initialize(false, false); editor.Appearance(commit, &initial);
        }
    }
    void BoxMenu(App& box, POINT point) {
        if (contextMenuOpen_) return;
        FlagGuard menuGuard(contextMenuOpen_);
        mouse_.SetPointerContext({}, false, nullptr);
        for (const auto& other : boxes_) other->HidePanel();
        box.Menu(point);
        menuGuard.Restore(); RefreshNative(true); NativePointer();
    }
    void ApplyGlobalDirection(box::Direction value) {
        Storage defaults(root_, icons_, journal_);
        const auto savedDefault = defaults.Read(L"NewBoxDirection");
        std::vector<std::pair<App*, box::Direction>> previous;
        for (const auto& box : boxes_) previous.emplace_back(box.get(), box->Direction());
        try {
            defaults.Write(L"NewBoxDirection", std::to_wstring(static_cast<int>(value)));
            for (const auto& box : boxes_) box->SetDirection(value);
        } catch (...) {
            for (const auto& saved : previous) { try { saved.first->SetDirection(saved.second); } catch (...) {} }
            try { defaults.Write(L"NewBoxDirection", savedDefault); } catch (...) {}
            throw;
        }
    }
    HMENU CreateTrayMenu() const {
        Storage defaults(root_, icons_, journal_);
        HMENU menu = CreatePopupMenu(), directions = CreatePopupMenu();
        if (!menu || !directions) {
            if (menu) DestroyMenu(menu);
            if (directions) DestroyMenu(directions);
            throw E_OUTOFMEMORY;
        }
        AppendMenuW(menu, MF_STRING, kNewBox, L"新建收纳盒…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kAppearance, L"全局设置（所有收纳盒）…");
        int direction = boxes_.empty() ? std::clamp(defaults.ReadInt(L"NewBoxDirection", 0), 0, 4) : static_cast<int>(boxes_.front()->Direction());
        for (const auto& box : boxes_) if (static_cast<int>(box->Direction()) != direction) { direction = -1; break; }
        const wchar_t* names[] = {L"自动选择方向", L"优先向左展开", L"优先向右展开", L"优先向上展开", L"优先向下展开"};
        for (int i = 0; i < 5; ++i) AppendMenuW(directions, MF_STRING | (direction == i ? MF_CHECKED : 0), kDirectionAuto + i, names[i]);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(directions), L"展开方向（所有收纳盒）");
        bool startup = false; if (integrateUser_) { try { StartupTask task; startup = task.Enabled(); } catch (...) {} }
        AppendMenuW(menu, MF_STRING | (startup ? MF_CHECKED : 0), kStartup, L"登录后自动启动");
        AppendMenuW(menu, MF_STRING, kRefresh, L"刷新全部收纳盒");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExit, L"退出");
        return menu;
    }
    void TrayCommand(UINT command) {
        switch (command) {
        case kNewBox: AskNewBox(); break;
        case kAppearance: GlobalAppearance(); break;
        case kDirectionAuto: case kDirectionLeft: case kDirectionRight: case kDirectionUp: case kDirectionDown:
            ApplyGlobalDirection(static_cast<box::Direction>(command - kDirectionAuto)); break;
        case kStartup: { StartupTask task; task.Set(!task.Enabled(), root_); break; }
        case kRefresh:
            for (const auto& box : boxes_) { box->Reveal(); box->Refresh(); }
            RefreshNative(true); break;
        case kExit: PostQuitMessage(0); break;
        }
    }
    void TrayMenu(POINT point) {
        if (contextMenuOpen_) return;
        FlagGuard menuGuard(contextMenuOpen_);
        mouse_.SetPointerContext({}, false, nullptr);
        for (const auto& box : boxes_) box->HidePanel();
        HMENU menu = CreateTrayMenu();
        SetForegroundWindow(controller_);
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, controller_, nullptr);
        DestroyMenu(menu); PostMessageW(controller_, WM_NULL, 0, 0);
        if (choice) TrayCommand(choice);
        menuGuard.Restore(); RefreshNative(true); NativePointer();
    }
    bool WriteHoverDiagnostics() const {
        POINT point{}; GetCursorPos(&point);
        wchar_t underClass[128]{}, foregroundClass[128]{};
        const HWND under = WindowFromPoint(point), foreground = GetForegroundWindow();
        GetClassNameW(under, underClass, 128); GetClassNameW(foreground, foregroundClass, 128);
        GUITHREADINFO gui{}; gui.cbSize = sizeof(gui);
        GetGUIThreadInfo(GetWindowThreadProcessId(desktopSurface_, nullptr), &gui);
        std::wstring text = L"process=" + std::to_wstring(GetCurrentProcessId()) + L"\r\nraw_input=" + std::to_wstring(mouse_.RawInputEnabled()) +
            L"\r\nhook_events=" + std::to_wstring(mouse_.HookEvents()) + L"\r\nraw_events=" + std::to_wstring(mouse_.RawEvents()) +
            L"\r\nhook_installations=" + std::to_wstring(mouse_.HookInstallations()) + L"\r\npointer_checks=" + std::to_wstring(pointerChecks_) +
            L"\r\nlast_pointer_age_ms=" + std::to_wstring(GetTickCount64() - lastPointerCheck_) +
            L"\r\nmenu_open=" + std::to_wstring(contextMenuOpen_) + L"\r\ndesktop_surface=" + std::to_wstring(IsWindow(desktopSurface_)) +
            L"\r\ntray_custom_icon=" + std::to_wstring(trayCustomIcon_) +
            L"\r\nmouse_ready_ms=" + std::to_wstring(mouseReadyAt_ ? mouseReadyAt_ - createdAt_ : 0) +
            L"\r\ndesktop_ready_ms=" + std::to_wstring(desktopReadyAt_ ? desktopReadyAt_ - createdAt_ : 0) +
            L"\r\ndesktop_retry_pending=" + std::to_wstring(desktopRetryDeadline_ != 0) +
            L"\r\ndesktop_retry_attempts=" + std::to_wstring(desktopRetries_) +
            L"\r\ncursor=" + std::to_wstring(point.x) + L"," + std::to_wstring(point.y) + L"\r\non_desktop=" + std::to_wstring(OnDesktop(point)) +
            L"\r\nwindow_under_cursor=" + underClass + L"\r\nforeground_window=" + foregroundClass +
            L"\r\nshell_gui_flags=" + std::to_wstring(gui.flags) + L"\r\nsettle_pending=" + std::to_wstring(pointerSettlePending_) + L"\r\n";
        for (const auto& box : boxes_) box->AppendHoverDiagnostics(text);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(bytes, '\0'); WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr, nullptr);
        return WriteBytes(root_ + L"\\hover-diagnostics.txt", utf8.data(), static_cast<DWORD>(utf8.size()));
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto* manager = reinterpret_cast<Manager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            manager = reinterpret_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(manager));
            manager->controller_ = hwnd;
        }
        if (!manager) return DefWindowProcW(hwnd, message, w, l);
        try { return manager->Handle(message, w, l); }
        catch (HRESULT hr) { MessageBoxW(hwnd, ErrorText(hr).c_str(), kTitle, MB_OK | MB_ICONWARNING); }
        catch (...) { MessageBoxW(hwnd, L"操作未能完成。", kTitle, MB_OK | MB_ICONWARNING); }
        return 0;
    }
    void Tray() {
        tray_.cbSize = sizeof(tray_); tray_.hWnd = controller_; tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray_.uCallbackMessage = kTray; tray_.hIcon = trayIcon_.value;
        wcscpy_s(tray_.szTip, L"桌面收纳盒 · 右键管理");
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
        if (trayAdded_) { tray_.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray_); }
    }
    void Attach(const std::wstring& directory) {
        auto create = [this](const std::wstring& name, POINT point) { NewBox(name, point); };
        auto remove = [this](App* box) { PostMessageW(controller_, kRemoveBoxMsg, 0, reinterpret_cast<LPARAM>(box)); };
        auto activate = [this](App* box) { return ActivatePanel(box); };
        auto box = std::make_unique<App>(instance_, directory, icons_, journal_, std::move(create), std::move(remove), controller_, std::move(activate));
        if (box->storage.ReadInt(L"Deleted", 0)) return;
        box->Initialize(integrateUser_, integrateUser_); boxes_.push_back(std::move(box));
    }
    void NewBox(const std::wstring& name, POINT point) {
        GUID guid{}; Check(CoCreateGuid(&guid)); wchar_t id[64]{}; StringFromGUID2(guid, id, 64);
        auto directory = root_ + L"\\boxes\\" + id;
        Storage data(directory, icons_, journal_);
        data.Write(L"Name", name); data.Write(L"X", std::to_wstring(point.x)); data.Write(L"Y", std::to_wstring(point.y));
        Storage defaults(root_, icons_, journal_);
        AppearanceSettings::NewBoxDefaults(defaults).Save(data);
        data.Write(L"Direction", std::to_wstring(std::clamp(defaults.ReadInt(L"NewBoxDirection", 0), 0, 4)));
        Attach(directory);
        RefreshNative(true);
    }
    void AskNewBox() {
        auto name = AskName(instance_, controller_, L"新收纳盒");
        if (!name.empty()) { POINT point{}; GetCursorPos(&point); NewBox(name, point); }
    }
    void RestoreDesktop() {
        std::vector<std::wstring> retry;
        boxes_.erase(std::remove_if(boxes_.begin(), boxes_.end(), [&](const auto& box) {
            if (box->AttachedToDesktop()) return false;
            retry.push_back(box->storage.root); return true;
        }), boxes_.end());
        for (const auto& directory : retry) Attach(directory);
    }
    LRESULT Handle(UINT message, WPARAM w, LPARAM l) {
        if (taskbarCreated_ && message == taskbarCreated_) { Tray(); BeginDesktopRetry(); return 0; }
        switch (message) {
        case kCompleteBackdropHide: FlushBackdropHide(); return 0;
        case kCompleteShellSetup: UpdateShellIntegration(); return 0;
        case kWarmIcons: for (const auto& box : boxes_) box->WarmIcons(); return 0;
        case WM_INPUT:
            mouse_.HandleRawInput(l);
            return DefWindowProcW(controller_, message, w, l); // Windows must clean up foreground raw-input messages.
        case kNativeChanged: {
            PIDLIST_ABSOLUTE* items = nullptr; LONG event = 0;
            HANDLE notification = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(w), static_cast<DWORD>(l), &items, &event);
            if (notification) SHChangeNotification_Unlock(notification);
            SetTimer(controller_, kNativeRefreshTimer, 80, nullptr); return 0;
        }
        case WM_COPYDATA: {
            const auto* packet = reinterpret_cast<COPYDATASTRUCT*>(l);
            if (!packet || (packet->dwData != 0x44424F58 && packet->dwData != 0x44424F59 && packet->dwData != 0x44424F5A && packet->dwData != 0x44424F5B) || packet->cbData < 4 || packet->cbData > 2 * 1024 * 1024 || packet->cbData % sizeof(wchar_t)) return FALSE;
            const auto* data = static_cast<const wchar_t*>(packet->lpData);
            const size_t size = packet->cbData / sizeof(wchar_t);
            if (!data || data[size - 1] != L'\0') return FALSE;
            std::vector<std::wstring> fields;
            for (size_t start = 0; start + 1 < size;) {
                size_t end = start; while (end < size && data[end]) ++end;
                if (end == size) return FALSE;
                fields.emplace_back(data + start, end - start); start = end + 1;
                if (start < size && !data[start]) break;
            }
            if (fields.empty()) return FALSE;
            if (packet->dwData == 0x44424F5A || packet->dwData == 0x44424F5B) {
                if (fields.size() != 1 || _wcsicmp(fields.front().c_str(), root_.c_str()) != 0) return FALSE;
                if (packet->dwData == 0x44424F5B) return WriteHoverDiagnostics();
                return PostMessageW(controller_, kCreateShellBox, 0, 0);
            }
            const auto target = fields.front(); fields.erase(fields.begin()); Open(target, fields, packet->dwData == 0x44424F59); return TRUE;
        }
        case kCreateShellBox: { POINT point{}; GetCursorPos(&point); NewBox(L"桌面收纳盒", point); return 0; }
        case kShellIconChanged:
            for (const auto& box : boxes_) if (box.get() == reinterpret_cast<App*>(l)) { if (integrateUser_) UpdateShellIntegration(); break; }
            return 0;
        case kNativeContext: {
            if (contextMenuOpen_) return 0;
            for (const auto& box : boxes_) if (reinterpret_cast<UINT_PTR>(box.get()) == w) {
                BoxMenu(*box, {GET_X_LPARAM(l), GET_Y_LPARAM(l)});
                break;
            }
            return 0;
        }
        case kNativeMouse: {
            if (mouse_.Consume()) SetTimer(controller_, kNativeRefreshTimer, 60, nullptr);
            NativePointer();
            return 0;
        }
        case WM_TIMER:
            if (w == kBackdropCacheTimer) { recentBackdrop.Expire(controller_); return 0; }
            if (w == kPointerSettleTimer) { SettlePointer(); return 0; }
            if (w == kNativeRefreshTimer) {
                KillTimer(controller_, kNativeRefreshTimer); RefreshNative(true); NativePointer(false); return 0;
            }
            if (w == kDesktopTimer) {
                RetryDesktop();
            }
            return 0;
        case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
            for (auto& box : boxes_) PostMessageW(box->host, message, w, l);
            SetTimer(controller_, kNativeRefreshTimer, 80, nullptr);
            return 0;
        case kDesktopVisible: NativePointer(); return 0;
        case kWake:
            RestoreDesktop();
            for (auto& box : boxes_) box->Reveal();
            if (!boxes_.empty()) boxes_.front()->ShowPanel();
            return 0;
        case kRemoveBoxMsg:
            boxes_.erase(std::remove_if(boxes_.begin(), boxes_.end(), [l](const auto& box) { return box.get() == reinterpret_cast<App*>(l); }), boxes_.end());
            for (auto& box : boxes_) box->Refresh();
            RefreshNative(true);
            return 0;
        case kTray:
            if (LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) {
                RestoreDesktop();
                for (auto& box : boxes_) box->Reveal();
                if (!boxes_.empty()) boxes_.front()->ShowPanel(); else AskNewBox();
            } else if (LOWORD(l) == WM_CONTEXTMENU) {
                POINT point{}; GetCursorPos(&point);
                TrayMenu(point);
            }
            return 0;
        case WM_CLOSE: PostQuitMessage(0); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        }
        return DefWindowProcW(controller_, message, w, l);
    }
public:
    Manager(HINSTANCE instance, std::wstring root, std::wstring icons)
        : instance_(instance), root_(std::move(root)), icons_(std::move(icons)), journal_(root_ + L"\\origins") {}
    ~Manager() {
        recentBackdrop.ForgetOwner(controller_);
        if (desktopNotifications_) SHChangeNotifyDeregister(desktopNotifications_);
        mouse_.Stop();
        boxes_.clear();
        if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
        if (IsWindow(controller_)) DestroyWindow(controller_);
    }
    void Open(const std::wstring& directory, const std::vector<std::wstring>& paths = {}, bool appearance = false) {
        if (directory.empty() && appearance) { GlobalAppearance(); return; }
        for (auto& box : boxes_) if (_wcsicmp(directory.c_str(), box->storage.root.c_str()) == 0) {
            if (!paths.empty()) box->AddPaths(paths);
            box->Reveal(); box->ShowPanel(); if (appearance) box->Appearance(); return;
        }
        if (directory.empty()) {
            for (auto& box : boxes_) box->Reveal();
            if (!boxes_.empty()) { boxes_.front()->ShowPanel(); if (appearance) boxes_.front()->Appearance(); }
        }
    }
    void Initialize(bool integrateUser = true) {
        integrateUser_ = integrateUser;
        EnsureDirectory(root_); EnsureDirectory(icons_); EnsureDirectory(journal_); EnsureDirectory(root_ + L"\\boxes");
        Storage defaults(root_, icons_, journal_);
        if (!defaults.ReadInt(L"HasNewBoxAppearance", 0)) AppearanceSettings::NewBoxDefaults(defaults).SaveNewBoxDefaults(defaults);
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance_; cls.lpfnWndProc = WindowProc; cls.lpszClassName = kControllerClass;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw HRESULT_FROM_WIN32(GetLastError());
        Check(CreateWindowExW(0, kControllerClass, kTitle, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance_, this) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        if (integrateUser_) {
            desktopSurface_ = box::DesktopSurface();
            Check(mouse_.Start(controller_, kNativeMouse, kNativeContext)); mouseReadyAt_ = GetTickCount64();
        }
        RefreshTrayIcon();
        if (integrateUser_) Tray();
        Attach(root_);
        WIN32_FIND_DATAW found{}; HANDLE search = FindFirstFileW((root_ + L"\\boxes\\*").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(found.cFileName, L".") && wcscmp(found.cFileName, L"..")) {
                    auto directory = root_ + L"\\boxes\\" + found.cFileName;
                    if (GetFileAttributesW((directory + L"\\settings.ini").c_str()) != INVALID_FILE_ATTRIBUTES) Attach(directory);
                }
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
        if (!integrateUser_) return;
        BeginDesktopRetry();
        RefreshNative(true);
        NativePointer();
        if (DesktopReady()) { desktopReadyAt_ = GetTickCount64(); KillTimer(controller_, kDesktopTimer); desktopRetryDeadline_ = 0; }
        PostMessageW(controller_, kCompleteShellSetup, 0, 0);
        PostMessageW(controller_, kWarmIcons, 0, 0);
        PIDLIST_ABSOLUTE desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desktop))) {
            SHChangeNotifyEntry entry{desktop, TRUE};
            desktopNotifications_ = SHChangeNotifyRegister(controller_, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                SHCNE_RENAMEITEM | SHCNE_CREATE | SHCNE_DELETE | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR, kNativeChanged, 1, &entry);
            CoTaskMemFree(desktop);
        }
    }
};

#ifdef DESKTOPBOX_TESTS
#include "../tests/performance.inc"
#include "../tests/self_test.inc"
#endif

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    if (FAILED(initialized)) return 1;
    int exitCode = 0;
    try {
        int count = 0; wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::wstring root, testDirectory, openBox;
        std::vector<std::wstring> dropPaths;
        bool dropping = false, appearance = false, newBox = false, registerShell = false, unregisterShell = false;
        bool enableStartup = false, disableStartup = false, shutdown = false, diagnostics = false;
        for (int i = 1; i < count; ++i) {
            if (dropping) { dropPaths.push_back(AbsolutePath(arguments[i])); continue; }
            if (wcscmp(arguments[i], L"--drop") == 0) dropping = true;
            else if (wcscmp(arguments[i], L"--appearance") == 0) appearance = true;
            else if (wcscmp(arguments[i], L"--new-box") == 0) newBox = true;
            else if (wcscmp(arguments[i], L"--register-shell") == 0) registerShell = true;
            else if (wcscmp(arguments[i], L"--unregister-shell") == 0) unregisterShell = true;
            else if (wcscmp(arguments[i], L"--enable-startup") == 0) enableStartup = true;
            else if (wcscmp(arguments[i], L"--disable-startup") == 0) disableStartup = true;
            else if (wcscmp(arguments[i], L"--shutdown") == 0) shutdown = true;
            else if (wcscmp(arguments[i], L"--diagnostics") == 0) diagnostics = true;
            else if (wcscmp(arguments[i], L"--open-box") == 0 && i + 1 < count) openBox = AbsolutePath(arguments[++i]);
            else if (wcscmp(arguments[i], L"--data-dir") == 0 && i + 1 < count) root = AbsolutePath(arguments[++i]);
            else if (wcscmp(arguments[i], L"--self-test") == 0 && i + 1 < count) testDirectory = AbsolutePath(arguments[++i]);
        }
        LocalFree(arguments);
        if (!testDirectory.empty()) {
#ifdef DESKTOPBOX_TESTS
            exitCode = GetFileAttributesW((testDirectory + L"\\benchmark.flag").c_str()) != INVALID_FILE_ATTRIBUTES ? PerformanceTest(testDirectory) : SelfTest(testDirectory);
#else
            exitCode = ERROR_NOT_SUPPORTED;
#endif
        }
        else {
            if (root.empty() && GetFileAttributesW((box::ParentPath(ExePath()) + L"\\portable.flag").c_str()) != INVALID_FILE_ATTRIBUTES)
                root = box::ParentPath(ExePath()) + L"\\data";
            if (root.empty()) {
                wchar_t* local = nullptr; Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local));
                root = std::wstring(local) + L"\\DesktopBox"; CoTaskMemFree(local);
            }
            const bool portable = GetFileAttributesW((box::ParentPath(ExePath()) + L"\\portable.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
            const std::wstring icons = portable ? box::ParentPath(ExePath()) + L"\\ico" : root + L"\\ico";
            if (diagnostics) {
                const HWND existing = FindWindowW(kControllerClass, kTitle);
                std::wstring request = root; request.push_back(0); request.push_back(0);
                COPYDATASTRUCT packet{0x44424F5Bu, static_cast<DWORD>(request.size() * sizeof(wchar_t)), request.data()};
                DWORD_PTR received = 0;
                const bool ok = existing && SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&packet), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &received) && received;
                OleUninitialize(); return ok ? 0 : 2;
            }
            if (registerShell || unregisterShell || enableStartup || disableStartup || shutdown) {
                if (shutdown) {
                    const HWND existing = FindWindowW(kControllerClass, kTitle);
                    DWORD process = 0; if (existing) GetWindowThreadProcessId(existing, &process);
                    KernelHandle running(process ? OpenProcess(SYNCHRONIZE, FALSE, process) : nullptr);
                    if (existing) PostMessageW(existing, WM_CLOSE, 0, 0);
                    if (running.value && WaitForSingleObject(running.value, 10000) == WAIT_TIMEOUT) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                }
                if (unregisterShell) Check(box::UnregisterUserShellNew(ExePath()));
                if (registerShell) {
                    Storage defaults(root, icons, root + L"\\origins");
                    const auto selected = AppearanceSettings::NewBoxDefaults(defaults).icon;
                    Check(box::RegisterUserShellNew(ExePath(), root,
                        box::ShellNewIcon(selected.empty() ? L"" : icons + L"\\" + box::BaseName(selected))));
                }
                if (enableStartup || disableStartup) { StartupTask task; task.Set(enableStartup && !disableStartup, root); }
                OleUninitialize(); return 0;
            }
            KernelHandle instanceMutex(CreateMutexW(nullptr, FALSE, L"Local\\DesktopBox.SingleInstance.01"));
            if (!instanceMutex.value) throw HRESULT_FROM_WIN32(GetLastError());
            const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
            const HWND existing = FindWindowW(kControllerClass, kTitle);
            if (alreadyRunning || existing) {
                if (existing && (!openBox.empty() || appearance || newBox)) {
                    std::wstring payload = newBox ? root : openBox; payload.push_back(L'\0');
                    for (const auto& path : dropPaths) { payload += path; payload.push_back(L'\0'); }
                    payload.push_back(L'\0');
                    COPYDATASTRUCT packet{newBox ? 0x44424F5Au : appearance ? 0x44424F59u : 0x44424F58u, static_cast<DWORD>(payload.size() * sizeof(wchar_t)), payload.data()};
                    DWORD_PTR received = 0;
                    if (!SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&packet), SMTO_ABORTIFHUNG, 5000, &received) || !received)
                        throw HRESULT_FROM_WIN32(ERROR_NOT_READY);
                } else if (existing) PostMessageW(existing, kWake, 0, 0);
                else throw HRESULT_FROM_WIN32(ERROR_NOT_READY);
            }
            else {
                Manager manager(instance, root, icons); manager.Initialize();
                if (newBox) PostMessageW(FindWindowW(kControllerClass, kTitle), kCreateShellBox, 0, 0);
                if (!openBox.empty() || appearance) manager.Open(openBox, dropPaths, appearance);
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
    } catch (HRESULT hr) { MessageBoxW(nullptr, ErrorText(hr).c_str(), L"桌面文件盒启动失败", MB_OK | MB_ICONERROR); exitCode = 1; }
    catch (...) { MessageBoxW(nullptr, L"启动失败，请检查数据目录是否可写。", kTitle, MB_OK | MB_ICONERROR); exitCode = 1; }
    OleUninitialize(); return exitCode;
}
