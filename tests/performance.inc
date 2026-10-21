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
        Storage first(data, icons, data + L"\\origins");
        first.Write(L"Deleted", L"0"); first.Write(L"BackgroundMode", L"0"); first.Write(L"GlassTransparency", L"55"); first.Write(L"GlassBlur", L"18");
        for (int i = 0; i < 80; ++i) WriteBytes(first.items + L"\\sample-" + std::to_wstring(i) + L".txt", "benchmark", 9);
        Manager manager(GetModuleHandleW(nullptr), data, icons); manager.Initialize(false);
        while (manager.boxes_.size() < 5) manager.NewBox(L"Benchmark " + std::to_wstring(manager.boxes_.size()), {600, 300});
        for (const auto& box : manager.boxes_) {
            for (int i = 0; i < 80; ++i) WriteBytes(box->storage.items + L"\\sample-" + std::to_wstring(i) + L".txt", "benchmark", 9);
            box->Refresh();
        }
        pump(); sample("initialized");
        LARGE_INTEGER frequency{}, started{}, finished{}; QueryPerformanceFrequency(&frequency);
        LONGLONG renderTicks = 0;
        for (const auto& box : manager.boxes_) {
            box->ShowPanel();
            const auto deadline = GetTickCount64() + 10000;
            bool ready = false;
            do {
                pump();
                ready = true;
                const size_t count = std::min(box->items_.size(), static_cast<size_t>(box->VisibleRows() * 4));
                for (size_t i = 0; i < count; ++i) ready = ready && box->items_[i].icon.value;
                if (!ready) Sleep(1);
            } while (!ready && GetTickCount64() < deadline);
            if (!ready) throw HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            HDC screen = GetDC(box->panel); QueryPerformanceCounter(&started);
            HRESULT rendered = S_OK;
            for (int frame = 0; frame < 12 && SUCCEEDED(rendered); ++frame) rendered = box->RenderSurface(box->panel, true, screen);
            QueryPerformanceCounter(&finished); ReleaseDC(box->panel, screen); Check(rendered);
            renderTicks += finished.QuadPart - started.QuadPart;
            sample("panel_open"); box->HidePanel();
        }
        pump(); sample("all_panels_closed");
        report += "\n  ],\n  \"render_60_frames_ms\": " + std::to_string(1000. * renderTicks / frequency.QuadPart) + "\n}\n";
    } catch (...) { result = 1; report += "\n  ],\n  \"error\":true\n}\n"; }
    if (!WriteBytes(directory + L"\\metrics.json", report.data(), static_cast<DWORD>(report.size()))) return 2;
    return result;
}
