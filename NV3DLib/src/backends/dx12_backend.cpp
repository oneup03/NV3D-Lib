#include "backends/dx12_backend.h"

#ifndef NV3DLIB_DISABLE_DX12

#include <chrono>
#include <thread>

#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include "log.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

namespace NV3D {

namespace {

bool IsRgbaOrBgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM      ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM      ||
           f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

bool LuidsEqual(LUID a, LUID b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

}  // anonymous

HRESULT DX12Backend::Init(ID3D12Device* device, ID3D12CommandQueue* queue,
                            const InitParams& params) {
    if (!device || !queue) return E_POINTER;
    params_      = params;
    host_device_ = device;
    host_queue_  = queue;

    if (!CreateBridgeDeviceOn12()) return E_FAIL;
    if (!CreateSwizzleShaders()) return E_FAIL;

    window_ = std::make_unique<PresentWindow>();
    PresentWindowConfig wcfg{};
    wcfg.target_monitor = params.target_monitor;
    wcfg.host_hwnd      = params.host_hwnd;
    wcfg.on_top         = params.on_top;
    wcfg.title          = L"NV3DLib (DX12)";
    if (!window_->Init(wcfg)) {
        NV3D_LOG_ERROR(L"DX12Backend: PresentWindow::Init failed");
        return E_FAIL;
    }
    presenter_ = std::make_unique<D3D9Presenter>();
    if (!presenter_->Init(window_.get(), params)) {
        NV3D_LOG_ERROR(L"DX12Backend: D3D9Presenter::Init failed");
        return E_FAIL;
    }

    if (params_.enable_suppressor) suppressor_.Install();
    return S_OK;
}

bool DX12Backend::CreateBridgeDeviceOn12() {
    // Create our own DIRECT command queue on the host's DX12 device. We pass
    // it to D3D11On12CreateDevice; all DX11 work (the swizzle shader pass) is
    // serialized through this queue. Using a queue we own keeps us out of
    // the host's submission order.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0;
    HRESULT hr = host_device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&our_queue_));
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX12Backend: CreateCommandQueue(DIRECT) failed hr=0x%08X — "
                        L"host DX12 device must support DIRECT queues", hr);
        return false;
    }

    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    IUnknown* queues[] = { our_queue_.Get() };
    hr = D3D11On12CreateDevice(
        host_device_.Get(),
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        fls, _countof(fls),
        queues, 1,
        0,                          // single GPU node
        &bridge_dev_, &bridge_ctx_, nullptr);
    if (FAILED(hr) || !bridge_dev_) {
        NV3D_LOG_ERROR(L"DX12Backend: D3D11On12CreateDevice failed hr=0x%08X — "
                        L"d3d11on12.dll may be missing (Win10+) or host device incompatible", hr);
        return false;
    }
    hr = bridge_dev_->QueryInterface(IID_PPV_ARGS(&on12_dev_));
    if (FAILED(hr) || !on12_dev_) {
        NV3D_LOG_ERROR(L"DX12Backend: ID3D11On12Device QI failed hr=0x%08X", hr);
        return false;
    }
    hr = bridge_dev_->QueryInterface(IID_PPV_ARGS(&bridge_dev5_));
    if (FAILED(hr) || !bridge_dev5_) {
        NV3D_LOG_ERROR(L"DX12Backend: ID3D11Device5 QI failed hr=0x%08X "
                        L"(Windows 10 1809+ required for shared fences)", hr);
        return false;
    }
    hr = bridge_ctx_->QueryInterface(IID_PPV_ARGS(&bridge_ctx4_));
    if (FAILED(hr) || !bridge_ctx4_) {
        NV3D_LOG_ERROR(L"DX12Backend: ID3D11DeviceContext4 QI failed hr=0x%08X", hr);
        return false;
    }
    NV3D_LOG_INFO(L"DX12Backend: D3D11On12 bridge created (DIRECT queue, BGRA support)");
    return true;
}

