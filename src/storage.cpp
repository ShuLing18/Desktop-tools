#include "storage.h"
#include "model.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace {
void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }
template<class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** out() { return &p; }
};
std::wstring Full(const std::wstring& path) {
    auto result = box::FullPath(path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool Within(const std::wstring& child, const std::wstring& parent) {
    return Same(child, parent) || (child.size() > parent.size() && _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) == 0 &&
                                  (parent.back() == L'\\' || child[parent.size()] == L'\\'));
}
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void Directory(const std::wstring& path) {
    const int error = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (error && error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) throw HRESULT_FROM_WIN32(error);
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) throw HRESULT_FROM_WIN32(ERROR_DIRECTORY);
}
bool Bytes(const std::wstring& path, const void* data, DWORD size, DWORD disposition = CREATE_ALWAYS) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; bool result = WriteFile(file, data, size, &written, nullptr) && written == size;
    if (result) result = FlushFileBuffers(file) != FALSE;
    CloseHandle(file); return result;
}
std::wstring IniRead(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t small[256];
    DWORD n = GetPrivateProfileStringW(section, key, L"", small, 256, ini.c_str());
    if (n < 255) return {small, n};
    for (DWORD size = 512;; size = std::min<DWORD>(size * 2, 32768)) {
        std::wstring result(size, L'\0');
        n = GetPrivateProfileStringW(section, key, L"", result.data(), size, ini.c_str());
        if (n < size - 1 || size == 32768) { result.resize(n); return result; }
    }
}
std::wstring NewId() { GUID guid{}; Check(CoCreateGuid(&guid)); wchar_t text[64]{}; StringFromGUID2(guid, text, 64); return text; }
std::wstring Identity(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"";
    BY_HANDLE_FILE_INFORMATION info{}; const bool obtained = GetFileInformationByHandle(file, &info) != FALSE; CloseHandle(file);
    if (!obtained) return L"";
    return std::to_wstring(info.dwVolumeSerialNumber) + L":" + std::to_wstring(info.nFileIndexHigh) + L":" + std::to_wstring(info.nFileIndexLow);
}
struct Record { std::wstring file, original, current, source, identity; };
void Save(const Record& record) {
    const auto identity = Exists(record.current) ? Identity(record.current) : record.identity;
    const auto text = L"\xFEFF[Origin]\r\nOriginal=" + record.original + L"\r\nCurrent=" + record.current + L"\r\nPendingSource=" + record.source + L"\r\nFileId=" + identity + L"\r\n";
    const auto temporary = record.file + L".pending";
    if (!Bytes(temporary, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)))) throw HRESULT_FROM_WIN32(GetLastError());
    if (!MoveFileExW(temporary.c_str(), record.file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw HRESULT_FROM_WIN32(GetLastError());
}
std::vector<Record> Records(const std::wstring& directory) {
    std::vector<Record> result; WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((directory + L"\\*.ini").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return result;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const auto file = directory + L"\\" + entry.cFileName;
            Record record{file, IniRead(file, L"Origin", L"Original"), IniRead(file, L"Origin", L"Current"), IniRead(file, L"Origin", L"PendingSource"), IniRead(file, L"Origin", L"FileId")};
            if (!record.original.empty() && !record.current.empty() && (record.source.empty() || !Exists(record.source))) result.push_back(std::move(record));
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search); return result;
}
std::wstring ItemPath(IShellItem* item) {
    if (!item) return L"";
    wchar_t* path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return L"";
    std::wstring result(path); CoTaskMemFree(path); return result;
}
class MoveSink final : public IFileOperationProgressSink {
    LONG refs_ = 1;
    std::function<HRESULT(HRESULT, IShellItem*)> done_;
public:
    explicit MoveSink(std::function<HRESULT(HRESULT, IShellItem*)> callback) : done_(std::move(callback)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IFileOperationProgressSink)) { *result = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { LONG result = InterlockedDecrement(&refs_); if (!result) delete this; return result; }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override { return done_(hr, created); }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};
struct Plan { std::wstring source, destination, name, journal; bool track = false; };
box::MoveResult Move(const std::vector<Plan>& plans, HWND owner, bool quiet) {
    static bool busy = false;
    if (busy) throw HRESULT_FROM_WIN32(ERROR_BUSY);
    struct Guard { bool& value; explicit Guard(bool& flag) : value(flag) { value = true; } ~Guard() { value = false; } } guard(busy);
    box::MoveResult result;
    if (plans.empty()) return result;
    Ptr<IFileOperation> operation;
    Check(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOperation, reinterpret_cast<void**>(operation.out())));
    Check(operation->SetOwnerWindow(owner));
    DWORD flags = FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR | FOFX_NOCOPYHOOKS;
    flags |= quiet ? FOF_NO_UI : (FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
    Check(operation->SetOperationFlags(flags));
    std::vector<std::unique_ptr<MoveSink, void(*)(MoveSink*)>> sinks;
    for (const auto& plan : plans) {
        Directory(plan.destination);
        auto source = Full(plan.source), destination = Full(plan.destination);
        if (Same(box::ParentPath(source), destination) && Same(box::BaseName(source), plan.name)) continue;
        if (Within(destination, source)) throw E_INVALIDARG;
        if (!Exists(source)) throw HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        Record prior;
        std::vector<std::wstring> oldRecords;
        for (const auto& record : Records(plan.journal)) if (Same(record.current, source) && (record.identity.empty() || record.identity == Identity(source))) {
            if (prior.file.empty() || (record.source.empty() && !prior.source.empty())) prior = record;
            oldRecords.push_back(record.file);
        }
        Record pending;
        if (plan.track) {
            auto name = plan.name;
            if (Exists(destination + L"\\" + name)) {
                const bool folder = (GetFileAttributesW(source.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const auto dot = folder ? std::wstring::npos : name.find_last_of(L'.');
                auto stem = dot == std::wstring::npos ? name : name.substr(0, dot);
                auto extension = dot == std::wstring::npos ? L"" : name.substr(dot);
                name = box::BaseName(box::UniquePath(destination, stem, extension));
            }
            pending = {plan.journal + L"\\" + NewId() + L".ini", prior.original.empty() ? source : prior.original, destination + L"\\" + name, source, L""};
            Save(pending);
        }
        Ptr<IShellItem> item, target;
        Check(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.out())));
        Check(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(target.out())));
        auto* sink = new MoveSink([&, source, oldRecords, pending, track = plan.track](HRESULT hr, IShellItem* created) -> HRESULT {
            try {
                auto actual = ItemPath(created);
                if (SUCCEEDED(hr) && !actual.empty() && Exists(actual) && !Exists(source)) {
                    if (track) { auto completed = pending; completed.current = actual; completed.source.clear(); Save(completed); }
                    for (const auto& file : oldRecords) DeleteFileW(file.c_str());
                    ++result.moved;
                } else {
                    result.complete = false;
                    if (track && Exists(source) && !Exists(pending.current)) DeleteFileW(pending.file.c_str());
                }
                return S_OK;
            } catch (HRESULT error) { result.complete = false; return error; }
            catch (...) { result.complete = false; return E_FAIL; }
        });
        sinks.emplace_back(sink, [](MoveSink* p) { p->Release(); });
        const auto requestedName = plan.track ? box::BaseName(pending.current) : plan.name;
        Check(operation->MoveItem(item.p, target.p, requestedName.c_str(), sink));
    }
    HRESULT performed = operation->PerformOperations();
    BOOL aborted = FALSE; Check(operation->GetAnyOperationsAborted(&aborted));
    result.complete = result.complete && SUCCEEDED(performed) && !aborted;
    if (FAILED(performed) && performed != HRESULT_FROM_WIN32(ERROR_CANCELLED)) throw performed;
    return result;
}
}

