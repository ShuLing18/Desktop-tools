#pragma once
#include <windows.h>
#include <atomic>
#include <vector>

namespace box {
struct DesktopContextTarget { RECT bounds{}; UINT_PTR id = 0; };
class DesktopRightClick {
    POINT pressed_{};
    UINT_PTR candidate_ = 0;
    bool active_ = false;
public:
    UINT_PTR Handle(UINT message, POINT point, bool desktop, const std::vector<DesktopContextTarget>& targets);
    UINT_PTR HandleHit(UINT message, POINT point, UINT_PTR hit);
    bool Active() const { return active_; }
};
// Keep the low-level callback off the Shell/UI thread. Hover notifications are coalesced;
// context clicks use a small cached snapshot. COM, file operations and drawing stay on the UI thread.
class DesktopMouse {
    HWND target_ = nullptr;
    UINT message_ = 0;
    HANDLE thread_ = nullptr, ready_ = nullptr;
    DWORD threadId_ = 0, error_ = ERROR_SUCCESS;
    std::atomic<bool> pending_{false}, refresh_{false};
    SRWLOCK contextLock_ = SRWLOCK_INIT;
    std::vector<DesktopContextTarget> contextTargets_;
    HWND contextSurface_ = nullptr;
    UINT contextMessage_ = 0;
    HWND contextForeground_ = nullptr;
    UINT_PTR approvedContext_ = 0;
    bool rawInput_ = false;
    bool useHook_ = true;
    HHOOK hook_ = nullptr; // Owned exclusively by the listener thread.
    std::atomic<ULONGLONG> lastHookInput_{0}, lastHookRepair_{0};
    std::atomic<unsigned> hookEvents_{0}, rawEvents_{0}, hookInstallations_{0};
    std::atomic<bool> repairPending_{false};
    DesktopRightClick rightClick_;
    inline static thread_local DesktopMouse* current_ = nullptr;
    static DWORD WINAPI Run(void* context);
    static LRESULT CALLBACK Hook(int code, WPARAM message, LPARAM data);
    void Notify(bool refresh);
    bool InstallHook();
public:
    DesktopMouse() = default;
    ~DesktopMouse() { Stop(); }
    DesktopMouse(const DesktopMouse&) = delete;
    DesktopMouse& operator=(const DesktopMouse&) = delete;
    HRESULT Start(HWND target, UINT message, UINT contextMessage = 0, bool useHook = true);
    void SetContextTargets(HWND surface, std::vector<DesktopContextTarget> targets);
    void SetPointerContext(POINT point, bool desktop, HWND foreground);
    UINT_PTR ContextHit(POINT point);
    bool HandleRawInput(LPARAM input);
    void ObserveRawMouse(USHORT buttons);
    bool RequestHookRepair();
    bool RawInputEnabled() const { return rawInput_; }
    unsigned HookEvents() const { return hookEvents_.load(); }
    unsigned RawEvents() const { return rawEvents_.load(); }
    unsigned HookInstallations() const { return hookInstallations_.load(); }
    void Stop();
    bool Consume(); // Returns whether an icon move or desktop zoom may have occurred.
    DWORD ThreadId() const { return threadId_; }
};
}
