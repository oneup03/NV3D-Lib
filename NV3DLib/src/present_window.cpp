#include "present_window.h"

#include "log.h"

namespace NV3D {

namespace {

// Resolve monitor dimensions + best refresh rate via EnumDisplaySettings.
bool QueryMonitorInfo(HMONITOR* mon_inout, uint32_t* w, uint32_t* h, float* refresh,
                       std::wstring* gdi_device) {
    if (!*mon_inout) {
        *mon_inout = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(*mon_inout, &mi)) return false;

    *w = mi.rcMonitor.right  - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (gdi_device) *gdi_device = mi.szDevice;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        *refresh = static_cast<float>(dm.dmDisplayFrequency);
    } else {
        *refresh = 60.0f;
    }
    return true;
}

}  // anonymous

PresentWindow::~PresentWindow() {
    Shutdown();
}

bool PresentWindow::Init(const PresentWindowConfig& cfg) {
    cfg_ = cfg;
    host_owned_ = (cfg.host_hwnd == nullptr);

    if (!host_owned_) {
        // Host-provided HWND mode: just subclass + capture monitor info.
        hwnd_ = cfg.host_hwnd;
        monitor_ = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (!QueryMonitorInfo(&monitor_, &width_, &height_, &refresh_hz_, nullptr)) {
            NV3D_LOG_ERROR(L"PresentWindow::Init: QueryMonitorInfo failed for host_hwnd");
            return false;
        }
        InstallSubclass(hwnd_);
        return true;
    }

    // Library-owned mode: spawn a window thread.
    monitor_ = cfg.target_monitor;
    if (!QueryMonitorInfo(&monitor_, &width_, &height_, &refresh_hz_, nullptr)) {
        NV3D_LOG_ERROR(L"PresentWindow::Init: QueryMonitorInfo failed");
        return false;
    }

    window_thread_ = std::thread([this]() { WindowThreadLoop(); });
    while (!window_ready_.load() && !window_failed_.load()) {
        Sleep(5);
    }
    return !window_failed_.load();
}

void PresentWindow::Shutdown() {
    if (host_owned_ && hwnd_) {
        RemoveSubclass();
        hwnd_ = nullptr;
        return;
    }

    // Hide first so the FSE window disappears immediately on shutdown,
    // even if the rest of teardown takes a moment (D3D9 release etc.).
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);

    window_stop_.store(true);
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    if (window_thread_.joinable()) window_thread_.join();
    hwnd_ = nullptr;
}

void PresentWindow::SetSuppressMinimize(bool suppress) {
    suppress_minimize_.store(suppress);
}

void PresentWindow::ApplyClickThrough() {
    if (!hwnd_ || host_owned_) return;
    // WS_EX_LAYERED is required for WS_EX_TRANSPARENT to take effect on a
    // top-level window. Alpha=255 means fully opaque — the only behavioural
    // effect is that the window now participates in layered-input routing
    // and ignores mouse messages, which is what we want for click-through.
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
}