namespace box {
Storage::Storage(std::wstring directory, std::wstring icons, std::wstring journal)
    : root(Full(directory)), items(root + L"\\items"), ini(root + L"\\settings.ini"),
      iconDirectory(icons.empty() ? root + L"\\ico" : Full(icons)), journalDirectory(journal.empty() ? root + L"\\origins" : Full(journal)) {
    Directory(root); Directory(items); Directory(iconDirectory); Directory(journalDirectory);
    if (!Exists(ini)) { const WORD bom = 0xFEFF; if (!Bytes(ini, &bom, sizeof(bom), CREATE_NEW)) throw HRESULT_FROM_WIN32(GetLastError()); }
}
std::wstring Storage::Read(const wchar_t* key, const wchar_t* fallback) const { auto result = IniRead(ini, L"DesktopBox", key); return result.empty() ? fallback : result; }
int Storage::ReadInt(const wchar_t* key, int fallback) const { return GetPrivateProfileIntW(L"DesktopBox", key, fallback, ini.c_str()); }
void Storage::Write(const wchar_t* key, const std::wstring& value) const {
    if (!WritePrivateProfileStringW(L"DesktopBox", key, value.c_str(), ini.c_str())) throw HRESULT_FROM_WIN32(GetLastError());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}
std::wstring Storage::SelectedIconPath() const {
    const auto name = Read(L"IconFile");
    if (name.empty() || BaseName(name) != name) return L"";
    return iconDirectory + L"\\" + name;
}
std::wstring Storage::ImportIcon(const std::wstring& source) const {
    const auto path = Full(source);
    HICON validation = reinterpret_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, 64, 64, LR_LOADFROMFILE));
    if (!validation) throw HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    DestroyIcon(validation);
    std::wstring destination = path;
    if (!Same(ParentPath(path), iconDirectory)) {
        auto name = BaseName(path); const auto dot = name.find_last_of(L'.');
        destination = UniquePath(iconDirectory, dot == std::wstring::npos ? name : name.substr(0, dot), L".ico");
        if (destination.empty()) throw HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
        if (!CopyFileW(path.c_str(), destination.c_str(), TRUE)) throw HRESULT_FROM_WIN32(GetLastError());
    }
    return BaseName(destination);
}
void Storage::SetCustomIcon(const std::wstring& source) const {
    Write(L"IconFile", ImportIcon(source));
}
std::vector<std::wstring> Storage::Entries() const {
    std::vector<std::wstring> result; WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((items + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return result;
    do { if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) result.push_back(items + L"\\" + entry.cFileName); } while (FindNextFileW(search, &entry));
    FindClose(search); return result;
}
std::wstring Storage::OriginalPath(const std::wstring& current) const {
    const auto identity = Identity(current);
    for (const auto& record : Records(journalDirectory))
        if ((!record.identity.empty() && record.identity == identity) || (record.identity.empty() && Same(record.current, Full(current)))) return record.original;
    return L"";
}
void Storage::ReconcileOrigins() const {
    std::vector<std::pair<std::wstring, std::wstring>> identities;
    bool complete = true;
    for (const auto& path : Entries()) { auto id = Identity(path); if (id.empty()) complete = false; else identities.emplace_back(std::move(id), path); }
    for (auto record : Records(journalDirectory)) if (Same(ParentPath(record.current), items) && record.source.empty()) {
        const auto currentId = Identity(record.current);
        if (record.identity.empty() && !currentId.empty()) { Save(record); continue; }
        if (record.identity.empty() || currentId == record.identity) continue;
        const auto renamed = std::find_if(identities.begin(), identities.end(), [&](const auto& entry) { return entry.first == record.identity; });
        if (renamed != identities.end()) { record.current = renamed->second; Save(record); }
        else if (complete && !Exists(record.current)) DeleteFileW(record.file.c_str());
    }
}
MoveResult Storage::MoveInto(const std::vector<std::wstring>& paths, HWND owner, bool quiet) const {
    std::vector<Plan> plans;
    for (const auto& input : paths) {
        const auto path = Full(input);
        if (Same(ParentPath(path), items)) continue;
        if (std::any_of(plans.begin(), plans.end(), [&](const Plan& plan) { return Same(plan.source, path); })) continue;
        if (Within(root, path) || Within(iconDirectory, path) || Within(journalDirectory, path)) throw E_INVALIDARG;
        plans.push_back({path, items, BaseName(path), journalDirectory, true});
    }
    return Move(plans, owner, quiet);
}
MoveResult Storage::MoveOut(const std::wstring& path, const std::wstring& destination, HWND owner, bool quiet) const {
    if (!Same(ParentPath(Full(path)), items)) throw E_INVALIDARG;
    return Move({{path, destination, BaseName(path), journalDirectory, false}}, owner, quiet);
}
MoveResult Storage::RestoreAll(HWND owner, const std::wstring& unknownDestination, bool quiet) const {
    std::vector<Plan> plans;
    for (const auto& path : Entries()) {
        auto original = OriginalPath(path);
        if (original.empty()) {
            if (unknownDestination.empty()) throw HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            original = Full(unknownDestination) + L"\\" + BaseName(path);
        }
        if (Within(ParentPath(original), items)) throw E_INVALIDARG;
        plans.push_back({path, ParentPath(original), BaseName(original), journalDirectory, false});
    }
    auto result = Move(plans, owner, quiet);
    result.complete = result.complete && Entries().empty();
    return result;
}
void Storage::ForgetMovedOut(const std::wstring& path) const {
    const auto current = Full(path);
    if (!Same(ParentPath(current), items)) throw E_INVALIDARG;
    if (Exists(current)) return;
    for (const auto& record : Records(journalDirectory)) if (Same(record.current, current)) DeleteFileW(record.file.c_str());
}
}