bool DX12Backend::CreateSwizzleShaders() {
    // SV_VertexID-driven fullscreen triangle + PS that swizzles RGBA → BGRA.
    static const char kVS[] =
        "struct V { float4 p:SV_POSITION; float2 uv:TEXCOORD0; };\n"
        "V main(uint id:SV_VertexID) {\n"
        "  V o;\n"
        "  o.uv = float2((id<<1)&2, id&2);\n"
        "  o.p = float4(o.uv*float2(2,-2) + float2(-1,1), 0, 1);\n"
        "  return o;\n"
        "}\n";
    // Pure copy. Format conversion (RGBA8 source → BGRA8 RTV) is handled
    // automatically by the driver — shader components are color channels
    // (R, G, B, A) regardless of the underlying texture's byte order, so
    // no swizzle here.
    static const char kPS[] =
        "Texture2D<float4> g_tex:register(t0);\n"
        "SamplerState g_smp:register(s0);\n"
        "float4 main(float4 p:SV_POSITION, float2 uv:TEXCOORD0):SV_TARGET {\n"
        "  return g_tex.SampleLevel(g_smp, uv, 0);\n"
        "}\n";

    Microsoft::WRL::ComPtr<ID3DBlob> vs_bc, ps_bc, err;
    HRESULT hr = D3DCompile(kVS, sizeof(kVS) - 1, "vs", nullptr, nullptr,
                              "main", "vs_5_0", 0, 0, &vs_bc, &err);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX12Backend: VS compile failed hr=0x%08X %hs",
                        hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "(no msg)");
        return false;
    }
    hr = D3DCompile(kPS, sizeof(kPS) - 1, "ps", nullptr, nullptr,
                     "main", "ps_5_0", 0, 0, &ps_bc, &err);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX12Backend: PS compile failed hr=0x%08X %hs",
                        hr, err ? static_cast<const char*>(err->GetBufferPointer()) : "(no msg)");
        return false;
    }
    hr = bridge_dev_->CreateVertexShader(vs_bc->GetBufferPointer(), vs_bc->GetBufferSize(),
                                           nullptr, &swizzle_vs_);
    if (FAILED(hr)) return false;
    hr = bridge_dev_->CreatePixelShader(ps_bc->GetBufferPointer(), ps_bc->GetBufferSize(),
                                          nullptr, &swizzle_ps_);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU       = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD         = 0;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = bridge_dev_->CreateSamplerState(&sd, &swizzle_sampler_);
    if (FAILED(hr)) return false;
    return true;
}

HRESULT DX12Backend::SetInputTexture(ID3D12Resource* sbs_tex,
                                       ID3D12Fence* sync_fence,
                                       uint64_t fence_value) {
    if (!sbs_tex) return E_POINTER;
    input_tex_         = sbs_tex;
    input_fence_       = sync_fence;
    input_fence_value_ = fence_value;
    D3D12_RESOURCE_DESC d = sbs_tex->GetDesc();
    input_w_   = static_cast<uint32_t>(d.Width);
    input_h_   = d.Height;
    input_fmt_ = d.Format;
    if (!IsRgbaOrBgra(d.Format)) {
        NV3D_LOG_ERROR(L"DX12Backend: input format %u unsupported (RGBA8 or BGRA8 required)",
                        d.Format);
        return E_INVALIDARG;
    }
    return S_OK;
}

