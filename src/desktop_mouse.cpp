#include "desktop_mouse.h"

namespace box {
UINT_PTR DesktopRightClick::Handle(UINT message, POINT point, bool desktop, const std::vector<DesktopContextTarget>& targets) {
    UINT_PTR hit = 0;
    if (desktop) for (const auto& target : targets) if (PtInRect(&target.bounds, point)) { hit = target.id; break; }
    return HandleHit(message, point, hit);
}
UINT_PTR DesktopRightClick::HandleHit(UINT message, POINT point, UINT_PTR hit) {
    if (message == WM_RBUTTONDOWN) {
        candidate_ = hit; active_ = false; pressed_ = point;
        active_ = candidate_ != 0;
    }
    if (message == WM_MOUSEMOVE || message == WM_RBUTTONUP) {
        if (abs(point.x - pressed_.x) > GetSystemMetrics(SM_CXDRAG) || abs(point.y - pressed_.y) > GetSystemMetrics(SM_CYDRAG)) candidate_ = 0;
    }
    if (message != WM_RBUTTONUP) return 0;
    const UINT_PTR candidate = candidate_; candidate_ = 0; active_ = false;
    return candidate && hit == candidate ? candidate : 0;
}
HRESULT DesktopMouse::Start(HWND target, UINT message, UINT contextMessage, bool useHook) {
    Stop();
    if (!IsWindow(target) || message < WM_APP) return E_INVALIDARG;
    target_ = target; message_ = message; contextMessage_ = contextMessage; useHook_ = useHook; error_ = ERROR_SUCCESS;
    RAWINPUTDEVICE device{0x01, 0x02, RIDEV_INPUTSINK, target_};
    rawInput_ = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) return HRESULT_FROM_WIN32(GetLastError());
    thread_ = CreateThread(nullptr, 0, Run, this, 0, &threadId_);
    if (!thread_) {
        const DWORD error = GetLastError(); CloseHandle(ready_); ready_ = nullptr;
        return HRESULT_FROM_WIN32(error);
    }
    WaitForSingleObject(ready_, INFINITE);
    CloseHandle(ready_); ready_ = nullptr;
    const HRESULT result = HRESULT_FROM_WIN32(error_);
    if (FAILED(result)) Stop();
    return result;
}
void DesktopMouse::SetContextTargets(HWND surface, std::vector<DesktopContextTarget> targets) {
    AcquireSRWLockExclusive(&contextLock_);
    contextSurface_ = surface; contextTargets_.swap(targets);
    approvedContext_ = 0;
    ReleaseSRWLockExclusive(&contextLock_);
}
void DesktopMouse::SetPointerContext(POINT point, bool desktop, HWND foreground) {
    AcquireSRWLockExclusive(&contextLock_);
    approvedContext_ = 0; contextForeground_ = foreground;
    if (desktop && contextSurface_) for (const auto& target : contextTargets_)
        if (PtInRect(&target.bounds, point)) { approvedContext_ = target.id; break; }
    ReleaseSRWLockExclusive(&contextLock_);
}
UINT_PTR DesktopMouse::ContextHit(POINT point) {
    // Window queries can synchronously call another window procedure. The hook must
    // never perform them or wait for the UI while holding its snapshot lock.
    const HWND foreground = GetForegroundWindow();
    if (!TryAcquireSRWLockShared(&contextLock_)) return 0;
    UINT_PTR hit = 0;
    if (approvedContext_ && foreground == contextForeground_) for (const auto& target : contextTargets_)
        if (target.id == approvedContext_ && PtInRect(&target.bounds, point)) { hit = target.id; break; }
    ReleaseSRWLockShared(&contextLock_);
    return hit;
}
void DesktopMouse::Notify(bool refresh) {
    if (refresh) refresh_.store(true);
    if (!pending_.exchange(true) && !PostMessageW(target_, message_, 0, 0)) pending_.store(false);
}
bool DesktopMouse::HandleRawInput(LPARAM input) {
    RAWINPUT raw{}; UINT bytes = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(input), RID_INPUT, &raw, &bytes, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        bytes < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) || raw.header.dwType != RIM_TYPEMOUSE) return false;
    ObserveRawMouse(raw.data.mouse.usButtonFlags); return true;
}
void DesktopMouse::ObserveRawMouse(USHORT buttons) {
    rawEvents_.fetch_add(1, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    // Healthy hooks already post an immediate wake-up. Raw Input remains independent
    // when Windows removes a timed-out hook, without polling the mouse while idle.
    if (now - lastHookInput_.load() > 8) {
        Notify((buttons & (RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) != 0);
        if (now - lastHookInput_.load() > 2000 && now - lastHookRepair_.load() > 2000) RequestHookRepair();
    }
}
bool DesktopMouse::RequestHookRepair() {
    if (!thread_ || !useHook_ || repairPending_.exchange(true)) return false;
    if (PostThreadMessageW(threadId_, WM_APP + 31, 0, 0)) return true;
    repairPending_.store(false); return false;
}
bool DesktopMouse::InstallHook() {
    if (hook_) UnhookWindowsHookEx(hook_);
    if (!useHook_) { hook_ = nullptr; return false; }
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, Hook, GetModuleHandleW(nullptr), 0);
    if (hook_) hookInstallations_.fetch_add(1);
    lastHookRepair_.store(GetTickCount64()); repairPending_.store(false);
    return hook_ != nullptr;
}

void DesktopMouse::Stop() {
    if (thread_) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
        // A different hook in the chain may be servicing one of our windows. Pump
        // sent messages while joining; do not dispatch posted commands during shutdown.
        while (MsgWaitForMultipleObjects(1, &thread_, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
            MSG sent{}; PeekMessageW(&sent, nullptr, 0, 0, PM_NOREMOVE);
        }
        CloseHandle(thread_); thread_ = nullptr;
    }
    threadId_ = 0; target_ = nullptr;
    pending_.store(false); refresh_.store(false);
    SetContextTargets(nullptr, {}); rightClick_ = DesktopRightClick{};
    if (rawInput_) { RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr}; RegisterRawInputDevices(&device, 1, sizeof(device)); rawInput_ = false; }
    lastHookInput_.store(0); lastHookRepair_.store(0); repairPending_.store(false);
}

