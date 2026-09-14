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
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <windows.h>

#include "log.h"

namespace NV3D {

namespace detail {

// MSVC-only structured-exception guard around the work function. The
// reason this exists: when the FSE D3D9Ex popup loses foreground (e.g.
// the user clicks the host's window), D3D9 / NvAPI / the user-mode
// driver can fault inside Present-time calls — StretchRect on a back
// buffer whose state the driver reshuffled, NvAPI_Stereo_SetActiveEye on
// a stereo handle that no longer maps to an active device, PresentEx on
// an occluded swap chain that the driver has freed underneath us. These
// surface as SEH access violations, not C++ exceptions, so a plain
// catch(...) does not stop them on the MSVC /EHsc default.
//
// __except (EXCEPTION_EXECUTE_HANDLER) swallows the AV. The work
// function's captured ComPtrs leak their refs during SEH unwind because
// /EHsc does NOT run C++ destructors on structured-exception unwind —
// that's a slow per-incident leak, not a crash, and it stops the host
// process going down with the driver fault.
//
// After the swallow, the caller (via SetOnSeh) marks its D3D9 device
// dead. Retrying through the same driver context after an AV inside
// nvd3dumx.dll is exactly the "swallow → next frame → GPU wedge → TDR"
// sequence we're trying to avoid — better to force the host to rebuild
// the device via its existing recovery path than to keep feeding a
// poisoned context.
inline HRESULT InvokeWithSEH(std::function<HRESULT()>& fn, DWORD* out_code = nullptr) {
    HRESULT hr = E_FAIL;
    __try {
        if (fn) hr = fn();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (out_code) *out_code = GetExceptionCode();
        hr = E_FAIL;
    }
    return hr;
}

}  // namespace detail

class AsyncPresenter {
public:
    using WorkFn  = std::function<HRESULT()>;
    using OnSehFn = std::function<void(DWORD /*code*/)>;

    AsyncPresenter() = default;
    ~AsyncPresenter() { Stop(); }

    AsyncPresenter(const AsyncPresenter&) = delete;
    AsyncPresenter& operator=(const AsyncPresenter&) = delete;

    // Install a callback fired from the worker thread whenever the SEH
    // guard catches a fault inside a work item. The backend uses this to
    // mark its D3D9 device dead so the next Submit fast-fails and the
    // host recovery path (RecreateDevice) runs, instead of re-entering
    // the poisoned driver context. Call before Start().
    void SetOnSeh(OnSehFn fn) { on_seh_ = std::move(fn); }

    // Spawn the worker thread. No-op if already started.
    void Start() {
        if (worker_.joinable()) return;
        stop_.store(false);
        work_pending_.store(false);
        last_result_.store(S_OK);
        // Idle event — manual-reset, created SIGNALLED so the host's very
        // first frame of a session can submit immediately. Reset the moment
        // a work item is accepted, set again when the worker finishes it.
        // A host that paces off this handle submits exactly one frame per
        // completed present: no over-submission, no drops, and no blocking
        // wait inside Submit(). See PresentDoneEvent().
        if (!present_done_) {
            present_done_ = CreateEventW(nullptr, /*manualReset=*/TRUE,
                                         /*initial=*/TRUE, nullptr);
            if (!present_done_) {
                NV3D_LOG_WARN(L"AsyncPresenter: CreateEventW(present_done) failed err=%lu — "
                              L"host falls back to timer pacing",
                              GetLastError());
            }
        }
        worker_ = std::thread([this]() { Loop(); });
    }

    // Manual-reset event that is SET whenever the worker is idle and RESET
    // while a work item is in flight. Null if creation failed or Start()
    // hasn't run. The host may wait on it (e.g. in
    // MsgWaitForMultipleObjectsEx) to clock its capture/submit loop off the
    // display instead of a wall-clock timer. Do not close it; the handle is
    // owned by this object and released in Stop().
    HANDLE PresentDoneEvent() const { return present_done_; }

    // True while a work item is in flight. Cheap, lock-free companion to
    // PresentDoneEvent() for callers that want to poll rather than wait.
    bool IsBusy() const { return work_pending_.load(); }