HRESULT DX12Backend::EnsureResourceImport(ID3D12Resource* sbs) {
    if (!sbs || !on12_dev_ || !presenter_) return E_NOT_VALID_STATE;

    D3D12_RESOURCE_DESC d = sbs->GetDesc();
    if (cached_input_ptr_ == static_cast<void*>(sbs) &&
        legacy_w_ == d.Width && legacy_h_ == d.Height && d3d9_sfc_) {
        return S_OK;
    }

    // Tear down prior state. legacy_handle_ is a KMT handle owned by the
    // texture (NOT an NT handle) — do NOT CloseHandle it.
    output_rtv_.Reset();
    input_srv_.Reset();
    d3d9_sfc_.Reset();
    d3d9_tex_.Reset();
    legacy_shared_.Reset();
    legacy_handle_ = nullptr;
    wrapped_.Reset();

    NV3D_LOG_INFO(L"DX12Backend: wrapping %ux%u fmt=%u dim=%d flags=0x%x layout=%d",
                    static_cast<uint32_t>(d.Width), d.Height,
                    d.Format, d.Dimension, d.Flags, d.Layout);

    // 1. Wrap the host's DX12 resource as a DX11 resource via D3D11On12.
    //    No NT handle, no LUID matching. The host promises the resource is
    //    in COMMON state when we Acquire (it transitions back to COMMON
    //    before signaling its fence).
    D3D11_RESOURCE_FLAGS r11{};
    r11.BindFlags         = D3D11_BIND_SHADER_RESOURCE;
    r11.MiscFlags         = 0;
    r11.CPUAccessFlags    = 0;
    r11.StructureByteStride = 0;
    HRESULT hr = on12_dev_->CreateWrappedResource(
        sbs, &r11,
        D3D12_RESOURCE_STATE_COMMON,    // InState: state on Acquire
        D3D12_RESOURCE_STATE_COMMON,    // OutState: state on Release
        IID_PPV_ARGS(&wrapped_));
    if (FAILED(hr) || !wrapped_) {
        NV3D_LOG_ERROR(L"DX12Backend: CreateWrappedResource failed hr=0x%08X", hr);
        return hr;
    }
    NV3D_LOG_INFO(L"DX12Backend: D3D11On12 wrapped resource created OK");

    // 3. Internal DX11 MISC_SHARED (legacy HANDLE) BGRA mirror — needs
    //    RENDER_TARGET binding so the swizzle shader can write into it.
    //    D3D9Ex opens the legacy KMT handle from this texture.
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(d.Width);
    td.Height           = d.Height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
    hr = bridge_dev_->CreateTexture2D(&td, nullptr, &legacy_shared_);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX12Backend: legacy-shared CreateTexture2D failed hr=0x%08X", hr);
        return hr;
    }

    // 4. Get the legacy HANDLE off the legacy_shared texture.
    Microsoft::WRL::ComPtr<IDXGIResource> dxgi;
    hr = legacy_shared_->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (FAILED(hr)) return hr;
    hr = dxgi->GetSharedHandle(&legacy_handle_);
    if (FAILED(hr) || !legacy_handle_) {
        NV3D_LOG_ERROR(L"DX12Backend: legacy GetSharedHandle failed hr=0x%08X", hr);
        return hr;
    }

    // 5. Open as D3D9 texture.
    hr = presenter_->Device()->CreateTexture(
        td.Width, td.Height, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &d3d9_tex_, &legacy_handle_);
    if (FAILED(hr) || !d3d9_tex_) {
        NV3D_LOG_ERROR(L"DX12Backend: D3D9 CreateTexture(pSharedHandle) failed hr=0x%08X", hr);
        return hr;
    }
    hr = d3d9_tex_->GetSurfaceLevel(0, &d3d9_sfc_);
    if (FAILED(hr) || !d3d9_sfc_) return hr;

    // 6. SRV on the D3D11On12 wrapped resource (RGBA source) and RTV on our
    //    BGRA mirror.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                    = d.Format;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MostDetailedMip = 0;
    srvd.Texture2D.MipLevels       = 1;
    hr = bridge_dev_->CreateShaderResourceView(wrapped_.Get(), &srvd, &input_srv_);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX12Backend: CreateShaderResourceView(wrapped) failed hr=0x%08X", hr);
        return hr;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format                 = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtvd.ViewDimension          = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvd.Texture2D.MipSlice     = 0;
    hr = bridge_dev_->CreateRenderTargetView(legacy_shared_.Get(), &rtvd, &output_rtv_);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX12Backend: CreateRenderTargetView(mirror) failed hr=0x%08X", hr);
        return hr;
    }

    cached_input_ptr_ = sbs;
    legacy_w_ = td.Width;
    legacy_h_ = td.Height;
    NV3D_LOG_INFO(L"DX12Backend: imported %ux%u DX12 → DX11 alias → legacy shared → D3D9",
                    td.Width, td.Height);
    return S_OK;
}

HRESULT DX12Backend::EnsureFenceImport(ID3D12Fence* fence) {
    if (!fence) return S_OK;  // sync_fence is optional
    if (cached_fence_ptr_ == static_cast<void*>(fence) && shared_fence_) return S_OK;

    shared_fence_.Reset();
    if (nt_fence_handle_) { CloseHandle(nt_fence_handle_); nt_fence_handle_ = nullptr; }

    HRESULT hr = host_device_->CreateSharedHandle(fence, nullptr, GENERIC_ALL,
                                                     nullptr, &nt_fence_handle_);
    if (FAILED(hr) || !nt_fence_handle_) {
        NV3D_LOG_ERROR(L"DX12Backend: CreateSharedHandle(fence) failed hr=0x%08X — "
                        L"fence must be created with D3D12_FENCE_FLAG_SHARED", hr);
        return hr;
    }
    hr = bridge_dev5_->OpenSharedFence(nt_fence_handle_, IID_PPV_ARGS(&shared_fence_));
    if (FAILED(hr) || !shared_fence_) {
        NV3D_LOG_ERROR(L"DX12Backend: OpenSharedFence failed hr=0x%08X", hr);
        return hr;
    }
    cached_fence_ptr_ = fence;
    return S_OK;
}

