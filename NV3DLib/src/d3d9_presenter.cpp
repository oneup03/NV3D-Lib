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

// NV3D AUTOMATIC-mode packed-stereo signature. The NVIDIA driver scans the
// last row of the packed surface presented to PresentEx for this marker;
// when found, it routes the left half of the body to the left eye and the
// right half to the right eye through the 3D Vision IR emitter.
constexpr DWORD kNvStereoSignature = 0x4433564Eu;   // 'N','V','3','D'

#pragma pack(push, 1)
struct NvStereoImageHeader {
    DWORD signature;
    DWORD width;     // per-eye width
    DWORD height;
    DWORD bpp;       // bits per pixel
    DWORD flags;     // 0 = normal, 1 = swap eyes
};
#pragma pack(pop)

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
    eye_swap_live_.store(params.eye_swap);
    monitor_w_ = window->Width();
    monitor_h_ = window->Height();
    activation_retries_left_ = (params.activation_retry_budget < 0)
                                   ? INT_MAX
                                   : params.activation_retry_budget;
    return BuildD3D9Stack();
}

void D3D9Presenter::Shutdown() {
    const bool dead = d3d9_dead_.load(std::memory_order_acquire);

    // -1. If the popup is iconic (Ctrl+F8 minimized) at Shutdown start,
    //     RESTORE it before any teardown. Releasing a D3D9Ex FSE device
    //     whose owner window is iconic is a known driver-wedge surface:
    //     the device is in OCCLUDED state, FSE lock is gone but the mode
    //     list is still in FSE-mode territory, and Release on that
    //     ambiguous state has been observed to wedge the display.
    //     Restoring puts the device back into a clean FSE state for the
    //     rest of teardown.
    //
    //     Use SetWantVisible(true) — the window thread observes the flip
    //     and does SW_RESTORE from its own context (cross-thread
    //     SW_RESTORE on the FSE popup is forbidden — wedges DWM). Poll
    //     IsIconic with a 1s bounded wait so a stuck window thread
    //     doesn't block shutdown.
    if (!dead && window_ && window_->Hwnd() && IsIconic(window_->Hwnd())) {
        NV3D_LOG_INFO(L"D3D9Presenter::Shutdown: popup is iconic — restoring before teardown");
        window_->SetWantVisible(true);
        for (int i = 0; i < 20 && IsIconic(window_->Hwnd()); ++i) {
            Sleep(50);  // window thread polls want_visible_ every 50ms
        }
        if (IsIconic(window_->Hwnd())) {
            NV3D_LOG_WARN(L"D3D9Presenter::Shutdown: popup did not restore within 1s — proceeding anyway");
        } else {
            NV3D_LOG_INFO(L"D3D9Presenter::Shutdown: popup restored");
            // SW_RESTORE returns as soon as the window is non-iconic, but
            // the driver's FSE re-engagement (modeset + stereo re-engage)
            // is still in flight. Releasing the device mid-re-engagement
            // shows no immediate symptom, but can leave the driver's
            // stereo state dirty — the NEXT process to engage 3D Vision
            // trips over it (observed: adapter reset when a fresh Geo-11
            // producer started after a Stop-while-minimized session).
            // Let the re-engagement land, then drain the GPU before the
            // teardown below touches the device.
            Sleep(400);
            WaitForGpuIdle(250);
            NV3D_LOG_INFO(L"D3D9Presenter::Shutdown: post-restore settle complete");
        }
    }

    // 0. Strip WS_EX_LAYERED | WS_EX_TRANSPARENT from the FSE window FIRST
    //    and let DWM settle. Doing this after the D3D9 release races with
    //    DWM's compositing state for the layered window and freezes input
    //    OS-wide on some configs (VRto3D's NvStereoDx9Presenter does the
    //    same thing in RemoveFseSubclass for the same reason). Skipped on
    //    the dead-device path because the GPU has already TDR'd anyway.
    if (!dead && window_) {
        window_->RemoveClickThrough();
    }

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
        (void)packed_default_.Detach();
        (void)device9_.Detach();
        (void)d3d9_.Detach();
    } else {
        packed_default_.Reset();
        device9_.Reset();
        d3d9_.Reset();
    }
    packed_w_ = packed_h_ = 0;
    sig_valid_ = false;

    // No LightBoost revert. Enable() commits the timing to NVCP via
    // NvAPI_DISP_SaveCustomDisplay; we deliberately leave it applied so it
    // survives across host sessions (and across reboots). Removing the
    // revert step also shrinks the teardown's blast radius — the old
    // RevertCustomDisplayTrial + ChangeDisplaySettingsExW fallback ran
    // after D3D9 release, during the DWM-reclaim window, and was a
    // contributor to the post-quit display-freeze pattern.

    // NvAPI_Unload mirrors VRto3D's teardown step 5. Unconditional — runs
    // even on the dead-device path (NvAPI cleans up the leaked stereo handle
    // here). NvAPI is reference-counted process-wide, so this only fully
    // disables NVAPI when our Initialize was the last live ref.
    NvAPI_Unload();
}

