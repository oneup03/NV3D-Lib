/*
 * DX11 backend — DIRECT-mode shared-handle path.
 *
 * Fast path: host's ID3D11Texture2D is MISC_SHARED + B8G8R8A8_UNORM(_SRGB).
 *   IDXGIResource::GetSharedHandle() → device9_->CreateTexture(pSharedHandle=h).
 *   Per-frame: ID3D11Query EVENT sync, presenter->Present(shared_d3d9_sfc).
 *
 * Mirror path (auto): host's texture is not MISC_SHARED or not BGRA.
 *   Library creates an internal MISC_SHARED + BGRA mirror sized to host.
 *   Per-frame: CopyResource host → mirror, EVENT sync, presenter->Present.
 *   Format must be BGRA8 _or_ RGBA8 family — RGBA mirrors require a tiny
 *   shader-side swap which we'll add as a follow-up. For now: BGRA only.
 */
#pragma once

#ifndef NV3DLIB_DISABLE_DX11

#include "NV3D.hpp"
#include "async_presenter.h"
#include "d3d9_presenter.h"
#include "nv_3dvision_suppressor.h"
#include "present_window.h"

#include <wrl/client.h>
#include <d3d11.h>
#include <d3d11_4.h>     // ID3D11Device5 / ID3D11DeviceContext4 / ID3D11Fence
#include <memory>

namespace NV3D {

class DX11Backend final : public InterfaceDX11 {
public:
    HRESULT Init(ID3D11Device* device, const InitParams& params);

    HRESULT SetInputTexture(ID3D11Texture2D* sbs_tex) override;
    HRESULT Present() override;
    void    SetVisible(bool visible) override;
    void    SetEyeSwap(bool enable) override;
    void    Delete() override;

private:
    bool EnsureSharedImport(ID3D11Texture2D* sbs);
    bool EnsureMirror(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool EnsureSyncQuery();
    HRESULT WaitForDx11Writes();

    // Fence path: when supported, replaces the EVENT-query GetData spin with
    // an ID3D11Fence + Win32 event so the wait moves entirely to the async
    // worker. Returns true on systems with D3D11.4 (ID3D11Device5).
    bool TryInitFencePath();

    InitParams params_{};
    std::unique_ptr<PresentWindow>  window_;
    std::unique_ptr<D3D9Presenter>  presenter_;
    Nv3DVisionSuppressor            suppressor_;

    // FULL ASYNC when ID3D11Fence is available (D3D11.4 / Win10+). The host
    // thread runs the mirror CopyResource (brief, host-context-bound),
    // signals an ID3D11Fence on host_ctx4_, then hands off to the worker —
    // the worker waits on a Win32 event via fence_->SetEventOnCompletion
    // (no host_ctx access) and only then runs the D3D9 PresentEx. Submit
    // returns in microseconds.
    //
    // Fallback (no D3D11.4): SPLIT ASYNC, same as before — the host thread
    // spins on GetData up to 500 ms before handing off. Only used on very
    // old hardware where ID3D11Device5 isn't available.
    AsyncPresenter                  async_;

    Microsoft::WRL::ComPtr<ID3D11Device>        host_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> host_ctx_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_tex_;
    DXGI_FORMAT input_fmt_ = DXGI_FORMAT_UNKNOWN;
    uint32_t    input_w_   = 0;
    uint32_t    input_h_   = 0;

    // Internal mirror used when host's texture isn't MISC_SHARED + BGRA.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mirror_tex_;
    uint32_t    mirror_w_ = 0;
    uint32_t    mirror_h_ = 0;
    DXGI_FORMAT mirror_fmt_ = DXGI_FORMAT_UNKNOWN;

    // The texture we actually import into D3D9 — either host's directly, or
    // our mirror. Pointer identity used to drive cache invalidation.
    ID3D11Texture2D* shared_src_ = nullptr;

    // D3D9 view onto shared_src_.
    Microsoft::WRL::ComPtr<IDirect3DTexture9>  shared_d3d9_tex_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>  shared_d3d9_sfc_;
    void*    shared_cache_ptr_   = nullptr;
    HANDLE   shared_cache_handle_= nullptr;
    uint32_t shared_cache_w_     = 0;
    uint32_t shared_cache_h_     = 0;
    DXGI_FORMAT shared_cache_fmt_ = DXGI_FORMAT_UNKNOWN;

    // ID3D11Query EVENT for cross-device sync. Used only as a fallback when
    // the fence path below isn't available (very old hardware).
    Microsoft::WRL::ComPtr<ID3D11Query> sync_query_;

    // Fence-based sync (D3D11.4). When fence_ is non-null, Present() uses
    // the fully-async path: Signal on host_ctx4_ then defer the wait to the
    // worker via fence_->SetEventOnCompletion. Host thread returns to the
    // caller in microseconds instead of spinning on GetData up to 500 ms.
    Microsoft::WRL::ComPtr<ID3D11Device5>        host_device5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> host_ctx4_;
    Microsoft::WRL::ComPtr<ID3D11Fence>          fence_;
    HANDLE                                       fence_event_ = nullptr;  // auto-reset
    uint64_t                                     fence_value_ = 0;
};

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_DX11
