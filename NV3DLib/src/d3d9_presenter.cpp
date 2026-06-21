#include "d3d9_presenter.h"

#include <climits>
#include <excpt.h>
#include <delayimp.h>

#include "log.h"
#include "present_window.h"

#include <nvapi.h>

// VcppException macro for SEH-filtering missing DLL errors. delayimp.h
// defines this on MSVC; if not available, fall back to the constant form.
#ifndef VcppException
#define VcppException(sev, err) ((sev) | 0x40000000 | (FACILITY_VISUALCPP << 16) | (err))
#endif

namespace NV3D {

namespace {

// SEH guard around NvAPI_Initialize so a missing nvapi64.dll (or delay-load
// failure on non-NVIDIA systems) returns a clean error instead of crashing
// the host. Mirrors VRto3D's TryNvAPIInitializeSEH.
NvAPI_Status TryNvAPIInitializeSEH(bool* dll_failure) {
    *dll_failure = false;
    __try {
        return NvAPI_Initialize();
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
        *dll_failure = true;
        return NVAPI_LIBRARY_NOT_FOUND;
    }
}

UINT AdapterOrdinalFromMonitor(IDirect3D9Ex* d3d, HMONITOR monitor) {
    if (!d3d || !monitor) return D3DADAPTER_DEFAULT;
    UINT count = d3d->GetAdapterCount();
    for (UINT i = 0; i < count; ++i) {
        if (d3d->GetAdapterMonitor(i) == monitor) return i;
    }
    return D3DADAPTER_DEFAULT;
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

void PumpMessages(int cycles = 3) {
    MSG msg;
    for (int i = 0; i < cycles; ++i) {
        Sleep(20);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}  // anonymous

D3D9Presenter::~D3D9Presenter() {
    Shutdown();
}

bool D3D9Presenter::Init(PresentWindow* window, const InitParams& params) {
    window_    = window;
    params_    = params;
    monitor_w_ = window->Width();
    monitor_h_ = window->Height();
    activation_retries_left_ = (params.activation_retry_budget < 0)
                                   ? INT_MAX
                                   : params.activation_retry_budget;
    return BuildD3D9Stack();
}

void D3D9Presenter::Shutdown() {
    const bool dead = d3d9_dead_.load(std::memory_order_acquire);

    // 1. NvAPI stereo handle.
    //    Skipping DestroyHandle on a dead device is intentional — that call
    //    can block in the kernel-mode driver waiting for GPU work that will
    //    never complete. NvAPI cleans up on Unload anyway.
    if (stereo_handle_) {
        if (!dead) {
            NvAPI_Stereo_DestroyHandle(stereo_handle_);
        } else {
            NV3D_LOG_WARN(L"D3D9Presenter::Shutdown: skipping Stereo_DestroyHandle (device dead)");
        }
        stereo_handle_ = nullptr;
    }

    // 2. D3D9 objects. If the device is dead, Detach() drops our ComPtr ref
    //    without calling IUnknown::Release — the underlying COM object is
    //    leaked but the call is non-blocking. The OS cleans up the leaked
    //    refs on process exit. If we did the standard Release here, the
    //    kernel-mode driver would block trying to flush GPU work that's
    //    already wedged.
    if (dead) {
        (void)back_buffer_.Detach();
        (void)device9_.Detach();
        (void)d3d9_.Detach();
    } else {
        back_buffer_.Reset();
        device9_.Reset();
        d3d9_.Reset();
    }

    // LightBoost revert AFTER D3D9 release — the display is no longer FSE-
    // locked at this point so NVAPI's RevertCustomDisplayTrial (or the
    // ChangeDisplaySettingsExW fallback) can push the original timing.
    lightboost_.Disable();

    // NvAPI_Unload mirrors VRto3D's teardown step 5. Unconditional — runs
    // even on the dead-device path (NvAPI cleans up the leaked stereo handle
    // here). NvAPI is reference-counted process-wide, so this only fully
    // disables NVAPI when our Initialize was the last live ref.
    NvAPI_Unload();
}

bool D3D9Presenter::BuildD3D9Stack() {
    // NvAPI must be initialised + stereo mode forced to DIRECT BEFORE the
    // D3D9Ex device is created. The docs on NvAPI_Stereo_SetDriverMode are
    // explicit: "This API must be called before the device is created." If
    // we skip this the driver stays in AUTOMATIC mode and SetActiveEye is
    // silently ignored — symptom is one-eye-only output with the emitter on.
    {
        bool dll_missing = false;
        NvAPI_Status s = TryNvAPIInitializeSEH(&dll_missing);
        if (dll_missing) {
            NV3D_LOG_ERROR(L"nvapi64.dll not resolvable. Install NVIDIA drivers or "
                            L"pick a different output mode.");
            return false;
        }
        if (s != NVAPI_OK) {
            NV3D_LOG_ERROR(L"NvAPI_Initialize failed code=%d", static_cast<int>(s));
            return false;
        }
    }
    {
        NvU8 enabled = 0;
        NvAPI_Status s = NvAPI_Stereo_IsEnabled(&enabled);
        if (s != NVAPI_OK || !enabled) {
            NV3D_LOG_ERROR(L"3D Vision is not enabled in NVCP — enable 'Set up stereoscopic 3D' "
                            L"in NVIDIA Control Panel. NvAPI_Stereo_IsEnabled status=%d enabled=%d",
                            static_cast<int>(s), enabled);
            return false;
        }
    }
    {
        NvAPI_Status s = NvAPI_Stereo_SetDriverMode(NVAPI_STEREO_DRIVER_MODE_DIRECT);
        if (s != NVAPI_OK) {
            NV3D_LOG_WARN(L"NvAPI_Stereo_SetDriverMode(DIRECT) failed status=%d — "
                           L"SetActiveEye routing may not work", static_cast<int>(s));
        } else {
            NV3D_LOG_INFO(L"D3D9Presenter: stereo driver mode = DIRECT");
        }
    }

    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9_);
    if (FAILED(hr) || !d3d9_) {
        NV3D_LOG_ERROR(L"Direct3DCreate9Ex failed hr=0x%08X", hr);
        return false;
    }

    HWND hwnd = window_->Hwnd();
    UINT adapter = AdapterOrdinalFromMonitor(d3d9_.Get(), window_->Monitor());

    // LightBoost: apply BEFORE CreateDeviceEx so the FSE device sees the new
    // timing. Resolve the GDI device name from the target HMONITOR.
    if (params_.enable_lightboost) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(window_->Monitor(), &mi)) {
            const std::wstring json_path = params_.nvtimings_json_path
                ? std::wstring(params_.nvtimings_json_path) : std::wstring();
            lightboost_.Enable(mi.szDevice, json_path);
        } else {
            NV3D_LOG_WARN(L"LightBoost: GetMonitorInfoW failed — skipped");
        }
    }