bool D3D9Presenter::BuildD3D9Stack() {
    // NvAPI must be initialised BEFORE the D3D9Ex device is created. We
    // deliberately do NOT call NvAPI_Stereo_SetDriverMode here — leaving
    // the driver in its default AUTOMATIC mode is what enables the NV3D
    // signature scanner that consumes the magic header row we write into
    // the packed surface every frame. See header file for the contract.
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
    NV3D_LOG_INFO(L"D3D9Presenter: stereo driver mode = AUTOMATIC (NV3D signature)");
    // Build stamp so NV3D-Glass.log self-identifies which lib mitigations
    // were actually in the running binary — repro logs are useless without it.
    NV3D_LOG_INFO(L"NV3DLib build r2: bbcount=3 maxlatency=1 per-frame-backbuffer "
                  L"seh-marks-dead drain-on-hide import-cache=4 stall+stereo-health probes");

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
        // BackBufferCount = 3: Geo-11's nvidia_dx9 output mode explicitly
        // forces 3 for this same legacy FSE signature path. At count 2 with
        // DISCARD rotation the driver's stereo shadow-surface cache churns
        // identity every present, and the ~30s stereo revalidation window
        // becomes correspondingly fragile.
        pp.BackBufferCount            = 3;
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

    // CreateDeviceEx FSE retry loop. The first attempt after a GPU TDR
    // (when the host rebuilds its D3D11 device + immediately re-Starts)
    // frequently returns E_FAIL or DEVICELOST because the driver hasn't
    // fully reclaimed the display for fullscreen-exclusive use yet — the
    // user-visible symptom is "SBS in the host's control panel window"
    // because we fall through to windowed and there's no stereo routing.
    // Backoff retries give the driver up to ~1s to settle on its own.
    hr = E_FAIL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        hr = d3d9_->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, hwnd,
                                     create_flags, &pp, &fs_mode, &device9_);
        if (SUCCEEDED(hr) && device9_) break;
        NV3D_LOG_INFO(L"D3D9Presenter: CreateDeviceEx FSE attempt %d/5 failed hr=0x%08X — retrying",
                       attempt + 1, hr);
        device9_.Reset();
        Sleep(static_cast<DWORD>(100 * (attempt + 1)));   // 100..500 ms
    }
    if (FAILED(hr) || !device9_) {
        NV3D_LOG_WARN(L"CreateDeviceEx FSE failed after 5 retries hr=0x%08X — falling back to windowed", hr);
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
    // Leave surface-creation mode at the driver default (AUTO). FORCESTEREO
    // is a DIRECT-mode leftover — with the NV3D signature scanner active,
    // the packed RT is a mono source that the driver demuxes at PresentEx.
    // Force-stereoizing it makes the packed RT itself a stereo pair, which
    // doubles per-frame driver allocations / copies and puts the scanner in
    // an untested corner (mono-demux-from-stereo-source). Not required for
    // AUTOMATIC + signature.

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

    // Cap queued frames at 1. During the driver's ~20-30s stereo revalidation
    // any queued stereo blits sitting on the D3D9 engine are prime targets
    // for the reshuffle — the shorter the queue, the smaller the window in
    // which a queued StretchRect can be reading a backbuffer whose backing
    // allocation just got swapped out from under it.
    if (HRESULT lat_hr = device9_->SetMaximumFrameLatency(1); FAILED(lat_hr)) {
        NV3D_LOG_WARN(L"D3D9Presenter: SetMaximumFrameLatency(1) failed hr=0x%08X — continuing", lat_hr);
    }

    // NOTE: no cached GetBackBuffer here. The driver's periodic stereo
    // revalidation (~20-30s) reshuffles backbuffer state; a session-cached
    // surface pointer becomes stale across that boundary and either UM-AVs
    // or references a freed allocation in a queued command buffer (→ TDR).
    // Present() acquires the current backbuffer per frame — GetBackBuffer
    // on a live swap chain is just an AddRef and is negligible.
    return true;
}

