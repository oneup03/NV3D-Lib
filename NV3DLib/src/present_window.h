/*
 * FSE present window + WndProc subclass + focus thread.
 *
 * Two modes:
 *  1. Library-owned: CreateWindowEx on the target monitor, full focus thread.
 *  2. Host-provided (host_hwnd != nullptr): subclass the host's HWND, skip
 *     creating our own window and skip the focus thread.
 */
#pragma once

#include <Windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace NV3D {

struct PresentWindowConfig {
    HMONITOR target_monitor = nullptr;  // null = primary 3D-Vision-capable display
    HWND     host_hwnd      = nullptr;  // null = library-owned window
    bool     on_top         = true;
    std::wstring title      = L"NV3DLib";

    // When non-zero, the window thread runs the VRto3D-style focus loop
    // (BringToTop + ForceFocus to this PID's main window) instead of the
    // default minimize-on-focus-loss behavior. See InitParams::tracked_game_pid
    // in NV3D.hpp for the rationale.
    DWORD    tracked_game_pid = 0;
};

class PresentWindow {
public:
    PresentWindow() = default;
    ~PresentWindow();

    // Library-owned mode: spawns a thread that owns the FSE window and pumps
    // its message loop. Returns once the window is created and the HWND is
    // available, or false on failure.
    // Host-provided mode: installs the WndProc subclass on host_hwnd and
    // returns immediately. No thread spawned.
    bool Init(const PresentWindowConfig& cfg);

    void Shutdown();

    HWND Hwnd() const { return hwnd_; }
    HMONITOR Monitor() const { return monitor_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    float Refresh() const { return refresh_hz_; }
    bool IsHostOwned() const { return host_owned_; }

    // Toggles whether the FSE WndProc subclass swallows deactivation messages.
    // When false, the host can minimize / lose focus normally.
    void SetSuppressMinimize(bool suppress);

    // Mirrors VRto3D's is_on_top_ / man_on_top_ toggle. The window thread
    // polls this atomic — flipping it to false triggers a coordinated
    // SW_MINIMIZE (suppress_minimize_ temporarily released, then re-armed for
    // future runs), flipping it back to true triggers SW_RESTORE + topmost
    // reassertion. Doing the ShowWindow on the window's own thread (rather
    // than cross-thread from a hotkey handler) avoids the FSE D3D9Ex device
    // + DWM state getting wedged.
    void SetWantVisible(bool visible);

    // Make the popup transparent to mouse input via WM_NCHITTEST → HTTRANSPARENT
    // (the subclass returns HTTRANSPARENT once this flag is on). Intentionally
    // does NOT add WS_EX_LAYERED / WS_EX_TRANSPARENT — those force DWM-side
    // compositing, which means the D3D9Ex FSE scan-out is bypassed in favor of
    // the (never-written) layered surface, leaving the popup visually empty.
    // MUST still be called after CreateDeviceEx FSE succeeds so the foreground
    // / focus sequence completes first. Safe in both library- and host-owned
    // mode.
    void ApplyClickThrough();

private:
    static LRESULT CALLBACK SubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp);

    void WindowThreadLoop();
    void InstallSubclass(HWND hwnd);
    void RemoveSubclass();

    PresentWindowConfig cfg_{};
    HWND     hwnd_       = nullptr;
    HMONITOR monitor_    = nullptr;
    uint32_t width_      = 0;
    uint32_t height_     = 0;
    float    refresh_hz_ = 0.0f;
    bool     host_owned_ = false;

    WNDPROC          orig_wndproc_ = nullptr;
    std::atomic<bool> suppress_minimize_{true};
    std::atomic<bool> click_through_{false};
    std::atomic<bool> want_visible_{true};

    std::thread       window_thread_;
    std::atomic<bool> window_stop_{false};
    std::atomic<bool> window_ready_{false};
    std::atomic<bool> window_failed_{false};
};

}  // namespace NV3D
