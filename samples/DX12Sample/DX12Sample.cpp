// DX12Sample: minimal NV3DLib DX12 consumer.
//
// Creates a DX12 device + COPY queue, allocates an SbS texture, uploads a
// new test pattern each frame, fences, hands to NV3DLib.

#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "NV3D.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kWidth  = 2560;
constexpr UINT kHeight = 720;

// Writes RGBA byte order — the DX12 resource is DXGI_FORMAT_R8G8B8A8_UNORM.
// (BGRA8 cross-API sharing into DX11 OpenSharedResource1 returns E_INVALIDARG
// on NVIDIA; RGBA8 works. The lib's backend runs a swizzle shader to convert
// to BGRA for the D3D9 hop.)
void FillTestPattern(uint8_t* px, UINT pitch, UINT frame) {
    const UINT per_eye = kWidth / 2;
    const UINT cx = per_eye / 2;
    const UINT cy = kHeight / 2;
    const int disparity = static_cast<int>(20 + 15 * sinf(frame * 0.04f));
    const int quad_half = 40;
    for (UINT y = 0; y < kHeight; ++y) {
        uint8_t* row = px + y * pitch;
        for (UINT x = 0; x < kWidth; ++x) {
            uint8_t* p = row + x * 4;   // RGBA byte order
            const bool right_eye = (x >= per_eye);
            const UINT lx = right_eye ? (x - per_eye) : x;
            if (!right_eye) {
                // left eye: red gradient
                p[0] = static_cast<uint8_t>(lx * 255 / per_eye); p[1] = 0; p[2] = 0; p[3] = 0xFF;
            } else {
                // right eye: green gradient
                p[0] = 0; p[1] = static_cast<uint8_t>(lx * 255 / per_eye); p[2] = 0; p[3] = 0xFF;
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
    if (g_log_file) {
        fwprintf(g_log_file, L"[NV3D][%s] %s\n", lvl, msg);
        fflush(g_log_file);
    }
}

constexpr UINT AlignTo(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

}  // anonymous

int wmain(int, wchar_t**) {
    _wfopen_s(&g_log_file, L"dx12sample.log", L"w, ccs=UTF-16LE");
    NV3D::SetLogSink(LogSink, nullptr);

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { wprintf(L"CreateDXGIFactory1 hr=0x%08X\n", hr); return 1; }

    ComPtr<ID3D12Device> dev;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { wprintf(L"D3D12CreateDevice hr=0x%08X\n", hr); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ComPtr<ID3D12CommandQueue> queue;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ComPtr<ID3D12CommandAllocator> alloc;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();

    // SbS texture (DEFAULT heap, COPY_SOURCE state for handoff).
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width              = kWidth;
    td.Height             = kHeight;
    td.DepthOrArraySize   = 1;
    td.MipLevels          = 1;
    // Classic MS-sample combo for DX12 ↔ DX11 NT-shared textures:
    //   RGBA8 + RESOURCE_FLAG_NONE + COMMON state + explicit barriers around
    //   any non-COMMON usage. (BGRA8 cross-API fails E_INVALIDARG on the
    //   legacy 3D Vision driver; ALLOW_SIMULTANEOUS_ACCESS also failed.)
    // The lib's backend handles RGBA → BGRA via a tiny swizzle shader pass.
    td.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags              = D3D12_RESOURCE_FLAG_NONE;
    // Shared resources MUST be created in the COMMON state — cross-API
    // openers (DX11 OpenSharedResource1) will reject anything else with
    // E_INVALIDARG. We transition to COPY_DEST inside the command list when
    // we want to do the upload, then back to COMMON before signaling.
    ComPtr<ID3D12Resource> sbs;
    hr = dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_SHARED,
                                        &td, D3D12_RESOURCE_STATE_COMMON,
                                        nullptr, IID_PPV_ARGS(&sbs));
    if (FAILED(hr)) { wprintf(L"CreateCommittedResource(sbs) hr=0x%08X\n", hr); return 1; }

    // Upload heap buffer big enough for the 2D row-aligned copy.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total_bytes = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total_bytes);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ud{};
    ud.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    ud.Width              = total_bytes;
    ud.Height             = 1;
    ud.DepthOrArraySize   = 1;
    ud.MipLevels          = 1;
    ud.Format             = DXGI_FORMAT_UNKNOWN;
    ud.SampleDesc.Count   = 1;
    ud.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    dev->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &ud,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                   IID_PPV_ARGS(&upload));

    // Fences.
    ComPtr<ID3D12Fence> shared_fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared_fence));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;

    // NV3DLib init.
    NV3D::InitParams p{};
    p.enable_lightboost = true;
    NV3D::InterfaceDX12* nv3d = nullptr;
    hr = NV3D::CreateInterfaceDX12(dev.Get(), queue.Get(), &p, &nv3d);
    if (FAILED(hr) || !nv3d) { wprintf(L"CreateInterfaceDX12 hr=0x%08X\n", hr); return 1; }

    wprintf(L"DX12Sample: running. Ctrl+C to exit.\n");
    fflush(stdout);

    auto next = std::chrono::steady_clock::now();
    for (UINT frame = 0; ; ++frame) {
        next += std::chrono::milliseconds(16);

        // Fill upload buffer.
        uint8_t* mapped = nullptr;
        D3D12_RANGE empty_read{ 0, 0 };
        upload->Map(0, &empty_read, reinterpret_cast<void**>(&mapped));
        FillTestPattern(mapped, fp.Footprint.RowPitch, frame);
        upload->Unmap(0, nullptr);

        // Cross-API consumer reads the resource in COMMON state. Each frame:
        //   COMMON → COPY_DEST → CopyTextureRegion → COMMON → signal fence.
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource   = sbs.Get();
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_copy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = sbs.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource                       = upload.Get();
        src.Type                            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint                 = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER to_common{};
        to_common.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_common.Transition.pResource   = sbs.Get();
        to_common.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_common.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        to_common.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &to_common);

        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);

        ++fence_value;
        queue->Signal(shared_fence.Get(), fence_value);

        nv3d->SetInputTexture(sbs.Get(), shared_fence.Get(), fence_value);
        HRESULT phr = nv3d->Present();
        if (FAILED(phr)) {
            wprintf(L"Present failed hr=0x%08X\n", phr);
            break;
        }

        std::this_thread::sleep_until(next);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { nv3d->Delete(); return 0; }
        }
    }

    nv3d->Delete();
    if (event) CloseHandle(event);
    return 0;
}
