#include "present_window.h"

#include "log.h"

#include <chrono>

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

// ---- VRto3D port: focus helpers --------------------------------------------
// Ported verbatim from
// VRto3D/external/VRto3DLib/include/vrto3dlib/win32_helper.hpp so NV3D-Glass-
// style game-capture deployments (game running in another process, FSE popup
// on top of it) can use the same focus/minimize behavior the SteamVR-side
// NvStereoDx9Presenter relies on. Activated when InitParams::tracked_game_pid
// is non-zero (see WindowThreadLoop below).

bool IsProcessRunning(DWORD pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD exit_code = 0;
    bool running = false;
    if (GetExitCodeProcess(h, &exit_code)) {
        running = (exit_code == STILL_ACTIVE);
    }
    CloseHandle(h);
    return running;
}

HWND GetHWNDFromPID(DWORD target_pid) {
    struct Ctx { DWORD pid; HWND result; } ctx { target_pid, nullptr };
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        if (pid == c->pid && IsWindowVisible(hw)) {
            c->result = hw;
            return FALSE;   // stop enumeration
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

void ForceForeground(HWND hwnd) {
    HWND  fg     = GetForegroundWindow();
    DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD my_tid = GetCurrentThreadId();
    if (fg_tid && fg_tid != my_tid) AttachThreadInput(my_tid, fg_tid, TRUE);
    AllowSetForegroundWindow(GetCurrentProcessId());
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (fg_tid && fg_tid != my_tid) AttachThreadInput(my_tid, fg_tid, FALSE);
}

void ForceFocus(HWND target, DWORD my_tid, DWORD target_tid) {
    // Dummy Alt up/down — wakes Win32's focus-lock logic so SetForegroundWindow
    // is actually allowed. Mirrors VRto3D's ForceFocus exactly.
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_MENU;
    SendInput(1, &input, sizeof(INPUT));
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    AttachThreadInput(my_tid, target_tid, TRUE);
    ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    SetFocus(target);
    SetActiveWindow(target);
    BringWindowToTop(target);
    AttachThreadInput(my_tid, target_tid, FALSE);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    // host_owned_ is the (badly named) "library-owned" flag — true when
    // cfg.host_hwnd was null at Init and the library spawned its own window
    // thread. The host-provided path skips the thread + window destruction
    // because the host owns the HWND lifecycle.
    //
    // Earlier this check was inverted, which made the LIBRARY-owned path
    // return without setting window_stop_ / joining the thread. The
    // window_thread_ stayed joinable, so std::thread::~thread called
    // std::terminate, killing the host process on every Stop.
    if (!host_owned_ && hwnd_) {
        RemoveSubclass();
        hwnd_ = nullptr;
        return;
    }

    // Library-owned: full teardown. Hide first so the FSE window disappears
    // immediately on shutdown, even if the rest of teardown takes a moment
    // (D3D9 release etc.).
    NV3D_LOG_INFO(L"PresentWindow::Shutdown  library-owned teardown begin hwnd=%p", (void*)hwnd_);
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    NV3D_LOG_INFO(L"PresentWindow::Shutdown  after cross-thread SW_HIDE");

    window_stop_.store(true);
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    if (window_thread_.joinable()) {
        NV3D_LOG_INFO(L"PresentWindow::Shutdown  joining window thread");
        window_thread_.join();
        NV3D_LOG_INFO(L"PresentWindow::Shutdown  window thread joined");
    }
    hwnd_ = nullptr;
}

void PresentWindow::SetSuppressMinimize(bool suppress) {
    suppress_minimize_.store(suppress);
}

void PresentWindow::SetWantVisible(bool visible) {
    want_visible_.store(visible, std::memory_order_relaxed);
}

void PresentWindow::RemoveClickThrough() {
    if (!hwnd_) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if ((ex & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == 0) {
        click_through_.store(false, std::memory_order_relaxed);
        return;
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));
    click_through_.store(false, std::memory_order_relaxed);
    NV3D_LOG_INFO(L"PresentWindow: RemoveClickThrough exstyle 0x%08lX -> 0x%08lX",
                   static_cast<unsigned long>(ex),
                   static_cast<unsigned long>(ex & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT)));

    // Pump pending messages so DWM processes the WS_EX_LAYERED removal
    // before the D3D9Ex device releases. Without this settle, the DWM
    // compositing state for the (formerly) layered FSE window races with
    // D3D9 device release and freezes the OS-wide input loop. VRto3D's
    // NvStereoDx9Presenter::RemoveFseSubclass uses the same 5×500 ms pattern.
    MSG msg;
    for (int i = 0; i < 5; ++i) {
        while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(500);
    }
    NV3D_LOG_INFO(L"PresentWindow: DWM settle complete after click-through removal");
}

void PresentWindow::ApplyClickThrough() {
    if (!hwnd_) return;
    // WS_EX_LAYERED + WS_EX_TRANSPARENT — VRto3D's exact pattern from
    // NvStereoDx9Presenter::InstallFseSubclass. This is what actually makes
    // clicks pass through to other-process windows underneath; the earlier
    // WM_NCHITTEST→HTTRANSPARENT path only routes within our own thread, so
    // clicks on the FSE area silently went nowhere for the user's game.
    //
    // SetLayeredWindowAttributes(alpha=255) keeps the window fully opaque;
    // the only behavioural change is that the window is now ignored by
    // mouse hit-testing. The D3D9Ex FSE scan-out should keep driving the
    // display content as long as the fixed-staging + D3DCREATE_NOWINDOWCHANGES
    // combo holds.
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    // Also flip the WM_NCHITTEST→HTTRANSPARENT path for any in-process windows
    // we might be drawing on top of (the host's own control panel, etc).
    click_through_.store(true, std::memory_order_relaxed);
    NV3D_LOG_INFO(L"PresentWindow: ApplyClickThrough exstyle 0x%08lX -> 0x%08lX "
                   L"(host_owned=%s)",
                   static_cast<unsigned long>(ex),
                   static_cast<unsigned long>(ex | WS_EX_LAYERED | WS_EX_TRANSPARENT),
                   host_owned_ ? L"library-owned" : L"host-provided");
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
    // WS_EX_TOOLWINDOW hides the popup from both alt+tab and the taskbar.
    // We've had earlier iterations with WS_EX_APPWINDOW (to make it
    // alt+tab-accessible) but that made it easy for users to alt+tab onto
    // the FSE D3D9Ex device window directly, which tends to wedge the
    // device + DWM — the popup is meant to be driven only by the host's
    // hide/show toggle, never by the user picking it from the task switcher.
    DWORD style_ex = WS_EX_TOOLWINDOW | (cfg_.on_top ? WS_EX_TOPMOST : 0);

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
    const DWORD self_pid    = GetCurrentProcessId();
    const DWORD tracked_pid = cfg_.tracked_game_pid;
    DWORD last_fg_check_ms  = 0;
    bool was_host_focused   = true;  // popup starts visible w/ host as foreground
    bool was_on_top         = false;
    int  reassert_counter   = 0;

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

    if (tracked_pid) {
        NV3D_LOG_INFO(L"PresentWindow: VRto3D focus loop active, tracked_game_pid=%lu",
                       static_cast<unsigned long>(tracked_pid));
    }

    MSG msg;
    while (!window_stop_.load()) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            seh_dispatch(msg);
            continue;
        }

        const DWORD now_ms = GetTickCount();

        if (tracked_pid != 0) {
            // Tracked-game-pid mode (VRto3D pattern). The popup stays put
            // while the game is alive + want_visible_ is true; the window
            // thread does all the SW_MINIMIZE/SW_RESTORE/topmost work itself
            // so suppress_minimize_ can be sequenced safely.
            //
            // want_visible_ is flipped by the host via SetWantVisible() from
            // a hotkey handler. Doing the ShowWindow on the host's main
            // thread (cross-thread) while this thread's WndProc is absorbing
            // WM_SIZE/WM_SYSCOMMAND wedges D3D9Ex FSE + DWM and leaves the
            // host's other windows non-responsive. Always do it from here.
            const bool app_running  = IsProcessRunning(tracked_pid);
            const bool user_visible = want_visible_.load(std::memory_order_relaxed);
            const bool want_up      = app_running && user_visible;

            if (want_up && !was_on_top) {
                // OFF→ON: re-arm suppression first, then restore, then put
                // the popup back on its TOPMOST stack slot. We never call
                // ForceForeground here — that wedges input via
                // AttachThreadInput on the host process.
                suppress_minimize_.store(true, std::memory_order_relaxed);
                seh_show_window(hwnd_, SW_RESTORE);
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                was_on_top = true;
                NV3D_LOG_INFO(L"PresentWindow: popup restored");
            } else if (!want_up && was_on_top) {
                // ON→OFF: disarm suppression FIRST so the WndProc lets
                // WM_SYSCOMMAND/SC_MINIMIZE and WM_SIZE/SIZE_MINIMIZED through
                // — otherwise SW_MINIMIZE is a no-op.
                suppress_minimize_.store(false, std::memory_order_relaxed);
                seh_show_window(hwnd_, SW_MINIMIZE);
                was_on_top = false;
                if (!app_running) {
                    NV3D_LOG_INFO(L"PresentWindow: tracked game pid=%lu exited — minimizing popup",
                                   static_cast<unsigned long>(tracked_pid));
                } else {
                    NV3D_LOG_INFO(L"PresentWindow: user hid popup");
                }
            } else if (want_up) {
                // Steady-state visible. Keep suppress_minimize_ pinned so
                // stray deactivation messages don't push the FSE off-screen.
                suppress_minimize_.store(true, std::memory_order_relaxed);
            }

            // 50 ms cadence so hotkey toggles feel responsive.
            Sleep(50);
        } else {
            // Legacy behavior: minimize-on-host-focus-loss. Suitable for VR
            // / single-process consumers where the host process should keep
            // foreground while presenting.
            if (now_ms - last_fg_check_ms >= 100 && !window_stop_.load()) {
                last_fg_check_ms = now_ms;
                HWND fg = GetForegroundWindow();
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                const bool host_focused = (fg_pid == self_pid);

                if (host_focused && !was_host_focused) {
                    suppress_minimize_.store(true);
                    seh_show_window(hwnd_, SW_RESTORE);
                } else if (!host_focused && was_host_focused) {
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

    // Click-through hit testing — returning HTTRANSPARENT tells Win32 to
    // dispatch the underlying mouse event to whatever window is beneath this
    // one. Works without WS_EX_LAYERED, so the FSE D3D9 scan-out keeps
    // driving the display.
    if (self && msg == WM_NCHITTEST &&
        self->click_through_.load(std::memory_order_relaxed)) {
        return HTTRANSPARENT;
    }

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
            case WM_SIZE:
                // SIZE_MINIMIZED arrives when another fullscreen window forces
                // us off-screen, or when a stray ShowWindow(SW_MINIMIZE) slips
                // past us. Refusing to handle it keeps the FSE popup full-size
                // and D3D9Ex's scan-out engaged.
                if (wp == SIZE_MINIMIZED) return 0;
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
