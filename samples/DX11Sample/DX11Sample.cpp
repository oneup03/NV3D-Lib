// DX11Sample: minimal NV3DLib DX11 consumer.
//
// Renders a side-by-side test pattern into a DX11 MISC_SHARED + BGRA texture
// each frame, then hands it to NV3DLib. The new shared-texture pipeline
// imports the texture directly into D3D9Ex via the legacy DXGI shared handle
// — no CPU readback.

#include <Windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "NV3D.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth  = 2560;
constexpr UINT kHeight = 720;

void FillTestPattern(uint8_t* px, UINT pitch, UINT frame) {
    const UINT per_eye_w = kWidth / 2;
    const UINT cx = (kWidth / 2) / 2;
    const UINT cy = kHeight / 2;
    const int disparity = static_cast<int>(20 + 15 * sinf(frame * 0.04f));
    const int quad_half = 40;

    for (UINT y = 0; y < kHeight; ++y) {
        uint8_t* row = px + y * pitch;
        for (UINT x = 0; x < kWidth; ++x) {
            uint8_t* p = row + x * 4;  // BGRA
            const bool right_eye = (x >= per_eye_w);
            const UINT lx = right_eye ? (x - per_eye_w) : x;
            if (!right_eye) {
                p[0] = 0; p[1] = 0; p[2] = static_cast<uint8_t>(lx * 255 / per_eye_w); p[3] = 0xFF;
            } else {
                p[0] = 0; p[1] = static_cast<uint8_t>(lx * 255 / per_eye_w); p[2] = 0; p[3] = 0xFF;
            }
            int qx = static_cast<int>(lx) - static_cast<int>(cx);
            int qy = static_cast<int>(y)  - static_cast<int>(cy);
            qx += right_eye ? -disparity : disparity;
            if (qx >= -quad_half && qx <= quad_half &&
                qy >= -quad_half && qy <= quad_half) {
                p[0] = p[1] = p[2] = 0xFF;
            }
        }
    }
}

FILE* g_log_file = nullptr;

void LogSink(NV3D::LogLevel level, const wchar_t* msg, void*) {
    const wchar_t* lvl = L"";
    switch (level) {
        case NV3D::LogLevel::Debug:   lvl = L"D"; break;
        case NV3D::LogLevel::Info:    lvl = L"I"; break;
        case NV3D::LogLevel::Warning: lvl = L"W"; break;
        case NV3D::LogLevel::Error:   lvl = L"E"; break;
    }
    wprintf(L"[NV3D][%s] %s\n", lvl, msg);
    fflush(stdout);
    // Also write to dx11sample.log next to the exe so FSE-covered consoles
    // can be debugged after the fact.
    if (g_log_file) {
        fwprintf(g_log_file, L"[NV3D][%s] %s\n", lvl, msg);
        fflush(g_log_file);
    }
}

}  // anonymous

int wmain(int, wchar_t**) {
    _wfopen_s(&g_log_file, L"dx11sample.log", L"w, ccs=UTF-16LE");
    NV3D::SetLogSink(LogSink, nullptr);

    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl{};
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     fls, _countof(fls), D3D11_SDK_VERSION,
                                     &dev, &fl, &ctx);
    if (FAILED(hr)) { wprintf(L"D3D11CreateDevice hr=0x%08X\n", hr); return 1; }

    // SHARED MISC + BGRA texture so NV3DLib can import it directly into D3D9.
    // We use D3D11_USAGE_DEFAULT + UpdateSubresource (Map doesn't work on
    // SHARED textures without staging).
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = kWidth;
    td.Height           = kHeight;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> sbs;
    hr = dev->CreateTexture2D(&td, nullptr, &sbs);
    if (FAILED(hr)) { wprintf(L"CreateTexture2D(SHARED+BGRA) hr=0x%08X\n", hr); return 1; }

    std::vector<uint8_t> staging(static_cast<size_t>(kWidth) * kHeight * 4u);

    NV3D::InitParams p{};
    p.enable_lightboost = true;
    p.enable_suppressor = true;
    p.on_top            = true;
    NV3D::InterfaceDX11* nv3d = nullptr;
    hr = NV3D::CreateInterfaceDX11(dev.Get(), &p, &nv3d);
    if (FAILED(hr) || !nv3d) { wprintf(L"CreateInterfaceDX11 hr=0x%08X\n", hr); return 1; }

    wprintf(L"DX11Sample: running. Ctrl+C to exit.\n");
    fflush(stdout);

    auto next = std::chrono::steady_clock::now();
    for (UINT frame = 0; ; ++frame) {
        next += std::chrono::milliseconds(16);

        FillTestPattern(staging.data(), kWidth * 4u, frame);
        ctx->UpdateSubresource(sbs.Get(), 0, nullptr,
                                staging.data(), kWidth * 4u, 0);

        nv3d->SetInputTexture(sbs.Get());
        HRESULT phr = nv3d->Present();
        if (FAILED(phr)) { wprintf(L"Present hr=0x%08X\n", phr); break; }

        std::this_thread::sleep_until(next);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { nv3d->Delete(); return 0; }
        }
    }

    nv3d->Delete();
    return 0;
}
