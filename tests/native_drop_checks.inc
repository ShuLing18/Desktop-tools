// Exercise Explorer's actual drop target for the .lnk, not the panel DropTarget.
{
    const auto folderSettings = nativeData.items + L"\\desktop.ini";
    const DWORD metadata = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    expect((GetFileAttributesW(folderSettings.c_str()) & metadata) == metadata && nativeData.Entries().empty(),
           "native folder drop settings stay hidden and are excluded from box content and return counts");
    Check(WritePrivateProfileStringW(L".ShellClassInfo", L"InfoTip", L"preserve existing folder settings", folderSettings.c_str()) ? S_OK : E_FAIL);
    Check(WritePrivateProfileStringW(L".ShellClassInfo", L"DefaultDropEffect", L"1", folderSettings.c_str()) ? S_OK : E_FAIL);
    Check(SetFileAttributesW(folderSettings.c_str(), metadata | FILE_ATTRIBUTE_READONLY) ? S_OK : E_FAIL);
    native.Ensure(nativeData, L"原生入口", ExePath(), desktop, false, true);
    wchar_t infoTip[128]{};
    GetPrivateProfileStringW(L".ShellClassInfo", L"InfoTip", L"", infoTip, 128, folderSettings.c_str());
    expect(GetPrivateProfileIntW(L".ShellClassInfo", L"DefaultDropEffect", 0, folderSettings.c_str()) == DROPEFFECT_MOVE &&
           wcscmp(infoTip, L"preserve existing folder settings") == 0 &&
           (GetFileAttributesW(folderSettings.c_str()) & FILE_ATTRIBUTE_READONLY),
           "existing shortcuts repair a copy default while preserving folder customization and read-only settings");
    expect(!nativeData.EnsureMoveDropTarget(), "a configured native drop folder needs no further writes or Explorer notifications");

    const auto exerciseDrop = [&](const std::wstring& inputDirectory, const char* label) {
        const auto sourceDirectory = AbsolutePath(inputDirectory);
        EnsureDirectory(sourceDirectory);
        const auto text = sourceDirectory + L"\\拖入文本.txt";
        const auto shortcutPath = sourceDirectory + L"\\带参数.lnk";
        const auto folder = sourceDirectory + L"\\拖入目录";
        Check(WriteBytes(text, "native-drop-bytes", 17) ? S_OK : E_FAIL);
        EnsureDirectory(folder); Check(WriteBytes(folder + L"\\child.txt", "nested-bytes", 12) ? S_OK : E_FAIL);
        ComPtr<IShellLinkW> sourceShortcut; ComPtr<IPersistFile> persist;
        Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(sourceShortcut.put())));
        Check(sourceShortcut->SetPath(ExePath().c_str())); Check(sourceShortcut->SetArguments(L"--profile \"native drop\""));
        Check(sourceShortcut->SetWorkingDirectory(sourceDirectory.c_str()));
        Check(sourceShortcut->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persist.put()))); Check(persist->Save(shortcutPath.c_str(), TRUE));
        const std::vector<std::wstring> paths{text, shortcutPath, folder};
        ComPtr<IDataObject> data; ComPtr<IDropTarget> target;
        const auto sourceResult = box::ShellSelectionObject(nullptr, paths, IID_IDataObject, reinterpret_cast<void**>(data.put()));
        if (FAILED(sourceResult)) report += std::string(label) + " source Shell data object failed\r\n";
        Check(sourceResult);
        const auto targetResult = box::ShellSelectionObject(nullptr, {native.Path()}, IID_IDropTarget, reinterpret_cast<void**>(target.put()));
        if (FAILED(targetResult)) report += std::string(label) + " native shortcut drop target failed\r\n";
        Check(targetResult);
        const DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        DWORD effect = allowed;
        Check(target->DragEnter(data.get(), MK_LBUTTON, {0, 0}, &effect));
        expect(effect == DROPEFFECT_MOVE, (std::string(label) + " native desktop icon defaults to move with no modifier keys").c_str());
        effect = allowed; Check(target->DragOver(MK_LBUTTON | MK_CONTROL, {0, 0}, &effect));
        expect(effect == DROPEFFECT_COPY, (std::string(label) + " native desktop icon preserves an explicit Ctrl-copy request").c_str());
        effect = allowed; Check(target->DragOver(MK_LBUTTON, {0, 0}, &effect));
        expect(effect == DROPEFFECT_MOVE, (std::string(label) + " releasing Ctrl restores the move default before dropping").c_str());
        effect = allowed; Check(target->Drop(data.get(), 0, {0, 0}, &effect));
        const auto departed = [&] {
            return std::all_of(paths.begin(), paths.end(), [](const auto& path) { return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES; });
        };
        const auto deadline = GetTickCount64() + 5000;
        while (!departed() && GetTickCount64() < deadline) {
            MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(1);
        }
        expect(departed() && nativeData.Entries().size() == 3 &&
               ReadSmallFile(nativeData.items + L"\\拖入文本.txt") == "native-drop-bytes" &&
               ReadSmallFile(nativeData.items + L"\\拖入目录\\child.txt") == "nested-bytes",
               (std::string(label) + " native icon drop removes every source and preserves file and nested-folder bytes").c_str());
        Check(persist->Load((nativeData.items + L"\\带参数.lnk").c_str(), STGM_READ));
        wchar_t arguments[128]{}, workingDirectory[32768]{};
        Check(sourceShortcut->GetArguments(arguments, 128)); Check(sourceShortcut->GetWorkingDirectory(workingDirectory, 32768));
        expect(wcscmp(arguments, L"--profile \"native drop\"") == 0 && _wcsicmp(workingDirectory, sourceDirectory.c_str()) == 0,
               (std::string(label) + " native icon drop moves the shortcut itself with its arguments and working directory intact").c_str());
        const auto restored = nativeData.RestoreAll(nullptr, sourceDirectory, true);
        if (!restored.complete) report += std::string(label) + " return incomplete, moved=" + std::to_string(restored.moved) + "\r\n";
        expect(restored.complete && restored.moved == paths.size() && nativeData.Entries().empty() &&
               GetFileAttributesW(folderSettings.c_str()) != INVALID_FILE_ATTRIBUTES,
               (std::string(label) + " returning box contents leaves Explorer's folder settings in place").c_str());
        const auto imported = nativeData.MoveInto({folder}, nullptr, true);
        expect(imported.complete && imported.moved == 1 &&
               nativeData.OriginalPath(nativeData.items + L"\\拖入目录") == folder,
               (std::string(label) + " importing a folder records success and origin after Windows removes its source").c_str());
        expect(nativeData.RestoreAll(nullptr, L"", true).complete && ReadSmallFile(folder + L"\\child.txt") == "nested-bytes",
               (std::string(label) + " imported folder returns to its recorded origin with all nested bytes").c_str());
        // Remove only the known files created by this fixture, including on the other drive.
        DeleteFileW(text.c_str()); DeleteFileW(shortcutPath.c_str()); DeleteFileW((folder + L"\\child.txt").c_str());
        RemoveDirectoryW(folder.c_str()); RemoveDirectoryW(sourceDirectory.c_str());
    };
    exerciseDrop(sandbox + L"\\native-same-volume", "same-volume");
    wchar_t temporaryPath[32768]{}; Check(GetTempPathW(32768, temporaryPath) ? S_OK : E_FAIL);
    const auto temporaryRoot = AbsolutePath(temporaryPath);
    if (towupper(temporaryRoot[0]) != towupper(sandbox[0]))
        exerciseDrop(temporaryRoot + L"\\DesktopBox-native-drop-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()), "cross-volume");
    else report += "SKIP native cross-volume drop: temp and workspace use same drive\r\n";
}
