{
    struct NamespaceRegistryFixture {
        HKEY key = nullptr;
        std::wstring path = L"Software\\DesktopBox\\NamespaceTests\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        ~NamespaceRegistryFixture() { if (key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } registry;
    Check(HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_CURRENT_USER, registry.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &registry.key, nullptr)));
    auto read = [&](const std::wstring& path, const wchar_t* name) {
        wchar_t value[32768]{}; DWORD bytes = sizeof(value);
        RegGetValueW(registry.key, path.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &bytes); return std::wstring(value);
    };
    box::DesktopNamespace object{L"{9B7708F1-22C3-4F68-90BC-1BC384A974B1}", L"系统收纳盒",
        directory + L"\\程序 空间\\DesktopBox.exe", sandbox, sandbox + L"\\namespace-data",
        sandbox + L"\\namespace-data\\items", box::ShellNewIcon(L"", directory + L"\\程序 空间\\DesktopBox.exe")};
    const auto cls = box::DesktopNamespaceClassKey(object.id);
    const auto junction = box::DesktopNamespaceJunctionKey(object.id);
    bool changed = false;
    Check(box::RegisterDesktopNamespace(registry.key, object, changed));
    expect(changed && box::DesktopNamespacePresent(registry.key, object.id, object.boxRoot),
           "a desktop namespace publishes an owned persistent object identity in the isolated user registry");
    expect(read(cls + L"\\Instance", L"CLSID") == L"{0E5AAE11-A475-4C5B-AB00-C66DE400274E}" &&
           read(cls + L"\\Instance\\InitPropertyBag", L"TargetFolderPath") == object.items,
           "the system folder instance maps the native object directly to its existing real content directory");
    DWORD attributes = 0, bytes = sizeof(attributes);
    Check(HRESULT_FROM_WIN32(RegGetValueW(registry.key, (cls + L"\\ShellFolder").c_str(), L"Attributes", RRF_RT_REG_DWORD, nullptr, &attributes, &bytes)));
    expect((attributes & (SFGAO_FOLDER | SFGAO_CANRENAME | SFGAO_DROPTARGET)) == (SFGAO_FOLDER | SFGAO_CANRENAME | SFGAO_DROPTARGET) &&
           !(attributes & (SFGAO_LINK | SFGAO_FILESYSTEM | SFGAO_CANMOVE)),
           "namespace root supports native rename and drops without being a shortcut or a movable content directory");
    const auto action = read(cls + L"\\shell\\DesktopBox.Add\\command", nullptr);
    expect(action.find(L"\"" + object.executable + L"\" --data-dir \"" + object.dataRoot + L"\"") == 0 &&
           action.find(L"--open-box \"" + object.boxRoot + L"\" --shell-action add") != std::wstring::npos,
           "Explorer menu commands keep Unicode paths quoted and identify the exact box and action");
    expect(read(cls + L"\\shell\\DesktopBox.Settings\\command", nullptr).find(L"--appearance") != std::wstring::npos &&
           read(cls + L"\\shell\\DesktopBox.Direction\\shell\\left\\command", nullptr).find(L"--shell-action direction-left") != std::wstring::npos,
           "settings and direction commands are registered on the native object for both mouse and keyboard menus");
    Check(box::RegisterDesktopNamespace(registry.key, object, changed));
    expect(!changed, "restarting a configured namespace object does not rewrite its registry values");
    auto sibling = object; sibling.id = L"{0B042F14-94DF-48E7-B2DB-45566A8C0841}"; sibling.boxRoot += L"-other"; sibling.items += L"-other";
    Check(box::RegisterDesktopNamespace(registry.key, sibling, changed));
    expect(box::DesktopNamespacePresent(registry.key, sibling.id, sibling.boxRoot) &&
           box::DesktopNamespaceName(registry.key, object.id) == box::DesktopNamespaceName(registry.key, sibling.id),
           "same-name boxes have separate stable system identities without numbered shortcut files");
    auto wrongBox = object; wrongBox.boxRoot += L"-foreign";
    expect(FAILED(box::RegisterDesktopNamespace(registry.key, wrongBox, changed)) && !changed &&
           box::OwnsDesktopNamespace(registry.key, object.id, object.boxRoot),
           "a different box cannot take over an existing desktop object CLSID");
    auto replacement = object; replacement.executable = directory + L"\\新版\\DesktopBox.exe";
    Check(box::RegisterDesktopNamespace(registry.key, replacement, changed));
    Check(box::UnregisterDesktopNamespace(registry.key, object.id, object.boxRoot, object.executable, changed));
    expect(!changed && box::DesktopNamespacePresent(registry.key, object.id, object.boxRoot),
           "an old executable cannot unregister a newer installation's desktop object");
    Check(box::UnregisterDesktopNamespaces(registry.key, replacement.executable, changed));
    expect(changed && !box::DesktopNamespacePresent(registry.key, object.id, object.boxRoot) &&
           box::DesktopNamespacePresent(registry.key, sibling.id, sibling.boxRoot),
           "uninstall removes only namespace objects owned by that executable and preserves other installations");
    expect(NativeShellCommand(L"add") == kAddItems && NativeShellCommand(L"return") == kDeleteBox &&
           NativeShellCommand(L"direction-left") == kDirectionLeft && NativeShellCommand(L"arbitrary-command") == 0,
           "native menu dispatch accepts only explicitly supported box operations");
    Manager manager(GetModuleHandleW(nullptr), sandbox + L"\\native-menu-dispatch", storage.iconDirectory);
    manager.Initialize(false);
    auto& app = *manager.boxes_.front();
    manager.Open(app.storage.root, {}, false, kDirectionLeft);
    expect(app.Direction() == box::Direction::Left && !app.PanelVisible(),
           "a native menu command targets its box without opening an unrelated panel");
}