    D3DDISPLAYMODE dm{};
    if (FAILED(d3d9_->GetAdapterDisplayMode(adapter, &dm))) {
        dm.Width       = monitor_w_;
        dm.Height      = monitor_h_;
        dm.Format      = D3DFMT_X8R8G8B8;
        dm.RefreshRate = (window_->Refresh() > 1.0f)
                             ? static_cast<UINT>(window_->Refresh() + 0.5f) : 0;
    }

    auto fill_pp = [&](D3DPRESENT_PARAMETERS& pp, BOOL windowed) {
        pp = D3DPRESENT_PARAMETERS{};
        pp.BackBufferWidth            = dm.Width;
        pp.BackBufferHeight           = dm.Height;
        pp.BackBufferFormat           = dm.Format;
        pp.BackBufferCount            = 2;
        pp.MultiSampleType            = D3DMULTISAMPLE_NONE;
        pp.SwapEffect                 = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow              = hwnd;
        pp.Windowed                   = windowed;
        pp.EnableAutoDepthStencil     = FALSE;
        pp.Flags                      = 0;
        pp.FullScreen_RefreshRateInHz = windowed ? 0 : dm.RefreshRate;
        pp.PresentationInterval       = D3DPRESENT_INTERVAL_ONE;
    };

    // D3DCREATE_NOWINDOWCHANGES: tells D3D9 to NOT auto-handle window-state
    // transitions itself. Without it, D3D9Ex FSE devices silently minimize the
    // device window on Alt+Tab / WM_ACTIVATEAPP(FALSE) — bypassing the WndProc
    // subclass that swallows WM_ACTIVATE / WM_NCACTIVATE / SC_MINIMIZE — and
    // the popup just disappears for the user with no way to restore it.
    DWORD create_flags = D3DCREATE_HARDWARE_VERTEXPROCESSING
                       | D3DCREATE_MULTITHREADED
                       | D3DCREATE_FPU_PRESERVE
                       | D3DCREATE_NOWINDOWCHANGES;

