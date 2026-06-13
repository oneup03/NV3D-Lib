/*
 * NVIDIA 3D Vision DIRECT-mode presenter.
 *
 * Shared D3D9Ex device + NVAPI stereo handle (FORCESTEREO + DIRECT mode).
 * Each backend supplies a shared D3D9 surface (the SbS image, 2W x H) and
 * calls Present(). Per frame:
 *
 *   SetActiveEye(LEFT)  → StretchRect(shared, [0..W),  back, nullptr, POINT)
 *   SetActiveEye(RIGHT) → StretchRect(shared, [W..2W), back, nullptr, POINT)
 *   PresentEx
 *
 * DIRECT mode (not AUTOMATIC) is required because NV3D's AUTOMATIC-mode
 * signature scanning got one eye plane stuck on the prior frame's content
 * when the source resource changed, even with a stable signed RT in the
 * chain. DIRECT bypasses auto-detection — SetActiveEye is authoritative.
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

    IDirect3D9Ex*       D3D()    const { return d3d9_.Get(); }
    IDirect3DDevice9Ex* Device() const { return device9_.Get(); }
    StereoHandle        StereoHandleOpaque() const { return stereo_handle_; }
    bool                IsFSE()  const { return is_fse_; }

private:
    bool BuildD3D9Stack();
    void StereoActivationRetry();

    PresentWindow* window_ = nullptr;
    InitParams     params_{};

    Microsoft::WRL::ComPtr<IDirect3D9Ex>       d3d9_;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device9_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  back_buffer_;

    uint32_t monitor_w_ = 0;
    uint32_t monitor_h_ = 0;
    bool     is_fse_    = false;

    StereoHandle stereo_handle_         = nullptr;
    bool         stereo_activated_      = false;
    int          activation_retries_left_ = 0;
    DWORD        last_stereo_activate_tick_ = 0;
    bool         activation_summary_logged_ = false;

    std::atomic<bool> d3d9_dead_{false};
    int               frames_since_state_check_ = 0;

    LightBoost lightboost_;
};

}  // namespace NV3D
