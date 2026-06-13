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
#include "d3d9_presenter.h"
#include "nv_3dvision_suppressor.h"
#include "present_window.h"

#include <wrl/client.h>
#include <d3d11.h>
#include <memory>

namespace NV3D {

class DX11Backend final : public InterfaceDX11 {
public:
    HRESULT Init(ID3D11Device* device, const InitParams& params);

    HRESULT SetInputTexture(ID3D11Texture2D* sbs_tex) override;
    HRESULT Present() override;
    void    Delete() override;

private:
    bool EnsureSharedImport(ID3D11Texture2D* sbs);
    bool EnsureMirror(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool EnsureSyncQuery();
    HRESULT WaitForDx11Writes();

    InitParams params_{};
    std::unique_ptr<PresentWindow>  window_;
    std::unique_ptr<D3D9Presenter>  presenter_;
    Nv3DVisionSuppressor            suppressor_;

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

    // ID3D11Query EVENT for cross-device sync.
    Microsoft::WRL::ComPtr<ID3D11Query> sync_query_;
};

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_DX11