    {
        bool mode_found = false;
        UINT mode_count = d3d9_->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8);
        for (UINT i = 0; i < mode_count; ++i) {
            D3DDISPLAYMODE m{};
            if (SUCCEEDED(d3d9_->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &m))
                && m.Width == dm.Width && m.Height == dm.Height && m.RefreshRate == dm.RefreshRate) {
                mode_found = true;
                break;
            }
        }
        if (!mode_found) {
            NV3D_LOG_WARN(L"BuildD3D9Stack: adapter %u does not advertise mode %ux%u@%uHz",
                            adapter, dm.Width, dm.Height, dm.RefreshRate);
        }
    }

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,
                  static_cast<int>(dm.Width), static_cast<int>(dm.Height),
                  SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    ForceForeground(hwnd);
    PumpMessages(3);

    D3DPRESENT_PARAMETERS pp{};
    fill_pp(pp, FALSE);

    D3DDISPLAYMODEEX fs_mode{};
    fs_mode.Size             = sizeof(fs_mode);
    fs_mode.Width            = dm.Width;
    fs_mode.Height           = dm.Height;
    fs_mode.RefreshRate      = dm.RefreshRate;
    fs_mode.Format           = D3DFMT_X8R8G8B8;
    fs_mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;

    hr = d3d9_->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, hwnd,
                                 create_flags, &pp, &fs_mode, &device9_);
    if (FAILED(hr) || !device9_) {
        NV3D_LOG_WARN(L"CreateDeviceEx FSE failed hr=0x%08X — falling back to windowed", hr);
        device9_.Reset();
        fill_pp(pp, TRUE);
        hr = d3d9_->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, hwnd,
                                     create_flags, &pp, nullptr, &device9_);
        if (FAILED(hr) || !device9_) {
            NV3D_LOG_ERROR(L"CreateDeviceEx WINDOWED also failed hr=0x%08X", hr);
            return false;
        }
        is_fse_ = false;
        NV3D_LOG_INFO(L"D3D9Presenter: device created (windowed fallback)");
    } else {
        is_fse_ = true;
        NV3D_LOG_INFO(L"D3D9Presenter: device created (FSE) adapter=%u %ux%u@%uHz",
                       adapter, dm.Width, dm.Height, dm.RefreshRate);
        // Apply click-through styling AFTER FSE is established. VRto3D's
        // NvStereoDx9Presenter does this in the same order — applying
        // WS_EX_TRANSPARENT before CreateDeviceEx makes ForceForeground a
        // no-op and FSE never engages cleanly, so the first focus shake
        // pushes the device into a black/occluded state. Doing it after
        // means FSE is fully engaged when click-through goes on.
        window_->ApplyClickThrough();
    }

    NvAPI_Stereo_Enable();

    NvAPI_Status s = NvAPI_Stereo_CreateHandleFromIUnknown(device9_.Get(), &stereo_handle_);
    if (s != NVAPI_OK || !stereo_handle_) {
        NV3D_LOG_ERROR(L"NvAPI_Stereo_CreateHandleFromIUnknown failed code=%d", static_cast<int>(s));
        return false;
    }
    NvAPI_Stereo_SetSurfaceCreationMode(stereo_handle_,
                                          NVAPI_STEREO_SURFACECREATEMODE_FORCESTEREO);

    s = NvAPI_Stereo_Activate(stereo_handle_);
    {
        NvU8 active = 0;
        NvAPI_Stereo_IsActivated(stereo_handle_, &active);
        stereo_activated_ = (active != 0);
        float sep = 0.0f, conv = 0.0f;
        NvAPI_Stereo_GetSeparation(stereo_handle_, &sep);
        NvAPI_Stereo_GetConvergence(stereo_handle_, &conv);
        NV3D_LOG_INFO(L"NvAPI_Stereo_Activate code=%d IsActivated=%s sep=%.2f conv=%.2f",
                       static_cast<int>(s),
                       stereo_activated_ ? L"yes" : L"no (retry scheduled)",
                       sep, conv);
    }

    hr = device9_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer_);
    if (FAILED(hr) || !back_buffer_) {
        NV3D_LOG_ERROR(L"GetBackBuffer failed hr=0x%08X", hr);
        return false;
    }
    return true;
}

