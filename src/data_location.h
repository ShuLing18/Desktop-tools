#pragma once
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <string>
#include <vector>

namespace box {
inline std::wstring AbsoluteDataDirectory(const std::wstring& source) {
    const bool drive = source.size() >= 3 && ((source[0] >= L'A' && source[0] <= L'Z') ||
        (source[0] >= L'a' && source[0] <= L'z')) && source[1] == L':' && (source[2] == L'\\' || source[2] == L'/');
    if ((!drive && !PathIsUNCW(source.c_str())) || source.find_first_of(L"\"<>|?*\r\n") != std::wstring::npos)
        throw HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    const DWORD capacity = GetFullPathNameW(source.c_str(), 0, nullptr, nullptr);
    if (!capacity || capacity > 32768) throw HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    std::wstring result(capacity, L'\0');
    const DWORD length = GetFullPathNameW(source.c_str(), capacity, result.data(), nullptr);
    if (!length || length >= capacity) throw HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    result.resize(length);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}

inline std::wstring ConfiguredDataDirectory(const std::wstring& programDirectory) {
    const auto file = programDirectory + L"\\storage.ini";
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        throw HRESULT_FROM_WIN32(error);
    }
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetPrivateProfileStringW(L"DesktopBox", L"DataDirectory", L"", buffer.data(),
        static_cast<DWORD>(buffer.size()), file.c_str());
    if (!length || length >= buffer.size() - 1) throw HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    return AbsoluteDataDirectory(std::wstring(buffer.data(), length));
}

inline std::wstring InstalledDataDirectory(const std::wstring& programDirectory, const std::wstring& requested = {}) {
    if (!requested.empty()) return requested;
    const auto configured = ConfiguredDataDirectory(programDirectory);
    if (!configured.empty()) return configured;
    if (GetFileAttributesW((programDirectory + L"\\portable.flag").c_str()) != INVALID_FILE_ATTRIBUTES)
        return programDirectory + L"\\data";
    return {};
}

inline void SaveDataDirectory(const std::wstring& programDirectory, const std::wstring& directory) {
    const auto path = AbsoluteDataDirectory(directory);
    const auto file = programDirectory + L"\\storage.ini";
    const auto temporary = file + L".new";
    const auto contents = L"\xFEFF[DesktopBox]\r\nDataDirectory=" + path + L"\r\n";
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw HRESULT_FROM_WIN32(GetLastError());
    const DWORD size = static_cast<DWORD>(contents.size() * sizeof(wchar_t));
    DWORD written = 0;
    const bool saved = WriteFile(handle, contents.data(), size, &written, nullptr) && written == size && FlushFileBuffers(handle);
    const DWORD error = saved ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!saved || !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD failure = saved ? GetLastError() : error;
        DeleteFileW(temporary.c_str());
        throw HRESULT_FROM_WIN32(failure ? failure : ERROR_WRITE_FAULT);
    }
}
}
