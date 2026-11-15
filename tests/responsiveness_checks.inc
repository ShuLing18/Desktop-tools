// Exercise output and lifecycle guarantees instead of asserting machine-specific timings.
{
    static HANDLE entered = nullptr, release = nullptr;
    static std::atomic<unsigned> queries{0};
    static std::atomic<DWORD> queryThread{0};
    KernelHandle enteredEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    KernelHandle releaseEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    entered = enteredEvent.value; release = releaseEvent.value; queries = 0; queryThread = 0;
    const auto query = +[](const std::vector<std::wstring>& paths, HWND) {
        ++queries; queryThread = GetCurrentThreadId();
        if (!paths.empty() && paths.front() == L"slow") { SetEvent(entered); WaitForSingleObject(release, 5000); }
        std::vector<box::NativeBounds> result;
        for (const auto& path : paths) result.push_back({path, {10,20,110,140}, {24,22,96,94}, true});
        return result;
    };
    Manager startup(GetModuleHandleW(nullptr), sandbox + L"\\nonblocking-desktop", storage.iconDirectory);
    startup.Initialize(false);
    const UINT resultMessage = WM_APP + 92;
    box::DesktopBoundsLoader loader(query);
    struct ReleaseAtExit { HANDLE event; ~ReleaseAtExit() { SetEvent(event); } } guard{release};
    expect(loader.RequestBounds(startup.controller_, resultMessage, startup.controller_, {L"slow"}, 1) &&
           WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0 && queryThread != GetCurrentThreadId(),
           "cold desktop queries run in a separate COM apartment while the UI remains free");
    auto& app = *startup.boxes_.front(); app.nativeMode_ = true; app.nativeLocated_ = false;
    app.UpdateNativeBounds();
    expect(app.ShowPanel() && app.PanelVisible() && loader.Busy(),
           "explicit opening and the message loop remain usable while Explorer initialization is delayed");
    app.HidePanel(); app.nativeMode_ = false;
    expect(loader.RequestBounds(startup.controller_, resultMessage, startup.controller_, {L"obsolete"}, 2) &&
           loader.RequestBounds(startup.controller_, resultMessage, startup.controller_, {L"latest"}, 3),
           "desktop refresh bursts retain only the latest pending snapshot");
    SetEvent(release);
    const auto deadline = GetTickCount64() + 5000;
    bool latest = false;
    while (!latest && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, startup.controller_, resultMessage, resultMessage, PM_REMOVE)) {
            std::unique_ptr<box::DesktopBoundsResult> result(reinterpret_cast<box::DesktopBoundsResult*>(message.lParam));
            latest = result && result->generation == 3 && result->entries.size() == 1 && result->entries.front().path == L"latest";
        }
        if (!latest) Sleep(1);
    }
    expect(latest && queries == 2, "one active desktop query is followed by one newest snapshot without an unbounded queue");
    loader.Stop();
    expect(!loader.Busy(), "stopping the desktop worker drains results and releases its request state");
    app.nativeMode_ = true; app.nativeLocated_ = false;
    startup.desktopSurface_ = startup.controller_; startup.boundsGeneration_ = 7;
    startup.lastSurfaceRefresh_ = startup.lastNativeRefresh_ = GetTickCount64();
    box::DesktopBoundsResult current; current.generation = 6; current.desktop = startup.controller_;
    current.entries.push_back({app.NativePath(), {10,20,110,140}, {24,22,96,94}, true});
    startup.ReceiveNativeBounds(current);
    expect(!app.NativeReady(), "a superseded desktop result cannot move or reactivate a native icon");
    current.generation = 7; startup.ReceiveNativeBounds(current);
    expect(app.NativeReady() && startup.DesktopReady() && startup.desktopReadyAt_ != 0,
           "a current desktop snapshot activates the matching icon and records first readiness");
    const auto firstReady = startup.desktopReadyAt_; startup.BeginDesktopRetry();
    expect(startup.desktopReadyAt_ == firstReady,
           "a later Explorer taskbar restart cannot overwrite the original startup-ready measurement");
    current.desktop = nullptr; current.entries.front().image = {300,300,372,372};
    startup.ReceiveNativeBounds(current);
    expect(app.nativeImage_.left == 24, "results from a replaced desktop surface do not overwrite current hit-test bounds");
    app.nativeMode_ = false;
}
{
    Manager retry(GetModuleHandleW(nullptr), sandbox + L"\\hover-retry", storage.iconDirectory);
    retry.Initialize(false); retry.NewBox(L"临时阻塞", {800, 500});
    auto& target = *retry.boxes_.front(); auto& blocker = *retry.boxes_.back();
    target.nativeMode_ = true; target.nativeLocated_ = true; target.nativeImage_ = {100, 100, 148, 148};
    blocker.inMenu_ = true;
    const POINT hovered{120, 120};
    target.NativeMouse(hovered, true, false);
    MSG retryMessage{};
    const bool queued = PeekMessageW(&retryMessage, retry.controller_, kHoverOpenRetry, kHoverOpenRetry, PM_REMOVE) != FALSE;
    expect(queued && !target.PanelVisible() && target.hoverOpenRetryPending_,
           "a temporarily blocked hover queues one coalesced retry for the same native icon");
    retry.Handle(kHoverOpenRetry, retryMessage.wParam, retryMessage.lParam);
    expect(!target.hoverOpenRetryPending_ && retry.pointerSettlePending_,
           "a refused hover schedules another pointer check instead of relying on new mouse movement");
    blocker.inMenu_ = false; target.NativeMouse(hovered, true, false);
    expect(target.PanelVisible(), "a stationary hover opens as soon as the other box can yield its panel");
    target.HidePanel();
}
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto pixels = [](HICON icon, int size) {
        box::DibSurface surface;
        if (!icon || !surface.Ensure(nullptr, size, size)) return std::vector<DWORD>{};
        std::fill_n(surface.Pixels(), static_cast<size_t>(size) * size, 0);
        if (!DrawIconEx(surface.DC(), 0, 0, icon, size, size, 0, nullptr, DI_NORMAL)) return std::vector<DWORD>{};
        GdiFlush();
        return std::vector<DWORD>(surface.Pixels(), surface.Pixels() + static_cast<size_t>(size) * size);
    };
    const auto matchesDefault = [&](HICON icon, int size) {
        Icon expected(reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0)));
        const auto actual = pixels(icon, size);
        return std::any_of(actual.begin(), actual.end(), [](DWORD pixel) { return pixel != 0; }) &&
            actual == pixels(expected.value, size);
    };
    const auto root = sandbox + L"\\built-in-icon-data", icons = sandbox + L"\\built-in-icon-library";
    {
        Manager manager(instance, root, icons); manager.Initialize(false);
        auto& first = *manager.boxes_.front();
        expect(first.storage.SelectedIconPath().empty() &&
               matchesDefault(first.folderIcon_.value, std::max(first.Px(48), first.desktopIconSize_)),
               "a fresh box uses the embedded whale-girl icon without an external ICO file");
        const int traySize = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(manager.controller_));
        expect(!manager.trayCustomIcon_ && matchesDefault(manager.trayIcon_.value, traySize),
               "the default notification area icon matches the embedded application icon at the current DPI");
        manager.NewBox(L"内置默认", {900, 600});
        expect(manager.boxes_.back()->storage.Read(L"IconFile").empty() &&
               matchesDefault(manager.boxes_.back()->folderIcon_.value,
                   std::max(manager.boxes_.back()->Px(48), manager.boxes_.back()->desktopIconSize_)),
               "new boxes inherit the built-in default icon without writing a machine-specific image path");
        first.storage.Write(L"IconFile", L"missing-user-icon.ico"); first.LoadAppearance();
        expect(matchesDefault(first.folderIcon_.value, std::max(first.Px(48), first.desktopIconSize_)),
               "a removed custom ICO falls back to the embedded icon without erasing the saved choice");
        manager.ApplyGlobalAppearance({0, 55, 18, L""});
        expect(matchesDefault(first.folderIcon_.value, std::max(first.Px(48), first.desktopIconSize_)) &&
               matchesDefault(manager.trayIcon_.value, traySize),
               "Restore Default consistently restores the embedded icon for boxes and notification area");
    }
    {
        Manager reopened(instance, root, icons); reopened.Initialize(false);
        expect(reopened.boxes_.size() == 2 &&
               std::all_of(reopened.boxes_.begin(), reopened.boxes_.end(), [&](const auto& app) {
                   return app->storage.Read(L"IconFile").empty() &&
                       matchesDefault(app->folderIcon_.value, std::max(app->Px(48), app->desktopIconSize_));
               }), "the built-in default icon survives reopening all boxes");
    }
    const auto desktop = sandbox + L"\\built-in-icon-desktop"; EnsureDirectory(desktop);
    Storage data(sandbox + L"\\built-in-icon-link");
    box::NativeEntry entry; entry.Ensure(data, L"内置图标", ExePath(), desktop);
    const auto pointsToExecutableIcon = [&] {
        ComPtr<IShellLinkW> shortcut; ComPtr<IPersistFile> persisted;
        Check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(shortcut.put())));
        Check(shortcut->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persisted.put())));
        Check(persisted->Load(data.Read(L"DesktopLink").c_str(), STGM_READ));
        wchar_t location[32768]{}; int index = -1;
        Check(shortcut->GetIconLocation(location, 32768, &index));
        return index == 0 && _wcsicmp(location, ExePath().c_str()) == 0;
    };
    expect(pointsToExecutableIcon(), "native desktop links use the executable embedded icon by default");
    data.Write(L"IconFile", L"missing-user-icon.ico");
    entry.Ensure(data, L"内置图标", ExePath(), desktop, false, true);
    expect(pointsToExecutableIcon(), "native desktop links fall back to the executable when a custom ICO is missing");
    const auto defaultLocation = L"\"" + ExePath() + L"\",0";
    expect(box::ShellNewIcon(L"", ExePath()) == defaultLocation &&
           box::ShellNewIcon(icons + L"\\missing-user-icon.ico", ExePath()) == defaultLocation,
           "desktop New menu defaults and missing-icon fallback use the same embedded icon");
    HRSRC group = FindResourceW(instance, MAKEINTRESOURCEW(1), RT_GROUP_ICON);
    const auto* bytes = group ? static_cast<const BYTE*>(LockResource(LoadResource(instance, group))) : nullptr;
    const DWORD length = group ? SizeofResource(instance, group) : 0;
    expect(bytes && length >= 6 && *reinterpret_cast<const WORD*>(bytes + 4) == 10,
           "the executable embeds all ten DPI-aware ICO frames from 16 to 256 pixels");
}
{
    box::Backdrop source; source.SetSource(73, 49, std::vector<DWORD>(73 * 49, 0x003478BC));
    Check(source.Blur(18) ? S_OK : E_FAIL);
    auto snapshot = source.SourceSnapshot(); source.Clear();
    expect(snapshot.Matches(73, 49) && !snapshot.HasPixels() && snapshot.Blur(40) && snapshot.Ready(40),
           "background jobs share immutable capture pixels safely after the original owner releases them");
    std::atomic<bool> cancelled{true};
    auto cancelledJob = snapshot.SourceSnapshot();
    expect(!cancelledJob.Blur(18, &cancelled) && !cancelledJob.HasPixels(),
           "cancelled background calculations stop without publishing incomplete pixels");
    RECT screen{25, 37, 98, 86}; box::Backdrop capture;
    expect(capture.Capture(screen) && capture.Matches(screen) && !capture.Matches(RECT{26, 37, 99, 86}),
           "backdrop identity includes the screen origin so moving a same-sized panel cannot reuse stale glass");
}
{
    HDC screen = GetDC(nullptr); box::DibSurface surface;
    expect(surface.Ensure(screen, 160, 90), "a reusable native paint surface is allocated successfully");
    const HDC originalDC = surface.DC(); const DWORD* originalPixels = surface.Pixels();
    expect(surface.Ensure(screen, 160, 90) && surface.DC() == originalDC && surface.Pixels() == originalPixels,
           "repeated frames reuse their bitmap and DC instead of allocating GDI objects each time");
    expect(surface.Ensure(screen, 240, 120) && surface.Width() == 240 && surface.Height() == 120,
           "paint buffers adapt to a changed panel size without retaining obsolete dimensions");
    surface.Clear(); expect(!surface.DC() && !surface.Pixels(), "closing a panel releases the reusable native paint surface");
    ReleaseDC(nullptr, screen);
}
{
    App fast(GetModuleHandleW(nullptr), sandbox + L"\\responsive-box", storage.iconDirectory, storage.journalDirectory, {}, {});
    fast.Initialize(false, false);
    for (int i = 0; i < 12; ++i) WriteBytes(fast.storage.items + L"\\项目-" + std::to_wstring(i) + L".txt", "item", 4);
    fast.Reload(); fast.ShowPanel();
    expect(fast.PanelVisible() && IsWindowVisible(fast.panel) && fast.panelSurface_.DC(),
           "opening draws a real first frame without waiting for background blur or asynchronous file icons");
    expect(waitForBackdrop(fast) && fast.backdrop_.Ready(fast.Px(fast.glassBlur_)),
           "the completed asynchronous glass frame replaces the initial native backdrop without reopening the window");
    const auto generation = fast.backdropGeneration_; const HDC paintDC = fast.panelSurface_.DC();
    expect(fast.ShowPanel() && fast.backdropGeneration_ == generation && fast.panelSurface_.DC() == paintDC,
           "reopening an already visible box keeps its existing background job and paint resources");
    HDC screen = GetDC(fast.panel);
    const auto* glyph = fast.textMasks_.Get(screen, L"中文 & file.txt", fast.bodyFont_, 140, 80, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    expect(glyph && glyph->coverage.size(), "Unicode file labels retain real antialiased glyph coverage in the bounded cache");
    const auto coverage = glyph->coverage; const RECT ink = glyph->ink; const size_t count = fast.textMasks_.Count();
    glyph = fast.textMasks_.Get(screen, L"中文 & file.txt", fast.bodyFont_, 140, 80, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    expect(glyph && glyph->coverage == coverage && EqualRect(&glyph->ink, &ink) && fast.textMasks_.Count() == count,
           "cached filename coverage remains identical while repeated drawing avoids glyph regeneration");
    box::DibSurface reference; Check(reference.Ensure(screen, 140, 80) ? S_OK : E_FAIL);
    std::fill_n(reference.Pixels(), 140 * 80, 0u); const auto oldFont = SelectObject(reference.DC(), fast.bodyFont_);
    SetTextColor(reference.DC(), RGB(255, 255, 255)); SetBkMode(reference.DC(), TRANSPARENT);
    RECT layout{0,0,140,80}; DrawTextW(reference.DC(), L"中文 & file.txt", -1, &layout, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX); GdiFlush();
    bool identical = true;
    for (int y = 0; y < 80; ++y) for (int x = 0; x < 140; ++x)
        identical = identical && glyph->At(x,y) == (reference.Pixels()[y * 140 + x] & 255);
    SelectObject(reference.DC(), oldFont);
    expect(identical, "cropped cached glyphs match the original full-size GDI mask pixel for pixel");
    for (int i = 0; i < 180; ++i) fast.textMasks_.Get(screen, L"缓存项目 " + std::to_wstring(i), fast.bodyFont_, 140, 80, DT_CENTER | DT_WORDBREAK);
    expect(fast.textMasks_.Count() <= 48 && fast.textMasks_.Bytes() <= 192 * 1024,
           "filename masks stay under their fixed memory and entry limits during repeated searching");
    ReleaseDC(fast.panel, screen);
    fast.glassBlur_ = 40; fast.ApplyAppearance(); fast.glassBlur_ = 2; fast.ApplyAppearance();
    expect(waitForBackdrop(fast) && fast.backdrop_.Ready(fast.Px(2)), "rapid blur preview changes publish only the latest requested strength");
    fast.glassBlur_ = 39; fast.ApplyAppearance(); const auto obsoleteGeneration = fast.backdropGeneration_;
    fast.HidePanel();
    expect(!fast.backdropPending_ && !fast.panelSurface_.DC() && fast.textMasks_.Bytes() <= 192 * 1024 && !fast.backdrop_.HasPixels(),
           "hiding cancels background work, releases the paint buffer and keeps only bounded short-lived text and glass caches");
    fast.ReleaseTextCache(fast.textCacheUntil_);
    expect(fast.textMasks_.Bytes() == 0 && !fast.textCacheUntil_, "the text-cache expiry releases glyph masks and their scratch DC completely");
    auto stale = std::make_unique<box::BackdropResult>(); stale->generation = obsoleteGeneration;
    stale->value.SetSource(8,8,std::vector<DWORD>(64,0x00FFFFFF)); stale->value.Blur(2);
    SendMessageW(fast.host, kBackdropReady, 0, reinterpret_cast<LPARAM>(stale.release()));
    expect(!fast.PanelVisible() && !fast.customGlass_ && !fast.backdrop_.HasPixels(),
           "a delayed backdrop completion cannot revive a closed panel or attach pixels to a newer generation");
    fast.ShowPanel(); fast.backgroundMode_ = 2; fast.ApplyAppearance();
    expect(!fast.backdropPending_ && !fast.customGlass_ && !fast.acrylic_,
           "switching to a solid background cancels outstanding blur and retains the selected native appearance");
    fast.HidePanel();
}
{
    App warm(GetModuleHandleW(nullptr), sandbox + L"\\warm-icons", storage.iconDirectory, storage.journalDirectory, {}, {});
    warm.Initialize(false, false);
    for (int i = 0; i < 40; ++i) WriteBytes(warm.storage.items + L"\\warm-" + std::to_wstring(i) + L".txt", "warm", 4);
    warm.Reload(); warm.WarmIcons();
    const auto deadline = GetTickCount64() + 10000;
    size_t loaded = 0;
    do {
        MSG message{};
        while (PeekMessageW(&message, warm.host, kIconReady, kIconReady, PM_REMOVE)) DispatchMessageW(&message);
        loaded = std::count_if(warm.items_.begin(), warm.items_.end(), [](const auto& item) { return item.icon.value != nullptr; });
        if (loaded < 12) Sleep(1);
    } while (loaded < 12 && GetTickCount64() < deadline);
    expect(loaded == 12 && !warm.PanelVisible() && !IsWindowVisible(warm.panel),
           "startup warms only the bounded first page of real file icons in the background without showing a panel");
    warm.HidePanel(); warm.dpi_ = 96; // Keep the fixed-budget cache checks independent of the machine's display scaling.
    warm.nativeMode_ = true; warm.nativeLocated_ = true;
    GetWindowRect(warm.host, &warm.nativeCell_); warm.nativeImage_ = warm.nativeCell_;
    warm.nativeImage_.right = warm.nativeImage_.left + 24;
    warm.nativeImage_.bottom = warm.nativeImage_.top + 24;
    warm.direction_ = box::Direction::Right;
    const POINT point{warm.nativeImage_.left + 2, warm.nativeImage_.top + 2};
    warm.NativeMouse(point, true, false);
    RECT placed{}; GetWindowRect(warm.panel, &placed);
    expect(placed.left == warm.nativeImage_.right + warm.Px(10),
           "native desktop panels open beside the visible icon instead of leaving the hidden host cell as a wide gap");
    expect(warm.PanelVisible() && warm.nativeLocated_ && !IsWindowVisible(warm.host),
           "hover uses its verified native icon geometry without blocking on another Explorer lookup or showing a duplicate entrance");
    expect(waitForBackdrop(warm), "the real hover path receives its complete background before a rapid reopen");
    const DWORD* retainedPixels = warm.backdrop_.Pixels().data();
    warm.HidePanel();
    expect(recentBackdrop.Bytes() > 0 && recentBackdrop.Bytes() <= 4 * 1024 * 1024,
           "rapid reopen retains only one backdrop within the fixed 4 MiB budget");
    warm.ShowPanel(true);
    expect(!warm.backdropPending_ && warm.backdrop_.Pixels().data() == retainedPixels && !recentBackdrop.Bytes(),
           "a same-position rapid reopen consumes the exact previous image without a capture, blur or full-frame copy");
    const size_t glyphCount = warm.textMasks_.Count();
    SendMessageW(warm.host, WM_TIMER, kTextCacheTimer, 0);
    expect(warm.textMasks_.Count() == glyphCount && !warm.textCacheUntil_,
           "an already queued text-cache timer cannot clear the glyphs of a reopened visible panel");
    warm.HidePanel(); warm.nativeMode_ = false;
    expect(backdropNeedsFlush, "hiding a panel records that its composed frame must clear before another background capture");
    SendMessageW(warm.host, kCompleteBackdropHide, 0, 0);
    expect(!backdropNeedsFlush, "the one-shot hide completion moves composition waiting out of the next settled hover");
    const ULONGLONG expiry = recentBackdrop.Until();
    recentBackdrop.Expire(warm.host, expiry - 1);
    expect(recentBackdrop.Bytes() > 0 && recentBackdrop.Until() == expiry, "a stale cache timer cannot evict a newly retained frame early");
    recentBackdrop.Expire(warm.host, expiry);
    expect(!recentBackdrop.Bytes() && !recentBackdrop.Until(), "the half-second one-shot deadline releases retained background memory completely");
    box::Backdrop large; large.SetSource(1024,1024,std::vector<DWORD>(1024 * 1024,0x00ABCDEF)); large.Blur(0);
    expect(!recentBackdrop.Retain(std::move(large), warm.host) && !recentBackdrop.Bytes(),
           "large high-DPI captures bypass the rapid-reopen cache instead of exceeding its memory budget");
}
{
    App cycles(GetModuleHandleW(nullptr), sandbox + L"\\paint-cycles", storage.iconDirectory, storage.journalDirectory, {}, {});
    cycles.Initialize(false, false); cycles.HidePanel(); cycles.backgroundMode_ = 2; cycles.ApplyAppearance();
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 100; ++i) { Check(cycles.ShowPanel() ? S_OK : E_FAIL); cycles.HidePanel(); }
    cycles.ReleaseTextCache(cycles.textCacheUntil_);
    const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    expect(after <= before + 2 && !cycles.panelSurface_.DC() && cycles.textMasks_.Bytes() == 0,
           "one hundred actual open paint close cycles leave no accumulating GDI buffers or filename caches");
}
{
    const auto quick = box::ReadDesktopGrid(144, false);
    expect(quick.cell.cx > 0 && quick.cell.cy > 0 && quick.iconSize == 72 && quick.occupied.empty(),
           "native-entry startup obtains DPI-aware metrics without enumerating the entire Explorer desktop");
    Manager startup(GetModuleHandleW(nullptr), sandbox + L"\\startup-ready", storage.iconDirectory); startup.Initialize(false);
    auto& entry = *startup.boxes_.front(); entry.nativeMode_ = true; entry.nativeLocated_ = false;
    expect(entry.AttachedToDesktop() && !entry.NativeReady(),
           "a created owner window is not mistaken for a fully located native desktop icon at logon");
    startup.BeginDesktopRetry();
    expect(startup.desktopRetryDeadline_ > GetTickCount64() && startup.desktopRetries_ == 0,
           "desktop readiness retries begin immediately with a bounded recovery window");
    startup.desktopRetryDeadline_ = 1; startup.RetryDesktop();
    expect(!startup.desktopRetryDeadline_, "startup recovery stops its timer after the deadline instead of polling forever");
    entry.nativeMode_ = false;
}
