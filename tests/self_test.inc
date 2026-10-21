void MakeTestIcon(const std::wstring& path) {
    // A tiny self-contained ICO fixture for exercising the custom-icon path.
    const int side = 16, pixelBytes = side * side * 4, maskBytes = side * 4;
    std::vector<BYTE> data(22 + sizeof(BITMAPINFOHEADER) + pixelBytes + maskBytes, 0);
    auto put16 = [&](size_t offset, WORD value) { memcpy(data.data() + offset, &value, 2); };
    auto put32 = [&](size_t offset, DWORD value) { memcpy(data.data() + offset, &value, 4); };
    put16(2, 1); put16(4, 1); data[6] = side; data[7] = side;
    put16(10, 1); put16(12, 32); put32(14, static_cast<DWORD>(data.size() - 22)); put32(18, 22);
    BITMAPINFOHEADER header{}; header.biSize = sizeof(header); header.biWidth = side;
    header.biHeight = side * 2; header.biPlanes = 1; header.biBitCount = 32; header.biSizeImage = pixelBytes;
    memcpy(data.data() + 22, &header, sizeof(header));
    for (int i = 0; i < side * side; ++i) { BYTE* pixel = data.data() + 22 + sizeof(header) + i * 4; pixel[0] = 235; pixel[1] = 120; pixel[2] = 50; pixel[3] = 255; }
    if (!WriteBytes(path, data.data(), static_cast<DWORD>(data.size()))) throw E_FAIL;
}