HRESULT DX12Backend::Present() {
    if (!presenter_ || presenter_->IsDead()) return E_FAIL;
    if (!input_tex_) return E_NOT_VALID_STATE;

    HRESULT hr = EnsureResourceImport(input_tex_.Get());
    if (FAILED(hr)) return hr;
    hr = EnsureFenceImport(input_fence_.Get());
    if (FAILED(hr)) return hr;

    // GPU-side wait on host's fence (no CPU stall) so our shader pass sees
    // host's writes.
    if (shared_fence_) {
        bridge_ctx4_->Wait(shared_fence_.Get(), input_fence_value_);
    }

    // Acquire the wrapped resource: D3D11On12 transitions it from OutState
    // (COMMON) to whatever DX11 needs for our binding (SHADER_RESOURCE).
    ID3D11Resource* wrapped_list[] = { wrapped_.Get() };
    on12_dev_->AcquireWrappedResources(wrapped_list, 1);

    // Dispatch the RGBA→BGRA swizzle pass: fullscreen triangle reads
    // input_srv_ (D3D11On12 wrapped DX12 resource), writes output_rtv_
    // (our BGRA MISC_SHARED mirror that D3D9 opens via the KMT handle).
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = static_cast<float>(input_w_);
    vp.Height   = static_cast<float>(input_h_);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[] = { output_rtv_.Get() };
    bridge_ctx_->OMSetRenderTargets(1, rtvs, nullptr);
    bridge_ctx_->RSSetViewports(1, &vp);
    bridge_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    bridge_ctx_->IASetInputLayout(nullptr);
    ID3D11Buffer* null_vb = nullptr; UINT zero = 0;
    bridge_ctx_->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
    bridge_ctx_->VSSetShader(swizzle_vs_.Get(), nullptr, 0);
    bridge_ctx_->PSSetShader(swizzle_ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { input_srv_.Get() };
    bridge_ctx_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[] = { swizzle_sampler_.Get() };
    bridge_ctx_->PSSetSamplers(0, 1, samps);
    bridge_ctx_->Draw(3, 0);
    // Unbind so the mirror isn't held as a render target when D3D9 reads it.
    ID3D11RenderTargetView* null_rtv = nullptr;
    bridge_ctx_->OMSetRenderTargets(1, &null_rtv, nullptr);
    ID3D11ShaderResourceView* null_srv = nullptr;
    bridge_ctx_->PSSetShaderResources(0, 1, &null_srv);

    // Release the wrapped resource back to DX12 (InState COMMON → OutState COMMON).
    on12_dev_->ReleaseWrappedResources(wrapped_list, 1);

    // EVENT query on DX11 to sync into D3D9.
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
                NV3D_LOG_ERROR(L"DX12Backend: GetData(EVENT) failed hr=0x%08X", qhr);
                presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "DX12 sync error");
                return qhr;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                NV3D_LOG_ERROR(L"DX12Backend: sync_query timed out (>500ms) — GPU wedged?");
                presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "DX12 sync timeout");
                return E_FAIL;
            }
            std::this_thread::yield();
        }
    } else {
        bridge_ctx_->Flush();
    }

    return presenter_->Present(d3d9_sfc_.Get(), input_w_, input_h_);
}

void DX12Backend::Delete() {
    suppressor_.Uninstall();

    const bool dead = presenter_ && presenter_->IsDead();
    if (dead) {
        (void)d3d9_sfc_.Detach();
        (void)d3d9_tex_.Detach();
    } else {
        d3d9_sfc_.Reset();
        d3d9_tex_.Reset();
    }
    output_rtv_.Reset();
    input_srv_.Reset();
    swizzle_sampler_.Reset();
    swizzle_ps_.Reset();
    swizzle_vs_.Reset();
    // legacy_handle_ is a KMT handle — no CloseHandle.
    legacy_handle_ = nullptr;
    legacy_shared_.Reset();
    wrapped_.Reset();
    shared_fence_.Reset();
    if (nt_fence_handle_) { CloseHandle(nt_fence_handle_); nt_fence_handle_ = nullptr; }
    sync_query_.Reset();

    // DX11On12 bridge — independent of D3D9, always safe to Release.
    bridge_ctx4_.Reset();
    bridge_ctx_.Reset();
    bridge_dev5_.Reset();
    on12_dev_.Reset();
    bridge_dev_.Reset();
    our_queue_.Reset();

    presenter_.reset();
    window_.reset();
    delete this;
}

extern "C" HRESULT CreateInterfaceDX12(ID3D12Device* device,
                                        ID3D12CommandQueue* queue,
                                        const InitParams* params,
                                        InterfaceDX12** out) {
    if (!device || !queue || !params || !out) return E_POINTER;
    auto* impl = new DX12Backend();
    HRESULT hr = impl->Init(device, queue, *params);
    if (FAILED(hr)) {
        impl->Delete();
        return hr;
    }
    *out = impl;
    return S_OK;
}

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_DX12