    // Drain the in-flight work (if any), signal the worker to exit, join.
    // Safe to call multiple times.
    void Stop() {
        if (worker_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                stop_.store(true);
            }
            cv_work_.notify_all();
            worker_.join();
        }
        // Release the idle event unconditionally (not under the joinable
        // check) so a double Stop — an explicit call followed by the
        // destructor's — can't leak the handle. Set it first: a host
        // blocked on it must wake and observe the teardown rather than
        // wait out its timeout. The host waits on this handle from the
        // same thread that drives teardown, so it cannot be mid-wait here.
        if (present_done_) {
            SetEvent(present_done_);
            CloseHandle(present_done_);
            present_done_ = nullptr;
        }
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
            stat_dropped_.fetch_add(1, std::memory_order_relaxed);
            return last_result_.load();
        }
        pending_ = std::move(fn);
        work_pending_.store(true);
        // Busy from here until Loop() finishes the item. Reset under the
        // lock so the flag and the event can never disagree.
        if (present_done_) ResetEvent(present_done_);
        stat_accepted_.fetch_add(1, std::memory_order_relaxed);
        lk.unlock();
        cv_work_.notify_one();
        return last_result_.load();
    }

    // Snapshot the present-path counters and reset them. Host-side pacing
    // diagnostics only — call at most ~1 Hz. present_ms_* cover the whole
    // work item (fence wait + D3D9 StretchRects + the vsync-blocked
    // PresentEx), which is the number that tells you what the display
    // cadence actually is.
    void TakeStats(uint32_t* accepted, uint32_t* dropped, uint32_t* done,
                   float* ms_avg, float* ms_max) {
        const uint32_t a = stat_accepted_.exchange(0, std::memory_order_relaxed);
        const uint32_t d = stat_dropped_.exchange(0, std::memory_order_relaxed);
        const uint32_t n = stat_done_.exchange(0, std::memory_order_relaxed);
        const uint64_t us_sum = stat_us_sum_.exchange(0, std::memory_order_relaxed);
        const uint64_t us_max = stat_us_max_.exchange(0, std::memory_order_relaxed);
        if (accepted) *accepted = a;
        if (dropped)  *dropped  = d;
        if (done)     *done     = n;
        if (ms_avg)   *ms_avg   = n ? static_cast<float>(us_sum) / n / 1000.0f : 0.0f;
        if (ms_max)   *ms_max   = static_cast<float>(us_max) / 1000.0f;
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

    // Bounded variant for transition points (FSE hide). Returns true when
    // the worker is idle, false on timeout. A worker wedged inside a driver
    // call must not hang the host's UI thread — the caller decides whether
    // to proceed with its transition anyway.
    bool Drain(uint32_t timeout_ms) {
        if (!worker_.joinable()) return true;
        std::unique_lock<std::mutex> lk(mtx_);
        return cv_done_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]() {
            return !work_pending_.load() || stop_.load();
        });
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

            // SEH-guarded: a driver-side access violation (typically
            // triggered by FSE focus loss when the host's window
            // takes focus from our popup) gets turned into E_FAIL
            // instead of terminating the host process. See the comment
            // on detail::InvokeWithSEH for the gory details.
            DWORD seh_code = 0;
            LARGE_INTEGER t0{};
            QueryPerformanceCounter(&t0);
            HRESULT hr = detail::InvokeWithSEH(fn, &seh_code);
            RecordPresentDuration(t0);
            if (seh_code != 0) {
                NV3D_LOG_ERROR(L"AsyncPresenter: SEH caught in worker code=0x%08lX — "
                                L"D3D9/NvAPI driver fault; marking device dead so host "
                                L"recovery rebuilds it before next Submit",
                                static_cast<unsigned long>(seh_code));
                if (on_seh_) on_seh_(seh_code);
            }
            last_result_.store(hr);

            {
                std::lock_guard<std::mutex> lk(mtx_);
                work_pending_.store(false);
                // Idle again — wake a host that is pacing off this handle.
                // Inside the lock so it stays paired with work_pending_.
                if (present_done_) SetEvent(present_done_);
            }
            cv_done_.notify_one();
        }
    }

    // Accumulate one completed work item's wall time into the stat
    // counters. Worker thread only.
    void RecordPresentDuration(const LARGE_INTEGER& t0) {
        static const long long freq = []() {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f.QuadPart ? f.QuadPart : 1;
        }();
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        const long long ticks = t1.QuadPart - t0.QuadPart;
        if (ticks <= 0) return;
        const uint64_t us = static_cast<uint64_t>((ticks * 1000000LL) / freq);
        stat_done_.fetch_add(1, std::memory_order_relaxed);
        stat_us_sum_.fetch_add(us, std::memory_order_relaxed);
        uint64_t prev = stat_us_max_.load(std::memory_order_relaxed);
        while (us > prev &&
               !stat_us_max_.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
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
    OnSehFn on_seh_;

    // Set while idle, reset while a work item is in flight. See
    // PresentDoneEvent(); owned here, released in Stop().
    HANDLE present_done_ = nullptr;

    // Present-path telemetry, drained by TakeStats(). Relaxed ordering
    // throughout: these are counters for a once-per-second log line, never
    // control flow.
    std::atomic<uint32_t> stat_accepted_{0};
    std::atomic<uint32_t> stat_dropped_{0};
    std::atomic<uint32_t> stat_done_{0};
    std::atomic<uint64_t> stat_us_sum_{0};
    std::atomic<uint64_t> stat_us_max_{0};
};

}  // namespace NV3D