int SelfTest(const std::wstring& directory) {
    std::string report; int total = 0;
    auto expect = [&](bool ok, const char* description) {
        report += std::string(ok ? "PASS " : "FAIL ") + description + "\r\n";
        ++total; if (!ok) throw std::runtime_error(description);
    };
    int result = 0;
    try {
        EnsureDirectory(directory);
        const auto work = RECT{0, 0, 1920, 1040};
        auto placement = box::PlacePanel({1750, 900, 1862, 1008}, work, 368, 364, 10, box::Direction::Automatic);
        expect(placement.opensLeft && placement.rect.right <= 1920 && placement.rect.bottom <= 1040, "right/bottom edge opens left and remains on screen");
        placement = box::PlacePanel({20, 20, 132, 128}, work, 368, 276, 10, box::Direction::Left);
        expect(!placement.opensLeft && placement.rect.left >= 0, "preferred direction falls back at left edge");
        placement = box::PlacePanel({-1800, 900, -1688, 1008}, {-1920, 0, 0, 1080}, 552, 546, 15, box::Direction::Automatic);
        expect(placement.rect.left >= -1920 && placement.rect.right <= 0 && placement.rect.bottom <= 1080, "negative-coordinate monitor and 150 percent geometry");
        placement = box::PlacePanel({60, 10, 172, 118}, {0, 0, 320, 220}, 368, 364, 10, box::Direction::Automatic);
        expect(placement.rect.left == 0 && placement.rect.top == 0 && placement.rect.right == 320 && placement.rect.bottom == 220, "small work area clamps panel dimensions");
        placement = box::PlacePanel({600, 600, 712, 708}, work, 416, 308, 10, box::Direction::Up);
        expect(placement.direction == box::Direction::Up && placement.rect.bottom == 590 && placement.rect.left == 600,
               "upward expansion places the panel above the desktop icon with the configured gap");
        placement = box::PlacePanel({600, 100, 712, 208}, work, 416, 308, 10, box::Direction::Down);
        expect(placement.direction == box::Direction::Down && placement.rect.top == 218,
               "downward expansion places the panel below the desktop icon");
        placement = box::PlacePanel({600, 10, 712, 118}, work, 416, 308, 10, box::Direction::Up);
        expect(placement.direction == box::Direction::Down && placement.rect.top == 128,
               "upward expansion falls back below the icon at the top edge");
        placement = box::PlacePanel({600, 900, 712, 1008}, work, 416, 308, 10, box::Direction::Down);
        expect(placement.direction == box::Direction::Up && placement.rect.bottom == 890,
               "downward expansion falls back above the icon at the bottom edge");
        placement = box::PlacePanel({160, 100, 240, 180}, {0, 0, 500, 1000}, 416, 308, 10, box::Direction::Automatic);
        expect(placement.direction == box::Direction::Down && placement.rect.top == 190 && placement.rect.right <= 500,
               "automatic expansion uses vertical room when neither horizontal side fits");
        {
            box::Selection selected; selected.Reset(8);
            selected.Click(2); selected.Click(5, true);
            expect(selected.Indices() == std::vector<int>({2, 5}), "Ctrl click adds separate selected items");
            selected.Click(2, true);
            expect(selected.Indices() == std::vector<int>({5}), "Ctrl click on a selected item removes only that item");
            selected.Click(5); selected.Click(1, false, true); selected.Click(3, false, true);
            expect(selected.Indices() == std::vector<int>({3, 4, 5}) && selected.Anchor() == 5,
                   "successive reverse Shift ranges keep their original anchor and shrink correctly");
            selected.Click(0, true); selected.Click(2, true, true);
            expect(selected.Indices() == std::vector<int>({0, 1, 2, 3, 4, 5}), "Ctrl Shift extends the range without discarding other selections");
            selected.Focus(7);
            expect(selected.Count() == 6 && selected.Focus() == 7, "keyboard focus can move independently of the selected set");
            selected.Clear(); selected.Click(2); selected.Click(5, true); const auto baseline = selected.Values();
            selected.Box({1, 2}, baseline, true, false); selected.Box({1, 2}, baseline, true, false);
            expect(selected.Indices() == std::vector<int>({1, 5}), "Ctrl marquee toggles against its initial set without flickering on repeated updates");
            selected.Box({1, 2}, baseline, false, true);
            expect(selected.Indices() == std::vector<int>({1, 2, 5}), "Shift marquee adds intersecting items to the initial set");
            selected.Box({1, 3}, baseline, false, false);
            expect(selected.Indices() == std::vector<int>({1, 3}), "plain marquee replaces the previous selected set");
            selected.All(); selected.Set(-1, true); selected.Set(100, true);
            expect(selected.Count() == 8, "select all includes every item and ignores invalid indexes");
            selected.Reset(0); selected.All(); selected.Click(0);
            expect(selected.Count() == 0 && selected.Focus() == -1, "empty containers safely accept selection and keyboard commands");
            expect(box::ReorderSelection(5, {0, 2}, 5) == std::vector<int>({1, 3, 4, 0, 2}),
                   "group reorder moves separated items to the end while retaining relative order");
            expect(box::ReorderSelection(5, {2, 4}, 0) == std::vector<int>({2, 4, 0, 1, 3}),
                   "group reorder moves separated items before the first item");
            expect(box::ReorderSelection(5, {1, 3}, 3) == std::vector<int>({0, 1, 2, 3, 4}),
                   "dropping a group onto one of its own items leaves its order intact");
        }
        auto point = box::ClampAnchor({4000, -300}, {112, 108}, work);
        expect(point.x == 1808 && point.y == 0, "disconnected monitor anchor is clamped");
        point = box::SnapAnchor({121, 175}, {76, 100}, work);
        expect(point.x == 152 && point.y == 200, "desktop anchor snaps to nearest grid cell");
        point = box::SnapAnchor({121, 175}, {76, 100}, work, {{152, 200, 228, 300}});
        expect(!(point.x == 152 && point.y == 200), "desktop grid avoids an occupied native icon cell");
        point = box::SnapAnchor({-1810, 955}, {114, 150}, {-1920, 0, 0, 1040});
        expect((point.x + 1920) % 114 == 0 && point.y % 150 == 0 && point.y + 150 <= 1040, "grid snapping supports negative monitor coordinates and 150 percent cells");
        expect(box::SafeName(L"CON.txt") == L"_CON.txt" && box::SafeName(L"a:b?. ") == L"a_b_", "reserved names and invalid filename characters");
        const auto sandbox = directory + L"\\run-" + std::to_wstring(GetTickCount64()); EnsureDirectory(sandbox);
        Storage storage(sandbox + L"\\data");
#include "optimization_checks.inc"
        const auto source = sandbox + L"\\测试文件.txt";
        expect(WriteBytes(source, "original-data", 13), "create Unicode source fixture");
        auto moved = storage.MoveInto({source}, nullptr, true);
        auto actual = storage.items + L"\\测试文件.txt";
        expect(moved.complete && moved.moved == 1 && GetFileAttributesW(actual.c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES, "file itself moves into shelf; no shortcut created");
        expect(storage.OriginalPath(actual) == source, "original absolute path is journaled");
        expect(ReadSmallFile(actual) == "original-data", "moved file retains original bytes");
        const auto branch = sandbox + L"\\second"; EnsureDirectory(branch);
        auto duplicate = branch + L"\\测试文件.txt"; WriteBytes(duplicate, "second", 6);
        moved = storage.MoveInto({duplicate}, nullptr, true);
        expect(moved.complete && storage.Entries().size() == 2 && GetFileAttributesW(duplicate.c_str()) == INVALID_FILE_ATTRIBUTES, "same-name files are both kept and have separate origins");
        ComPtr<IShellLinkW> link;
        Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(link.put())));
        Check(link->SetPath(ExePath().c_str())); Check(link->SetArguments(L"--profile \"test profile\"")); Check(link->SetWorkingDirectory(sandbox.c_str()));
        ComPtr<IPersistFile> persist; Check(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persist.put())));
        const auto sourceLink = sandbox + L"\\带参数.lnk"; Check(persist->Save(sourceLink.c_str(), TRUE));
        moved = storage.MoveInto({sourceLink}, nullptr, true);
        Check(persist->Load((storage.items + L"\\带参数.lnk").c_str(), STGM_READ));
        wchar_t arguments[512]{}; Check(link->GetArguments(arguments, 512));
        expect(moved.complete && wcscmp(arguments, L"--profile \"test profile\"") == 0 && GetFileAttributesW(sourceLink.c_str()) == INVALID_FILE_ATTRIBUTES, "existing shortcut itself is moved with arguments preserved");
        auto sourceFolder = sandbox + L"\\目录"; EnsureDirectory(sourceFolder); WriteBytes(sourceFolder + L"\\child.txt", "child", 5);
        moved = storage.MoveInto({sourceFolder}, nullptr, true);
        expect(moved.complete && GetFileAttributesW((storage.items + L"\\目录\\child.txt").c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW(sourceFolder.c_str()) == INVALID_FILE_ATTRIBUTES, "folder and nested file move intact");
        auto url = sandbox + L"\\网页.url"; const char urlData[] = "[InternetShortcut]\r\nURL=https://example.com/\r\n";
        WriteBytes(url, urlData, sizeof(urlData) - 1); moved = storage.MoveInto({url}, nullptr, true);
        expect(moved.complete && GetFileAttributesW(url.c_str()) == INVALID_FILE_ATTRIBUTES, "URL file itself moves into shelf");
        auto ico = sandbox + L"\\custom.ico"; MakeTestIcon(ico); storage.SetCustomIcon(ico);
        const auto selectedIcon = storage.SelectedIconPath();
        Icon loaded(reinterpret_cast<HICON>(LoadImageW(nullptr, selectedIcon.c_str(), IMAGE_ICON, 48, 48, LR_LOADFROMFILE)));
        expect(loaded.value && box::ParentPath(selectedIcon) == storage.iconDirectory && GetFileAttributesW(ico.c_str()) != INVALID_FILE_ATTRIBUTES,
               "ICO imported into dedicated library and selected without depending on external source");
        bool invalidRejected = false; try { storage.SetCustomIcon(actual); } catch (HRESULT) { invalidRejected = true; }
        expect(invalidRejected && storage.SelectedIconPath() == selectedIcon, "invalid ICO leaves current icon unchanged");
        const auto draftIcon = sandbox + L"\\preview.ico"; MakeTestIcon(draftIcon);
        const auto importedDraft = storage.ImportIcon(draftIcon);
        expect(!importedDraft.empty() && storage.SelectedIconPath() == selectedIcon &&
               GetFileAttributesW((storage.iconDirectory + L"\\" + importedDraft).c_str()) != INVALID_FILE_ATTRIBUTES,
               "settings icon preview imports its asset without changing the saved icon selection");
        expect(DeleteFileW(ico.c_str()) && GetFileAttributesW(selectedIcon.c_str()) != INVALID_FILE_ATTRIBUTES, "ICO library survives removal of external uploaded file");
        Icon scaled(reinterpret_cast<HICON>(LoadImageW(nullptr, selectedIcon.c_str(), IMAGE_ICON, 72, 72, LR_LOADFROMFILE)));
        ICONINFO iconInfo{}; BITMAP iconBitmap{};
        bool sized = scaled.value && GetIconInfo(scaled.value, &iconInfo);
        if (sized) sized = GetObjectW(iconInfo.hbmColor, sizeof(iconBitmap), &iconBitmap) && iconBitmap.bmWidth == 72 && iconBitmap.bmHeight == 72;
        if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor); if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        expect(sized, "custom ICO adapts to a 150 percent display size");
        storage.Write(L"X", L"-1800"); storage.Write(L"Y", L"250"); storage.Write(L"Direction", L"1"); storage.Write(L"Name", L"工作 📁");
        Storage restored(storage.root, storage.iconDirectory, storage.journalDirectory);
        expect(restored.ReadInt(L"X", 0) == -1800 && restored.ReadInt(L"Direction", 0) == 1 && restored.Read(L"Name") == L"工作 📁" &&
               restored.SelectedIconPath() == selectedIcon && restored.OriginalPath(actual) == source, "name, position, ICO selection and origins persist across fresh storage instance");
        Storage other(sandbox + L"\\other-box", storage.iconDirectory, storage.journalDirectory);
        other.Write(L"Name", L"独立盒子");
        expect(other.Read(L"Name") == L"独立盒子" && storage.Read(L"Name") == L"工作 📁", "multiple boxes keep independent names and settings");
        other.MoveInto({actual}, nullptr, true);
        auto otherActual = other.items + L"\\测试文件.txt";
        storage.ForgetMovedOut(actual);
        expect(other.OriginalPath(otherActual) == source, "moving between boxes preserves first original location");
        expect(other.RestoreAll(nullptr, L"", true).complete && GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES,
               "deleting second box restores item to first original location");
        auto returned = storage.RestoreAll(nullptr, L"", true);
        expect(returned.complete && storage.Entries().empty() && GetFileAttributesW(duplicate.c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW(sourceLink.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW((sourceFolder + L"\\child.txt").c_str()) != INVALID_FILE_ATTRIBUTES &&
               GetFileAttributesW(url.c_str()) != INVALID_FILE_ATTRIBUTES, "delete-box restore returns every file/folder/link to its own original location");
        storage.MoveInto({source}, nullptr, true); WriteBytes(source, "occupied", 8);
        auto conflict = storage.RestoreAll(nullptr, L"", true);
        expect(conflict.complete && storage.Entries().empty() && GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES,
               "occupied original path keeps both files instead of overwriting");
        bool recoveredPayload = false; WIN32_FIND_DATAW restoredFile{};
        HANDLE restoredSearch = FindFirstFileW((sandbox + L"\\测试文件*.txt").c_str(), &restoredFile);
        if (restoredSearch != INVALID_HANDLE_VALUE) {
            do { if (ReadSmallFile(sandbox + L"\\" + restoredFile.cFileName) == "original-data") recoveredPayload = true; } while (FindNextFileW(restoredSearch, &restoredFile));
            FindClose(restoredSearch);
        }
        expect(ReadSmallFile(source) == "occupied" && recoveredPayload, "restore conflict preserves both original payloads byte-for-byte");
        auto outsider = storage.items + L"\\untracked.txt"; WriteBytes(outsider, "untracked", 9);
        bool unknownRejected = false; try { storage.RestoreAll(nullptr, L"", true); } catch (HRESULT) { unknownRejected = true; }
        expect(unknownRejected && GetFileAttributesW(outsider.c_str()) != INVALID_FILE_ATTRIBUTES, "unknown-origin content prevents silent deletion");
        auto fallback = sandbox + L"\\chosen-return"; EnsureDirectory(fallback);
        expect(storage.RestoreAll(nullptr, fallback, true).complete && GetFileAttributesW((fallback + L"\\untracked.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
               "explicit return folder recovers untracked content");
        const auto vanished = sandbox + L"\\vanished-parent"; EnsureDirectory(vanished);
        const auto vanishedFile = vanished + L"\\restore.txt"; WriteBytes(vanishedFile, "restore", 7);
        storage.MoveInto({vanishedFile}, nullptr, true); RemoveDirectoryW(vanished.c_str());
        expect(storage.RestoreAll(nullptr, L"", true).complete && ReadSmallFile(vanishedFile) == "restore", "restore recreates a removed original parent folder");
        wchar_t temporaryPath[32768]{}; GetTempPathW(32768, temporaryPath);
        const auto temporaryRoot = AbsolutePath(temporaryPath);
        if (towupper(temporaryRoot[0]) != towupper(sandbox[0])) {
            const auto temporaryDirectory = temporaryRoot + L"\\DesktopBox-selftest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
            EnsureDirectory(temporaryDirectory);
            const auto crossSource = temporaryDirectory + L"\\cross-volume.txt"; WriteBytes(crossSource, "cross-volume-data", 17);
            auto crossMoved = storage.MoveInto({crossSource}, nullptr, true);
            expect(crossMoved.complete && ReadSmallFile(storage.items + L"\\cross-volume.txt") == "cross-volume-data" && GetFileAttributesW(crossSource.c_str()) == INVALID_FILE_ATTRIBUTES, "cross-volume move transfers actual file and preserves bytes");
            expect(storage.RestoreAll(nullptr, L"", true).complete && ReadSmallFile(crossSource) == "cross-volume-data", "cross-volume restore returns file to original drive");
            DeleteFileW(crossSource.c_str()); RemoveDirectoryW(temporaryDirectory.c_str());
        } else report += "SKIP cross-volume fixture: temp and workspace use same drive\r\n";
        bool selfRejected = false; try { storage.MoveInto({storage.root}, nullptr, true); } catch (HRESULT) { selfRejected = true; }
        expect(selfRejected && GetFileAttributesW(storage.root.c_str()) != INVALID_FILE_ATTRIBUTES, "moving shelf into itself is rejected before any move");
        storage.Write(L"Deleted", L"1"); Storage deleted(storage.root);
        expect(deleted.ReadInt(L"Deleted", 0) == 1, "deleted-box state persists across restart");
        {
            const auto desktop = sandbox + L"\\desktop-fixture"; EnsureDirectory(desktop);
            Storage nativeData(sandbox + L"\\native-box", storage.iconDirectory);
            nativeData.Write(L"Name", L"原生入口"); nativeData.SetCustomIcon(selectedIcon);
            box::NativeEntry native;
            native.Ensure(nativeData, L"原生入口", ExePath(), desktop);
            ComPtr<IShellLinkW> shortcut; ComPtr<IPersistFile> file;
            Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(shortcut.put())));
            Check(shortcut->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()))); Check(file->Load(native.Path().c_str(), STGM_READ));
            wchar_t target[32768]{}, args[32768]{}, iconPath[32768]{}; int iconIndex = 0;
            shortcut->GetPath(target, 32768, nullptr, SLGP_RAWPATH); shortcut->GetArguments(args, 32768); shortcut->GetIconLocation(iconPath, 32768, &iconIndex);
            expect(_wcsicmp(target, ExePath().c_str()) == 0 && std::wstring(args).find(L"--open-box") != std::wstring::npos && std::wstring(args).find(L"--drop") != std::wstring::npos,
                   "native desktop shortcut launches its own box and accepts dropped original file paths");
            expect(nativeData.SelectedIconPath() == iconPath, "native Windows icon uses the persisted ICO library file");
            const auto renamed = desktop + L"\\Windows 重命名.lnk";
            expect(MoveFileW(native.Path().c_str(), renamed.c_str()) != FALSE, "simulate an Explorer rename of native desktop entry");
            std::wstring name;
            expect(native.RefreshName(nativeData, name) && name == L"Windows 重命名" && nativeData.Read(L"Name") == name,
                   "Windows file rename synchronizes box title and persistent name");
            native.Ensure(nativeData, name, ExePath(), desktop);
            expect(native.Path() == renamed, "restart reuses native desktop entry instead of creating a duplicate");
            auto alternateRoot = nativeData.root;
            std::transform(alternateRoot.begin(), alternateRoot.end(), alternateRoot.begin(), [](wchar_t c) {
                return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c;
            });
            Storage alternateCase(alternateRoot, nativeData.iconDirectory);
            native.Ensure(alternateCase, name, ExePath(), desktop);
            expect(native.Path() == renamed, "launcher path casing does not create a duplicate desktop entry");
            native.Remove(nativeData);
            expect(GetFileAttributesW(renamed.c_str()) == INVALID_FILE_ATTRIBUTES, "deleting a box removes only its owned native entry");
            {
                Storage otherBox(sandbox + L"\\same-name-box", storage.iconDirectory);
                box::NativeEntry first, second;
                first.Ensure(nativeData, L"同名入口", ExePath(), desktop);
                second.Ensure(otherBox, L"同名入口", ExePath(), desktop);
                const auto numberedPath = second.Path();
                expect(numberedPath != first.Path() && GetFileAttributesW(numberedPath.c_str()) != INVALID_FILE_ATTRIBUTES,
                       "same-name boxes receive separate native desktop entries");
                second.Ensure(otherBox, L"同名入口", ExePath(), desktop);
                second.Ensure(otherBox, L"同名入口", ExePath(), desktop);
                expect(second.Path() == numberedPath && otherBox.Read(L"DesktopLink") == numberedPath,
                       "restart and icon refresh reuse the numbered desktop entry without renaming it");
                second.Ensure(otherBox, L"主动改名", ExePath(), desktop, true);
                expect(box::BaseName(second.Path()) == L"主动改名.lnk" && GetFileAttributesW(numberedPath.c_str()) == INVALID_FILE_ATTRIBUTES,
                       "explicit rename still changes an owned numbered desktop entry");
                second.Remove(otherBox); first.Remove(nativeData);
            }
            nativeData.Write(L"BackgroundMode", L"2"); nativeData.Write(L"GlassTransparency", L"63"); nativeData.Write(L"GlassBlur", L"27");
            Storage loaded(nativeData.root, nativeData.iconDirectory);
            expect(loaded.ReadInt(L"BackgroundMode", 0) == 2 && loaded.ReadInt(L"GlassTransparency", 0) == 63 && loaded.ReadInt(L"GlassBlur", 0) == 27,
                   "background mode, glass transparency and blur persist across reload");
            const auto dragSource = sandbox + L"\\drag-original.txt"; WriteBytes(dragSource, "drag-data", 9);
            nativeData.MoveInto({dragSource}, nullptr, true); const auto dragInside = nativeData.items + L"\\drag-original.txt";
            nativeData.ForgetMovedOut(dragInside);
            expect(nativeData.OriginalPath(dragInside) == dragSource, "cancelled external drag retains original-location journal");
            const auto outside = sandbox + L"\\drag-outside.txt";
            Check(MoveFileW(dragInside.c_str(), outside.c_str()) ? S_OK : HRESULT_FROM_WIN32(GetLastError())); nativeData.ForgetMovedOut(dragInside);
            expect(ReadSmallFile(outside) == "drag-data" && nativeData.OriginalPath(dragInside).empty(), "external move preserves bytes and retires only the departed item's origin");
            const auto renameSource = sandbox + L"\\before-rename.txt"; WriteBytes(renameSource, "rename-bytes", 12);
            nativeData.MoveInto({renameSource}, nullptr, true);
            const auto renameInside = nativeData.items + L"\\before-rename.txt", renamedInside = nativeData.items + L"\\after-rename.txt";
            Check(MoveFileW(renameInside.c_str(), renamedInside.c_str()) ? S_OK : HRESULT_FROM_WIN32(GetLastError())); nativeData.ReconcileOrigins();
            expect(nativeData.OriginalPath(renamedInside) == renameSource, "native file rename preserves original location using Windows file identity");
            expect(nativeData.RestoreAll(nullptr, L"", true).complete && ReadSmallFile(renameSource) == "rename-bytes", "renamed item still restores to its original filename and folder");
            PIDLIST_ABSOLUTE absolute = ILCreateFromPathW(sourceLink.c_str()); PCUITEMID_CHILD child = nullptr;
            ComPtr<IShellFolder> folder; ComPtr<IContextMenu> context;
            Check(SHBindToParent(absolute, IID_IShellFolder, reinterpret_cast<void**>(folder.put()), &child));
            Check(folder->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(context.put()))); CoTaskMemFree(absolute);
            HMENU menu = CreatePopupMenu(); const HRESULT menuStatus = context->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_CANRENAME);
            bool copy = false, properties = false;
            if (SUCCEEDED(menuStatus)) for (UINT command = 0; command < std::min(2048u, static_cast<UINT>(HRESULT_CODE(menuStatus))); ++command) {
                wchar_t verb[128]{}; context->GetCommandString(command, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 128);
                copy = copy || _wcsicmp(verb, L"copy") == 0; properties = properties || _wcsicmp(verb, L"properties") == 0;
            }
            DestroyMenu(menu);
            expect(copy && properties, "Windows Shell supplies real Copy and Properties commands for a stored shortcut");
        }
        {
            const auto groupSource = sandbox + L"\\batch-source"; EnsureDirectory(groupSource);
            const auto otherSource = sandbox + L"\\batch-other"; EnsureDirectory(otherSource);
            const auto first = groupSource + L"\\first.txt", second = otherSource + L"\\second.txt";
            WriteBytes(first, "first-bytes", 11); WriteBytes(second, "second-bytes", 12);
            const auto shortcutPath = groupSource + L"\\shortcut.lnk", folderPath = groupSource + L"\\nested";
            EnsureDirectory(folderPath); WriteBytes(folderPath + L"\\child.txt", "nested-bytes", 12);
            {
                ComPtr<IShellLinkW> shortcut; ComPtr<IPersistFile> saved;
                Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(shortcut.put())));
                Check(shortcut->SetPath(first.c_str())); Check(shortcut->SetArguments(L"--keep parameters"));
                Check(shortcut->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(saved.put()))); Check(saved->Save(shortcutPath.c_str(), TRUE));
            }
            const auto shortcutBytes = ReadSmallFile(shortcutPath);
            ComPtr<IDataObject> invalid;
            expect(box::ShellSelectionObject(nullptr, {first, second}, IID_IDataObject, reinterpret_cast<void**>(invalid.put())) == E_INVALIDARG,
                   "Shell multi-selection rejects unrelated parent folders instead of binding wrong items");
            Storage group(sandbox + L"\\batch-box", storage.iconDirectory, storage.journalDirectory);
            const auto batch = group.MoveInto({first, second, shortcutPath, folderPath}, nullptr, true);
            const auto paths = group.Entries();
            expect(batch.complete && batch.moved == 4 && paths.size() == 4 &&
                   GetFileAttributesW(first.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(second.c_str()) == INVALID_FILE_ATTRIBUTES,
                   "batch import moves original files a shortcut and a folder into one container");
            ComPtr<IDataObject> data;
            Check(box::ShellSelectionObject(nullptr, paths, IID_IDataObject, reinterpret_cast<void**>(data.put())));
            FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
            Check(data->GetData(&format, &medium));
            bool allPaths = false;
            {
                MediumGuard release{medium}; const HDROP drop = static_cast<HDROP>(medium.hGlobal);
                std::vector<std::wstring> actualPaths;
                for (UINT i = 0; i < DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); ++i) {
                    wchar_t path[32768]{}; DragQueryFileW(drop, i, path, 32768); actualPaths.emplace_back(path);
                }
                auto expectedPaths = paths; std::sort(actualPaths.begin(), actualPaths.end()); std::sort(expectedPaths.begin(), expectedPaths.end());
                allPaths = actualPaths == expectedPaths;
            }
            expect(allPaths, "native drag data carries every selected original path without creating shortcuts");
            ComPtr<IContextMenu> menuObject;
            Check(box::ShellSelectionObject(nullptr, paths, IID_IContextMenu, reinterpret_cast<void**>(menuObject.put())));
            HMENU menu = CreatePopupMenu(); const HRESULT menuResult = menuObject->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL);
            bool copy = false, cut = false, remove = false;
            if (SUCCEEDED(menuResult)) for (UINT i = 0; i < std::min(2048u, static_cast<UINT>(HRESULT_CODE(menuResult))); ++i) {
                wchar_t verb[128]{}; menuObject->GetCommandString(i, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb), 128);
                copy = copy || _wcsicmp(verb, L"copy") == 0; cut = cut || _wcsicmp(verb, L"cut") == 0; remove = remove || _wcsicmp(verb, L"delete") == 0;
            }
            DestroyMenu(menu);
            expect(copy && cut && remove, "Windows supplies real batch Copy Cut and Delete commands for the complete selected set");
            const auto destination = sandbox + L"\\batch-out"; EnsureDirectory(destination);
            bool movedAll = true; for (const auto& path : paths) movedAll = group.MoveOut(path, destination, nullptr, true).complete && movedAll;
            expect(movedAll && group.Entries().empty() && ReadSmallFile(destination + L"\\first.txt") == "first-bytes" &&
                   ReadSmallFile(destination + L"\\second.txt") == "second-bytes" &&
                   ReadSmallFile(destination + L"\\shortcut.lnk") == shortcutBytes && ReadSmallFile(destination + L"\\nested\\child.txt") == "nested-bytes",
                   "batch move out preserves file bytes shortcut parameters and nested folder contents");
        }
        {
            box::Backdrop glass; std::vector<DWORD> checker(32 * 32);
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) checker[y * 32 + x] = (x + y) % 2 ? 0xFFFFFFFF : 0xFF000000;
            glass.SetSource(32, 32, checker); glass.Blur(0);
            expect(glass.Pixels() == checker, "zero glass blur preserves background detail exactly");
            glass.Blur(24); unsigned contrast = 0;
            for (DWORD pixel : glass.Pixels()) contrast += static_cast<unsigned>(abs(static_cast<int>(pixel & 255) - 128));
            expect(contrast < 32 * 32 * 30, "higher glass blur measurably reduces background contrast");
            std::vector<DWORD> painted(32 * 32);
            glass.Draw(painted.data(), 32, 32, 0, RGB(10, 20, 30));
            expect(std::all_of(painted.begin(), painted.end(), [](DWORD pixel) { return pixel == 0xFF0A141E; }), "zero transparency produces an opaque tint without changing text opacity");
            glass.Draw(painted.data(), 32, 32, 100, RGB(10, 20, 30));
            expect(painted == glass.Pixels(), "full transparency reveals the independently blurred background");
            glass.Clear(); expect(!glass.Matches(32, 32), "closing glass panel releases its background buffers");
        }
        {
            const auto renderRoot = sandbox + L"\\render-box";
            App preview(GetModuleHandleW(nullptr), renderRoot, storage.iconDirectory, storage.journalDirectory, {}, {});
            WriteBytes(preview.storage.items + L"\\render.txt", "render", 6);
            preview.Initialize(false, false); // No message pump or first WM_PAINT: the initial layered bitmap must already exist.
            std::vector<DWORD> pixels;
            expect(SUCCEEDED(preview.RenderAnchor(&pixels)), "layered anchor submits successfully before first WM_PAINT");
            RECT bounds{}; GetClientRect(preview.host, &bounds);
            auto visibleIn = [&](RECT region) {
                int count = 0;
                for (LONG y = region.top; y < region.bottom; ++y)
                    for (LONG x = region.left; x < region.right; ++x)
                        if ((pixels[y * bounds.right + x] >> 24) > 200) ++count;
                return count;
            };
            expect(visibleIn(preview.AnchorIconRect()) > preview.Px(100), "transparent anchor retains visible Windows folder icon");
            expect(visibleIn(preview.AnchorLabelRect()) > 20, "transparent anchor retains readable box name");
            const RECT icon = preview.AnchorIconRect();
            expect(visibleIn({icon.right - preview.Px(12), preview.Px(2), icon.right + preview.Px(10), preview.Px(21)}) > 100, "transparent anchor retains item-count badge");
            expect(visibleIn({0, 0, preview.Px(2), bounds.bottom}) == 0 && (pixels.front() >> 24) <= 1 && (pixels.back() >> 24) <= 1,
                   "anchor edges have no opaque white card or border");
            LOGFONTW actualFont{}, systemFont{};
            GetObjectW(preview.desktopFont_, sizeof(actualFont), &actualFont);
            const bool queriedFont = SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(systemFont), &systemFont, 0, preview.dpi_) != FALSE;
            expect(queriedFont && actualFont.lfHeight == systemFont.lfHeight && _wcsicmp(actualFont.lfFaceName, systemFont.lfFaceName) == 0,
                   "anchor uses Windows desktop icon font and size");
            int smooth = 0; bool premultiplied = true;
            const auto label = preview.AnchorLabelRect();
            for (LONG y = label.top; y < label.bottom; ++y) for (LONG x = label.left; x < label.right; ++x) {
                const DWORD pixel = pixels[y * bounds.right + x], alpha = pixel >> 24;
                if (alpha > 1 && alpha < 240 && (pixel & 255)) ++smooth;
                if ((pixel & 255) > alpha || ((pixel >> 8) & 255) > alpha || ((pixel >> 16) & 255) > alpha) premultiplied = false;
            }
            expect(smooth > 10 && premultiplied, "text keeps smooth premultiplied alpha edges without an opaque black fringe");
            BITMAPFILEHEADER fileHeader{}; BITMAPV4HEADER header{};
            header.bV4Size = sizeof(header); header.bV4Width = bounds.right; header.bV4Height = -bounds.bottom;
            header.bV4Planes = 1; header.bV4BitCount = 32; header.bV4V4Compression = BI_BITFIELDS;
            header.bV4RedMask = 0x00FF0000; header.bV4GreenMask = 0x0000FF00; header.bV4BlueMask = 0x000000FF; header.bV4AlphaMask = 0xFF000000;
            fileHeader.bfType = 0x4D42; fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(header);
            fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(DWORD));
            std::vector<BYTE> bitmap(fileHeader.bfSize);
            memcpy(bitmap.data(), &fileHeader, sizeof(fileHeader)); memcpy(bitmap.data() + sizeof(fileHeader), &header, sizeof(header));
            memcpy(bitmap.data() + fileHeader.bfOffBits, pixels.data(), pixels.size() * sizeof(DWORD));
            WriteBytes(directory + L"\\anchor-preview.bmp", bitmap.data(), static_cast<DWORD>(bitmap.size()));
            preview.storage.SetCustomIcon(selectedIcon); preview.LoadAppearance();
            expect(SUCCEEDED(preview.RenderAnchor(&pixels)) && visibleIn(preview.AnchorIconRect()) > preview.Px(100),
                   "custom ICO stays visible on transparent anchor after appearance refresh");
            preview.Reveal(); preview.HidePanel();
            expect(IsWindowVisible(preview.host) && !IsWindowVisible(preview.panel), "closing only the content panel leaves the desktop entrance visible");
            SendMessageW(preview.host, WM_CLOSE, 0, 0);
            expect(IsWindowVisible(preview.host), "close message no longer hides the desktop entrance");
            WriteBytes(preview.storage.items + L"\\a.txt", "a", 1); WriteBytes(preview.storage.items + L"\\b.txt", "b", 1);
            preview.Reload(); const auto first = preview.items_.front().path; preview.Reorder(0, 2); preview.Reload();
            expect(preview.items_.back().path == first && ReadSmallFile(first) == "a", "internal drag reorders persist without moving or changing file contents");
            const auto longName = std::wstring(40, L'长') + L".txt";
            WriteBytes(preview.storage.items + L"\\" + longName, "long", 4); preview.Reload(); preview.PositionPanel();
            const auto item = std::find_if(preview.items_.begin(), preview.items_.end(), [&](const auto& entry) { return entry.label == longName; });
            wchar_t fullName[512]{}; TOOLINFOW tool{}; tool.cbSize = TTTOOLINFOW_V2_SIZE; tool.hwnd = preview.panel;
            tool.uId = static_cast<UINT_PTR>(item - preview.items_.begin() + 1); tool.lpszText = fullName;
            SendMessageW(preview.tooltip_, TTM_GETTEXTW, 512, reinterpret_cast<LPARAM>(&tool));
            expect(longName == fullName, "long filename tooltip exposes the complete untruncated name");
            preview.backgroundMode_ = 0; preview.glassTransparency_ = 63; preview.glassBlur_ = 27;
            preview.previewIconFile_ = preview.storage.Read(L"IconFile");
            preview.settings_ = std::make_unique<box::FluentSettings>();
            const HWND settings = CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), preview.panel,
                App::AppearanceProc, reinterpret_cast<LPARAM>(&preview));
            Check(settings ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
            expect(SendDlgItemMessageW(settings, 1101, CB_GETCOUNT, 0, 0) == 4 &&
                   SendDlgItemMessageW(settings, 1102, TBM_GETRANGEMIN, 0, 0) == 0 &&
                   SendDlgItemMessageW(settings, 1102, TBM_GETRANGEMAX, 0, 0) == 100 &&
                   SendDlgItemMessageW(settings, 1103, TBM_GETRANGEMAX, 0, 0) == 40 &&
                   SendDlgItemMessageW(settings, 1102, TBM_GETPOS, 0, 0) == 63 &&
                   SendDlgItemMessageW(settings, 1103, TBM_GETPOS, 0, 0) == 27,
                   "Fluent dialog binds saved values to native controls with unchanged options and ranges");
            SendDlgItemMessageW(settings, 1102, TBM_SETPOS, TRUE, 42);
            SendDlgItemMessageW(settings, 1103, TBM_SETPOS, TRUE, 39);
            SendMessageW(settings, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(GetDlgItem(settings, 1102)));
            wchar_t opacity[32]{}, blurValue[32]{};
            GetDlgItemTextW(settings, 1104, opacity, 32); GetDlgItemTextW(settings, 1105, blurValue, 32);
            expect(preview.glassTransparency_ == 42 && preview.glassBlur_ == 39 && wcscmp(opacity, L"42%") == 0 && wcscmp(blurValue, L"39") == 0 &&
                   preview.storage.Read(L"GlassTransparency").empty(),
                   "slider notifications update actual preview values and badges without committing settings");
            SendDlgItemMessageW(settings, 1101, CB_SETCURSEL, 2, 0);
            SendMessageW(settings, WM_COMMAND, MAKEWPARAM(1101, CBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(settings, 1101)));
            expect(preview.backgroundMode_ == 2 && !IsWindowEnabled(GetDlgItem(settings, 1102)) && !IsWindowEnabled(GetDlgItem(settings, 1103)) &&
                   SendDlgItemMessageW(settings, 1102, TBM_GETPOS, 0, 0) == 42 && SendDlgItemMessageW(settings, 1103, TBM_GETPOS, 0, 0) == 39,
                   "solid background disables glass controls without discarding either parameter");
            expect(LOWORD(SendMessageW(settings, DM_GETDEFID, 0, 0)) == IDOK &&
                   (SendDlgItemMessageW(settings, IDOK, WM_GETDLGCODE, 0, 0) & DLGC_DEFPUSHBUTTON) != 0 &&
                   (GetWindowLongPtrW(GetDlgItem(settings, IDCANCEL), GWL_STYLE) & BS_TYPEMASK) == BS_PUSHBUTTON,
                   "styled dialog retains native default and cancel buttons for dialog keyboard handling");
            DestroyWindow(settings); preview.settings_.reset(); preview.previewIconFile_.clear();
            {
                box::DesktopMouse listener;
                expect(listener.Start(nullptr, kNativeMouse) == E_INVALIDARG && listener.ThreadId() == 0,
                       "desktop mouse listener rejects an invalid owner without creating a thread");
                Check(listener.Start(preview.host, kNativeMouse));
                KernelHandle listenerThread(OpenThread(SYNCHRONIZE, FALSE, listener.ThreadId()));
                expect(listener.ThreadId() != GetCurrentThreadId() && listenerThread.value &&
                       WaitForSingleObject(listenerThread.value, 0) == WAIT_TIMEOUT,
                       "desktop mouse hook installs on a live thread separate from the Shell and UI thread");
                expect(listener.RawInputEnabled(), "mouse monitoring independently registers background Raw Input");
                UINT deviceCount = 0; GetRegisteredRawInputDevices(nullptr, &deviceCount, sizeof(RAWINPUTDEVICE));
                std::vector<RAWINPUTDEVICE> devices(deviceCount);
                GetRegisteredRawInputDevices(devices.data(), &deviceCount, sizeof(RAWINPUTDEVICE));
                expect(std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
                    return device.usUsagePage == 1 && device.usUsage == 2 && device.hwndTarget == preview.host && (device.dwFlags & RIDEV_INPUTSINK);
                }), "Raw Input routes background mouse events to the application window without disabling ordinary mouse messages");
                const unsigned installed = listener.HookInstallations();
                expect(listener.RequestHookRepair(), "a stalled hook can request recovery on its own listener thread");
                const ULONGLONG repairStart = GetTickCount64();
                while (listener.HookInstallations() == installed && GetTickCount64() - repairStart < 1500) Sleep(1);
                expect(listener.HookInstallations() > installed && listener.ThreadId() != GetCurrentThreadId(),
                       "hook recovery completes without restarting the program or its interface");
                const std::vector<box::DesktopContextTarget> targets{{{100, 100, 176, 200}, 123}};
                listener.SetContextTargets(preview.host, targets);
                listener.SetPointerContext({120, 120}, false, GetForegroundWindow());
                expect(listener.ContextHit({120, 120}) == 0, "a cached icon under another window cannot intercept right clicks");
                listener.SetPointerContext({120, 120}, true, GetForegroundWindow());
                expect(listener.ContextHit({120, 120}) == 123 && listener.ContextHit({20, 20}) == 0,
                       "right click callback reads only a UI approved cached icon region");
                listener.SetPointerContext({120, 120}, true, nullptr);
                expect(listener.ContextHit({120, 120}) == 0, "a foreground window change invalidates cached desktop right click approval");
                std::atomic<int> snapshotReads{0};
                std::thread reader([&] { for (int i = 0; i < 20000; ++i) { listener.ContextHit({120, 120}); ++snapshotReads; } });
                for (int i = 0; i < 20000; ++i) { listener.SetContextTargets(preview.host, targets); listener.SetPointerContext({120, 120}, true, nullptr); }
                reader.join();
                expect(snapshotReads == 20000, "concurrent snapshot publication and callback hit testing finish while the UI is not pumping messages");
                listener.Stop();
                expect(listener.ThreadId() == 0 && WaitForSingleObject(listenerThread.value, 0) == WAIT_OBJECT_0,
                       "desktop mouse listener joins and removes its hook before its owner is destroyed");
            }
            {
                box::DesktopMouse rawOnly;
                Check(rawOnly.Start(preview.host, kNativeMouse, 0, false));
                expect(rawOnly.RawInputEnabled() && rawOnly.HookInstallations() == 0,
                       "background mouse monitoring remains available with no low level hook installed");
                MSG notification{};
                while (PeekMessageW(&notification, preview.host, kNativeMouse, kNativeMouse, PM_REMOVE)) {}
                for (int i = 0; i < 100; ++i) rawOnly.ObserveRawMouse(0);
                expect(rawOnly.RawEvents() == 100 && PeekMessageW(&notification, preview.host, kNativeMouse, kNativeMouse, PM_REMOVE) &&
                       !PeekMessageW(&notification, preview.host, kNativeMouse, kNativeMouse, PM_REMOVE) && !rawOnly.Consume(),
                       "Raw Input wakes hover without a hook and coalesces a burst into one pending UI notification");
                rawOnly.ObserveRawMouse(RI_MOUSE_LEFT_BUTTON_UP);
                expect(PeekMessageW(&notification, preview.host, kNativeMouse, kNativeMouse, PM_REMOVE) && rawOnly.Consume(),
                       "Raw Input independently refreshes native icon geometry after a button release");
                expect(!rawOnly.HandleRawInput(0), "invalid raw input data does not notify or corrupt listener state");
                rawOnly.Stop();
                expect(!rawOnly.RawInputEnabled() && rawOnly.ThreadId() == 0, "stopping mouse monitoring unregisters its raw device and joins its worker");
            }
            {
                FlagGuard nativeMode(preview.nativeMode_);
                preview.HidePanel(); preview.nativeLocated_ = true;
                preview.nativeImage_ = {100, 100, 148, 148};
                preview.NativeMouse({120, 120}, false, false);
                expect(!IsWindowVisible(preview.panel), "covered desktop icon never opens a panel through another window");
                preview.NativeMouse({90, 120}, true, false);
                expect(!IsWindowVisible(preview.panel), "blank grid space beside the native icon does not trigger hover");
                preview.NativeMouse({120, 120}, true, true);
                expect(!IsWindowVisible(preview.panel), "dragging a native desktop icon does not open its panel");
                preview.NativeMouse({120, 120}, true, false);
                expect(IsWindowVisible(preview.panel), "native icon hover opens immediately without any tray action");
                preview.HidePanel(); preview.nativeLocated_ = preview.nativePointerInside_ = false;
            }
        }
        {
            App grid(GetModuleHandleW(nullptr), sandbox + L"\\selection-grid", storage.iconDirectory, storage.journalDirectory, {}, {});
            const std::vector<std::wstring> manual{L"file10.txt", L"beta.txt", L"file2.txt", L"Alpha.txt", L"charlie.txt"};
            const std::vector<ULONGLONG> times{50, 10, 30, 100, 70};
            for (size_t i = 0; i < manual.size(); ++i) {
                const auto path = grid.storage.items + L"\\" + manual[i]; WriteBytes(path, "fixture", 7);
                WritePrivateProfileStringW(L"Order", manual[i].c_str(), std::to_wstring(i).c_str(), grid.storage.ini.c_str());
                KernelHandle file(CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
                const ULONGLONG ticks = 133000000000000000ULL + times[i] * 10000000ULL;
                const FILETIME stamp{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
                Check(SetFileTime(file.value, nullptr, nullptr, &stamp) ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
            }
            grid.motionAllowed_ = false; // Selection tests use settled positions; motion is verified separately below.
            grid.storage.Write(L"Direction", L"3"); grid.Initialize(false, false); grid.ShowPanel();
            expect(grid.direction_ == box::Direction::Up, "saved upward direction is loaded without being clamped to the old left right range");
            grid.Command(kDirectionDown);
            expect(grid.direction_ == box::Direction::Down && grid.storage.ReadInt(L"Direction", -1) == 4,
                   "downward direction command updates the actual layout setting and persists it");
            auto names = [&] { std::vector<std::wstring> result; for (const auto& item : grid.items_) result.push_back(box::BaseName(item.path)); return result; };
            expect(names() == manual, "manual sorting reads the saved item order");
            grid.selection_.Click(0); grid.selection_.Click(2, true); const auto originalSelection = grid.SelectedPaths();
            grid.Command(kSortName);
            expect(names() == std::vector<std::wstring>({L"Alpha.txt", L"beta.txt", L"charlie.txt", L"file2.txt", L"file10.txt"}),
                   "A to Z sorting uses real names with natural numeric ordering");
            auto selectedAfterSort = grid.SelectedPaths(), selectedBeforeSort = originalSelection;
            std::sort(selectedAfterSort.begin(), selectedAfterSort.end()); std::sort(selectedBeforeSort.begin(), selectedBeforeSort.end());
            expect(selectedAfterSort == selectedBeforeSort, "sorting preserves selection by file identity rather than old grid indexes");
            grid.Command(kSortModified);
            expect(names() == std::vector<std::wstring>({L"Alpha.txt", L"charlie.txt", L"file10.txt", L"file2.txt", L"beta.txt"}),
                   "time sorting uses actual last write timestamps from newest to oldest");
            {
                App reopened(GetModuleHandleW(nullptr), grid.storage.root, storage.iconDirectory, storage.journalDirectory, {}, {});
                reopened.Initialize(false, false);
                expect(reopened.sortMode_ == box::SortMode::Modified && reopened.direction_ == box::Direction::Down &&
                       box::BaseName(reopened.items_.front().path) == L"Alpha.txt",
                       "a fresh container instance restores both sorting and vertical expansion settings");
            }
            grid.Command(kSortManual);
            expect(names() == manual, "returning to manual sorting restores the saved drag order");
            grid.selection_.Click(0); grid.selection_.Click(2, true); grid.draggingPaths_ = grid.SelectedPaths();
            const RECT viewport = grid.ContentRect(); POINT blank{viewport.right - 1, viewport.bottom - 1}; ClientToScreen(grid.panel, &blank);
            expect(grid.ReorderDrop(blank), "internal group drop is handled as reorder rather than a physical file move"); grid.draggingPaths_.clear();
            expect(names() == std::vector<std::wstring>({L"beta.txt", L"Alpha.txt", L"charlie.txt", L"file10.txt", L"file2.txt"}) && grid.selection_.Count() == 2,
                   "internal group drag persists relative order and retains the complete selected group");
            grid.Command(kSortName); grid.selection_.Click(0); grid.selection_.Click(1, true); grid.ReorderSelected(static_cast<int>(grid.items_.size()));
            expect(grid.sortMode_ == box::SortMode::Manual && grid.storage.ReadInt(L"SortMode", -1) == 0,
                   "manual dragging after automatic sorting switches to persisted manual order");
            const RECT first = grid.ItemRect(0), second = grid.ItemRect(1);
            const POINT inside{first.left + grid.Px(25), first.top + grid.Px(25)};
            auto pointer = [&](UINT message, POINT point, WPARAM modifiers = 0) { SendMessageW(grid.panel, message, modifiers, MAKELPARAM(point.x, point.y)); };
            grid.selection_.Clear(); pointer(WM_LBUTTONDOWN, inside); pointer(WM_LBUTTONUP, inside);
            expect(grid.selection_.Count() == 1 && IsWindowVisible(grid.panel), "single click selects a file while leaving the panel open");
            const POINT insideSecond{second.left + grid.Px(25), second.top + grid.Px(25)};
            pointer(WM_LBUTTONDOWN, insideSecond, MK_CONTROL); pointer(WM_LBUTTONUP, insideSecond, MK_CONTROL);
            expect(grid.selection_.Count() == 2, "Ctrl click through native mouse messages adds another file without opening it");
            pointer(WM_LBUTTONDOWN, inside);
            expect(grid.selection_.Count() == 2, "pressing a selected file retains the entire group for a potential drag");
            pointer(WM_LBUTTONUP, inside);
            expect(grid.selection_.Count() == 1 && grid.selection_.Contains(0), "releasing a plain click collapses the selected group to its clicked item");
            auto savePanel = [&](const std::wstring& filename) {
                grid.backgroundMode_ = 2; grid.LoadAppearance();
                HDC screen = GetDC(grid.panel); std::vector<DWORD> pixels;
                const HRESULT painted = grid.RenderSurface(grid.panel, true, screen, &pixels); ReleaseDC(grid.panel, screen); Check(painted);
                RECT size{}; GetClientRect(grid.panel, &size);
                BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = size.right; info.biHeight = -size.bottom;
                info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
                BITMAPFILEHEADER header{}; header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(info);
                header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(DWORD));
                std::vector<BYTE> bitmap(header.bfSize);
                memcpy(bitmap.data(), &header, sizeof(header)); memcpy(bitmap.data() + sizeof(header), &info, sizeof(info));
                memcpy(bitmap.data() + header.bfOffBits, pixels.data(), pixels.size() * sizeof(DWORD));
                return WriteBytes(directory + L"\\" + filename, bitmap.data(), static_cast<DWORD>(bitmap.size()));
            };
            const POINT start{viewport.left + 1, viewport.top + 1}, end{second.right - 1, first.bottom - 1};
            pointer(WM_LBUTTONDOWN, start); pointer(WM_MOUSEMOVE, end, MK_LBUTTON);
            expect(grid.marquee_ && grid.selection_.Indices() == std::vector<int>({0, 1}) && IsWindowVisible(grid.panel),
                   "blank space drag displays a marquee and selects all intersected tiles without closing");
            expect(savePanel(L"panel-marquee.bmp"), "native renderer exports the live selection rectangle for visual inspection");
            pointer(WM_LBUTTONUP, end);
            expect(!grid.marquee_ && GetCapture() != grid.panel && grid.selection_.Count() == 2,
                   "finishing a marquee releases capture and keeps its selected set");
            expect(savePanel(L"panel-multiselect.bmp"), "native renderer exports selected checkmarks count and sorting button for visual inspection");
            const POINT reverseStart{second.right + 1, first.bottom + 1};
            pointer(WM_LBUTTONDOWN, reverseStart); pointer(WM_MOUSEMOVE, start, MK_LBUTTON); pointer(WM_LBUTTONUP, start);
            expect(grid.selection_.Indices() == std::vector<int>({0, 1}), "reverse direction marquee selects the same intersecting files");
            pointer(WM_LBUTTONDOWN, start, MK_CONTROL); pointer(WM_MOUSEMOVE, {first.right - 1, first.bottom - 1}, MK_LBUTTON | MK_CONTROL);
            pointer(WM_LBUTTONUP, {first.right - 1, first.bottom - 1}, MK_CONTROL);
            expect(grid.selection_.Indices() == std::vector<int>({1}), "Ctrl marquee toggles only the intersected selected tile");
            const auto beforeCancel = grid.selection_.Values();
            pointer(WM_LBUTTONDOWN, start, MK_SHIFT); pointer(WM_MOUSEMOVE, end, MK_LBUTTON | MK_SHIFT);
            SendMessageW(grid.panel, WM_KEYDOWN, VK_ESCAPE, 0);
            expect(!grid.marquee_ && grid.selection_.Values() == beforeCancel && GetCapture() != grid.panel,
                   "Escape cancels a live marquee and restores its original selection");
            grid.selection_.All(); grid.Reload();
            expect(grid.selection_.Count() == static_cast<int>(manual.size()) &&
                   std::all_of(grid.items_.begin(), grid.items_.end(), [&](const Item& item) { return ReadSmallFile(item.path) == "fixture"; }),
                   "refresh retains all selected surviving files and all bytes after sorting and group reorder");
            for (int i = 0; i < 16; ++i) WriteBytes(grid.storage.items + L"\\extra" + std::to_wstring(i) + L".txt", "fixture", 7);
            grid.Reload(); grid.firstRow_ = 0;
            const RECT scrolling = grid.ContentRect(); const POINT scrollStart{scrolling.left + 1, scrolling.top + 1};
            const POINT scrollEnd{scrolling.right - 1, scrolling.bottom - 1};
            pointer(WM_LBUTTONDOWN, scrollStart); pointer(WM_MOUSEMOVE, scrollEnd, MK_LBUTTON);
            const int initialCount = grid.selection_.Count(); SendMessageW(grid.host, WM_TIMER, kSelectionScrollTimer, 0);
            expect(grid.marquee_ && grid.firstRow_ == 1 && grid.selection_.Count() > initialCount,
                   "marquee edge scrolling extends selection into rows beyond the initial viewport");
            pointer(WM_LBUTTONUP, scrollEnd);
            expect(!grid.marqueeScroll_ && grid.selection_.Count() > initialCount,
                   "marquee scroll timer stops after release while retaining files from earlier rows");
            grid.StopMotion();
            const auto beforeSearch = names();
            SetWindowTextW(grid.searchEdit_, L"FILE .TXT");
            expect(names().size() == 2 && grid.hiddenItems_.size() + grid.items_.size() == beforeSearch.size() &&
                   std::all_of(grid.items_.begin(), grid.items_.end(), [](const Item& item) { return box::MatchesSearch(item.label, L"file"); }),
                   "native search edit filters only this box and preserves every hidden file in memory");
            SendMessageW(grid.panel, WM_KEYDOWN, 'A', 0); grid.selection_.All();
            expect(grid.SelectedPaths().size() == 2, "select all under search includes only visible results");
            const auto firstResult = grid.items_.front().path, lastResult = grid.items_.back().path;
            grid.selection_.Click(0); grid.ReorderSelected(2);
            expect(grid.items_.front().path == lastResult && grid.items_.back().path == firstResult,
                   "drag reordering filtered results swaps their real saved order");
            SendMessageW(grid.searchEdit_, WM_KEYDOWN, VK_ESCAPE, 0);
            auto expected = beforeSearch;
            auto firstPosition = std::find(expected.begin(), expected.end(), box::BaseName(firstResult));
            auto lastPosition = std::find(expected.begin(), expected.end(), box::BaseName(lastResult));
            std::iter_swap(firstPosition, lastPosition);
            expect(names() == expected && grid.searchQuery_.empty() && GetWindowTextLengthW(grid.searchEdit_) == 0,
                   "Escape clears native search and restores all hidden items while retaining their positions around a filtered reorder");
            SetWindowTextW(grid.searchEdit_, L"nothing matches this query");
            expect(grid.items_.empty() && grid.hiddenItems_.size() == beforeSearch.size() && grid.selection_.Count() == 0,
                   "zero search results clear hidden selections without deleting or moving files");
            grid.Reload(); SetWindowTextW(grid.searchEdit_, L"");
            expect(names() == expected && grid.storage.Entries().size() == beforeSearch.size(),
                   "directory refresh while filtering retains every file and restores the same complete order");
            grid.motionAllowed_ = true;
            if (grid.MotionEnabled()) {
                grid.StopMotion(); grid.firstRow_ = 0;
                const auto origin = grid.items_.front().path; const RECT originRect = grid.ItemRect(0);
                grid.Command(kSortName);
                const auto moving = std::find_if(grid.cardMotion_.begin(), grid.cardMotion_.end(), [&](const auto& motion) {
                    return grid.items_[motion.index].path == origin;
                });
                expect(!grid.cardMotion_.empty() && moving != grid.cardMotion_.end() && EqualRect(&moving->from, &originRect),
                       "sorting maps each surviving file identity from its old visible coordinates to the new grid");
                grid.motionStarted_ = GetTickCount64() - 110;
                expect(savePanel(L"panel-sort-motion.bmp"), "native renderer exports a real intermediate sorting animation frame");
                grid.StopMotion();
                SetWindowTextW(grid.searchEdit_, L"FILE");
                expect(grid.items_.size() == 2 && !grid.cardMotion_.empty() && !grid.ghostCards_.empty(),
                       "search animates surviving cards and retains outgoing cards for a short fade");
                grid.motionStarted_ = GetTickCount64() - 110;
                const int movingIndex = grid.cardMotion_.front().index; const RECT intermediate = grid.DisplayRect(movingIndex);
                const POINT middle{(intermediate.left + intermediate.right) / 2, (intermediate.top + intermediate.bottom) / 2};
                expect(grid.HitItem(middle) >= 0 && savePanel(L"panel-search-motion.bmp"),
                       "animated cards remain hit testable at their displayed positions and export a real search frame");
                const auto beforeRestart = grid.DisplayRect(0);
                SetWindowTextW(grid.searchEdit_, L"FILE2");
                const auto restarted = std::find_if(grid.cardMotion_.begin(), grid.cardMotion_.end(), [](const auto& motion) { return motion.index == 0; });
                expect(restarted != grid.cardMotion_.end() && abs(restarted->from.left - beforeRestart.left) <= grid.Px(20) &&
                       grid.ghostCards_.size() <= 12,
                       "a new query continues the current on screen position and bounds outgoing icon memory");
                grid.motionStarted_ = GetTickCount64() - 300; SendMessageW(grid.host, WM_TIMER, kLayoutTimer, 0);
                expect(grid.cardMotion_.empty() && grid.ghostCards_.empty(), "finished animation stops its timer and releases outgoing icons");
                expect(savePanel(L"panel-search.bmp"), "native renderer exports the completed local search layout");
            } else report += "SKIP Windows client area animations are disabled\r\n";
            SetWindowTextW(grid.searchEdit_, L"");
            grid.HidePanel();
            expect(grid.cardMotion_.empty() && grid.ghostCards_.empty() && grid.blendScratch_.empty(),
                   "hiding the panel stops motion and releases all temporary animation buffers");
        }
        {
            expect(box::MatchesSearch(L"常用工具 DesktopBox.LNK", L"工具 desktopbox") &&
                   box::MatchesSearch(L"年度报告.pdf", L"报告 PDF") && box::MatchesSearch(L"Alpha.txt", L"  \t  "),
                   "search matches Chinese and case insensitive terms including extensions and ignores empty whitespace");
            expect(!box::MatchesSearch(L"Alpha.txt", L"Alpha pdf") && !box::MatchesSearch(L"Beta.txt", L"alpha"),
                   "search excludes files that do not match every term");
            const RECT a{0, 0, 94, 108}, b{294, 110, 388, 218};
            const RECT beginning = box::InterpolateRect(a, b, box::LayoutEase(0));
            const RECT middle = box::InterpolateRect(a, b, box::LayoutEase(110));
            const RECT ending = box::InterpolateRect(a, b, box::LayoutEase(220));
            expect(EqualRect(&beginning, &a) && EqualRect(&ending, &b) && middle.left > a.left && middle.left < b.left && middle.top > a.top && middle.top < b.top,
                   "layout motion starts at the old coordinates interpolates between them and finishes exactly at the new coordinates");
            expect(box::LayoutEase(500) == 1. && box::LayoutEase(0, 0) == 1. && box::LayoutEase(55) < box::LayoutEase(110),
                   "layout easing is monotonic and clamps finished or zero length animations");
            box::DesktopRightClick click;
            const std::vector<box::DesktopContextTarget> targets{{{100, 100, 176, 200}, 1}, {{200, 100, 276, 200}, 2}};
            click.Handle(WM_RBUTTONDOWN, {120, 120}, true, targets);
            expect(click.Active() && click.Handle(WM_RBUTTONUP, {120, 120}, true, targets) == 1 && !click.Active(),
                   "right clicking an owned native desktop icon selects its management menu and releases the click pair");
            click.Handle(WM_RBUTTONDOWN, {220, 120}, true, targets);
            expect(click.Handle(WM_RBUTTONUP, {220, 120}, true, targets) == 2,
                   "each native desktop entrance routes its right click to its own box");
            click.Handle(WM_RBUTTONDOWN, {20, 20}, true, targets);
            expect(!click.Active() && !click.Handle(WM_RBUTTONUP, {20, 20}, true, targets),
                   "blank desktop and unrelated icons keep their Windows context menu");
            click.Handle(WM_RBUTTONDOWN, {120, 120}, false, targets);
            expect(!click.Active() && !click.Handle(WM_RBUTTONUP, {120, 120}, false, targets),
                   "a window covering an owned desktop icon is never intercepted");
            click.Handle(WM_RBUTTONDOWN, {120, 120}, true, targets); click.Handle(WM_MOUSEMOVE, {170, 180}, true, targets);
            expect(click.Active() && !click.Handle(WM_RBUTTONUP, {120, 120}, true, targets) && !click.Active(),
                   "moving away during a right click cancels its menu without leaving a captured button");
        }
        {
            struct RegistryFixture {
                HKEY key = nullptr;
                std::wstring path = L"Software\\DesktopBox\\Tests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
                ~RegistryFixture() { if (key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
            } registry;
            Check(HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_CURRENT_USER, registry.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &registry.key, nullptr)));
            auto read = [&](const wchar_t* path, const wchar_t* name) {
                wchar_t value[4096]{}; DWORD bytes = sizeof(value);
                RegGetValueW(registry.key, path, name, RRF_RT_REG_SZ, nullptr, value, &bytes); return std::wstring(value);
            };
            const auto executable = directory + L"\\安装 软件\\DesktopBox.exe", data = directory + L"\\用户 数据";
            const auto iconFile = directory + L"\\shell-new.ico"; MakeTestIcon(iconFile);
            const auto icon = box::ShellNewIcon(iconFile); bool changed = false;
            Check(box::RegisterShellNew(registry.key, executable, data, icon, changed));
            expect(changed && read(L".desktopbox", nullptr) == L"DesktopBox.StorageBox.1" &&
                   read(L"DesktopBox.StorageBox.1", nullptr) == L"桌面收纳盒",
                   "per user ShellNew registration names the native New submenu entry Desktop Storage Box");
            expect(read(L".desktopbox\\ShellNew", L"Command") == box::ShellNewCommand(executable, data) &&
                   read(L".desktopbox\\ShellNew", L"Command").find(L"\" --data-dir \"") != std::wstring::npos &&
                   read(L".desktopbox\\ShellNew", L"Command").find(L"--new-box") != std::wstring::npos,
                   "ShellNew launches the application with correctly quoted Unicode executable and data paths");
            expect(read(L".desktopbox\\ShellNew", L"IconPath") == icon && read(L"DesktopBox.StorageBox.1\\DefaultIcon", nullptr) == icon,
                   "New submenu and file association use the selected ICO icon");
            DWORD bytes = 0;
            expect(RegGetValueW(registry.key, L".desktopbox\\ShellNew", L"NullFile", RRF_RT_ANY, nullptr, nullptr, &bytes) == ERROR_FILE_NOT_FOUND,
                   "ShellNew runs a creation command without leaving an empty placeholder file");
            Check(box::RegisterShellNew(registry.key, executable, data, icon, changed));
            expect(!changed, "repeated automatic Shell registration does not rewrite values or trigger unnecessary Explorer refreshes");
            const auto replacement = directory + L"\\新版\\DesktopBox.exe";
            Check(box::RegisterShellNew(registry.key, replacement, data, box::ShellNewIcon(L""), changed));
            Check(box::UnregisterShellNew(registry.key, executable, changed));
            expect(!changed && read(L"DesktopBox.StorageBox.1", L"DesktopBoxExecutable") == replacement,
                   "uninstalling an old executable preserves a newer installation registration");
            Check(box::UnregisterShellNew(registry.key, replacement, changed));
            expect(changed && read(L".desktopbox", nullptr).empty() && read(L"DesktopBox.StorageBox.1", nullptr).empty(),
                   "uninstaller removes only its owned ShellNew and ProgID keys");
            HKEY foreign = nullptr;
            Check(HRESULT_FROM_WIN32(RegCreateKeyExW(registry.key, L".desktopbox", 0, nullptr, 0, KEY_WRITE, nullptr, &foreign, nullptr)));
            const wchar_t foreignType[] = L"OtherApplication.File";
            RegSetValueExW(foreign, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(foreignType), sizeof(foreignType)); RegCloseKey(foreign);
            expect(FAILED(box::RegisterShellNew(registry.key, executable, data, icon, changed)) && read(L".desktopbox", nullptr) == foreignType,
                   "automatic registration refuses to replace a foreign file association");
        }
        {
            const auto settingsRoot = sandbox + L"\\global-settings-data";
            const auto settingsIcons = sandbox + L"\\global-settings-icons";
            const auto sameAppearance = [](const App& box, const AppearanceSettings& value) {
                return AppearanceSettings::Read(box.storage) == value && box.backgroundMode_ == value.background &&
                       box.glassTransparency_ == value.transparency && box.glassBlur_ == value.blur && box.folderIcon_.value;
            };
            const auto sameTrayPixels = [](HICON actual, const std::wstring& path, HWND window) {
                const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(window));
                Icon expected(reinterpret_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE)));
                if (!actual || !expected.value) return false;
                auto pixels = [size](HICON icon) {
                    std::vector<DWORD> image(static_cast<size_t>(size) * size);
                    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    info.bmiHeader.biWidth = size; info.bmiHeader.biHeight = -size;
                    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
                    void* bits = nullptr;
                    HDC dc = CreateCompatibleDC(nullptr);
                    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                    HGDIOBJ old = SelectObject(dc, bitmap);
                    PatBlt(dc, 0, 0, size, size, BLACKNESS);
                    DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
                    std::copy_n(static_cast<DWORD*>(bits), image.size(), image.begin());
                    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
                    return image;
                };
                return pixels(actual) == pixels(expected.value);
            };
            {
                Manager manager(GetModuleHandleW(nullptr), settingsRoot, settingsIcons); manager.Initialize(false);
                manager.NewBox(L"工作", {500, 200}); manager.NewBox(L"娱乐", {700, 200});
                auto& first = *manager.boxes_.front(); auto& second = *manager.boxes_[1];
                const auto iconA = first.storage.ImportIcon(selectedIcon);
                const auto iconB = first.storage.ImportIcon(directory + L"\\shell-new.ico");
                first.CommitAppearance({1, 21, 8, iconA}); second.CommitAppearance({2, 37, 11, iconB});
                WriteBytes(first.storage.items + L"\\keep.txt", "unchanged", 9);
                const auto originalName = second.storage.Read(L"Name");
                second.Command(kDirectionUp); second.storage.Write(L"SortMode", L"2");
                HMENU trayMenu = manager.CreateTrayMenu();
                expect(GetMenuState(trayMenu, kRenameBox, MF_BYCOMMAND) == static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kDeleteBox, MF_BYCOMMAND) == static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kAddItems, MF_BYCOMMAND) == static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kOpenData, MF_BYCOMMAND) == static_cast<UINT>(-1),
                       "the tray menu excludes rename delete move-in and open-current-folder commands belonging to one box");
                wchar_t menuLabel[128]{}; GetMenuStringW(trayMenu, kAppearance, menuLabel, 128, MF_BYCOMMAND);
                expect(std::wstring(menuLabel) == L"全局设置（所有收纳盒）…" &&
                       GetMenuState(trayMenu, kNewBox, MF_BYCOMMAND) != static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kStartup, MF_BYCOMMAND) != static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kRefresh, MF_BYCOMMAND) != static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kExit, MF_BYCOMMAND) != static_cast<UINT>(-1),
                       "the tray menu offers explicitly global settings together with create startup refresh-all and exit");
                HMENU directions = GetSubMenu(trayMenu, 3); bool noDirectionChecked = directions != nullptr;
                for (UINT command = kDirectionAuto; command <= kDirectionDown; ++command)
                    noDirectionChecked = noDirectionChecked && !(GetMenuState(directions, command, MF_BYCOMMAND) & MF_CHECKED);
                expect(noDirectionChecked, "mixed individual expansion directions do not show a misleading single-box checkmark in the tray");
                DestroyMenu(trayMenu);
                manager.TrayCommand(kRenameBox); manager.TrayCommand(kDeleteBox);
                expect(manager.boxes_.size() == 3 && second.storage.Read(L"Name") == originalName && ReadSmallFile(first.storage.items + L"\\keep.txt") == "unchanged",
                       "the tray command router ignores per-box rename and delete commands");

                struct DialogDriver {
                    HWND owner = nullptr; App* expected = nullptr;
                    std::function<void(HWND)> edit;
                    bool visited = false, failed = false;
                    static BOOL CALLBACK Visit(HWND dialog, LPARAM parameter) {
                        auto& driver = *reinterpret_cast<DialogDriver*>(parameter);
                        wchar_t type[32]{}; GetClassNameW(dialog, type, 32);
                        const auto app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
                        if (wcscmp(type, L"#32770") || !app || (driver.expected && app != driver.expected) || !GetDlgItem(dialog, 1101)) return TRUE;
                        KillTimer(driver.owner, 0xDB20); driver.visited = true;
                        try { driver.edit(dialog); }
                        catch (...) { driver.failed = true; SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); }
                        return FALSE;
                    }
                    static void CALLBACK Tick(HWND owner, UINT, UINT_PTR, DWORD) {
                        auto* driver = reinterpret_cast<DialogDriver*>(GetPropW(owner, L"DesktopBox.SelfTest.Settings"));
                        if (driver) EnumThreadWindows(GetCurrentThreadId(), Visit, reinterpret_cast<LPARAM>(driver));
                    }
                };
                auto exerciseDialog = [&](App* editor, bool global, const AppearanceSettings& draft, bool accept, const std::function<void()>& open) {
                    bool scopeShown = false, controlsBound = false;
                    DialogDriver driver; driver.owner = manager.controller_; driver.expected = editor;
                    driver.edit = [&](HWND dialog) {
                        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
                        wchar_t title[64]{}; GetWindowTextW(dialog, title, 64);
                        scopeShown = app->globalSettings_ == global && std::wstring(title) == (global ? L"全局设置" : L"收纳盒设置");
                        SendDlgItemMessageW(dialog, 1101, CB_SETCURSEL, draft.background, 0);
                        SendDlgItemMessageW(dialog, 1102, TBM_SETPOS, TRUE, draft.transparency);
                        SendDlgItemMessageW(dialog, 1103, TBM_SETPOS, TRUE, draft.blur);
                        SendMessageW(dialog, WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(GetDlgItem(dialog, 1102)));
                        if (draft.icon.empty()) SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dialog, 1141)));
                        else { app->previewIconFile_ = draft.icon; app->LoadAppearance(); app->UpdateAppearanceDialog(dialog); }
                        controlsBound = app->backgroundMode_ == draft.background && app->glassTransparency_ == draft.transparency &&
                                        app->glassBlur_ == draft.blur && app->previewIconFile_ == draft.icon;
                        SendMessageW(dialog, WM_COMMAND, accept ? IDOK : IDCANCEL, 0);
                    };
                    Check(SetPropW(manager.controller_, L"DesktopBox.SelfTest.Settings", &driver) ? S_OK : E_FAIL);
                    Check(SetTimer(manager.controller_, 0xDB20, 10, DialogDriver::Tick) ? S_OK : E_FAIL);
                    try { open(); }
                    catch (...) { KillTimer(manager.controller_, 0xDB20); RemovePropW(manager.controller_, L"DesktopBox.SelfTest.Settings"); throw; }
                    KillTimer(manager.controller_, 0xDB20); RemovePropW(manager.controller_, L"DesktopBox.SelfTest.Settings");
                    expect(driver.visited && !driver.failed && scopeShown && controlsBound,
                           global ? "actual global settings dialog identifies all boxes and binds the edited values" :
                                    "actual individual settings dialog identifies one box and binds its edited values");
                };
                const AppearanceSettings global{0, 72, 26, iconA};
                exerciseDialog(&first, true, global, true, [&] { manager.TrayCommand(kAppearance); });
                expect(std::all_of(manager.boxes_.begin(), manager.boxes_.end(), [&](const auto& box) { return sameAppearance(*box, global); }),
                       "confirming tray settings updates every existing box icon background opacity and blur in memory and on disk");
                expect(manager.trayCustomIcon_ && manager.tray_.hIcon == manager.trayIcon_.value &&
                       sameTrayPixels(manager.trayIcon_.value, settingsIcons + L"\\" + iconA, manager.controller_),
                       "global ICO selection updates the actual notification-area icon pixels");
                expect(ReadSmallFile(first.storage.items + L"\\keep.txt") == "unchanged" && second.storage.Read(L"Name") == originalName &&
                       second.storage.ReadInt(L"Direction", 0) == 3 && second.storage.ReadInt(L"SortMode", 0) == 2,
                       "global appearance leaves files names expansion direction and sorting intact");
                manager.TrayCommand(kDirectionLeft);
                expect(std::all_of(manager.boxes_.begin(), manager.boxes_.end(), [](const auto& box) {
                    return box->Direction() == box::Direction::Left && box->storage.ReadInt(L"Direction", 0) == 1;
                }), "tray expansion direction applies to every box in memory and on disk");
                trayMenu = manager.CreateTrayMenu(); directions = GetSubMenu(trayMenu, 3);
                expect(directions && (GetMenuState(directions, kDirectionLeft, MF_BYCOMMAND) & MF_CHECKED),
                       "the tray direction checkmark represents a setting shared by all boxes");
                DestroyMenu(trayMenu);
                second.Command(kDirectionDown);
                expect(second.Direction() == box::Direction::Down && first.Direction() == box::Direction::Left &&
                       first.storage.ReadInt(L"NewBoxDirection", 0) == 1,
                       "an individual expansion direction remains separate from other boxes and global defaults");
                manager.NewBox(L"继承全局", {900, 200});
                expect(sameAppearance(*manager.boxes_.back(), global), "a new box inherits all globally saved appearance values");
                expect(manager.boxes_.back()->Direction() == box::Direction::Left, "a new box inherits the globally saved expansion direction");

                AppearanceSettings individual = global; individual.icon = iconB;
                exerciseDialog(&second, false, individual, true, [&] { second.Command(kAppearance); });
                expect(sameAppearance(second, individual) && sameAppearance(first, global) && sameAppearance(*manager.boxes_.back(), global) &&
                       AppearanceSettings::NewBoxDefaults(first.storage) == global,
                       "individual ICO settings only change the selected box and preserve other boxes and global defaults");
                expect(manager.trayCustomIcon_ && sameTrayPixels(manager.trayIcon_.value,
                       settingsIcons + L"\\" + iconA, manager.controller_),
                       "changing one box ICO leaves the taskbar icon on the global ICO");
                std::vector<AppearanceSettings> beforeCancel;
                for (const auto& box : manager.boxes_) beforeCancel.push_back(AppearanceSettings::Read(box->storage));
                exerciseDialog(&first, true, {2, 19, 3, L""}, false, [&] { manager.GlobalAppearance(); });
                bool cancelled = AppearanceSettings::NewBoxDefaults(first.storage) == global;
                for (size_t i = 0; i < manager.boxes_.size(); ++i) cancelled = sameAppearance(*manager.boxes_[i], beforeCancel[i]) && cancelled;
                expect(cancelled, "cancelling global background and default-icon previews preserves every box and new-box defaults");
                const AppearanceSettings reset{2, 44, 9, L""};
                exerciseDialog(&first, true, reset, true, [&] { manager.GlobalAppearance(); });
                expect(std::all_of(manager.boxes_.begin(), manager.boxes_.end(), [&](const auto& box) { return sameAppearance(*box, reset); }),
                       "global Restore Default resets all box ICOs including a previously customized individual box");
                expect(!manager.trayCustomIcon_ && manager.trayIcon_.value,
                       "global Restore Default also restores the Windows folder taskbar icon");
                AppearanceSettings rootOnly = reset; rootOnly.icon = iconB;
                first.CommitAppearance(rootOnly);
                manager.NewBox(L"默认图标继承", {1000, 300});
                expect(sameAppearance(*manager.boxes_.back(), reset) && AppearanceSettings::NewBoxDefaults(first.storage) == reset,
                       "an explicit empty global ICO default is not replaced by a later individual icon on the default box");
                for (const auto& box : manager.boxes_) WriteBytes(box->storage.items + L"\\refresh-all.txt", "refresh", 7);
                manager.TrayCommand(kRefresh);
                expect(std::all_of(manager.boxes_.begin(), manager.boxes_.end(), [](const auto& box) {
                    return std::any_of(box->items_.begin(), box->items_.end(), [](const auto& item) { return box::BaseName(item.path) == L"refresh-all.txt"; });
                }), "tray Refresh All reloads the content of every box");
            }
            {
                Manager reopened(GetModuleHandleW(nullptr), settingsRoot, settingsIcons); reopened.Initialize(false);
                const auto defaults = AppearanceSettings::NewBoxDefaults(reopened.boxes_.front()->storage);
                expect(reopened.boxes_.size() == 5 && defaults.background == 2 && defaults.transparency == 44 && defaults.blur == 9 && defaults.icon.empty() &&
                       !reopened.boxes_.front()->storage.Read(L"IconFile").empty() &&
                       std::all_of(reopened.boxes_.begin() + 1, reopened.boxes_.end(), [&](const auto& box) { return sameAppearance(*box, defaults); }),
                       "a full manager restart restores global appearance and the separately customized individual ICO");
                expect(reopened.boxes_.front()->storage.ReadInt(L"NewBoxDirection", 0) == 1 &&
                       std::all_of(reopened.boxes_.begin(), reopened.boxes_.end(), [](const auto& box) {
                           return box->Direction() == (box->storage.Read(L"Name") == L"工作" ? box::Direction::Down : box::Direction::Left);
                       }), "a full manager restart restores global expansion defaults and the individual direction override");
            }
            {
                const auto emptyRoot = sandbox + L"\\empty-global-settings-data";
                Manager empty(GetModuleHandleW(nullptr), emptyRoot, settingsIcons); empty.Initialize(false);
                empty.boxes_.front()->storage.Write(L"Deleted", L"1"); empty.boxes_.clear();
                const AppearanceSettings defaults{1, 36, 14, L""}; empty.ApplyGlobalAppearance(defaults);
                Storage saved(emptyRoot, settingsIcons, emptyRoot + L"\\origins");
                expect(empty.boxes_.empty() && saved.ReadInt(L"Deleted", 0) == 1 && AppearanceSettings::NewBoxDefaults(saved) == defaults,
                       "global defaults can be saved with no boxes without recreating a deleted box");
                HMENU trayMenu = empty.CreateTrayMenu();
                expect(GetMenuState(trayMenu, kAppearance, MF_BYCOMMAND) != static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kRefresh, MF_BYCOMMAND) != static_cast<UINT>(-1) &&
                       GetMenuState(trayMenu, kDeleteBox, MF_BYCOMMAND) == static_cast<UINT>(-1),
                       "the same global tray menu remains available after the last box is deleted");
                DestroyMenu(trayMenu); empty.TrayCommand(kDirectionUp);
                empty.NewBox(L"新盒子", {400, 400});
                expect(empty.boxes_.size() == 1 && sameAppearance(*empty.boxes_.front(), defaults),
                       "the first box created after deleting all boxes inherits the global appearance");
                expect(empty.boxes_.front()->Direction() == box::Direction::Up,
                       "global expansion direction set with no boxes is inherited by the first newly created box");
            }
        }
        {
            const auto managerRoot = sandbox + L"\\new-command-data";
            std::wstring created;
            {
                Manager manager(GetModuleHandleW(nullptr), managerRoot, sandbox + L"\\new-command-icons");
                manager.Initialize(false); // Real manager/IPC path with no desktop links, hooks, tray or user associations.
                auto& original = manager.boxes_.front()->storage;
                const auto imported = original.ImportIcon(directory + L"\\shell-new.ico"); original.Write(L"NewBoxIconFile", imported);
                WriteBytes(original.items + L"\\preserved.txt", "keep", 4);
                const auto before = manager.boxes_.size();
                std::wstring request = managerRoot; request.push_back(0); request.push_back(0);
                COPYDATASTRUCT packet{0x44424F5Au, static_cast<DWORD>(request.size() * sizeof(wchar_t)), request.data()};
                expect(SendMessageW(manager.controller_, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&packet)) == TRUE && manager.boxes_.size() == before,
                       "ShellNew IPC accepts the current data root and defers creation until after replying to Explorer");
                MSG creation{};
                expect(PeekMessageW(&creation, manager.controller_, kCreateShellBox, kCreateShellBox, PM_REMOVE),
                       "accepted ShellNew IPC posts exactly one asynchronous creation request");
                DispatchMessageW(&creation); created = manager.boxes_.back()->storage.root;
                expect(manager.boxes_.size() == before + 1 && manager.boxes_.back()->storage.Read(L"Name") == L"桌面收纳盒" &&
                       manager.boxes_.back()->storage.SelectedIconPath() == original.iconDirectory + L"\\" + imported &&
                       ReadSmallFile(original.items + L"\\preserved.txt") == "keep",
                       "ShellNew creates a separate persistent storage box using the saved ICO and leaves original contents intact");
                std::wstring unrelated = directory + L"\\different-data"; unrelated.push_back(0); unrelated.push_back(0);
                packet.cbData = static_cast<DWORD>(unrelated.size() * sizeof(wchar_t)); packet.lpData = unrelated.data();
                expect(!SendMessageW(manager.controller_, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&packet)) &&
                       !PeekMessageW(&creation, manager.controller_, kCreateShellBox, kCreateShellBox, PM_REMOVE),
                       "a creation request for a different data directory is rejected without making a box");
                request = managerRoot; request.push_back(0); request.push_back(0);
                packet.dwData = 0x44424F5B; packet.cbData = static_cast<DWORD>(request.size() * sizeof(wchar_t)); packet.lpData = request.data();
                expect(SendMessageW(manager.controller_, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&packet)) &&
                       ReadSmallFile(managerRoot + L"\\hover-diagnostics.txt").find("pointer_checks=") != std::string::npos,
                       "running instance reports live hover and menu state without opening or closing panels");
                auto& first = *manager.boxes_.front(); auto& second = *manager.boxes_.back();
                auto visibleCount = [&] { return std::count_if(manager.boxes_.begin(), manager.boxes_.end(), [](const auto& box) { return box->PanelVisible(); }); };
                expect(first.ShowPanel() && first.PanelVisible() && visibleCount() == 1,
                       "showing a managed box opens exactly one panel");
                first.ScheduleClose(); const ULONGLONG pendingClose = first.closeDeadline_;
                expect(second.ShowPanel() && !first.PanelVisible() && second.PanelVisible() && !first.closeDeadline_ && visibleCount() == 1,
                       "switching boxes hides the old panel before preparing the new one and cancels its delayed close");
                bool exclusive = true;
                for (int i = 0; i < 30; ++i) {
                    auto& current = i % 2 ? first : second;
                    exclusive = current.ShowPanel() && visibleCount() == 1 && exclusive;
                }
                expect(exclusive && pendingClose != 0, "rapid repeated box switching never leaves two expanded panels");
                first.ShowPanel();
                {
                    FlagGuard opening(first.openingPanel_);
                    expect(!second.ShowPanel() && first.PanelVisible() && visibleCount() == 1,
                           "reentrant hover during Shell or backdrop preparation cannot start a second panel");
                }
                {
                    FlagGuard menu(first.inMenu_);
                    expect(!second.ShowPanel() && first.PanelVisible() && visibleCount() == 1,
                           "hovering another box cannot interrupt an active context menu");
                }
                {
                    FlagGuard dialog(first.inDialog_);
                    expect(!second.ShowPanel() && first.PanelVisible() && visibleCount() == 1,
                           "hovering another box cannot interrupt an active settings or file dialog");
                }
                SetCapture(first.panel);
                expect(!second.ShowPanel() && GetCapture() == first.panel && first.PanelVisible(),
                       "box switching preserves an active selection gesture and mouse capture");
                ReleaseCapture();
                {
                    FlagGuard incoming(first.dragIncoming);
                    expect(!second.ShowPanel() && first.PanelVisible(), "an active drop target remains open until the drag leaves");
                }
                {
                    FlagGuard dragging(first.inDialog_); first.draggingPaths_.push_back(original.items + L"\\preserved.txt");
                    expect(second.ShowPanel() && !first.PanelVisible() && second.PanelVisible() && !first.draggingPaths_.empty(),
                           "a file drag may reveal another box without cancelling its source operation");
                    first.draggingPaths_.clear();
                }
                first.ShowPanel(); first.CancelClose();
                const ULONGLONG scheduled = GetTickCount64(); first.ScheduleClose();
                const ULONGLONG deadline = first.closeDeadline_;
                expect(deadline >= scheduled + 120 && deadline <= GetTickCount64() + 120,
                       "pointer leave schedules panel closing after 120 milliseconds instead of 380");
                first.ScheduleClose(); first.ScheduleClose();
                expect(first.closeDeadline_ == deadline, "duplicate leave events do not keep extending the close deadline");
                first.CloseTimer({-30000, -30000}, deadline - 1);
                expect(first.PanelVisible() && first.closeDeadline_ == deadline, "a stale timer cannot close a newly scheduled panel early");
                RECT panelBounds{}; GetWindowRect(first.panel, &panelBounds);
                first.CloseTimer({panelBounds.left + 10, panelBounds.top + 10}, deadline);
                expect(first.PanelVisible() && !first.closeDeadline_, "entering the panel before timeout keeps its controls accessible");
                first.ScheduleClose(); first.CloseTimer({-30000, -30000}, first.closeDeadline_);
                expect(!first.PanelVisible() && !IsWindowVisible(first.panel) && !first.closeDeadline_,
                       "leaving both icon and panel closes the actual window at the shortened deadline");
                first.ShowPanel(); first.ScheduleClose();
                {
                    FlagGuard menu(first.inMenu_);
                    first.CloseTimer({-30000, -30000}, first.closeDeadline_);
                    expect(first.PanelVisible(), "an already queued close timer cannot dismiss a context menu's panel");
                }
                first.HidePanel(); second.HidePanel();
            }
            Manager reopened(GetModuleHandleW(nullptr), managerRoot, sandbox + L"\\new-command-icons"); reopened.Initialize(false);
            expect(reopened.boxes_.size() == 2 && reopened.boxes_.back()->storage.root == created &&
                   reopened.boxes_.back()->storage.Read(L"Name") == L"桌面收纳盒" && !reopened.boxes_.back()->storage.SelectedIconPath().empty(),
                   "a ShellNew box and its selected ICO are restored by the manager after a complete restart");
        }
        {
            const RECT icon{100, 100, 148, 148};
            expect(box::InPanelTransit({155, 120}, icon, {166, 90, 582, 398}) &&
                   box::InPanelTransit({90, 120}, icon, {-334, 90, 82, 398}),
                   "narrow left and right transit gaps preserve movement from an icon into its panel");
            expect(box::InPanelTransit({120, 155}, icon, {90, 166, 506, 474}) &&
                   box::InPanelTransit({120, 90}, icon, {90, -226, 506, 82}),
                   "upward and downward expansion also preserve their narrow transit gap");
            expect(!box::InPanelTransit({155, 200}, icon, {166, 90, 582, 398}) &&
                   !box::InPanelTransit({80, 155}, icon, {90, 166, 506, 474}) &&
                   !box::InPanelTransit({155, 200}, icon, {166, 190, 582, 498}),
                   "panel transit never turns surrounding blank space or diagonal regions into hover targets");
        }
        expect(XmlEscape(L"a&b<c>\"'") == L"a&amp;b&lt;c&gt;&quot;&apos;", "startup executable paths are XML-escaped");
        const auto login = StartupXml(L"S-1-5-21-test", L"C:\\工具\\DesktopBox.exe", L"D:\\用户 数据");
        expect(login.find(L"<Delay>PT0S</Delay>") != std::wstring::npos && login.find(L"<RunOnlyIfIdle>false</RunOnlyIfIdle>") != std::wstring::npos &&
               login.find(L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>") != std::wstring::npos &&
               login.find(L"--data-dir &quot;D:\\用户 数据&quot;") != std::wstring::npos,
               "login startup has no configured delay idle or network gate and preserves the selected data directory");
        try {
            StartupTask startup;
            expect(startup.Validate(ExePath()), "login task XML validates without registering a task");
        } catch (HRESULT hr) { report += "SKIP task-service validation unavailable: " + std::to_string(static_cast<unsigned long>(hr)) + "\r\n"; }
        report += "\r\nAll " + std::to_string(total) + " checks passed.\r\n";
    } catch (HRESULT hr) {
        report += "ERROR HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + "\r\n"; result = 1;
    } catch (const std::exception& error) { report += "ERROR "; report += error.what(); report += "\r\n"; result = 1; }
    if (!WriteBytes(directory + L"\\results.txt", report.data(), static_cast<DWORD>(report.size()))) return 2;
    return result;
}