bool D3D9Presenter::EnsurePackedSurface() {
    if (!device9_) return false;
    const uint32_t want_w = monitor_w_ * 2u;
    const uint32_t want_h = monitor_h_;
    if (packed_default_ && packed_w_ == want_w && packed_h_ == want_h) return true;

    packed_default_.Reset();
    sig_valid_ = false;   // surface gone → signature row needs rewriting

    // Lockable so RefreshSignatureIfNeeded can LockRect+memcpy the 20-byte
    // NV3D signature into row H. Default pool so it lives in VRAM and can
    // be a StretchRect source/dest. +1 row of vertical height carries the
    // signature; the body occupies rows 0..H-1.
    HRESULT hr = device9_->CreateRenderTarget(
        want_w, want_h + 1u,
        D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0,
        /*Lockable=*/TRUE,
        &packed_default_, nullptr);
    if (FAILED(hr) || !packed_default_) {
        NV3D_LOG_ERROR(L"D3D9Presenter: CreateRenderTarget(packed %ux%u) failed hr=0x%08X",
                       want_w, want_h + 1u, hr);
        CheckAndMarkD3D9Dead(hr, "CreateRenderTarget(packed)");
        return false;
    }
    packed_w_ = want_w;
    packed_h_ = want_h;
    return true;
}

