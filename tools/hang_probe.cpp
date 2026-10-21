// Read-only diagnostic of DesktopBox's message loop and thread wait chains.
#include <windows.h>
#include <tlhelp32.h>
#include <wct.h>
#include <cstdio>

BOOL CALLBACK Window(HWND window, LPARAM parameter) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    if (process != static_cast<DWORD>(parameter)) return TRUE;
    wchar_t type[128]{}; GetClassNameW(window, type, 128);
    RECT rect{}; GetWindowRect(window, &rect);
    DWORD_PTR reply = 0; const ULONGLONG start = GetTickCount64();
    const bool responsive = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 400, &reply) != 0;
    std::printf("window=%p class=%ls visible=%d enabled=%d responsive=%d time=%llu rect=%ld,%ld,%ld,%ld\n",
        window, type, IsWindowVisible(window), IsWindowEnabled(window), responsive,
        GetTickCount64() - start, rect.left, rect.top, rect.right, rect.bottom);
    return TRUE;
}
int main() {
    HWND controller = FindWindowW(L"DesktopBox.Controller.02", L"桌面收纳盒");
    DWORD process = 0; const DWORD mainThread = GetWindowThreadProcessId(controller, &process);
    std::printf("pid=%lu main_thread=%lu controller=%p\n", process, mainThread, controller);
    if (!process) return 1;
    EnumWindows(Window, process);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    HWCT session = OpenThreadWaitChainSession(0, nullptr);
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE && session && Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID != process) continue;
        WAITCHAIN_NODE_INFO nodes[16]{}; DWORD count = 16; BOOL cycle = FALSE;
        const BOOL ok = GetThreadWaitChain(session, 0, 0, entry.th32ThreadID, &count, nodes, &cycle);
        std::printf("thread=%lu success=%d cycle=%d count=%lu error=%lu\n", entry.th32ThreadID, ok, cycle, count, ok ? 0 : GetLastError());
        if (ok) for (DWORD i = 0; i < count; ++i) {
            std::printf("  type=%d status=%d", nodes[i].ObjectType, nodes[i].ObjectStatus);
            if (nodes[i].ObjectType == WctThreadType) std::printf(" pid=%lu tid=%lu", nodes[i].ThreadObject.ProcessId, nodes[i].ThreadObject.ThreadId);
            std::printf("\n");
        }
    } while (Thread32Next(snapshot, &entry));
    if (session) CloseThreadWaitChainSession(session);
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    return 0;
}
