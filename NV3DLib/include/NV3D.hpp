/*
 * NV3DLib — NVIDIA 3D Vision output library
 * Public API.
 *
 * Accepts a side-by-side stereo image from the host renderer (DX11, DX12,
 * OpenGL, or Vulkan) and presents it as NVIDIA 3D Vision output via D3D9Ex
 * + NVAPI signature row.
 *
 * Backend headers are gated by NV3DLIB_DISABLE_{DX11,DX12,OGL,VULKAN} so
 * consumers don't pay for headers they don't use.
 *
 * Distributed under the GNU LGPL v3 — see LICENSE.
 */
#pragma once

#include <Windows.h>
#include <cstdint>

#ifndef NV3DLIB_DISABLE_DX11
struct ID3D11Device;
struct ID3D11Texture2D;
#endif

#ifndef NV3DLIB_DISABLE_DX12
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12Fence;
#endif

#ifndef NV3DLIB_DISABLE_OGL
using NV3DGLuint = unsigned int;
#endif

#ifndef NV3DLIB_DISABLE_VULKAN
struct VkInstance_T;        using NV3DVkInstance       = VkInstance_T*;
struct VkPhysicalDevice_T;  using NV3DVkPhysicalDevice = VkPhysicalDevice_T*;
struct VkDevice_T;          using NV3DVkDevice         = VkDevice_T*;
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
using NV3DVkImage     = uint64_t;
using NV3DVkSemaphore = uint64_t;
#else
struct VkImage_T;       using NV3DVkImage     = VkImage_T*;
struct VkSemaphore_T;   using NV3DVkSemaphore = VkSemaphore_T*;
#endif
using NV3DVkFormat = int;
#endif

namespace NV3D {

// Common init parameters — apply to every backend.
struct InitParams {
    // Display targeting. If null, library selects the primary 3D-Vision-capable
    // display on the system (the one Control Panel marks as the 3D Vision target).
    HMONITOR target_monitor = nullptr;

    // Swap left/right halves of the input SbS texture before presenting.
    bool eye_swap = false;

    // Keep the FSE window above other windows after focus loss.
    bool on_top = true;

    // Apply a LightBoost custom resolution if the target panel's EDID matches
    // an entry in the embedded nvtimings database (built into the lib at
    // compile time from nvtimings.json). Default ON; non-matching panels
    // degrade gracefully (no match -> no timing change -> init continues).
    bool enable_lightboost = true;

    // Override for the LightBoost timings database. If null, the lib uses
    // the embedded copy. Set this to ship a custom DB (e.g. with additional
    // panel entries) without rebuilding the lib.
    const wchar_t* nvtimings_json_path = nullptr;

    // Window ownership model. Mutually exclusive:
    //   host_hwnd == nullptr (default): library creates its own FSE popup on
    //     target_monitor. Cleanest model — library handles window/focus/
    //     minimize plumbing entirely.
    //   host_hwnd != nullptr: library uses the host's window as the D3D9Ex
    //     FSE device window. Preserves native audio routing and cursor.
    //     The host must agree not to fight FSE state once Init returns.
    HWND host_hwnd = nullptr;

    // Install the in-process NVIDIA 3D Vision behaviour suppressor after the
    // D3D9Ex device is created (so nvd3dumx.dll is loaded). Suppresses the
    // depth-amount slider OSD, "non-stereo display mode" warning, "not rated
    // by NVIDIA Corp." rating overlay, and Ctrl+F3..F11 hotkey hijacks.
    bool enable_suppressor = true;

    // -1 = retry forever. 0 = no retry. >0 = bounded attempts at 50ms
    // cadence inside Present(). The lib's SetActiveEye-based per-eye routing
    // works even if NvAPI_Stereo_IsActivated never reports true, so the
    // retry budget is mostly diagnostic.
    int activation_retry_budget = 60;
};

// -----------------------------------------------------------------------------
// DX11 backend
// -----------------------------------------------------------------------------
#ifndef NV3DLIB_DISABLE_DX11
class InterfaceDX11 {
public:
    // The texture must be 2W x H with the left eye in [0..W) and the right
    // eye in [W..2W). Fast path: B8G8R8A8_UNORM(_SRGB) + D3D11_RESOURCE_MISC_SHARED.
    // If MISC_SHARED is missing, the lib creates an internal MISC_SHARED
    // BGRA mirror and CopyResources into it each frame. Non-BGRA formats
    // are rejected.
    virtual HRESULT SetInputTexture(ID3D11Texture2D* sbs_tex) = 0;

    // Wait for the host's DX11 writes (ID3D11Query EVENT), then per-eye
    // SetActiveEye + StretchRect into the D3D9Ex back buffer + PresentEx.
    virtual HRESULT Present() = 0;