void PresentWindow::WindowThreadLoop() {
    // Owns the FSE present window's lifetime + message pump. Window is a
    // borderless top-most popup sized to the target monitor; the WndProc
    // subclass (SubclassProc) suppresses WM_ACTIVATE / WM_ACTIVATEAPP /
    // WM_KILLFOCUS / SC_MINIMIZE / SC_SCREENSAVE / SC_MONITORPOWER so the
    // FSE state survives the user interacting with other displays.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NV3DLib_PresentWindow";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor_, &mi);

    DWORD style    = WS_POPUP | WS_VISIBLE;
    DWORD style_ex = cfg_.on_top ? WS_EX_TOPMOST : 0;

    hwnd_ = CreateWindowExW(
        style_ex, wc.lpszClassName, cfg_.title.c_str(), style,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd_) {
        NV3D_LOG_ERROR(L"PresentWindow: CreateWindowExW failed err=%lu", GetLastError());
        window_failed_.store(true);
        return;
    }

    InstallSubclass(hwnd_);

    // NOTE: WS_EX_LAYERED + WS_EX_TRANSPARENT are deliberately NOT applied
    // here. VRto3D's NvStereoDx9Presenter (see
    // vrto3d/src/presenter/nvstereo_dx9_presenter.cpp:InstallFseSubclass)
    // applies them ONLY after CreateDeviceEx FSE succeeds. The reason:
    // WS_EX_TRANSPARENT marks the window ineligible for activation, which
    // makes ForceForeground a no-op when it runs inside BuildD3D9Stack
    // right before CreateDeviceEx. Without the foreground transition,
    // D3D9Ex's FSE engagement is fragile — it appears to work, but the
    // first focus shake of any kind (a click on another window, an Alt+Tab,
    // the driver's periodic stereo revalidation) is enough to push it
    // into a black-screen / occluded state we can't recover from.
    //
    // The click-through styling is applied later, by ApplyClickThrough(),
    // which BuildD3D9Stack calls after CreateDeviceEx FSE returns success.

    window_ready_.store(true);

    // Foreground-following with edge-triggered minimize/restore — ported
    // from VRto3D's FocusThreadLoop (vrto3d/src/presenter/nvstereo_dx9_presenter.cpp
    // ~line 1241). The earlier design here re-fired ShowWindow on every
    // poll iteration based on the current `iconic` state, which races with
    // D3D9 internal state shuffling during the NVIDIA driver's periodic
    // ~20-30s stereo revalidation and AVs.
    //
    // VRto3D's pattern tracks `was_host_focused` explicitly so ShowWindow
    // fires only on real transitions, not on each tick. We skip VRto3D's
    // SetWindowPos(HWND_TOPMOST) reassertion and ForceForeground watcher
    // because WS_EX_TRANSPARENT means our popup never gets activation and
    // forcing foreground would steal input from the host's game window.
    //
    // ShowWindow + message dispatch are SEH-wrapped: D3D9Ex's internal
    // hook procedures can fault inside DispatchMessage's WndProc path or
    // inside ShowWindow's WM_SIZE/WM_WINDOWPOSCHANGED chain, especially
    // post-freeze when the driver state is mid-revalidation.
    const DWORD self_pid = GetCurrentProcessId();
    DWORD last_fg_check_ms = 0;
    bool was_host_focused = true;  // popup starts visible w/ host as foreground

    auto seh_show_window = [](HWND h, int mode) {
        __try {
            ShowWindow(h, mode);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            const DWORD code = GetExceptionCode();
            NV3D_LOG_ERROR(L"PresentWindow: SEH caught in ShowWindow(mode=%d) code=0x%08lX — "
                            L"D3D9 internal hook AV; surviving, next transition will retry",
                            mode, static_cast<unsigned long>(code));
        }
    };
    auto seh_dispatch = [](const MSG& m) {
        __try {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            const DWORD code = GetExceptionCode();
            NV3D_LOG_ERROR(L"PresentWindow: SEH caught in DispatchMessage(msg=0x%04X) code=0x%08lX — "
                            L"D3D9/NvAPI hook AV; surviving",
                            static_cast<unsigned>(m.message), static_cast<unsigned long>(code));
        }
    };

    MSG msg;
    while (!window_stop_.load()) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            seh_dispatch(msg);
        } else {
            const DWORD now_ms = GetTickCount();
            if (now_ms - last_fg_check_ms >= 100 && !window_stop_.load()) {
                last_fg_check_ms = now_ms;
                HWND fg = GetForegroundWindow();
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                const bool host_focused = (fg_pid == self_pid);

                if (host_focused && !was_host_focused) {
                    // Transition: host regained foreground → restore FSE
                    // popup so D3D9 re-engages exclusive scan-out. Re-arm
                    // suppress_minimize_ FIRST so any focus-loss messages
                    // that fire during the restore sequence don't push us
                    // back to minimized.
                    suppress_minimize_.store(true);
                    seh_show_window(hwnd_, SW_RESTORE);
                } else if (!host_focused && was_host_focused) {
                    // Transition: host lost foreground (alt+tab to another
                    // app) → first DISARM suppress_minimize_ so the
                    // WM_NCACTIVATE FALSE / SC_MINIMIZE handlers stop
                    // returning TRUE/0, then minimize. Without disarming
                    // first the popup's WndProc subclass refuses to let
                    // the window deactivate, ShowWindow(SW_MINIMIZE) is
                    // effectively a no-op, and the alt+tabbed-to app stays
                    // hidden behind the FSE popup.
                    suppress_minimize_.store(false);
                    seh_show_window(hwnd_, SW_MINIMIZE);
                }
                was_host_focused = host_focused;
            }
            Sleep(1);
        }
    }

    RemoveSubclass();
    DestroyWindow(hwnd_);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void PresentWindow::InstallSubclass(HWND hwnd) {
    if (orig_wndproc_) return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    orig_wndproc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PresentWindow::SubclassProc)));
}

void PresentWindow::RemoveSubclass() {
    if (!orig_wndproc_ || !hwnd_) return;
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig_wndproc_));
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    orig_wndproc_ = nullptr;
}

LRESULT CALLBACK PresentWindow::SubclassProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PresentWindow*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    // NOTE: WM_DISPLAYCHANGE is deliberately NOT treated as fatal here. FSE
    // D3D9Ex itself raises that message when it modesets the display for
    // fullscreen-exclusive entry — treating it as fatal kills the device on
    // the first frame. The periodic CheckDeviceState in the present path
    // catches genuinely unrecoverable states.
    if (self && self->suppress_minimize_.load()) {
        switch (msg) {
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) return 0;
                break;
            case WM_ACTIVATEAPP:
                if (wp == FALSE) return 0;
                break;
            case WM_KILLFOCUS:
                return 0;
            case WM_NCACTIVATE:
                if (wp == FALSE) return TRUE;
                break;
            // SC_MINIMIZE / SC_SCREENSAVE / SC_MONITORPOWER all power-down or
            // minimize the FSE window; suppress while suppress_minimize_ is set.
            case WM_SYSCOMMAND: {
                const WPARAM cmd = wp & 0xFFF0;
                if (cmd == SC_MINIMIZE ||
                    cmd == SC_SCREENSAVE ||
                    cmd == SC_MONITORPOWER) return 0;
                break;
            }
        }
    }
    WNDPROC orig = self ? self->orig_wndproc_ : nullptr;
    return orig ? CallWindowProcW(orig, hw, msg, wp, lp)
                : DefWindowProcW(hw, msg, wp, lp);
}

}  // namespace NV3D
