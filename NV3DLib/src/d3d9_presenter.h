/*
 * NVIDIA 3D Vision AUTOMATIC-mode presenter (NV3D signature path).
 *
 * Shared D3D9Ex device + NVAPI stereo handle. The driver stays in its
 * default AUTOMATIC mode — we don't call NvAPI_Stereo_SetDriverMode.
 * Backends supply a shared D3D9 surface (the SbS image, 2W_input × H_input)
 * and call Present(). Per frame:
 *
 *   StretchRect(shared,  full,  packed[body], full)       // 2W×H scratch
 *   RefreshSignatureIfNeeded()                            // NV3D row at y=H
 *   StretchRect(packed,  full,  back_buffer, nullptr, LINEAR)
 *   PresentEx
 *
 * `packed` is a lockable RT sized 2*panel_w × (panel_h + 1). Its last row
 * carries the NV3D signature (0x4433564E + per-eye width/height/bpp/swap),
 * which the NVIDIA driver scans on PresentEx and uses to route the left/
 * right halves of the source body to alternate eyes through the IR
 * emitter — no per-eye SetActiveEye needed.
 *
 * Why we moved off DIRECT mode: per-eye SetActiveEye + StretchRect-to-
 * backbuffer was empirically wedging the FSE D3D9 pipeline after ~30 s of
 * presents in a Katanga-style "shared SbS texture" scenario, leaving
 * stereo on but no new frames. AUTOMATIC + signature replicates VRto3D's
 * proven pattern. The historical concern about AUTOMATIC mode (one eye
 * plane stuck on prior content when the *source resource* changed) does
 * not apply here — our staging surface identity is stable for the lifetime
 * of a session; only its content changes.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <wrl/client.h>
#include <d3d9.h>

#include "NV3D.hpp"
#include "lightboost.h"

using StereoHandle = void*;

namespace NV3D {

class PresentWindow;

class D3D9Presenter {
public:
    D3D9Presenter() = default;
    ~D3D9Presenter();

    bool Init(PresentWindow* window, const InitParams& params);
    void Shutdown();

    // Per-frame: the backend supplies the shared D3D9 surface (2 * eye_w × h
    // SbS image), the presenter does per-eye SetActiveEye + StretchRect +
    // PresentEx + activation retry.
    HRESULT Present(IDirect3DSurface9* shared_input,
                     uint32_t input_w, uint32_t input_h);

    bool CheckAndMarkD3D9Dead(HRESULT hr, const char* origin);
    bool IsDead() const { return d3d9_dead_.load(); }

    // Bounded wait for the D3D9 device's command stream to fully retire
    // (D3DQUERYTYPE_EVENT spin). Used by the hide path so the FSE popup's
    // SW_MINIMIZE — and the driver's stereo-teardown modeset it triggers —
    // starts from a quiet pipeline instead of racing in-flight stereo blits.
    // Only call when the async worker is known idle: with
    // D3DCREATE_MULTITHREADED the device critical section would otherwise
    // serialize us behind a possibly-wedged in-flight call.
    void WaitForGpuIdle(DWORD timeout_ms);

    // Live toggle of the eye-swap flag. Writes to an atomic that
    // RefreshSignatureIfNeeded reads on the next frame, which causes the
    // NV3D signature row to be rewritten with the new flag — driver picks
    // up the new routing on the next PresentEx. No teardown / re-Init
    // needed, no visible glitch.
    void SetEyeSwap(bool enable) { eye_swap_live_.store(enable); }

    IDirect3D9Ex*       D3D()    const { return d3d9_.Get(); }
    IDirect3DDevice9Ex* Device() const { return device9_.Get(); }
    StereoHandle        StereoHandleOpaque() const { return stereo_handle_; }
    bool                IsFSE()  const { return is_fse_; }

private:
    bool BuildD3D9Stack();
    void StereoActivationRetry();
    void StereoHealthProbe();
    bool EnsurePackedSurface();
    void RefreshSignatureIfNeeded();

    PresentWindow* window_ = nullptr;
    InitParams     params_{};

    Microsoft::WRL::ComPtr<IDirect3D9Ex>       d3d9_;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device9_;

    // Lockable render target carrying the SbS body + NV3D signature row.
    // Dimensions: 2 * monitor_w_ × (monitor_h_ + 1). Created lazily on the
    // first Present so we have device9_ to allocate against.
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  packed_default_;
    uint32_t packed_w_ = 0;
    uint32_t packed_h_ = 0;   // body height (excludes the +1 signature row)

    // Cached state of the NV3D signature row. Re-written only when one of
    // these has changed since the last write — saves a per-frame LockRect /
    // GPU-sync on the steady-state path.
    uint32_t sig_width_  = 0;
    uint32_t sig_height_ = 0;
    bool     sig_swap_   = false;
    bool     sig_valid_  = false;

    uint32_t monitor_w_ = 0;
    uint32_t monitor_h_ = 0;
    bool     is_fse_    = false;

    StereoHandle stereo_handle_         = nullptr;
    bool         stereo_activated_      = false;
    int          activation_retries_left_ = 0;
    DWORD        last_stereo_activate_tick_ = 0;
    bool         activation_summary_logged_ = false;

    // 1Hz IsActivated tripwire (worker thread only). A FALSE flap right
    // before a freeze/TDR pins the blame on the driver's periodic stereo
    // revalidation; a clean TRUE across the incident rules it out.
    DWORD        last_health_poll_tick_ = 0;
    bool         health_active_last_    = true;

    std::atomic<bool> d3d9_dead_{false};
    int               frames_since_state_check_ = 0;

    // Live eye-swap state. Initialised from params_.eye_swap at Init,
    // updated by SetEyeSwap. Read by RefreshSignatureIfNeeded.
    std::atomic<bool> eye_swap_live_{false};

    LightBoost lightboost_;
};

}  // namespace NV3D
