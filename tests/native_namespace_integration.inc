int NativeNamespaceIntegrationTest(const std::wstring& directory) {
    std::string report; int total = 0, result = 0;
    auto expect = [&](bool ok, const char* description) {
        report += std::string(ok ? "PASS " : "FAIL ") + description + "\r\n";
        ++total; if (!ok) throw std::runtime_error(description);
    };
    std::vector<std::wstring> roots;
    const auto sandbox = directory + L"\\run-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    try {
        EnsureDirectory(directory); EnsureDirectory(sandbox);
        const auto desktop = sandbox + L"\\legacy-desktop"; EnsureDirectory(desktop);
        Storage nativeData(sandbox + L"\\box", sandbox + L"\\ico", sandbox + L"\\origins"); roots.push_back(nativeData.root);
        nativeData.Write(L"Name", L"DesktopBox 原生系统验证");
        box::NativeEntry native;
        native.Ensure(nativeData, nativeData.Read(L"Name"), ExePath(), desktop);
        const auto oldLink = native.Path();
        const auto sentinel = nativeData.items + L"\\kept-during-migration.txt"; Check(WriteBytes(sentinel, "keep-native", 11) ? S_OK : E_FAIL);
        native.Ensure(nativeData, nativeData.Read(L"Name"), ExePath());
        expect(native.NamespaceMode() && native.Path() == L"::" + nativeData.Read(L"DesktopNamespace") &&
               GetFileAttributesW(oldLink.c_str()) == INVALID_FILE_ATTRIBUTES && ReadSmallFile(sentinel) == "keep-native",
               "legacy shortcut migrates to an independent system object without changing collected file bytes");
        Check(DeleteFileW(sentinel.c_str()) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        ComPtr<IShellFolder> shell;
        Check(SHGetDesktopFolder(shell.put()));
        PIDLIST_ABSOLUTE pidl = nullptr; Check(SHParseDisplayName(native.Path().c_str(), nullptr, &pidl, 0, nullptr));
        struct PidlScope { PIDLIST_ABSOLUTE item; ~PidlScope() { CoTaskMemFree(item); } } scope{pidl};
        PCUITEMID_CHILD child = pidl;
        SFGAOF flags = SFGAO_LINK | SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_CANRENAME | SFGAO_DROPTARGET;
        Check(shell->GetAttributesOf(1, &child, &flags));
        expect(!(flags & (SFGAO_LINK | SFGAO_FILESYSTEM)) &&
               (flags & (SFGAO_FOLDER | SFGAO_CANRENAME | SFGAO_DROPTARGET)) == (SFGAO_FOLDER | SFGAO_CANRENAME | SFGAO_DROPTARGET),
               "Windows reports a native folder object with rename and drop support and no shortcut identity");
        ComPtr<IContextMenu> menu;
        Check(shell->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu.put())));
        HMENU popup = CreatePopupMenu();
        const auto menuResult = menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL);
        bool hasSettings = false, hasExpand = false, hasReturn = false;
        for (int i = 0; i < GetMenuItemCount(popup); ++i) {
            wchar_t text[512]{}; GetMenuStringW(popup, i, text, 512, MF_BYPOSITION);
            const std::wstring label = text;
            hasSettings = hasSettings || label.find(L"设置此收纳盒") != std::wstring::npos;
            hasExpand = hasExpand || label.find(L"展开收纳盒") != std::wstring::npos;
            hasReturn = hasReturn || label.find(L"归还内容") != std::wstring::npos;
        }
        DestroyMenu(popup); Check(menuResult);
        expect(hasSettings && hasExpand && hasReturn,
               "Explorer itself supplies the registered storage-box menu without a right-click mouse hook");
        PITEMID_CHILD renamed = nullptr;
        Check(shell->SetNameOf(nullptr, child, L"DesktopBox Windows F2 改名验证", SHGDN_NORMAL, &renamed)); CoTaskMemFree(renamed);
        std::wstring name;
        expect(native.RefreshName(nativeData, name) && name == L"DesktopBox Windows F2 改名验证" && nativeData.Read(L"Name") == name,
               "native Shell rename persists the Windows display name and synchronizes the storage-box title");
        const auto identity = native.Path();
        native.Ensure(nativeData, name, ExePath(), L"", false, true);
        box::NativeEntry restarted; restarted.Ensure(nativeData, name, ExePath(), L"", false, true);
        expect(restarted.Path() == identity && box::DesktopNamespaceName(HKEY_CURRENT_USER, nativeData.Read(L"DesktopNamespace")) == name,
               "restart and icon refresh preserve the same native object and its user-assigned name");
        bool located = false; const auto deadline = GetTickCount64() + 5000;
        do {
            const auto bounds = box::ReadNativeBounds({identity}, box::DesktopSurface());
            located = !bounds.empty() && bounds.front().located;
            if (!located) { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } Sleep(20); }
        } while (!located && GetTickCount64() < deadline);
        expect(located, "the real Windows desktop view enumerates and locates the new namespace icon for hover expansion");
#include "native_drop_checks.inc"
        const auto protectedFile = nativeData.items + L"\\uninstall-keeps-content.txt";
        Check(WriteBytes(protectedFile, "keep-files", 10) ? S_OK : E_FAIL);
        native.Remove(nativeData);
        expect(!box::DesktopNamespacePresent(HKEY_CURRENT_USER, identity.substr(2), nativeData.root) && ReadSmallFile(protectedFile) == "keep-files",
               "unregistering a desktop object leaves the real storage directory and user files intact");
        report += "TOTAL " + std::to_string(total) + " checks\r\n";
    } catch (HRESULT hr) { report += "HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + "\r\n"; result = 1; }
    catch (const std::exception& error) { report += "ERROR "; report += error.what(); report += "\r\n"; result = 1; }
    for (const auto& root : roots) {
        try { Storage data(root); box::NativeEntry entry; entry.Remove(data); }
        catch (...) { report += "ERROR temporary desktop object cleanup failed\r\n"; result = 1; }
    }
    if (!WriteBytes(directory + L"\\results.txt", report.data(), static_cast<DWORD>(report.size()))) return 2;
    return result;
}