    // Tear down the library. Equivalent to deleting the object.
    virtual void Delete() = 0;

protected:
    ~InterfaceDX11() = default;
};

extern "C" HRESULT CreateInterfaceDX11(ID3D11Device* device,
                                        const InitParams* params,
                                        InterfaceDX11** out);
#endif

// -----------------------------------------------------------------------------
// DX12 backend
// -----------------------------------------------------------------------------
#ifndef NV3DLIB_DISABLE_DX12
class InterfaceDX12 {
public:
    // sbs_tex must be 2W x H, RGBA8 or BGRA8 family. Must be created with
    // D3D12_HEAP_FLAG_SHARED and in COMMON state (or
    // ALLOW_SIMULTANEOUS_ACCESS) at the handoff. sync_fence signals when
    // the host's writes complete; fence_value is the value to wait for.
    // Must be created with D3D12_FENCE_FLAG_SHARED.
    //
    // The lib wraps the resource via D3D11On12 on the host's DX12 device,
    // runs a swizzle/copy shader pass into an internal MISC_SHARED BGRA
    // mirror, and presents that mirror via D3D9Ex.
    virtual HRESULT SetInputTexture(ID3D12Resource* sbs_tex,
                                     ID3D12Fence* sync_fence,
                                     uint64_t fence_value) = 0;

    virtual HRESULT Present() = 0;

    virtual void Delete() = 0;

protected:
    ~InterfaceDX12() = default;
};

// `queue` is used only to capture the host's queue for reference; the lib
// creates its own DIRECT queue on the same DX12 device for the D3D11On12
// bridge's DX11 work submission. No commands are submitted to the host's
// queue.
extern "C" HRESULT CreateInterfaceDX12(ID3D12Device* device,
                                        ID3D12CommandQueue* queue,
                                        const InitParams* params,
                                        InterfaceDX12** out);
#endif

// -----------------------------------------------------------------------------
// OpenGL backend
// -----------------------------------------------------------------------------
#ifndef NV3DLIB_DISABLE_OGL
class InterfaceOGL {
public:
    // The texture must be 2W x H. flip_y = true if the texture is rendered
    // with OpenGL's bottom-left origin (the common case) — the library
    // applies a vertical flip during compose.
    virtual HRESULT SetInputTexture(NV3DGLuint sbs_tex,
                                     int width, int height,
                                     bool flip_y) = 0;

    virtual HRESULT Present() = 0;

    virtual void Delete() = 0;

protected:
    ~InterfaceOGL() = default;
};

// The library calls wglDXOpenDeviceNV on its own D3D9Ex device; the host's
// GL context just needs to own the SbS texture name passed to SetInputTexture.
extern "C" HRESULT CreateInterfaceOGL(HGLRC gl_context,
                                       HDC gl_dc,
                                       const InitParams* params,
                                       InterfaceOGL** out);
#endif

// -----------------------------------------------------------------------------
// Vulkan backend
// -----------------------------------------------------------------------------
#ifndef NV3DLIB_DISABLE_VULKAN
class InterfaceVulkan {
public:
    // INVERTED-EXPORT API. The lib creates an internal DX11 NT-shared texture
    // + D3D11 shared fence on its bridge device, returns Win32 NT handles
    // for the host to import as VkImage + VkSemaphore. The inbound (host-
    // exports) path doesn't work on the legacy 3D Vision driver — its
    // OpenSharedResource1 / OpenSharedFence reject all NT-shared handles
    // with E_INVALIDARG. This inverted flow side-steps that entirely.
    //
    // Host responsibilities (after this returns):
    //   1. Create VkImage with VkExternalMemoryImageCreateInfo
    //      (handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT),
    //      same dims/format as passed in.
    //   2. Allocate VkDeviceMemory with VkImportMemoryWin32HandleInfoKHR
    //      pointing to memory_handle, then vkBindImageMemory.
    //   3. Create VkSemaphore (timeline) with VkImportSemaphoreWin32HandleInfoKHR
    //      pointing to fence_handle, handleType
    //      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT.
    //   4. Each frame: write into the VkImage, signal the semaphore at the
    //      value passed to Present.
    //
    // dxgi_format is e.g. DXGI_FORMAT_B8G8R8A8_UNORM (87) — passed as
    // uint32_t so the public header stays Vulkan-only.
    //
    // Call once before any Present. Re-call requires Delete first.
    virtual HRESULT InitSharedResources(uint32_t width, uint32_t height,
                                         uint32_t dxgi_format,
                                         HANDLE* out_memory_handle,
                                         HANDLE* out_fence_handle) = 0;

    // Wait on the host-side semaphore reaching sem_value (GPU-side wait —
    // no CPU stall), copy the shared texture into the lib's D3D9-openable
    // legacy mirror, and present.
    virtual HRESULT Present(uint64_t sem_value) = 0;

    virtual void Delete() = 0;

protected:
    ~InterfaceVulkan() = default;
};

// Init takes the host's Vulkan handles so the lib can match the adapter LUID
// via vkGetPhysicalDeviceProperties2 → VkPhysicalDeviceIDProperties.
extern "C" HRESULT CreateInterfaceVulkan(NV3DVkInstance instance,
                                          NV3DVkPhysicalDevice phys,
                                          NV3DVkDevice device,
                                          uint32_t queue_family_index,
                                          const InitParams* params,
                                          InterfaceVulkan** out);
#endif

// -----------------------------------------------------------------------------
// Logging hook (optional)
// -----------------------------------------------------------------------------
// All backends route diagnostic messages through a single sink. By default
// the library writes to OutputDebugStringW. Hosts can override this to
// integrate with their own logging.
enum class LogLevel { Debug, Info, Warning, Error };
using LogSinkFn = void (*)(LogLevel level, const wchar_t* message, void* user);

extern "C" void SetLogSink(LogSinkFn sink, void* user);

}  // namespace NV3D
