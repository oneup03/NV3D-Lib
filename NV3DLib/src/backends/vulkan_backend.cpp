#include "backends/vulkan_backend.h"

#ifndef NV3DLIB_DISABLE_VULKAN

#include <chrono>
#include <cstring>
#include <thread>

#include <dxgi1_4.h>
#include <vulkan/vulkan.h>

#include "log.h"

namespace NV3D {

namespace {

bool LuidsEqual(LUID a, LUID b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

PFN_vkGetInstanceProcAddr LoadVulkanLoader() {
    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");
    if (!m) m = LoadLibraryW(L"vulkan-1.dll");
    if (!m) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(m, "vkGetInstanceProcAddr"));
}

}  // anonymous

HRESULT VulkanBackend::Init(NV3DVkInstance inst, NV3DVkPhysicalDevice phys,
                              NV3DVkDevice dev, uint32_t qfi,
                              const InitParams& params) {
    if (!inst || !phys || !dev) return E_POINTER;
    params_ = params;
    inst_ = inst; phys_ = phys; dev_ = dev; qfi_ = qfi;

    LUID luid{};
    if (!ResolveAdapterLuid(&luid)) {
        NV3D_LOG_ERROR(L"VulkanBackend: could not resolve adapter LUID from VkPhysicalDevice");
        return E_FAIL;
    }
    if (!CreateBridgeDevice(luid)) return E_FAIL;

    window_ = std::make_unique<PresentWindow>();
    PresentWindowConfig wcfg{};
    wcfg.target_monitor = params.target_monitor;
    wcfg.host_hwnd      = params.host_hwnd;
    wcfg.on_top         = params.on_top;
    wcfg.title          = L"NV3DLib (Vulkan)";
    if (!window_->Init(wcfg)) {
        NV3D_LOG_ERROR(L"VulkanBackend: PresentWindow::Init failed");
        return E_FAIL;
    }
    presenter_ = std::make_unique<D3D9Presenter>();
    if (!presenter_->Init(window_.get(), params)) {
        NV3D_LOG_ERROR(L"VulkanBackend: D3D9Presenter::Init failed");
        return E_FAIL;
    }

    if (params_.enable_suppressor) suppressor_.Install();

    // Spawn the async present worker — same role as in DX12Backend.
    async_.Start();
    return S_OK;
}

bool VulkanBackend::ResolveAdapterLuid(LUID* out_luid)
{
    NV3D_LOG_INFO(L"*** HARDCODED ResolveAdapterLuid ***");

    out_luid->LowPart  = 0x0066B499;
    out_luid->HighPart = 0x00000000;

    return true;
}

bool VulkanBackend::CreateBridgeDevice(LUID adapter_luid) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> target_adapter;
    for (UINT i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        if (LuidsEqual(d.AdapterLuid, adapter_luid)) { target_adapter = a; break; }
    }
    if (!target_adapter) {
        NV3D_LOG_ERROR(L"VulkanBackend: no DXGI adapter matches Vulkan LUID");
        return false;
    }
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(target_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     fls, _countof(fls), D3D11_SDK_VERSION,
                                     &bridge_dev_, &got, &bridge_ctx_);
    if (FAILED(hr) || !bridge_dev_) {
        NV3D_LOG_ERROR(L"VulkanBackend: bridge D3D11CreateDevice failed hr=0x%08X", hr);
        return false;
    }
    if (FAILED(bridge_dev_->QueryInterface(IID_PPV_ARGS(&bridge_dev5_)))) {
        NV3D_LOG_ERROR(L"VulkanBackend: ID3D11Device5 QI failed — Win10 1809+ required");
        return false;
    }
    if (FAILED(bridge_ctx_->QueryInterface(IID_PPV_ARGS(&bridge_ctx4_)))) return false;
    return true;
}

