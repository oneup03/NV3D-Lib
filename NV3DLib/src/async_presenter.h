#pragma once

// Single-slot producer/consumer queue + worker thread. Used by each backend
// to defer the slow path of Present() (the D3D11_QUERY_EVENT spin and/or the
// vsync'd D3D9 PresentEx) off the host's calling thread.
//
// Usage pattern (DX12-style full async):
//   AsyncPresenter async_;
//   async_.Start();
//   ...
//   HRESULT Present() {
//       // SetInputTexture has already cached the input state.
//       // Submit the per-frame work to the worker; this blocks only if the
//       // previous Submit() hasn't completed yet (single-slot queue).
//       return async_.Submit([this]() { return PresentSyncBody(); });
//   }
//
// Usage pattern (DX11/OGL split async — only the D3D9 portion deferred):
//   HRESULT Present() {
//       // ... host-context bridge work runs synchronously here ...
//       return async_.Submit([this]() {
//           return presenter_->Present(d3d9_sfc, w, h);
//       });
//   }
//
// Threading contract:
//   - Start() must be called from a single thread before any Submit().
//   - Submit() is single-producer: the host must serialise its own calls,
//     which is the normal case (Present from the render thread).
//   - Stop() drains and joins the worker. Safe to call from any thread,
//     but only once.
//   - The work function is invoked on the worker thread and must be safe
//     to run there — i.e. it must only touch resources the lib owns or
//     resources whose thread-safety the host has explicitly opted into.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <windows.h>

namespace NV3D {

class AsyncPresenter {
public:
    using WorkFn = std::function<HRESULT()>;

    AsyncPresenter() = default;
    ~AsyncPresenter() { Stop(); }

    AsyncPresenter(const AsyncPresenter&) = delete;
    AsyncPresenter& operator=(const AsyncPresenter&) = delete;

    // Spawn the worker thread. No-op if already started.
    void Start() {
        if (worker_.joinable()) return;
        stop_.store(false);
        work_pending_.store(false);
        last_result_.store(S_OK);
        worker_ = std::thread([this]() { Loop(); });
    }

    // Drain the in-flight work (if any), signal the worker to exit, join.
    // Safe to call multiple times.
    void Stop() {
        if (!worker_.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_.store(true);
        }
        cv_work_.notify_all();
        worker_.join();
    }

    // Submit a work item.
    //
    // If the worker is still processing a prior submission, wait briefly
    // (`submit_timeout_ms_`) and then DROP this submission rather than
    // back-pressuring the host. Real-time consumers (games) need their
    // render thread to keep ticking on a steady cadence — blocking here
    // when the worker hitches causes the host's audio/video clocks to
    // drift apart (audio thread runs on its own clock, video presents
    // pause). One dropped stereo frame just leaves the previous frame on
    // the 3D Vision panel for one extra flip; that's invisible to most
    // viewers and far less disruptive than a present-thread stall.
    //
    // Returns the last completed work item's HRESULT (S_OK on a healthy
    // first frame). Drops are silent — they look like S_OK to the caller.
    // The wait timeout is tunable via SetSubmitTimeout().
    HRESULT Submit(WorkFn fn) {
        if (!worker_.joinable()) {
            // Fallback to synchronous if Start() wasn't called or if Stop()
            // already ran. Lets shutdown paths still work.
            return fn ? fn() : E_FAIL;
        }
        std::unique_lock<std::mutex> lk(mtx_);
        const auto timeout = std::chrono::milliseconds(submit_timeout_ms_);
        const bool ready = cv_done_.wait_for(lk, timeout, [&]() {
            return !work_pending_.load() || stop_.load();
        });
        if (stop_.load()) return E_FAIL;
        if (!ready) {
            // Worker still busy — drop this frame.
            return last_result_.load();
        }
        pending_ = std::move(fn);
        work_pending_.store(true);
        lk.unlock();
        cv_work_.notify_one();
        return last_result_.load();
    }

    // Tune the submit timeout (ms). Default 8ms ≈ half a frame at 60Hz —
    // long enough to absorb minor scheduler/GPU hiccups, short enough to
    // keep the host's present cadence well within one frame budget.
    // Setting to 0 makes Submit fully non-blocking (try-submit semantics).
    void SetSubmitTimeout(uint32_t ms) { submit_timeout_ms_ = ms; }

    // Block until the worker has drained its current work item. Useful at
    // teardown points where the caller needs to know the last frame
    // actually shipped before tearing down its own resources.
    void Drain() {
        if (!worker_.joinable()) return;
        std::unique_lock<std::mutex> lk(mtx_);
        cv_done_.wait(lk, [&]() { return !work_pending_.load() || stop_.load(); });
    }

private:
    void Loop() {
        while (true) {
            WorkFn fn;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_work_.wait(lk, [&]() { return work_pending_.load() || stop_.load(); });
                if (stop_.load()) return;
                fn = std::move(pending_);
            }

            HRESULT hr = E_FAIL;
            try {
                if (fn) hr = fn();
            }
            catch (...) {
                hr = E_UNEXPECTED;
            }
            last_result_.store(hr);

            {
                std::lock_guard<std::mutex> lk(mtx_);
                work_pending_.store(false);
            }
            cv_done_.notify_one();
        }
    }

    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_work_;
    std::condition_variable cv_done_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> work_pending_{false};
    std::atomic<HRESULT> last_result_{S_OK};
    uint32_t submit_timeout_ms_ = 8;
    WorkFn pending_;
};

}  // namespace NV3D
