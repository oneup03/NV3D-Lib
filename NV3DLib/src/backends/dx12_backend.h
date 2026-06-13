#pragma once

#ifndef NV3DLIB_DISABLE_DX12

#include "NV3D.hpp"
#include "d3d9_presenter.h"
#include "nv_3dvision_suppressor.h"
#include "present_window.h"

#include <wrl/client.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <memory>

namespace NV3D {

// DX12 → DX11 → D3D9 via D3D11On12 (the legacy 3D Vision driver rejects
// the OpenSharedResource1 path with E_INVALIDARG on DX12 NT handles —
// D3D11On12 uses a Direct3D-managed wrapper layer that works regardless).
//
// Init:
//   - Create our own DIRECT command queue on the host's DX12 device.
//   - D3D11On12CreateDevice(host_device, our_queue) → bridge DX11 device +
//     immediate context. The bridge runs on the host's DX12 adapter.
//   - QI for ID3D11On12Device (wrap APIs), ID3D11Device5 (shared fence),
//     ID3D11DeviceContext4 (Wait).
//   - PresentWindow + D3D9Presenter (D3D9Ex on the same adapter).
//
// On first SetInputTexture (and on resource / dim / format change):
//   - on12_dev_->CreateWrappedResource(host_resource, BIND_SHADER_RESOURCE,
//                                       InState=COMMON, OutState=COMMON)
//                                       → ID3D11Texture2D wrapper.
//     No NT handle, no LUID matching, no OpenSharedResource1.
//   - Lib creates an MISC_SHARED (legacy KMT handle) BGRA mirror sized to
//     host with BIND_SHADER_RESOURCE | BIND_RENDER_TARGET.
//   - D3D9Ex::CreateTexture(pSharedHandle=legacy_kmt) → IDirect3DTexture9.
//
// On first sync_fence:
//   - Host's ID3D12Fence (FLAG_SHARED) → CreateSharedHandle (NT).
//   - ID3D11Device5::OpenSharedFence (cached).
//
// Per Present:
//   - bridge_ctx4_->Wait(shared_fence, value) on our DIRECT queue.
//   - on12_dev_->AcquireWrappedResources(&wrapped, 1) — transitions DX12 →
//     DX11 visible.
//   - RGBA → BGRA swizzle shader pass writes wrapped (RGBA SRV) into the
//     legacy mirror (BGRA RTV).
//   - on12_dev_->ReleaseWrappedResources(&wrapped, 1) — transitions back.
//   - presenter->Present(d3d9_view_of_legacy_mirror).
//
// Format contract: host's DX12 resource must be RGBA8 or BGRA8 family.
class DX12Backend final : public InterfaceDX12 {
public:
    HRESULT Init(ID3D12Device* device, ID3D12CommandQueue* queue,
                  const InitParams& params);
    HRESULT SetInputTexture(ID3D12Resource* sbs_tex,
                             ID3D12Fence* sync_fence,
                             uint64_t fence_value) override;
    HRESULT Present() override;
    void    Delete() override;

private:
    bool CreateBridgeDeviceOn12();
    bool CreateSwizzleShaders();
    HRESULT EnsureResourceImport(ID3D12Resource* sbs);
    HRESULT EnsureFenceImport(ID3D12Fence* fence);

    InitParams params_{};
    std::unique_ptr<PresentWindow> window_;
    std::unique_ptr<D3D9Presenter> presenter_;
    Nv3DVisionSuppressor           suppressor_;

    Microsoft::WRL::ComPtr<ID3D12Device>            host_device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>      host_queue_;        // unused by us, retained for completeness
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>      our_queue_;         // DIRECT queue we own, used by D3D11On12

    Microsoft::WRL::ComPtr<ID3D11Device>            bridge_dev_;
    Microsoft::WRL::ComPtr<ID3D11On12Device>        on12_dev_;
    Microsoft::WRL::ComPtr<ID3D11Device5>           bridge_dev5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     bridge_ctx_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4>    bridge_ctx4_;

    // DX12-side input.
    Microsoft::WRL::ComPtr<ID3D12Resource>          input_tex_;
    Microsoft::WRL::ComPtr<ID3D12Fence>             input_fence_;
    uint64_t                                        input_fence_value_ = 0;
    DXGI_FORMAT                                     input_fmt_ = DXGI_FORMAT_UNKNOWN;
    uint32_t                                        input_w_   = 0;
    uint32_t                                        input_h_   = 0;

    // D3D11On12 wrapper around the host's DX12 resource.
    void*                                           cached_input_ptr_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         wrapped_;

    // Internal DX11 legacy-shared texture (D3D9-openable).
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         legacy_shared_;
    HANDLE                                          legacy_handle_   = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DTexture9>       d3d9_tex_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>       d3d9_sfc_;
    uint32_t                                        legacy_w_ = 0;
    uint32_t                                        legacy_h_ = 0;

    // Imported shared fence.
    void*                                           cached_fence_ptr_ = nullptr;
    HANDLE                                          nt_fence_handle_  = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Fence>             shared_fence_;

    Microsoft::WRL::ComPtr<ID3D11Query>             sync_query_;

    // RGBA → BGRA swizzle shader pass. The DX12 source is RGBA8 (NVIDIA's
    // DX12→DX11 OpenSharedResource1 rejects BGRA); we sample the alias and
    // write `.bgra` into our BGRA mirror so D3D9 sees the right byte order.
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      swizzle_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       swizzle_ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      swizzle_sampler_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> input_srv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  output_rtv_;
};

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_DX12
