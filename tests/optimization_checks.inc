// Included inside SelfTest so the checks can inspect actual private runtime state.
{
    Storage compact(sandbox + L"\\compact-settings");
    compact.Write(L"ShortValue", L"常用");
    const auto shortValue = compact.Read(L"ShortValue");
    expect(shortValue == L"常用" && shortValue.capacity() < 256 && compact.root.capacity() < 1024 &&
           compact.iconDirectory.capacity() < 1024 && compact.journalDirectory.capacity() < 1024,
           "short settings and normalized paths do not retain 64 KB allocations per string");
    const std::wstring longValue(8000, L'长'); compact.Write(L"LongValue", longValue);
    expect(compact.Read(L"LongValue") == longValue, "adaptive settings buffers preserve long Unicode values exactly");
    const auto longPath = compact.root + L"\\" + std::wstring(1400, L'文');
    wchar_t reference[32768]{}; const DWORD fullLength = GetFullPathNameW(longPath.c_str(), 32768, reference, nullptr);
    expect(fullLength && box::FullPath(longPath) == std::wstring(reference, fullLength),
           "compact path buffers preserve paths exceeding MAX_PATH without truncation");
}
{
    // Retain the old full-frame algorithm only as a test oracle.
    auto referenceBlur = [](std::vector<DWORD> pixels, int width, int height, int strength) {
        for (auto& pixel : pixels) pixel |= 0xFF000000;
        if (!strength) return pixels;
        std::vector<DWORD> scratch(pixels.size());
        const int radius = std::max(1, strength / 3);
        auto pass = [&](const std::vector<DWORD>& input, std::vector<DWORD>& output, bool horizontal) {
            const int lines = horizontal ? height : width, length = horizontal ? width : height;
            auto index = [&](int line, int pixel) { return horizontal ? line * width + pixel : pixel * width + line; };
            for (int line = 0; line < lines; ++line) {
                unsigned r = 0, g = 0, b = 0;
                auto add = [&](int pixel, bool remove) {
                    const DWORD color = input[index(line, std::clamp(pixel, 0, length - 1))];
                    if (remove) { r -= (color >> 16) & 255; g -= (color >> 8) & 255; b -= color & 255; }
                    else { r += (color >> 16) & 255; g += (color >> 8) & 255; b += color & 255; }
                };
                for (int pixel = -radius; pixel <= radius; ++pixel) add(pixel, false);
                for (int pixel = 0; pixel < length; ++pixel) {
                    output[index(line, pixel)] = 0xFF000000 | ((r / (radius * 2 + 1)) << 16) | ((g / (radius * 2 + 1)) << 8) | (b / (radius * 2 + 1));
                    add(pixel - radius, true); add(pixel + radius + 1, false);
                }
            }
        };
        for (int i = 0; i < 3; ++i) { pass(pixels, scratch, true); pass(scratch, pixels, false); }
        return pixels;
    };
    bool identical = true;
    for (const auto size : {SIZE{1, 1}, SIZE{2, 5}, SIZE{31, 17}, SIZE{64, 37}}) {
        std::vector<DWORD> pixels(static_cast<size_t>(size.cx) * size.cy);
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<DWORD>(i * 2654435761u);
        box::Backdrop glass; glass.SetSource(size.cx, size.cy, pixels);
        for (const int strength : {0, 1, 18, 40, 120}) {
            glass.Blur(strength);
            identical = identical && glass.Pixels() == referenceBlur(pixels, size.cx, size.cy, strength);
        }
    }
    expect(identical, "line-sized blur scratch matches the old full-frame algorithm pixel for pixel across sizes and strengths");
}
{
    struct Targets {
        HWND first = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        HWND second = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        ~Targets() { if (first) DestroyWindow(first); if (second) DestroyWindow(second); }
    } targets;
    Check(targets.first && targets.second ? S_OK : E_FAIL);
    IconLoader first, second; first.Start(targets.first); second.Start(targets.second);
    expect(first.worker_ == second.worker_ && first.worker_->clients.size() == 2,
           "multiple box icon loaders share exactly one background worker");
    const auto path = sandbox + L"\\worker-test.txt"; WriteBytes(path, "icon", 4);
    first.RequestIcon(7, 3, path, 36); second.RequestIcon(9, 5, path, 36);
    bool receivedFirst = false, receivedSecond = false, stale = false;
    auto waitIcons = [&](unsigned firstGeneration, unsigned secondGeneration) {
        const auto deadline = GetTickCount64() + 10000;
        while ((!receivedFirst || !receivedSecond) && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == kIconReady && (message.hwnd == targets.first || message.hwnd == targets.second)) {
                    std::unique_ptr<IconResult> result(reinterpret_cast<IconResult*>(message.lParam));
                    if (message.hwnd == targets.first && result->generation == firstGeneration && result->icon.value) receivedFirst = true;
                    else if (message.hwnd == targets.second && result->generation == secondGeneration && result->icon.value) receivedSecond = true;
                    else stale = true;
                } else { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (!receivedFirst || !receivedSecond) Sleep(1);
        }
    };
    waitIcons(7, 9);
    expect(receivedFirst && receivedSecond && !stale, "shared worker delivers separate generations and icons to the correct box windows");
    const auto oldClient = first.client_; first.Stop(); first.Start(targets.first);
    {
        std::lock_guard<std::mutex> lock(first.worker_->mutex);
        first.worker_->requests.push_back({targets.first, oldClient, 999, 0, path, 36});
    }
    receivedFirst = receivedSecond = false;
    first.RequestIcon(11, 1, path, 36); second.RequestIcon(13, 2, path, 36); waitIcons(11, 13);
    expect(receivedFirst && receivedSecond && !stale && first.client_ != oldClient,
           "closing or restarting one icon client leaves the other working and rejects late results for a reused window");
    first.Stop(); second.Stop();
    expect(IconLoader::sharedWorker_.expired(), "the shared icon thread and queue are released after the last client closes");
}
{
    App cache(GetModuleHandleW(nullptr), sandbox + L"\\cache-box", storage.iconDirectory, storage.journalDirectory, {}, {});
    cache.Initialize(false, false); cache.ShowPanel();
    expect(!cache.iconThreadStarted_, "an empty box does not create an idle icon worker");
    cache.HidePanel();
    for (int i = 0; i < 80; ++i) WriteBytes(cache.storage.items + L"\\cache-" + std::to_wstring(i) + L".txt", "cache", 5);
    cache.Reload();
    for (size_t i = 0; i < cache.items_.size(); ++i) { cache.items_[i].icon = Icon(CopyIcon(cache.folderIcon_.value)); cache.requestedIcons_[i] = true; }
    cache.firstRow_ = 5; cache.HidePanel();
    const auto kept = std::count_if(cache.items_.begin(), cache.items_.end(), [](const auto& item) { return item.icon.value != nullptr; });
    expect(kept == cache.VisibleRows() * 4 && cache.items_[20].icon.value && !cache.items_[0].icon.value && cache.totalItems_ == 80,
           "a closed box keeps its current visible icons while releasing offscreen icons without dropping files");
    cache.glassTransparency_ = 0; cache.ShowPanel();
    RECT rect{}; GetClientRect(cache.panel, &rect);
    expect(cache.customGlass_ && !cache.acrylic_ && !cache.backdrop_.Matches(rect.right, rect.bottom),
           "fully opaque glass preserves the opaque background without allocating screenshot or blur buffers");
    cache.glassTransparency_ = 55; cache.ApplyAppearance();
    expect(waitForBackdrop(cache) && cache.backdrop_.Matches(rect.right, rect.bottom), "making glass transparent again restores real capture and blur");
    cache.backgroundMode_ = 2; cache.ApplyAppearance();
    expect(!cache.customGlass_ && !cache.backdrop_.Matches(rect.right, rect.bottom), "switching from glass to a solid background releases existing backdrop memory immediately");
}
{
    Manager manager(GetModuleHandleW(nullptr), sandbox + L"\\pointer-settle", storage.iconDirectory); manager.Initialize(false);
    const auto before = manager.pointerChecks_;
    manager.SchedulePointerSettle(); const auto firstDue = manager.pointerSettleDue_;
    manager.SchedulePointerSettle();
    expect(manager.pointerSettlePending_ && manager.pointerSettleDue_ >= firstDue, "mouse recovery coalesces input into one pending final position check");
    manager.pointerSettleDue_ = 0;
    SendMessageW(manager.controller_, WM_TIMER, kPointerSettleTimer, 0);
    expect(!manager.pointerSettlePending_ && !manager.pointerSettleDue_ && manager.pointerChecks_ == before + 1,
           "final mouse recovery refreshes the desktop and rechecks a stationary pointer without another movement");
    SendMessageW(manager.controller_, WM_TIMER, kPointerSettleTimer, 0);
    expect(manager.pointerChecks_ == before + 1, "completed mouse recovery does not leave an idle polling timer or repeat a stale timer message");
}
