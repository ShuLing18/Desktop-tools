#define wWinMain DesktopBoxInstallerEntryPoint
#include "../installer/setup.cpp"
#undef wWinMain
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>

int wmain(int count, wchar_t** arguments) {
    if (count != 2) return 2;
    if (FAILED(OleInitialize(nullptr))) return 2;
    int total = 0, result = 0;
    auto expect = [&](bool ok, const char* description) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
        ++total;
        if (!ok) throw std::runtime_error(description);
    };
    auto touch = [](const std::wstring& path) {
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create fixture");
        DWORD written = 0;
        if (!WriteFile(file.value, "keep", 4, &written, nullptr) || written != 4) throw std::runtime_error("cannot write fixture");
    };
    try {
        const auto root = FullPath(arguments[1]) + L"\\run-" + std::to_wstring(GetCurrentProcessId());
        const auto program = InstallFolder(root + L"\\安装 目录");
        const auto data = root + L"\\自选 保存位置";
        EnsureDirectory(program);
        expect(ValidateDataDirectory(program, data) == data, "new installations accept a separate Unicode storage folder");
        Extract(program + L"\\" + kExe); MakePortable(program, data);
        expect(box::InstalledDataDirectory(program) == data &&
               GetFileAttributesW((data + L"\\").c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW((program + L"\\data").c_str()) == INVALID_FILE_ATTRIBUTES,
               "installer creates and persists the selected folder without creating a different data folder");
        DWORD payloadSize = 0; const void* payload = Payload(payloadSize);
        Handle extracted(CreateFileW((program + L"\\" + kExe).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        std::vector<BYTE> bytes(payloadSize); DWORD read = 0;
        expect(extracted.value != INVALID_HANDLE_VALUE && GetFileSize(extracted.value, nullptr) == payloadSize &&
               ReadFile(extracted.value, bytes.data(), payloadSize, &read, nullptr) && read == payloadSize &&
               memcmp(bytes.data(), payload, payloadSize) == 0,
               "extracted executable exactly matches the embedded release payload");
        touch(data + L"\\settings.ini");
        EnsureDirectory(data + L"\\items"); touch(data + L"\\items\\保留.txt");
        MakePortable(program, ValidateDataDirectory(program, data));
        expect(box::InstalledDataDirectory(program) == data &&
               GetFileAttributesW((data + L"\\items\\保留.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
               "reinstalling at the same location retains selected storage and existing files");
        bool rejected = false;
        try { ValidateDataDirectory(program, root + L"\\另一位置"); } catch (const std::wstring&) { rejected = true; }
        expect(rejected && box::InstalledDataDirectory(program) == data,
               "changing a populated installation's storage cannot hide or replace its existing data");
        const auto legacy = InstallFolder(root + L"\\旧安装");
        EnsureDirectory(legacy + L"\\data\\items"); touch(legacy + L"\\portable.flag");
        touch(legacy + L"\\data\\settings.ini"); touch(legacy + L"\\data\\items\\旧文件.txt");
        expect(SavedDataDirectory(legacy) == legacy + L"\\data", "older installations retain their original portable data folder");
        rejected = false;
        try { ValidateDataDirectory(legacy, data); } catch (const std::wstring&) { rejected = true; }
        expect(rejected, "upgrade validation also protects older installations without storage.ini");
        MakePortable(legacy, ValidateDataDirectory(legacy, legacy + L"\\data"));
        expect(box::ConfiguredDataDirectory(legacy) == legacy + L"\\data" &&
               GetFileAttributesW((legacy + L"\\data\\items\\旧文件.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
               "upgrading an older installation records its existing folder and preserves its files");
        for (const auto& invalid : std::vector<std::wstring>{program, Parent(program), program + L"\\ico", program + L"\\ico\\nested", L"relative\\folder", L"D:relative"}) {
            rejected = false;
            try { ValidateDataDirectory(program, invalid); } catch (const std::wstring&) { rejected = true; }
            expect(rejected, "installer rejects relative paths and overlapping program or icon folders");
        }
        const auto blocked = root + L"\\不是文件夹"; touch(blocked);
        rejected = false;
        try { MakePortable(program, blocked); } catch (const std::wstring&) { rejected = true; }
        expect(rejected && box::ConfiguredDataDirectory(program) == data,
               "a non-writable destination leaves the previous storage configuration intact");
        std::printf("PASS %d installer checks\n", total);
    } catch (const std::wstring& error) {
        std::fwprintf(stderr, L"FAIL %ls\n", error.c_str()); result = 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what()); result = 1;
    } catch (...) { std::fprintf(stderr, "FAIL unexpected installer error\n"); result = 1; }
    OleUninitialize();
    return result;
}
