// Exercise output and lifecycle guarantees instead of asserting machine-specific timings.
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
    const POINT point{warm.nativeImage_.left + 2, warm.nativeImage_.top + 2};
    warm.NativeMouse(point, true, false);
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