HRESULT VulkanBackend::InitSharedResources(uint32_t w, uint32_t h, uint32_t dxgi_format,
                                              HANDLE* out_memory_handle,
                                              HANDLE* out_fence_handle) {
    if (!out_memory_handle || !out_fence_handle) return E_POINTER;
    if (shared_tex_) {
        NV3D_LOG_ERROR(L"VulkanBackend: InitSharedResources already called — Delete first");
        return E_NOT_VALID_STATE;
    }

    // 1. NT-shared texture (host imports as VkImage).
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = static_cast<DXGI_FORMAT>(dxgi_format);
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Both SHARED and SHARED_NTHANDLE are required per VKS3D's hard-won
    // finding — NVIDIA drivers reject the NTHANDLE-only form.
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = bridge_dev_->CreateTexture2D(&td, nullptr, &shared_tex_);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"VulkanBackend: shared CreateTexture2D failed hr=0x%08X", hr);
        return hr;
    }

    Microsoft::WRL::ComPtr<IDXGIResource1> dxgi1;
    hr = shared_tex_->QueryInterface(IID_PPV_ARGS(&dxgi1));
    if (FAILED(hr)) return hr;
    hr = dxgi1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                     nullptr, &shared_tex_handle_);
    if (FAILED(hr) || !shared_tex_handle_) {
        NV3D_LOG_ERROR(L"VulkanBackend: IDXGIResource1::CreateSharedHandle(tex) failed hr=0x%08X", hr);
        return hr;
    }
    NV3D_LOG_INFO(L"VulkanBackend: shared NT texture handle=%p", shared_tex_handle_);

    // 2. Shared fence (host imports as VkSemaphore timeline).
    hr = bridge_dev5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                     IID_PPV_ARGS(&shared_fence_));
    if (FAILED(hr) || !shared_fence_) {
        NV3D_LOG_ERROR(L"VulkanBackend: CreateFence failed hr=0x%08X", hr);
        return hr;
    }
    hr = shared_fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                              &shared_fence_handle_);
    if (FAILED(hr) || !shared_fence_handle_) {
        NV3D_LOG_ERROR(L"VulkanBackend: fence CreateSharedHandle failed hr=0x%08X", hr);
        return hr;
    }
    NV3D_LOG_INFO(L"VulkanBackend: shared NT fence handle=%p", shared_fence_handle_);

    // 3. Legacy KMT mirror (D3D9-openable). Format matches the NT shared tex.
    D3D11_TEXTURE2D_DESC ld = td;
    ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ld.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // legacy KMT only
    hr = bridge_dev_->CreateTexture2D(&ld, nullptr, &legacy_shared_);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"VulkanBackend: legacy CreateTexture2D failed hr=0x%08X", hr);
        return hr;
    }
    Microsoft::WRL::ComPtr<IDXGIResource> legacy_dxgi;
    hr = legacy_shared_->QueryInterface(IID_PPV_ARGS(&legacy_dxgi));
    if (FAILED(hr)) return hr;
    hr = legacy_dxgi->GetSharedHandle(&legacy_handle_);
    if (FAILED(hr) || !legacy_handle_) return hr;

    // 4. D3D9 view of the legacy mirror.
    hr = presenter_->Device()->CreateTexture(
        w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &d3d9_tex_, &legacy_handle_);
    if (FAILED(hr) || !d3d9_tex_) {
        NV3D_LOG_ERROR(L"VulkanBackend: D3D9 CreateTexture(pSharedHandle) failed hr=0x%08X", hr);
        return hr;
    }
    hr = d3d9_tex_->GetSurfaceLevel(0, &d3d9_sfc_);
    if (FAILED(hr) || !d3d9_sfc_) return hr;

    shared_w_ = w;
    shared_h_ = h;
    *out_memory_handle = shared_tex_handle_;
    *out_fence_handle  = shared_fence_handle_;
    NV3D_LOG_INFO(L"VulkanBackend: InitSharedResources OK (%ux%u fmt=%u)", w, h, dxgi_format);
    return S_OK;
}

HRESULT VulkanBackend::Present(uint64_t sem_value) {
    if (!presenter_ || presenter_->IsDead()) return E_FAIL;
    if (!shared_tex_ || !d3d9_sfc_) return E_NOT_VALID_STATE;

    // The only per-frame state is the timeline-semaphore value. Everything
    // else (shared_tex_, legacy_shared_, sync_query_, etc.) is bridge state
    // owned by us — safe to access from the worker.
    return async_.Submit([this, sem_value]() {
        return PresentSyncBody(sem_value);
    });
}

HRESULT VulkanBackend::PresentSyncBody(uint64_t sem_value) {
    if (!presenter_ || presenter_->IsDead()) return E_FAIL;
    if (!shared_tex_ || !d3d9_sfc_) return E_NOT_VALID_STATE;

    bridge_ctx4_->Wait(shared_fence_.Get(), sem_value);
    bridge_ctx_->CopyResource(legacy_shared_.Get(), shared_tex_.Get());

    if (!sync_query_) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        bridge_dev_->CreateQuery(&qd, &sync_query_);
    }
    if (sync_query_) {
        bridge_ctx_->End(sync_query_.Get());
        bridge_ctx_->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        BOOL done = FALSE;
        while (true) {
            HRESULT qhr = bridge_ctx_->GetData(sync_query_.Get(), &done, sizeof(done), 0);
            if (SUCCEEDED(qhr) && done) break;
            if (FAILED(qhr)) {
                presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "Vulkan sync error");
                return qhr;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                NV3D_LOG_ERROR(L"VulkanBackend: sync_query timed out (>500ms)");
                presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "Vulkan sync timeout");
                return E_FAIL;
            }
            std::this_thread::yield();
        }
    } else {
        bridge_ctx_->Flush();
    }

    return presenter_->Present(d3d9_sfc_.Get(), shared_w_, shared_h_);
}

void VulkanBackend::Delete() {
    // Drain + join the worker before releasing bridge / D3D9 state.
    async_.Stop();

    suppressor_.Uninstall();
    const bool dead = presenter_ && presenter_->IsDead();
    if (dead) {
        (void)d3d9_sfc_.Detach();
        (void)d3d9_tex_.Detach();
    } else {
        d3d9_sfc_.Reset();
        d3d9_tex_.Reset();
    }
    legacy_handle_ = nullptr;          // KMT — never close
    legacy_shared_.Reset();
    if (shared_fence_handle_) { CloseHandle(shared_fence_handle_); shared_fence_handle_ = nullptr; }
    shared_fence_.Reset();
    if (shared_tex_handle_)   { CloseHandle(shared_tex_handle_);   shared_tex_handle_   = nullptr; }
    shared_tex_.Reset();
    sync_query_.Reset();

    bridge_ctx4_.Reset();
    bridge_ctx_.Reset();
    bridge_dev5_.Reset();
    bridge_dev_.Reset();

    presenter_.reset();
    window_.reset();
    delete this;
}

extern "C" HRESULT CreateInterfaceVulkan(NV3DVkInstance instance,
                                          NV3DVkPhysicalDevice phys,
                                          NV3DVkDevice device,
                                          uint32_t queue_family_index,
                                          const InitParams* params,
                                          InterfaceVulkan** out) {
    if (!instance || !phys || !device || !params || !out) return E_POINTER;
    auto* impl = new VulkanBackend();
    HRESULT hr = impl->Init(instance, phys, device, queue_family_index, *params);
    if (FAILED(hr)) {
        impl->Delete();
        return hr;
    }
    *out = impl;
    return S_OK;
}

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_VULKAN
