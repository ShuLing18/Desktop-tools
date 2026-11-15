#pragma once
#include "native_entry.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace box {
struct DesktopBoundsResult {
    ULONGLONG generation = 0;
    HWND desktop = nullptr;
    ULONGLONG elapsed = 0;
    std::vector<NativeBounds> entries;
};

// Shell startup may block a COM call. Keep it off the input/UI thread, with
// one active snapshot and at most one latest pending request. COM pointers
// never leave the worker apartment.
class DesktopBoundsLoader {
public:
    using Query = std::vector<NativeBounds>(*)(const std::vector<std::wstring>&, HWND);
private:
    struct Request {
        HWND destination, desktop;
        UINT message;
        ULONGLONG generation;
        std::vector<std::wstring> paths;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::unique_ptr<Request> pending;
        bool stop = false, running = false;
        Query query = ReadNativeBounds;
    };
    std::shared_ptr<State> state_;
    HANDLE thread_ = nullptr;
    DWORD threadId_ = 0;
    HWND destination_ = nullptr;
    UINT message_ = 0;
    Query query_;
    static DWORD WINAPI Run(void* context) {
        std::unique_ptr<std::shared_ptr<State>> holder(static_cast<std::shared_ptr<State>*>(context));
        auto state = *holder; holder.reset();
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            std::unique_ptr<Request> request;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stop || state->pending != nullptr; });
                if (state->stop) break;
                request = std::move(state->pending); state->running = true;
            }
            std::unique_ptr<DesktopBoundsResult> result;
            const ULONGLONG started = GetTickCount64();
            const bool cancellable = SUCCEEDED(initialized) && SUCCEEDED(CoEnableCallCancellation(nullptr));
            try {
                result = std::make_unique<DesktopBoundsResult>();
                result->generation = request->generation; result->desktop = request->desktop;
                if (SUCCEEDED(initialized)) result->entries = state->query(request->paths, request->desktop);
                result->elapsed = GetTickCount64() - started;
            } catch (...) {}
            if (cancellable) CoDisableCallCancellation(nullptr);
            {
                std::lock_guard<std::mutex> lock(state->mutex); state->running = false;
                if (!state->stop && PostMessageW(request->destination, request->message, static_cast<WPARAM>(request->generation),
                    reinterpret_cast<LPARAM>(result.get()))) result.release();
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        return 0;
    }
public:
    explicit DesktopBoundsLoader(Query query = ReadNativeBounds) : query_(query) {}
    ~DesktopBoundsLoader() { Stop(); }
    DesktopBoundsLoader(const DesktopBoundsLoader&) = delete;
    DesktopBoundsLoader& operator=(const DesktopBoundsLoader&) = delete;
    bool Busy() const {
        if (!state_) return false;
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->running || state_->pending != nullptr;
    }
    bool RequestBounds(HWND destination, UINT message, HWND desktop,
                       std::vector<std::wstring> paths, ULONGLONG generation) {
        if (!IsWindow(destination) || message < WM_APP) return false;
        if (!state_) {
            auto state = std::make_shared<State>(); state->query = query_;
            auto context = std::make_unique<std::shared_ptr<State>>(state);
            thread_ = CreateThread(nullptr, 0, Run, context.get(), 0, &threadId_);
            if (!thread_) return false;
            context.release(); state_ = std::move(state);
        }
        destination_ = destination; message_ = message;
        auto request = std::make_unique<Request>(Request{destination, desktop, message, generation, std::move(paths)});
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->pending = std::move(request); }
        state_->wake.notify_one(); return true;
    }
    void Stop() {
        if (state_) {
            { std::lock_guard<std::mutex> lock(state_->mutex); state_->stop = true; state_->pending.reset(); }
            state_->wake.notify_one();
        }
        if (thread_) {
            CoCancelCall(threadId_, 0);
            // Never make quitting depend on an unresponsive Explorer. A still
            // returning worker owns its state and cannot post to the old HWND.
            WaitForSingleObject(thread_, 500); CloseHandle(thread_); thread_ = nullptr; threadId_ = 0;
        }
        MSG message{};
        while (destination_ && PeekMessageW(&message, destination_, message_, message_, PM_REMOVE))
            delete reinterpret_cast<DesktopBoundsResult*>(message.lParam);
        state_.reset(); destination_ = nullptr; message_ = 0;
    }
};
}
