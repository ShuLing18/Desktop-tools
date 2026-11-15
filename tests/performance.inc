// Included in the test executable only. This uses isolated files and windows;
// it never registers desktop entries, startup tasks, hooks or file associations.
int PerformanceTest(const std::wstring& directory) {
    std::string report = "{\n  \"scenario\": \"5 boxes, 80 files each, 150% or current Windows DPI\",\n  \"samples\": [\n";
    bool firstSample = true;
    auto sample = [&](const char* stage) {
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        Check(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) ? S_OK : E_FAIL);
        DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
        unsigned threads = 0;
        KernelHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        if (snapshot.value != INVALID_HANDLE_VALUE && Thread32First(snapshot.value, &entry)) do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++threads;
        } while (Thread32Next(snapshot.value, &entry));
        if (!firstSample) report += ",\n";
        firstSample = false;
        report += "    {\"stage\":\"" + std::string(stage) + "\",\"private_bytes\":" + std::to_string(memory.PrivateUsage) +
                  ",\"working_set\":" + std::to_string(memory.WorkingSetSize) + ",\"peak_private_bytes\":" + std::to_string(memory.PeakPagefileUsage) +
                  ",\"threads\":" + std::to_string(threads) + ",\"handles\":" + std::to_string(handles) +
                  ",\"gdi_objects\":" + std::to_string(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) + "}";
    };
    auto pump = [] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    };
    int result = 0;
    try {
        const auto data = directory + L"\\data", icons = directory + L"\\ico";
        // Prepare the full reference load before starting the initialization clock.
        for (int boxIndex = 0; boxIndex < 5; ++boxIndex) {
            const auto folder = boxIndex == 0 ? data : data + L"\\boxes\\benchmark-" + std::to_wstring(boxIndex);
            Storage fixture(folder, icons, data + L"\\origins");
            fixture.Write(L"Deleted", L"0"); fixture.Write(L"BackgroundMode", L"0"); fixture.Write(L"GlassTransparency", L"55"); fixture.Write(L"GlassBlur", L"18");
            for (int i = 0; i < 80; ++i) WriteBytes(fixture.items + L"\\sample-" + std::to_wstring(i) + L".txt", "benchmark", 9);
        }
        LARGE_INTEGER initFrequency{}, initStart{}, initEnd{}; QueryPerformanceFrequency(&initFrequency);
        IO_COUNTERS initIoBefore{}, initIoAfter{}; GetProcessIoCounters(GetCurrentProcess(), &initIoBefore);
        QueryPerformanceCounter(&initStart);
        Manager manager(GetModuleHandleW(nullptr), data, icons); manager.Initialize(false);
        QueryPerformanceCounter(&initEnd); GetProcessIoCounters(GetCurrentProcess(), &initIoAfter);
        const double initializationMs = 1000. * (initEnd.QuadPart - initStart.QuadPart) / initFrequency.QuadPart;
        while (manager.boxes_.size() < 5) manager.NewBox(L"Benchmark " + std::to_wstring(manager.boxes_.size()), {600, 300});
        for (const auto& box : manager.boxes_) {
            for (int i = 0; i < 80; ++i) WriteBytes(box->storage.items + L"\\sample-" + std::to_wstring(i) + L".txt", "benchmark", 9);
            box->Refresh();
        }
        pump(); sample("initialized");
        LARGE_INTEGER frequency{}, started{}, finished{}; QueryPerformanceFrequency(&frequency);
        LONGLONG renderTicks = 0;
        std::vector<double> firstFrameMs, reopenMs, settledReopenMs, hotReopenMs, glassReadyMs, captureMs, flushMs, blurMs;
        auto openAndMeasure = [&](App& box, std::vector<double>& times) {
            LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
            Check(box.ShowPanel() ? S_OK : E_FAIL);
            UpdateWindow(box.panel);
            QueryPerformanceCounter(&end);
            times.push_back(1000. * (end.QuadPart - begin.QuadPart) / frequency.QuadPart);
        };
        for (const auto& box : manager.boxes_) {
            LARGE_INTEGER glassStarted{}; QueryPerformanceCounter(&glassStarted);
            openAndMeasure(*box, firstFrameMs);
            const auto deadline = GetTickCount64() + 10000;
            bool ready = false;
            do {
                pump();
                ready = !box->backdropPending_;
                const size_t count = std::min(box->items_.size(), static_cast<size_t>(box->VisibleRows() * 4));
                for (size_t i = 0; i < count; ++i) ready = ready && box->items_[i].icon.value;
                if (!ready) Sleep(1);
            } while (!ready && GetTickCount64() < deadline);
            if (!ready) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            QueryPerformanceCounter(&finished);
            glassReadyMs.push_back(1000. * (finished.QuadPart - glassStarted.QuadPart) / frequency.QuadPart);
            HDC screen = GetDC(box->panel); QueryPerformanceCounter(&started);
            HRESULT rendered = S_OK;
            for (int frame = 0; frame < 12 && SUCCEEDED(rendered); ++frame) rendered = box->RenderSurface(box->panel, true, screen);
            QueryPerformanceCounter(&finished); ReleaseDC(box->panel, screen); Check(rendered);
            renderTicks += finished.QuadPart - started.QuadPart;
            sample("panel_open"); box->HidePanel();
        }
        for (int cycle = 0; cycle < 3; ++cycle) for (const auto& box : manager.boxes_) {
            openAndMeasure(*box, reopenMs); pump(); box->HidePanel();
        }
        for (int cycle = 0; cycle < 3; ++cycle) for (const auto& box : manager.boxes_) {
            pump(); openAndMeasure(*box, settledReopenMs); pump(); box->HidePanel(); pump();
        }
        for (const auto& box : manager.boxes_) {
            Check(box->ShowPanel() ? S_OK : E_FAIL);
            const auto deadline = GetTickCount64() + 5000;
            while (box->backdropPending_ && GetTickCount64() < deadline) { pump(); Sleep(1); }
            if (box->backdropPending_) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            for (int cycle = 0; cycle < 3; ++cycle) { box->HidePanel(); openAndMeasure(*box, hotReopenMs); pump(); }
            box->HidePanel(); pump();
        }
        sample("all_panels_closed");
        const auto cacheDeadline = GetTickCount64() + 2500;
        auto cachePending = [&] { return recentBackdrop.Bytes() || std::any_of(manager.boxes_.begin(), manager.boxes_.end(), [](const auto& box) { return box->textCacheUntil_ != 0; }); };
        while (cachePending() && GetTickCount64() < cacheDeadline) { pump(); Sleep(1); }
        if (cachePending()) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        pump(); sample("idle_after_cache_expiry");
        FILETIME created{}, exited{}, kernelBefore{}, userBefore{}, kernelAfter{}, userAfter{};
        IO_COUNTERS idleIoBefore{}, idleIoAfter{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelBefore, &userBefore);
        GetProcessIoCounters(GetCurrentProcess(), &idleIoBefore);
        const auto idleStarted = GetTickCount64(), idleDeadline = idleStarted + 10000;
        while (GetTickCount64() < idleDeadline) {
            const auto now = GetTickCount64(); if (now >= idleDeadline) break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(idleDeadline - now), QS_ALLINPUT);
            pump();
        }
        const auto idleWallMs = GetTickCount64() - idleStarted;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelAfter, &userAfter);
        GetProcessIoCounters(GetCurrentProcess(), &idleIoAfter);
        auto ticks = [](FILETIME time) { return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        const double idleCpuMs = (ticks(kernelAfter) + ticks(userAfter) - ticks(kernelBefore) - ticks(userBefore)) / 10000.;
        sample("after_idle_observation");
        RECT captureRect{}; GetWindowRect(manager.boxes_.front()->panel, &captureRect);
        for (int probe = 0; probe < 5; ++probe) {
            box::Backdrop capture;
            QueryPerformanceCounter(&started); DwmFlush(); QueryPerformanceCounter(&finished);
            flushMs.push_back(1000. * (finished.QuadPart - started.QuadPart) / frequency.QuadPart);
            QueryPerformanceCounter(&started); Check(capture.Capture(captureRect) ? S_OK : E_FAIL); QueryPerformanceCounter(&finished);
            captureMs.push_back(1000. * (finished.QuadPart - started.QuadPart) / frequency.QuadPart);
            QueryPerformanceCounter(&started); Check(capture.Blur(manager.boxes_.front()->Px(18)) ? S_OK : E_FAIL); QueryPerformanceCounter(&finished);
            blurMs.push_back(1000. * (finished.QuadPart - started.QuadPart) / frequency.QuadPart);
        }
        report += "\n  ],\n  \"render_60_frames_ms\": " + std::to_string(1000. * renderTicks / frequency.QuadPart);
        report += ",\n  \"initialization_ms\": " + std::to_string(initializationMs) +
                  ",\n  \"initialization_read_operations\": " + std::to_string(initIoAfter.ReadOperationCount - initIoBefore.ReadOperationCount) +
                  ",\n  \"initialization_write_operations\": " + std::to_string(initIoAfter.WriteOperationCount - initIoBefore.WriteOperationCount) +
                  ",\n  \"idle_wall_ms\": " + std::to_string(idleWallMs) +
                  ",\n  \"idle_cpu_ms\": " + std::to_string(idleCpuMs) +
                  ",\n  \"idle_single_core_percent\": " + std::to_string(idleCpuMs * 100. / idleWallMs) +
                  ",\n  \"idle_read_operations\": " + std::to_string(idleIoAfter.ReadOperationCount - idleIoBefore.ReadOperationCount) +
                  ",\n  \"idle_write_operations\": " + std::to_string(idleIoAfter.WriteOperationCount - idleIoBefore.WriteOperationCount);
        auto addTimes = [&](const char* name, const std::vector<double>& times) {
            report += ",\n  \"" + std::string(name) + "\": [";
            for (size_t i = 0; i < times.size(); ++i) { if (i) report += ", "; report += std::to_string(times[i]); }
            report += "]";
        };
        addTimes("first_frame_ms", firstFrameMs); addTimes("reopen_first_frame_ms", reopenMs);
        addTimes("settled_reopen_first_frame_ms", settledReopenMs);
        addTimes("same_box_rapid_reopen_ms", hotReopenMs);
        addTimes("glass_and_icons_ready_ms", glassReadyMs);
        addTimes("capture_probe_ms", captureMs); addTimes("composition_flush_probe_ms", flushMs); addTimes("blur_probe_ms", blurMs);
        report += "\n}\n";
    } catch (...) { result = 1; report += "\n  ],\n  \"error\":true\n}\n"; }
    if (!WriteBytes(directory + L"\\metrics.json", report.data(), static_cast<DWORD>(report.size()))) return 2;
    return result;
}