void D3D9Presenter::RefreshSignatureIfNeeded() {
    if (!packed_default_) return;
    const bool live_swap = eye_swap_live_.load();

    if (sig_valid_ &&
        sig_width_  == monitor_w_ &&
        sig_height_ == monitor_h_ &&
        sig_swap_   == live_swap) {
        return;
    }

    NvStereoImageHeader hdr{};
    hdr.signature = kNvStereoSignature;
    hdr.width     = monitor_w_;
    hdr.height    = monitor_h_;
    hdr.bpp       = 32;
    hdr.flags     = live_swap ? 1u : 0u;

    // Header lives in the (H)th row of the packed surface. LockRect just
    // the 5-DWORD prefix in that row — driver only inspects the first
    // 20 bytes of the row.
    RECT hdr_rect{ 0, static_cast<LONG>(packed_h_), 5,
                   static_cast<LONG>(packed_h_) + 1 };
    D3DLOCKED_RECT lr{};
    HRESULT hr = packed_default_->LockRect(&lr, &hdr_rect, 0);
    if (FAILED(hr)) {
        NV3D_LOG_WARN(L"D3D9Presenter: LockRect(NV3D header row) failed hr=0x%08X", hr);
        CheckAndMarkD3D9Dead(hr, "LockRect(NV3D header)");
        return;
    }
    std::memcpy(lr.pBits, &hdr, sizeof(hdr));
    packed_default_->UnlockRect();

    sig_width_  = monitor_w_;
    sig_height_ = monitor_h_;
    sig_swap_   = live_swap;
    sig_valid_  = true;
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

    if (!EnsurePackedSurface()) return E_FAIL;

    // Stage 1: shared input (2W_in × H_in SbS) → packed body (2W_out × H_out).
    // LINEAR filter resamples between input and panel dimensions if they
    // differ (e.g. producer at 3840×1080, panel at 2560×1440).
    const RECT in_full{ 0, 0, static_cast<LONG>(input_w),  static_cast<LONG>(input_h) };
    const RECT body{    0, 0, static_cast<LONG>(packed_w_), static_cast<LONG>(packed_h_) };
    HRESULT hr = device9_->StretchRect(shared_input, &in_full,
                                        packed_default_.Get(), &body,
                                        D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"StretchRect(shared->packed) failed hr=0x%08X", hr);
        CheckAndMarkD3D9Dead(hr, "StretchRect(shared->packed)");
        return hr;
    }

    // Stage 2: refresh the NV3D signature row if dims or eye-swap changed.
    // No-op in steady state.
    RefreshSignatureIfNeeded();

    // Stage 3: packed (2W × H + signature) → back buffer (W × H). The 2:1
    // horizontal squash is what triggers the driver's signature scanner —
    // it sees the magic header and routes left/right halves of the body
    // to the alternate eyes instead of squashing them into one plane.
    //
    // Re-acquire the current backbuffer every frame instead of caching it
    // in BuildD3D9Stack. See the SetMaximumFrameLatency comment there for
    // the ~30s revalidation-vs-cached-pointer rationale.
    Microsoft::WRL::ComPtr<IDirect3DSurface9> bb;
    hr = device9_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        NV3D_LOG_ERROR(L"GetBackBuffer failed hr=0x%08X", hr);
        CheckAndMarkD3D9Dead(hr, "GetBackBuffer");
        return hr;
    }
    hr = device9_->StretchRect(packed_default_.Get(), &body,
                                bb.Get(), nullptr,
                                D3DTEXF_LINEAR);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"StretchRect(packed->backbuf) failed hr=0x%08X", hr);
        CheckAndMarkD3D9Dead(hr, "StretchRect(packed->backbuf)");
        return hr;
    }

    hr = device9_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"PresentEx failed hr=0x%08X", hr);
        CheckAndMarkD3D9Dead(hr, "PresentEx");
        return hr;
    }

    StereoActivationRetry();
    StereoHealthProbe();
    return S_OK;
}

void D3D9Presenter::StereoHealthProbe() {
    if (!stereo_handle_ || !stereo_activated_) return;
    const DWORD now = GetTickCount();
    if (now - last_health_poll_tick_ < 1000) return;
    last_health_poll_tick_ = now;

    NvU8 active = 0;
    NvAPI_Stereo_IsActivated(stereo_handle_, &active);
    const bool is_active = (active != 0);
    if (is_active != health_active_last_) {
        if (!is_active) {
            NV3D_LOG_WARN(L"D3D9Presenter: stereo health — IsActivated flipped FALSE "
                          L"(driver stereo revalidation / state loss in progress)");
        } else {
            NV3D_LOG_INFO(L"D3D9Presenter: stereo health — IsActivated recovered to TRUE");
        }
        health_active_last_ = is_active;
    }
}

void D3D9Presenter::WaitForGpuIdle(DWORD timeout_ms) {
    if (!device9_ || d3d9_dead_.load()) return;
    Microsoft::WRL::ComPtr<IDirect3DQuery9> q;
    if (FAILED(device9_->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) return;
    q->Issue(D3DISSUE_END);
    const DWORD t0 = GetTickCount();
    // S_FALSE = still in flight; anything else (S_OK or a device error)
    // ends the wait — errors are picked up by the next Present's own checks.
    while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        if (GetTickCount() - t0 >= timeout_ms) {
            NV3D_LOG_WARN(L"D3D9Presenter: WaitForGpuIdle timed out after %lums — proceeding",
                          static_cast<unsigned long>(timeout_ms));
            return;
        }
        Sleep(1);
    }
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
