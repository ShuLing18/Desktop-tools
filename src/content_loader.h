#pragma once
#include "storage.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace box {
struct ContentResult {
    unsigned generation = 0;
    HRESULT status = S_OK;
    std::vector<StorageEntry> entries;
};

// One shared filesystem worker. Each live box has at most one pending scan;
// replaced requests cancel during enumeration and cannot deliver to a reused HWND.
class ContentLoader {
    struct Client {
        HWND destination;
        UINT message;
        std::atomic<unsigned> generation{0};
        bool active = true; // Protected by State::mutex, including final delivery.
        Client(HWND window, UINT notification) : destination(window), message(notification) {}
    };
    struct Request { std::shared_ptr<Client> client; unsigned generation; Storage storage; };
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Request> pending;
        std::atomic<bool> stop{false};
    };
    class Worker {
        static DWORD WINAPI Run(void* context) {
            std::unique_ptr<std::shared_ptr<State>> holder(static_cast<std::shared_ptr<State>*>(context));
            const auto state = *holder; holder.reset();
            for (;;) {
                std::unique_ptr<Request> request;
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->wake.wait(lock, [&] { return state->stop.load() || !state->pending.empty(); });
                    if (state->stop.load()) return 0;
                    request = std::make_unique<Request>(std::move(state->pending.front())); state->pending.pop_front();
                }
                auto cancelled = [&] { return state->stop.load() || request->client->generation.load() != request->generation; };
                if (cancelled()) continue;
                auto result = std::make_unique<ContentResult>(); result->generation = request->generation;
                try { result->entries = request->storage.Snapshot(true, cancelled); }
                catch (HRESULT status) { result->status = status; }
                catch (...) { result->status = E_OUTOFMEMORY; }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->stop.load() && request->client->active && !cancelled() &&
                    PostMessageW(request->client->destination, request->client->message, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
            }
        }
    public:
        std::shared_ptr<State> state = std::make_shared<State>();
        HANDLE thread = nullptr;
        Worker() {
            auto context = std::make_unique<std::shared_ptr<State>>(state);
            thread = CreateThread(nullptr, 0, Run, context.get(), 0, nullptr);
            if (!thread) throw HRESULT_FROM_WIN32(GetLastError());
            context.release();
        }
        ~Worker() {
            { std::lock_guard<std::mutex> lock(state->mutex); state->stop.store(true); state->pending.clear(); }
            state->wake.notify_one(); CancelSynchronousIo(thread);
            WaitForSingleObject(thread, 500); CloseHandle(thread);
        }
    };
    inline static std::weak_ptr<Worker> sharedWorker_;
    std::shared_ptr<Worker> worker_;
    std::shared_ptr<Client> client_;
public:
    ~ContentLoader() { Stop(); }
    void Start(HWND destination, UINT message) {
        if (client_ && client_->destination == destination && client_->message == message) return;
        Stop();
        worker_ = sharedWorker_.lock();
        if (!worker_) { worker_ = std::make_shared<Worker>(); sharedWorker_ = worker_; }
        client_ = std::make_shared<Client>(destination, message);
    }
    unsigned RequestSnapshot(const Storage& storage, bool priority = false) {
        if (!worker_ || !client_) return 0;
        const unsigned generation = ++client_->generation;
        auto& state = *worker_->state;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            auto& pending = state.pending;
            for (auto item = pending.begin(); item != pending.end();) {
                if (item->client == client_) item = pending.erase(item); else ++item;
            }
            Request request{client_, generation, storage};
            if (priority) pending.push_front(std::move(request)); else pending.push_back(std::move(request));
        }
        state.wake.notify_one(); return generation;
    }
    void Prioritize() {
        if (!worker_ || !client_) return;
        auto& state = *worker_->state; std::lock_guard<std::mutex> lock(state.mutex);
        for (auto item = state.pending.begin(); item != state.pending.end(); ++item) if (item->client == client_) {
            if (item != state.pending.begin()) { auto request = std::move(*item); state.pending.erase(item); state.pending.push_front(std::move(request)); }
            break;
        }
    }
    void Stop() {
        if (worker_ && client_) {
            auto& state = *worker_->state;
            { std::lock_guard<std::mutex> lock(state.mutex); client_->active = false; ++client_->generation;
              for (auto item = state.pending.begin(); item != state.pending.end();) {
                  if (item->client == client_) item = state.pending.erase(item); else ++item;
              }
            }
            MSG message{};
            while (PeekMessageW(&message, client_->destination, client_->message, client_->message, PM_REMOVE))
                delete reinterpret_cast<ContentResult*>(message.lParam);
        }
        client_.reset(); worker_.reset();
    }
};
}
