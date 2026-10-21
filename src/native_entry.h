#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <string>
#include "storage.h"

namespace box {
class NativeEntry {
    IFolderView2* view_ = nullptr;
    ITEMIDLIST* item_ = nullptr;
    std::wstring path_;
public:
    NativeEntry() = default;
    explicit NativeEntry(const std::wstring& path) : path_(path) {}
    ~NativeEntry();
    void Invalidate();
    void Ensure(const Storage& storage, const std::wstring& name, const std::wstring& executable,
                const std::wstring& desktopOverride = L"", bool renameExisting = false);
    void Remove(const Storage& storage);
    bool Bounds(RECT& cell, RECT& image);
    bool RefreshName(const Storage& storage, std::wstring& name);
    bool BeginRename();
    const std::wstring& Path() const { return path_; }
};
}
