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
// Presentation interval of the internal D3D9Ex swap chain.
//
// A 3D Vision panel runs frame-sequential: one stereo pair occupies TWO
// display refreshes, so a 120Hz panel tops out at 60 stereo frames/second
// and VsyncOn blocks each PresentEx until that pair's slot comes round.
//
//   VsyncOn  - D3DPRESENT_INTERVAL_ONE. Tear-free and the safe default, but
//              a present that misses its slot waits for the next one, which
//              is the classic "DX9 frame rate halves" behaviour.
//   VsyncOff - D3DPRESENT_INTERVAL_IMMEDIATE. Presents are never throttled.
//              On its own this tears; its real purpose is to let the DRIVER
//              do the syncing, because NVIDIA's Fast Sync only engages for
//              applications that are not already vsync-limited. Pair it with
//              a Fast Sync driver profile (NV3D-Glass writes one for itself)
//              to get unthrottled presents with the newest frame shown at
//              each vblank and no tearing.
//   Half     - D3DPRESENT_INTERVAL_TWO. Halves the stereo rate deliberately
//              (30fps on a 120Hz panel); useful to lock a source that can't
//              hold 60 to a stable cadence rather than an uneven one.
enum class PresentInterval : int {
    VsyncOn  = 0,
    VsyncOff = 1,
    Half     = 2,
};

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

    // Tracked-game PID. When non-zero, the library uses VRto3D's
    // FocusThreadLoop pattern: keep the FSE popup on HWND_TOPMOST, force the
    // tracked game's window into the foreground when our popup first goes on
    // top, and re-assert topmost ~every 500 ms. Set this to the PID of the
    // game/app the host is capturing for. Leave at 0 to keep the default
    // library-owned mode (auto-minimize when the host process loses focus —
    // suitable for VR runtimes that bring their own focus management).
    DWORD tracked_game_pid = 0;

    // Presentation interval of the D3D9Ex swap chain. See PresentInterval.
    // Device-creation parameter: changing it requires a teardown + re-Init.
    PresentInterval present_interval = PresentInterval::VsyncOn;

    // IDirect3DDevice9Ex::SetMaximumFrameLatency value. 1 keeps the driver's
    // queue as short as possible, which is what the periodic stereo
    // revalidation wants (a deeper queue widens the window in which a queued
    // blit can reference a backbuffer whose allocation the driver just
    // swapped). The cost is that any hitch immediately costs a full stereo
    // pair. Raise to 2-3 to trade a frame of latency for hitch tolerance.
    // 0 leaves the driver default untouched.
    uint32_t max_frame_latency = 1;

    // EXPERIMENTAL. Create the swap chain with D3DSWAPEFFECT_FLIPEX instead
    // of D3DSWAPEFFECT_DISCARD, and pass D3DPRESENT_FORCEIMMEDIATE on each
    // PresentEx when present_interval is VsyncOff (the flag is only legal on
    // a FLIPEX chain). FLIPEX moves D3D9Ex onto the flip present model,
    // which is normally lower-latency.
    //
    // Unknown interaction with 3D Vision: this library drives the driver's
    // AUTOMATIC packed-stereo path, where the driver SCANS the presented
    // surface for a signature row and demuxes the left/right halves out of
    // it. Changing the present model underneath that scanner is exactly the
    // kind of thing it may not tolerate, and the failure mode is "output
    // goes mono" rather than an error. Off by default; if CreateDeviceEx
    // refuses FLIPEX the library falls back to DISCARD on its own and logs.
    bool use_flipex = false;
};

// Present-path telemetry, for hosts that want to verify their own frame
// pacing. Snapshot semantics: GetPresentStats() returns the counts since
// the previous call and resets them, so call it on a fixed cadence
// (~1 Hz) and never from more than one thread.
struct PresentStats {
    uint32_t submits_accepted = 0;    // work items handed to the present worker
    uint32_t submits_dropped  = 0;    // dropped because the worker was still busy
    uint32_t presents_done    = 0;    // work items the worker completed
    // Wall time of a completed work item — the cross-device sync wait plus
    // the D3D9 blits plus the vsync-blocked PresentEx. On a healthy 120Hz
    // 3D Vision panel this sits near 16.7ms (one stereo pair = two refreshes).
    float    present_ms_avg   = 0.0f;
    float    present_ms_max   = 0.0f;
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

