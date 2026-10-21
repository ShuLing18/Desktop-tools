#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace box {
struct MoveResult { bool complete = true; size_t moved = 0; };
class Storage {
public:
    std::wstring root, items, ini, iconDirectory, journalDirectory;
    explicit Storage(std::wstring directory, std::wstring icons = L"", std::wstring journal = L"");
    std::wstring Read(const wchar_t* key, const wchar_t* fallback = L"") const;
    int ReadInt(const wchar_t* key, int fallback) const;
    void Write(const wchar_t* key, const std::wstring& value) const;
    std::wstring SelectedIconPath() const;
    std::wstring ImportIcon(const std::wstring& source) const;
    void SetCustomIcon(const std::wstring& source) const;
    std::vector<std::wstring> Entries() const;
    std::wstring OriginalPath(const std::wstring& current) const;
    MoveResult MoveInto(const std::vector<std::wstring>& paths, HWND owner = nullptr, bool quiet = false) const;
    MoveResult MoveOut(const std::wstring& path, const std::wstring& destination, HWND owner = nullptr, bool quiet = false) const;
    void ForgetMovedOut(const std::wstring& path) const;
    void ReconcileOrigins() const;
    MoveResult RestoreAll(HWND owner = nullptr, const std::wstring& unknownDestination = L"", bool quiet = false) const;
};
}

