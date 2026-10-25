#pragma once
#include "backdrop.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

namespace box {
struct BackdropResult {
    ULONGLONG generation = 0;
    Backdrop value;
};

// Only one panel is visible. Keep one active calculation and one latest request,
// sharing immutable capture pixels rather than making a full-frame copy per tick.
class BackdropLoader {
    struct Request {
        HWND destination;
        UINT message;
        ULONGLONG client, generation;
        int strength;
        Backdrop source;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    struct Worker {
        std::mutex mutex;
        std::condition_variable event;
        std::vector<ULONGLONG> clients;
        std::unique_ptr<Request> pending;
        std::shared_ptr<std::atomic<bool>> runningCancel;
        ULONGLONG nextClient = 0, runningClient = 0;
        bool stop = false;
        std::thread thread;
        Worker() : thread([this] { Run(); }) {}
        ~Worker() {
            {
                std::lock_guard<std::mutex> lock(mutex); stop = true; pending.reset();
                if (runningCancel) runningCancel->store(true, std::memory_order_relaxed);
            }
            event.notify_one(); if (thread.joinable()) thread.join();
        }
        void Run() {
            for (;;) {
                std::unique_ptr<Request> request;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    event.wait(lock, [this] { return stop || pending != nullptr; });
                    if (stop) return;
                    request = std::move(pending);
                    runningClient = request->client; runningCancel = request->cancelled;
                }
                bool ready = false;
                try { ready = request->source.Blur(request->strength, request->cancelled.get()); } catch (...) {}
                std::lock_guard<std::mutex> lock(mutex);
                runningClient = 0; runningCancel.reset();
                if (request->cancelled->load(std::memory_order_relaxed) ||
                    std::find(clients.begin(), clients.end(), request->client) == clients.end()) continue;
                std::unique_ptr<BackdropResult> result(new (std::nothrow) BackdropResult());
                if (result) {
                    result->generation = request->generation;
                    if (ready) result->value = std::move(request->source);
                }
                if (PostMessageW(request->destination, request->message, static_cast<WPARAM>(request->generation), reinterpret_cast<LPARAM>(result.get()))) result.release();
            }
        }
    };
    inline static std::weak_ptr<Worker> sharedWorker_;
    std::shared_ptr<Worker> worker_;
    HWND destination_ = nullptr;
    UINT message_ = 0;
    ULONGLONG client_ = 0;
public:
    BackdropLoader() = default;
    BackdropLoader(const BackdropLoader&) = delete;
    BackdropLoader& operator=(const BackdropLoader&) = delete;
    void Start(HWND destination, UINT message) {
        if (worker_ && destination_ == destination && message_ == message) return;
        Stop(); auto worker = sharedWorker_.lock();
        if (!worker) { worker = std::make_shared<Worker>(); sharedWorker_ = worker; }
        { std::lock_guard<std::mutex> lock(worker->mutex); client_ = ++worker->nextClient; worker->clients.push_back(client_); }
        destination_ = destination; message_ = message; worker_ = std::move(worker);
    }
    void RequestBlur(const Backdrop& source, int strength, ULONGLONG generation) {
        if (!worker_) return;
        auto request = std::make_unique<Request>(Request{destination_, message_, client_, generation,
            strength, source.SourceSnapshot(), std::make_shared<std::atomic<bool>>(false)});
        {
            std::lock_guard<std::mutex> lock(worker_->mutex);
            if (worker_->runningCancel) worker_->runningCancel->store(true, std::memory_order_relaxed);
            worker_->pending = std::move(request);
        }
        worker_->event.notify_one();
    }
    void Cancel() {
        if (!worker_) return;
        std::lock_guard<std::mutex> lock(worker_->mutex);
        if (worker_->pending && worker_->pending->client == client_) worker_->pending.reset();
        if (worker_->runningClient == client_ && worker_->runningCancel)
            worker_->runningCancel->store(true, std::memory_order_relaxed);
    }
    void Stop() {
        Cancel();
        if (worker_) {
            std::lock_guard<std::mutex> lock(worker_->mutex);
            auto& clients = worker_->clients;
            clients.erase(std::remove(clients.begin(), clients.end(), client_), clients.end());
        }
        MSG message{};
        while (destination_ && PeekMessageW(&message, destination_, message_, message_, PM_REMOVE))
            delete reinterpret_cast<BackdropResult*>(message.lParam);
        destination_ = nullptr; message_ = 0; client_ = 0; worker_.reset();
    }
    ~BackdropLoader() { Stop(); }
};
}
