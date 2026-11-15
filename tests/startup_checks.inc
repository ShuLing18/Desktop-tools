{
    const auto contentRoot = sandbox + L"\\async-content";
    App app(GetModuleHandleW(nullptr), contentRoot, storage.iconDirectory, contentRoot + L"\\origins", {}, {});
    app.Initialize(false, false);
    for (int i = 0; i < 80; ++i) Check(WriteBytes(app.storage.items + L"\\entry-" + std::to_wstring(i) + L".txt", "snapshot", 8) ? S_OK : E_FAIL);
    app.storage.Write(L"Name", L"无需重复写入");
    const FILETIME marker{0x10203040, 0x01D90000};
    {
        KernelHandle file(CreateFileW(app.storage.ini.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        Check(file.value != INVALID_HANDLE_VALUE && SetFileTime(file.value, nullptr, nullptr, &marker) ? S_OK : E_FAIL);
    }
    app.storage.Write(L"Name", L"无需重复写入");
    WIN32_FILE_ATTRIBUTE_DATA attributes{}; Check(GetFileAttributesExW(app.storage.ini.c_str(), GetFileExInfoStandard, &attributes) ? S_OK : E_FAIL);
    expect(CompareFileTime(&marker, &attributes.ftLastWriteTime) == 0, "writing an unchanged setting does not touch or flush its configuration file");
    app.nativeMode_ = true; app.contentLoaded_ = false; app.Reload();
    const auto obsolete = app.contentGeneration_; app.Reload();
    expect(app.contentPending_ && !app.contentLoaded_, "native content scanning is queued without waiting for the directory on the UI thread");
    auto stale = std::make_unique<box::ContentResult>(); stale->generation = obsolete;
    stale->entries.push_back({L"obsolete", 0, 0}); app.ReceiveContent(std::move(stale));
    expect(!app.contentLoaded_ && app.items_.empty(), "a superseded content snapshot cannot populate or clear the active view");
    const auto deadline = GetTickCount64() + 5000;
    while (!app.contentLoaded_ && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, app.host, kContentReady, kContentReady, PM_REMOVE)) DispatchMessageW(&message);
        if (!app.contentLoaded_) Sleep(1);
    }
    expect(app.contentLoaded_ && app.items_.size() == 80 && !app.contentPending_, "the shared content worker publishes the latest complete eighty-file catalog");
    const auto snapshot = app.storage.Snapshot();
    expect(snapshot.size() == 80 && std::all_of(snapshot.begin(), snapshot.end(), [](const auto& item) { return item.modified != 0; }),
           "one directory enumeration supplies real file modification times together with names");
    app.contentLoaded_ = false; app.nativeLocated_ = true;
    expect(!app.NativeReady(), "a located desktop icon is not reported fully interactive until its content catalog is ready");
    app.contentLoaded_ = true;
    expect(app.NativeReady(), "interactive readiness requires both the native icon and the available file catalog");
    app.Reload(); app.contentLoader_.Stop();
    MSG abandoned{};
    expect(!PeekMessageW(&abandoned, app.host, kContentReady, kContentReady, PM_REMOVE), "stopping a content client cancels scans and drains messages before HWND reuse");
    app.nativeMode_ = false;
}
{
    const auto root = sandbox + L"\\staged-startup";
    for (int i = 0; i < 5; ++i) {
        Storage data(i ? root + L"\\boxes\\stage-" + std::to_wstring(i) : root, storage.iconDirectory, root + L"\\origins");
        data.Write(L"Name", L"分阶段启动");
    }
    Manager manager(GetModuleHandleW(nullptr), root, storage.iconDirectory);
    manager.Initialize(false, true);
    expect(IsWindow(manager.controller_) && manager.boxes_.empty() && manager.pendingBoxes_.size() == 5,
           "staged startup exposes a working controller and returns before constructing all boxes");
    const auto requested = manager.pendingBoxes_.back().directory;
    manager.Open(requested, {}, false, kDirectionLeft);
    expect(manager.boxes_.size() == 1 && manager.boxes_.front()->storage.root == requested && manager.pendingBoxes_.size() == 4,
           "an explicit request initializes its own pending box before unrelated startup work");
    MSG message{};
    Check(PeekMessageW(&message, manager.controller_, kInitializeBox, kInitializeBox, PM_REMOVE) ? S_OK : E_FAIL);
    DispatchMessageW(&message);
    expect(manager.boxes_.size() == 2 && manager.pendingBoxes_.size() == 3,
           "each bootstrap message initializes only one box and returns to the input loop");
    while (PeekMessageW(&message, manager.controller_, kInitializeBox, kInitializeBox, PM_REMOVE)) DispatchMessageW(&message);
    expect(manager.boxes_.size() == 5 && manager.pendingBoxes_.empty(), "staged initialization eventually restores every configured box");
}
{
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"DesktopBox.InputFilterTest";
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    Check(window ? S_OK : E_FAIL);
    box::DesktopMouse listener;
    const UINT notification = WM_APP + 97; Check(listener.Start(window, notification, 0, false));
    listener.SetHoverRegions({{10, 10, 30, 30}}, true);
    MSG message{}; while (PeekMessageW(&message, window, notification, notification, PM_REMOVE)) {} listener.Consume();
    listener.ObservePointer({100, 100});
    expect(!PeekMessageW(&message, window, notification, notification, PM_REMOVE), "mouse motion outside every known icon and panel does not wake the UI");
    listener.ObservePointer({20, 20});
    expect(PeekMessageW(&message, window, notification, notification, PM_REMOVE) != FALSE, "entering an icon immediately posts a hover notification"); listener.Consume();
    listener.ObservePointer({100, 100});
    expect(PeekMessageW(&message, window, notification, notification, PM_REMOVE) != FALSE, "the first pointer leave still wakes the UI to close an open panel"); listener.Consume();
    listener.ObservePointer({101, 101});
    expect(!PeekMessageW(&message, window, notification, notification, PM_REMOVE), "continued unrelated mouse movement produces no repeated UI wake-ups");
    listener.ObservePointer({101, 101}, true);
    expect(PeekMessageW(&message, window, notification, notification, PM_REMOVE) != FALSE, "a button release or zoom still requests desktop geometry recovery outside cached regions");
    listener.Stop(); DestroyWindow(window);
}
