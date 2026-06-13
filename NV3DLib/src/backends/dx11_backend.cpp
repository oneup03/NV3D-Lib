#include "backends/dx11_backend.h"

#ifndef NV3DLIB_DISABLE_DX11

#include <chrono>
#include <thread>

#include <dxgi.h>

#include "log.h"

namespace NV3D {

namespace {

bool IsBgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

}  // anonymous

HRESULT DX11Backend::Init(ID3D11Device* device, const InitParams& params) {
    if (!device) return E_POINTER;
    params_ = params;
    host_device_ = device;
    host_device_->GetImmediateContext(&host_ctx_);

    window_ = std::make_unique<PresentWindow>();
    PresentWindowConfig wcfg{};
    wcfg.target_monitor = params.target_monitor;
    wcfg.host_hwnd      = params.host_hwnd;
    wcfg.on_top         = params.on_top;
    wcfg.title          = L"NV3DLib (DX11)";
    if (!window_->Init(wcfg)) {
        NV3D_LOG_ERROR(L"DX11Backend: PresentWindow::Init failed");
        return E_FAIL;
    }

    presenter_ = std::make_unique<D3D9Presenter>();
    if (!presenter_->Init(window_.get(), params)) {
        NV3D_LOG_ERROR(L"DX11Backend: D3D9Presenter::Init failed");
        return E_FAIL;
    }

    if (params_.enable_suppressor) suppressor_.Install();

    // Split-async present worker. Only the final D3D9 Present call is
    // moved off-thread (see Present() below for why).
    async_.Start();
    return S_OK;
}

HRESULT DX11Backend::SetInputTexture(ID3D11Texture2D* sbs_tex) {
    if (!sbs_tex) return E_POINTER;
    input_tex_ = sbs_tex;
    D3D11_TEXTURE2D_DESC td{};
    sbs_tex->GetDesc(&td);
    input_w_   = td.Width;
    input_h_   = td.Height;
    input_fmt_ = td.Format;

    if (!IsBgra(td.Format)) {
        // For now only BGRA family is supported. RGBA → BGRA needs a shader
        // mirror; the auto-mirror below assumes the host's format matches D3D9
        // X8R8G8B8 byte order.
        NV3D_LOG_ERROR(L"DX11Backend: input texture format %u unsupported (BGRA8 family required)",
                        td.Format);
        return E_INVALIDARG;
    }
    return S_OK;
}

bool DX11Backend::EnsureMirror(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (mirror_tex_ && mirror_w_ == w && mirror_h_ == h && mirror_fmt_ == fmt)
        return true;
    mirror_tex_.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width            = w;
    d.Height           = h;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = fmt;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_DEFAULT;
    d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = host_device_->CreateTexture2D(&d, nullptr, &mirror_tex_);
    if (FAILED(hr)) {
        NV3D_LOG_ERROR(L"DX11Backend: mirror CreateTexture2D failed hr=0x%08X", hr);
        return false;
    }
    mirror_w_ = w; mirror_h_ = h; mirror_fmt_ = fmt;
    NV3D_LOG_INFO(L"DX11Backend: created internal mirror %ux%u fmt=%u", w, h, fmt);
    return true;
}

bool DX11Backend::EnsureSharedImport(ID3D11Texture2D* src) {
    if (!src || !presenter_ || !presenter_->Device()) return false;

    D3D11_TEXTURE2D_DESC td{};
    src->GetDesc(&td);
    if (shared_d3d9_sfc_ && shared_cache_ptr_ == src &&
        shared_cache_w_ == td.Width && shared_cache_h_ == td.Height &&
        shared_cache_fmt_ == td.Format) {
        return true;
    }

    shared_d3d9_sfc_.Reset();
    shared_d3d9_tex_.Reset();

    if ((td.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0) {
        NV3D_LOG_ERROR(L"DX11Backend: source lacks MISC_SHARED — internal bug");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIResource> dxgi;
    HRESULT hr = src->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (FAILED(hr) || !dxgi) {
        NV3D_LOG_ERROR(L"DX11Backend: QueryInterface(IDXGIResource) failed hr=0x%08X", hr);
        return false;
    }
    HANDLE h = nullptr;
    hr = dxgi->GetSharedHandle(&h);
    if (FAILED(hr) || !h) {
        NV3D_LOG_ERROR(L"DX11Backend: GetSharedHandle failed hr=0x%08X handle=%p", hr, h);
        return false;
    }

    hr = presenter_->Device()->CreateTexture(
        td.Width, td.Height, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &shared_d3d9_tex_, &h);
    if (FAILED(hr) || !shared_d3d9_tex_) {
        NV3D_LOG_ERROR(L"DX11Backend: D3D9 CreateTexture(pSharedHandle) failed hr=0x%08X "
                        L"dims=%ux%u (adapter mismatch?)", hr, td.Width, td.Height);
        return false;
    }
    hr = shared_d3d9_tex_->GetSurfaceLevel(0, &shared_d3d9_sfc_);
    if (FAILED(hr) || !shared_d3d9_sfc_) {
        NV3D_LOG_ERROR(L"DX11Backend: GetSurfaceLevel(0) failed hr=0x%08X", hr);
        shared_d3d9_tex_.Reset();
        return false;
    }

    shared_cache_ptr_   = src;
    shared_cache_handle_= h;
    shared_cache_w_     = td.Width;
    shared_cache_h_     = td.Height;
    shared_cache_fmt_   = td.Format;
    NV3D_LOG_INFO(L"DX11Backend: shared D3D9 view imported %ux%u handle=%p",
                    td.Width, td.Height, h);
    return true;
}

bool DX11Backend::EnsureSyncQuery() {
    if (sync_query_) return true;
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    HRESULT hr = host_device_->CreateQuery(&qd, &sync_query_);
    if (FAILED(hr) || !sync_query_) {
        NV3D_LOG_WARN(L"DX11Backend: CreateQuery(EVENT) failed hr=0x%08X — using Flush only", hr);
        return false;
    }
    return true;
}

HRESULT DX11Backend::WaitForDx11Writes() {
    if (!EnsureSyncQuery()) {
        host_ctx_->Flush();
        return S_OK;
    }
    host_ctx_->End(sync_query_.Get());
    host_ctx_->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    BOOL done = FALSE;
    while (true) {
        HRESULT hr = host_ctx_->GetData(sync_query_.Get(), &done, sizeof(done), 0);
        if (SUCCEEDED(hr) && done) return S_OK;
        if (FAILED(hr)) {
            NV3D_LOG_ERROR(L"DX11Backend: GetData(EVENT) failed hr=0x%08X", hr);
            // DX11 failing here doesn't necessarily mean D3D9 is dead, but it
            // strongly suggests GPU trouble. Mark D3D9 dead so subsequent
            // Present calls fast-fail instead of feeding into a wedge.
            if (presenter_) presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "DX11 sync error");
            return hr;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            NV3D_LOG_ERROR(L"DX11Backend: sync_query timed out (>500ms) — GPU wedged?");
            if (presenter_) presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "sync_query timeout");
            return E_FAIL;
        }
        std::this_thread::yield();
    }
}

HRESULT DX11Backend::Present() {
    if (!presenter_ || presenter_->IsDead()) return E_FAIL;
    if (!input_tex_) return E_NOT_VALID_STATE;

    D3D11_TEXTURE2D_DESC td{};
    input_tex_->GetDesc(&td);

    // Decide source: host directly if it's already shared, else mirror.
    ID3D11Texture2D* src = input_tex_.Get();
    if ((td.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0) {
        if (!EnsureMirror(td.Width, td.Height, td.Format)) return E_FAIL;
        host_ctx_->CopyResource(mirror_tex_.Get(), input_tex_.Get());
        src = mirror_tex_.Get();
    }

    if (!EnsureSharedImport(src)) return E_FAIL;

    HRESULT hr = WaitForDx11Writes();
    if (FAILED(hr)) return hr;

    // SPLIT ASYNC. Everything above this point ran on the calling thread:
    //   - host_ctx_->CopyResource (if mirror path) — uses HOST'S immediate
    //     context, can't move to a worker without external synchronization
    //     because the host may also be using their context.
    //   - WaitForDx11Writes (host_ctx_ End/Flush/GetData EVENT spin) — same
    //     constraint; uses the host's immediate context.
    // From here down only the D3D9Presenter is involved — that's our own
    // device, created with D3DCREATE_MULTITHREADED, so the worker can take
    // over the StretchRect + vsync'd PresentEx without thread-safety
    // concerns. The vsync wait is the biggest single cost; off-loading
    // just it captures most of the latency win.
    Microsoft::WRL::ComPtr<IDirect3DSurface9> snap_sfc = shared_d3d9_sfc_;
    const uint32_t snap_w = input_w_;
    const uint32_t snap_h = input_h_;
    return async_.Submit([this, snap_sfc, snap_w, snap_h]() {
        return presenter_->Present(snap_sfc.Get(), snap_w, snap_h);
    });
}

void DX11Backend::Delete() {
    // Teardown order matches VRto3D:
    //   0. Drain + join the async worker. Required FIRST so the worker
    //      isn't mid-D3D9-Present when we tear D3D9 down below.
    //   1. Uninstall the in-process NV3D suppressor (unhook nvd3dumx.dll &
    //      user32.dll) — done BEFORE D3D9 release so the driver isn't being
    //      held while we're unhooking it.
    //   2. Release D3D9 child resources. If the underlying D3D9 device is
    //      dead (GPU TDR'd) Detach() instead of Reset() — Release() on a
    //      wedged device blocks in the kernel-mode driver.
    //   3. Tear down the D3D9 device + NvAPI (D3D9Presenter::Shutdown).
    //   4. Destroy the present window LAST.
    async_.Stop();
    suppressor_.Uninstall();

    const bool dead = presenter_ && presenter_->IsDead();
    if (dead) {
        (void)shared_d3d9_sfc_.Detach();
        (void)shared_d3d9_tex_.Detach();
    } else {
        shared_d3d9_sfc_.Reset();
        shared_d3d9_tex_.Reset();
    }
    // DX11 resources are always safe — the host's DX11 device is independent
    // of D3D9 device state.
    sync_query_.Reset();
    mirror_tex_.Reset();
    input_tex_.Reset();

    presenter_.reset();   // → D3D9Presenter::Shutdown (dead-aware)
    window_.reset();      // → PresentWindow destructor + thread join
    delete this;
}

extern "C" HRESULT CreateInterfaceDX11(ID3D11Device* device,
                                        const InitParams* params,
                                        InterfaceDX11** out) {
    if (!device || !params || !out) return E_POINTER;
    auto* impl = new DX11Backend();
    HRESULT hr = impl->Init(device, *params);
    if (FAILED(hr)) {
        impl->Delete();
        return hr;
    }
    *out = impl;
    return S_OK;
}

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_DX11