DWORD WINAPI DesktopMouse::Run(void* context) {
    auto& listener = *static_cast<DesktopMouse*>(context);
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // Create the thread queue before signalling ready.
    current_ = &listener;
    const bool installed = listener.InstallHook();
    listener.error_ = installed || listener.rawInput_ ? ERROR_SUCCESS : GetLastError();
    SetEvent(listener.ready_);
    if (installed || listener.rawInput_) {
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.hwnd == nullptr && message.message == WM_APP + 31) { listener.InstallHook(); continue; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (listener.hook_) UnhookWindowsHookEx(listener.hook_);
    }
    listener.hook_ = nullptr;
    current_ = nullptr;
    return 0;
}

LRESULT CALLBACK DesktopMouse::Hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && current_) {
        current_->lastHookInput_.store(GetTickCount64()); current_->hookEvents_.fetch_add(1, std::memory_order_relaxed);
        if (message == WM_MOUSEMOVE) current_->rightClick_.HandleHit(WM_MOUSEMOVE, reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt, 0);
        if (current_->contextMessage_ && (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)) {
            const bool wasCaptured = current_->rightClick_.Active();
                const POINT point = reinterpret_cast<const MSLLHOOKSTRUCT*>(data)->pt;
                const UINT_PTR selected = current_->rightClick_.HandleHit(static_cast<UINT>(message), point, current_->ContextHit(point));
                // Consume the pair together so Explorer cannot be left holding capture after its button-down.
                // Native left-drag, selection, F2 and all other desktop icons continue through unchanged.
                if (message == WM_RBUTTONDOWN && current_->rightClick_.Active()) return 1;
                if (message == WM_RBUTTONUP && wasCaptured) {
                    if (selected) PostMessageW(current_->target_, current_->contextMessage_, selected, MAKELPARAM(point.x, point.y));
                    return 1;
                }
        }
        const bool refresh = message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
                             message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
        if (refresh || message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) current_->Notify(refresh);
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool DesktopMouse::Consume() {
    pending_.store(false);
    return refresh_.exchange(false);
}
}