HRESULT D3D9Presenter::Present(IDirect3DSurface9* shared_input,
                                 uint32_t input_w, uint32_t input_h) {
    if (d3d9_dead_.load()) return E_FAIL;
    if (!device9_ || !shared_input) return E_NOT_VALID_STATE;

    if (++frames_since_state_check_ >= 60) {
        frames_since_state_check_ = 0;
        HRESULT cs = device9_->CheckDeviceState(window_->Hwnd());
        if (cs != S_OK && cs != S_PRESENT_OCCLUDED) {
            CheckAndMarkD3D9Dead(cs, "CheckDeviceState");
            if (d3d9_dead_.load()) return cs;
        }
    }

    // DIRECT mode per-eye routing.
    const LONG eye_w = static_cast<LONG>(input_w / 2u);
    const LONG eye_h = static_cast<LONG>(input_h);
    RECT left_src  { 0,     0, eye_w,     eye_h };
    RECT right_src { eye_w, 0, eye_w * 2, eye_h };
    const RECT& src_left  = params_.eye_swap ? right_src : left_src;
    const RECT& src_right = params_.eye_swap ? left_src  : right_src;

    auto blit = [&](NV_STEREO_ACTIVE_EYE eye, const RECT& src, const char* eye_name) {
        NvAPI_Stereo_SetActiveEye(stereo_handle_, eye);
        HRESULT bhr = device9_->StretchRect(shared_input, &src,
                                              back_buffer_.Get(), nullptr,
                                              D3DTEXF_POINT);
        if (FAILED(bhr)) {
            NV3D_LOG_ERROR(L"StretchRect(shared->backbuf %hs) failed hr=0x%08X", eye_name, bhr);
            CheckAndMarkD3D9Dead(bhr, "StretchRect(shared->backbuf)");
        }
        return bhr;
    };

    HRESULT hr = blit(NVAPI_STEREO_EYE_LEFT,  src_left,  "LEFT");
    if (FAILED(hr)) return hr;
    hr = blit(NVAPI_STEREO_EYE_RIGHT, src_right, "RIGHT");
    if (FAILED(hr)) return hr;

    hr = device9_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"PresentEx failed hr=0x%08X", hr);
        CheckAndMarkD3D9Dead(hr, "PresentEx");
        return hr;
    }

    StereoActivationRetry();
    return S_OK;
}

void D3D9Presenter::StereoActivationRetry() {
    if (stereo_activated_ || !device9_ || !stereo_handle_) return;
    if (activation_retries_left_ <= 0) return;

    DWORD now = GetTickCount();
    if (now - last_stereo_activate_tick_ < 50) return;
    last_stereo_activate_tick_ = now;

    NvAPI_Stereo_Activate(stereo_handle_);
    NvU8 active = 0;
    NvAPI_Stereo_IsActivated(stereo_handle_, &active);
    stereo_activated_ = (active != 0);

    if (activation_retries_left_ != INT_MAX) --activation_retries_left_;

    if (stereo_activated_ && !activation_summary_logged_) {
        NV3D_LOG_INFO(L"D3D9Presenter: stereo ACTIVE");
        activation_summary_logged_ = true;
    } else if (!stereo_activated_ && activation_retries_left_ == 0 &&
                !activation_summary_logged_) {
        NV3D_LOG_WARN(L"D3D9Presenter: IsActivated stayed false after retries — "
                       L"SetActiveEye writes should still route per-eye if the IR "
                       L"emitter is paired. If you see mono output, verify the 3D "
                       L"Vision driver + NVCP enable + 120Hz target panel.");
        activation_summary_logged_ = true;
    }
}

bool D3D9Presenter::CheckAndMarkD3D9Dead(HRESULT hr, const char* origin) {
    if (SUCCEEDED(hr)) return false;
    if (d3d9_dead_.load()) return true;
    const bool dead =
        (hr == D3DERR_DEVICELOST) ||
        (hr == D3DERR_DEVICEHUNG) ||
        (hr == D3DERR_DEVICEREMOVED) ||
        (hr == D3DERR_OUTOFVIDEOMEMORY);
    if (dead) {
        d3d9_dead_.store(true);
        NV3D_LOG_ERROR(L"D3D9Presenter: device dead at %hs hr=0x%08X", origin, hr);
    }
    return d3d9_dead_.load();
}

}  // namespace NV3D
