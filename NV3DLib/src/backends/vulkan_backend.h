#pragma once

#ifndef NV3DLIB_DISABLE_VULKAN

#include "NV3D.hpp"
#include "async_presenter.h"
#include "d3d9_presenter.h"
#include "nv_3dvision_suppressor.h"
#include "present_window.h"

#include <wrl/client.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <memory>

namespace NV3D {

// Vulkan backend — inverted-export pipeline (lib creates DX11 resources,
// host imports them as VkImage / VkSemaphore). See the public InterfaceVulkan
// docs in NV3D.hpp for the host's contract.
//
// Internal pipeline (all on lib's bridge DX11 device, on the Vulkan adapter):
//   1. shared_tex_       : MISC_SHARED|MISC_SHARED_NTHANDLE BGRA texture.
//                           Host imports as VkImage and renders into it.
//   2. shared_fence_     : ID3D11Fence (FLAG_SHARED).
//                           Host imports as VkSemaphore (timeline) and signals
//                           it after each frame.
//   3. legacy_shared_    : MISC_SHARED (legacy KMT) BGRA mirror. D3D9 opens
//                           via legacy_handle_. Per frame: CopyResource from
//                           shared_tex_ → legacy_shared_ on bridge_ctx.
//   4. d3d9_tex_/_sfc_   : D3D9 view of legacy_shared_ (via pSharedHandle).
//
// Per-frame Present(sem_value):
//   - bridge_ctx4_->Wait(shared_fence_, sem_value) — GPU-side wait.
//   - bridge_ctx_->CopyResource(shared_tex_ → legacy_shared_).
//   - sync_query EVENT poll (500ms deadline, marks D3D9 dead on timeout).
//   - presenter_->Present(d3d9_sfc_, w, h).
class VulkanBackend final : public InterfaceVulkan {
public:
    HRESULT Init(NV3DVkInstance inst, NV3DVkPhysicalDevice phys,
                  NV3DVkDevice dev, uint32_t qfi,
                  const InitParams& params);

    HRESULT InitSharedResources(uint32_t w, uint32_t h, uint32_t dxgi_format,
                                  HANDLE* out_memory_handle,
                                  HANDLE* out_fence_handle) override;
    HRESULT Present(uint64_t sem_value) override;
    void    Delete() override;

private:
    bool ResolveAdapterLuid(LUID* out_luid);
    bool CreateBridgeDevice(LUID adapter_luid);
    HRESULT PresentSyncBody(uint64_t sem_value);

    InitParams params_{};
    std::unique_ptr<PresentWindow> window_;
    std::unique_ptr<D3D9Presenter> presenter_;
    Nv3DVisionSuppressor           suppressor_;

    // Off-thread present worker — same role as in DX12Backend. Bridge state
    // and the D3D9 presenter are all lib-owned so the worker can use them
    // safely without any synchronization with the host's render thread.
    AsyncPresenter                 async_;

    NV3DVkInstance       inst_ = {};
    NV3DVkPhysicalDevice phys_ = {};
    NV3DVkDevice         dev_  = {};
    uint32_t             qfi_  = 0;

    Microsoft::WRL::ComPtr<ID3D11Device>            bridge_dev_;
    Microsoft::WRL::ComPtr<ID3D11Device5>           bridge_dev5_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     bridge_ctx_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4>    bridge_ctx4_;

    // Resources created in InitSharedResources, owned by us.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         shared_tex_;
    HANDLE                                          shared_tex_handle_ = nullptr;  // NT, we own + close
    Microsoft::WRL::ComPtr<ID3D11Fence>             shared_fence_;
    HANDLE                                          shared_fence_handle_ = nullptr;  // NT, we own + close

    Microsoft::WRL::ComPtr<ID3D11Texture2D>         legacy_shared_;
    HANDLE                                          legacy_handle_ = nullptr;       // KMT — never close
    Microsoft::WRL::ComPtr<IDirect3DTexture9>       d3d9_tex_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9>       d3d9_sfc_;
    uint32_t                                        shared_w_ = 0;
    uint32_t                                        shared_h_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Query>             sync_query_;
};

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_VULKAN