    // Toggle the FSE popup visibility. Flipping to false has the library's
    // window thread do a coordinated SW_MINIMIZE (releasing the FSE scan-out
    // slot and the WndProc's deactivation suppression for one transition);
    // flipping back to true reverses it (SW_RESTORE + topmost + re-armed
    // suppression). The toggle is observed on the library's window thread —
    // do not call ShowWindow on the popup HWND yourself; cross-thread
    // ShowWindow on a FSE D3D9Ex device window wedges DWM and leaves the
    // host's other windows non-responsive.
    virtual void SetVisible(bool visible) = 0;

    // Live eye-swap toggle. Updates the NV3D signature row on the next
    // frame; the driver picks up the new routing on the following PresentEx.
    // No teardown / re-Init needed — safe to call mid-session at any time.
    virtual void SetEyeSwap(bool enable) = 0;

    // Toggle whether the library-owned popup is interactive (solid to mouse
    // input) or click-through. After Init the popup is click-through: clicks
    // fall through to whatever is behind it (WS_EX_TRANSPARENT + a
    // WM_NCHITTEST→HTTRANSPARENT WndProc path). Pass true while the host shows
    // an in-popup UI (e.g. an OSD/menu composited into the SbS frame) so
    // clicks land on the popup instead of the app beneath, and false to
    // restore click-through. Only WS_EX_TRANSPARENT + the hit-test path are
    // touched (never WS_EX_LAYERED, which would race D3D9 present), and there
    // is no DWM settle, so it is cheap enough to toggle on every menu
    // open/close. Only meaningful in library-owned window mode
    // (host_hwnd == nullptr). Safe to call from any thread.
    virtual void SetInteractive(bool interactive) = 0;

    // The library-owned FSE popup's HWND (or the host HWND in host_hwnd mode).
    // Valid once CreateInterfaceDX11 returns success. The host may use it to
    // map screen cursor coordinates into the popup's client area (e.g. for an
    // OSD hit-test). Do not destroy or re-style it — the library owns its
    // lifetime and its FSE / click-through state.
    virtual HWND GetWindowHandle() const = 0;

    // Host-side notification that the GPU backing this interface was lost
    // (TDR / DXGI_ERROR_DEVICE_REMOVED / _RESET observed on the host's own
    // D3D11 device). Marks the internal D3D9 device dead so the subsequent
    // Delete() takes the non-blocking teardown path: no Stereo_DestroyHandle,
    // no COM Release into the kernel-mode driver (both can block indefinitely
    // against a wedged adapter), no restore-from-iconic window dance. Call
    // BEFORE Delete() whenever the host detects device removal itself — the
    // library often can't tell on its own, because a hidden popup means no
    // D3D9 call runs to observe the failure.
    virtual void NotifyDeviceLost() = 0;

    // Tear down the library. Equivalent to deleting the object.
    virtual void Delete() = 0;

    // Manual-reset event that is SET while the present worker is idle and
    // RESET while a frame is in flight. Null if the backend couldn't create
    // it — a host must then fall back to timer pacing.
    //
    // This is the hook for display-clocked pacing: the library's PresentEx
    // blocks on the panel's vblank, so "worker went idle" is the most
    // accurate frame clock available to the host. Waiting on this handle
    // (e.g. alongside window messages in MsgWaitForMultipleObjectsEx) and
    // only then capturing + SetInputTexture + Present gives exactly one
    // submitted frame per completed present: no over-submission, no frames
    // dropped inside Submit, and the host's capture happens as late as
    // possible before the present that consumes it.
    //
    // The handle is owned by the library and closed during Delete(). Do not
    // close it, and do not wait on it from a thread other than the one that
    // drives teardown.
    virtual HANDLE GetPresentCompletedEvent() const = 0;

    // Drain the present-path counters (see PresentStats). Resets on read.
    virtual void GetPresentStats(PresentStats* out) = 0;

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
