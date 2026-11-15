#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include "storage.h"

namespace box {
struct NativeBounds {
    std::wstring path;
    RECT cell{}, image{};
    bool located = false;
};
// One read-only Shell connection for the entire desktop snapshot.
std::vector<NativeBounds> ReadNativeBounds(const std::vector<std::wstring>& paths, HWND desktop);
class NativeEntry {
    IFolderView2* view_ = nullptr;
    ITEMIDLIST* item_ = nullptr;
    std::wstring path_;
    void EnsureShortcut(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                        const std::wstring& desktopOverride, bool renameExisting, bool onlyIfChanged);
    void EnsureNamespace(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                         bool renameExisting, bool onlyIfChanged);
public:
    NativeEntry() = default;
    explicit NativeEntry(const std::wstring& path) : path_(path) {}
    ~NativeEntry();
    void Invalidate();
    void Ensure(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                const std::wstring& desktopOverride = L"", bool renameExisting = false, bool onlyIfChanged = false);
    void Remove(const Storage& storage);
    bool Bounds(RECT& cell, RECT& image);
    bool RefreshName(const Storage& storage, std::wstring& name);
    bool BeginRename();
    bool NamespaceMode() const { return path_.rfind(L"::{", 0) == 0; }
    bool RestorePosition(const Storage& storage);
    void Reveal(const Storage& storage, const std::wstring& name, const std::wstring& executable);
    const std::wstring& Path() const { return path_; }
};
}
