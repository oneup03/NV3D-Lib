# NV3DLib

Static C++ library that turns a side-by-side stereo image from a host
renderer (DX11, DX12, OpenGL, or Vulkan) into NVIDIA 3D Vision output via
D3D9Ex + NVAPI. Includes an in-process behaviour suppressor (no more
"not rated by NVIDIA Corp." overlay or Ctrl+F-key hotkey hijacks) and
LightBoost auto-apply with an embedded timings database.

## Architecture

D3D9Ex device in `NVAPI_STEREO_DRIVER_MODE_DIRECT`. Each frame:

```
SetActiveEye(LEFT)  -> StretchRect(input, [0..W),  back_buffer, POINT)
SetActiveEye(RIGHT) -> StretchRect(input, [W..2W), back_buffer, POINT)
PresentEx()
```

DIRECT mode bypasses the legacy AUTOMATIC-mode signature-row scan. There
is no packed surface, no signature, no CPU readback — every backend hands
the D3D9 presenter a shared `IDirect3DSurface9*` and the per-eye routing
is explicit.

| Backend | Sharing mechanism | Notes |
|---|---|---|
| **DX11** | `IDXGIResource::GetSharedHandle` (legacy KMT) → `D3D9Ex::CreateTexture(pSharedHandle=…)` | Direct. Identity-cached so the import happens once per session. `ID3D11Query(EVENT)` for DX11→D3D9 sync. |
| **DX12** | `D3D11On12CreateDevice` wraps the host's `ID3D12Device`; `ID3D11On12Device::CreateWrappedResource` aliases the host `ID3D12Resource` as `ID3D11Texture2D`. A DX11 swizzle shader copies into our `MISC_SHARED` BGRA mirror; D3D9 opens the mirror via the legacy KMT handle. | The plain `OpenSharedResource1` path is broken on the legacy 3D Vision driver — D3D11On12 sidesteps it entirely. RGBA8 host input is fine; the swizzle shader handles the conversion. |
| **OpenGL** | `WGL_NV_DX_interop2`. Lib creates a D3D9 RT, aliases it as a GL texture via `wglDXRegisterObjectNV`. Per frame: `wglDXLockObjectsNV` → FBO blit host's tex → aliased tex (Y-flip optional) → `wglDXUnlockObjectsNV` → `presenter->Present`. | No DX11 hop. `ScopedGLContext` saves/restores the host's current context around every call. |
| **Vulkan** | **Inverted-export.** Lib creates a `MISC_SHARED \| MISC_SHARED_NTHANDLE` DX11 texture + `D3D11_FENCE_FLAG_SHARED` fence, calls `CreateSharedHandle` on both, returns the NT handles to the host. Host imports them as `VkImage` + timeline `VkSemaphore` via the `external_memory_win32` / `external_semaphore_win32` extensions. | The inbound (host-exports, lib-imports via `OpenSharedResource1`) path is also broken on the legacy driver. The inverted flow avoids it. |

All four backends converge on the same downstream: lib's DX11 device →
`MISC_SHARED` (legacy KMT) BGRA mirror → D3D9Ex view → DIRECT-mode
`SetActiveEye` per-eye `StretchRect` → `PresentEx`.

## Build

```
git clone --recursive https://github.com/<user>/NV3DLib.git
```

Open `NV3DLib.sln` in Visual Studio 2022. Pick a platform (`x86` / `x64`)
and a configuration (`Debug-MT`, `Release-MT`, `Debug-MD`, `Release-MD`),
build. Output: `NV3DLib/lib/x{32,64}/<Configuration>/NV3DLib-<runtime>.lib`.

Submodules pulled in by `--recursive`:
- `external/nvapi` — NVIDIA NVAPI SDK
- `external/Vulkan-Headers` — Khronos Vulkan headers (header-only)
- `external/minhook` — used by the in-process 3D Vision behaviour suppressor
- `external/json` — nlohmann/json (header-only), used by the embedded
  LightBoost timings DB

A PowerShell pre-build step (`tools/embed_nvtimings.ps1`) bakes
`nvtimings.json` into `NV3DLib/src/nvtimings_embedded.h` so consumers don't
need to ship the JSON separately. The generated header is gitignored.

If you cloned without `--recursive`:
```
git submodule update --init --recursive
```

## Public API

Single header [`include/NV3D.hpp`](NV3DLib/include/NV3D.hpp). Four
backend-specific factories return a backend-specific COM-style interface
with explicit `Delete()`:

### DX11

```cpp
#include <NV3D.hpp>

NV3D::InitParams p{};
p.enable_lightboost = true;
p.enable_suppressor = true;

NV3D::InterfaceDX11* nv3d = nullptr;
NV3D::CreateInterfaceDX11(my_d3d11_device, &p, &nv3d);

// Per frame:
nv3d->SetInputTexture(sbs_tex);   // 2W x H, DXGI_FORMAT_B8G8R8A8_UNORM(_SRGB)
nv3d->Present();

// Shutdown:
nv3d->Delete();
```

The fast path requires the host's texture to be `D3D11_RESOURCE_MISC_SHARED`
+ BGRA8. If it isn't, the lib creates an internal `MISC_SHARED` + BGRA8
mirror and `CopyResource`s into it each frame.

### DX12

```cpp
NV3D::InterfaceDX12* nv3d = nullptr;
NV3D::CreateInterfaceDX12(my_d3d12_device, my_d3d12_queue, &p, &nv3d);

// Per frame:
my_queue->Signal(my_fence, value);                    // host signals fence
nv3d->SetInputTexture(my_resource, my_fence, value);  // resource + sync fence
nv3d->Present();
```

The DX12 resource should be created with `D3D12_HEAP_FLAG_SHARED` and
either `RGBA8_UNORM` or `BGRA8_UNORM`. State at the handoff moment must
be `COMMON` (or `ALLOW_SIMULTANEOUS_ACCESS`). The fence must be
`D3D12_FENCE_FLAG_SHARED`. RGBA→BGRA swizzle is done by the lib's internal
DX11 shader pass.

### OpenGL

```cpp
NV3D::InterfaceOGL* nv3d = nullptr;
NV3D::CreateInterfaceOGL(my_hglrc, my_hdc, &p, &nv3d);

// Per frame:
nv3d->SetInputTexture(my_gl_tex, w, h, /*flip_y=*/true);
nv3d->Present();
```

Requires the driver to expose `WGL_NV_DX_interop2` (all current NVIDIA
drivers do). The host's GL context must be current when calling
`CreateInterfaceOGL`. The lib saves/restores the host's current context
around every internal GL call, so it's safe to keep using the same
context for the host's own rendering in between `Present()` calls.

### Vulkan

The Vulkan flow is inverted compared to the other three — the lib creates
the shared image + fence and gives the host NT handles to import. Set up
once after init:

```cpp
NV3D::InterfaceVulkan* nv3d = nullptr;
NV3D::CreateInterfaceVulkan(instance, phys, device, queue_family_index, &p, &nv3d);

HANDLE mem_nt = nullptr;
HANDLE sem_nt = nullptr;
nv3d->InitSharedResources(2560, 720,
                            DXGI_FORMAT_B8G8R8A8_UNORM,   // = 87
                            &mem_nt, &sem_nt);

// Host imports mem_nt as VkImage via VkExternalMemoryImageCreateInfo +
// VkImportMemoryWin32HandleInfoKHR (handleType =
// VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT).
//
// Host imports sem_nt as timeline VkSemaphore via VkSemaphoreTypeCreateInfo +
// vkImportSemaphoreWin32HandleKHR (handleType =
// VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT).
```

Per frame:

```cpp
// Host renders into the imported VkImage, then submits with a timeline-
// semaphore signal at `value`. The lib waits GPU-side on `value`.
nv3d->Present(value);
```

See [`samples/VulkanSample/VulkanSample.cpp`](samples/VulkanSample/VulkanSample.cpp)
for a complete worked example (CPU upload via staging buffer, semaphore
import via `vkImportSemaphoreWin32HandleKHR`).

### `InitParams`

```cpp
struct InitParams {
    HMONITOR target_monitor = nullptr;  // null = primary 3D Vision display
    bool eye_swap           = false;
    bool on_top             = true;
    bool enable_lightboost  = true;     // embedded nvtimings DB — see below
    const wchar_t* nvtimings_json_path = nullptr;  // overrides embedded DB
    HWND host_hwnd          = nullptr;  // null = lib creates its own FSE window
    bool enable_suppressor  = true;
    int  activation_retry_budget = 60;  // 0 = no retry; -1 = retry forever
};
```

## In-process suppressor

`enable_suppressor = true` (default) installs three MinHook detours into
`nvd3dumx.dll` and `user32.dll` after the D3D9Ex device is created:

1. **OSD warnings dispatcher** — clears bit 0 (depth-amount slider) and
   bit 10 ("non-stereo display mode" warning) from the per-frame warning
   bitmask before letting the original dispatcher run.
2. **Rating / info overlay** — no-op detour, suppresses the
   "not rated by NVIDIA Corp." red overlay and companion lines.
3. **`GetAsyncKeyState` hotkey blocker** — caller-filtered: when the
   call site is inside `nvd3dumx.dll`'s `.text` AND Ctrl is held, lies
   that F3/F4/F5/F6/F7/F10/F11 are not pressed (the NVCP-side
   StereoSeparation / Convergence / WriteConfig / RHWAtScreenMore /
   CycleFrustumAdjust hotkeys).

Hooks are removed at the start of teardown so the driver is in a pristine
state if the host re-creates the presenter later.

## LightBoost (embedded)

`enable_lightboost = true` (default) looks up the target panel's EDID
vendor+product in the embedded `nvtimings.json` database (~5500 entries).
If a matching low-persistence (strobed-backlight) timing is found, the lib
calls `NvAPI_DISP_TryCustomDisplay` before `CreateDeviceEx`, then syncs
GDI's `DEVMODE` via `ChangeDisplaySettingsExW` so apps see the new refresh.

On shutdown, `NvAPI_DISP_RevertCustomDisplayTrial` reverts; if the trial
got invalidated mid-session by FSE D3D9Ex modesets (a known VRto3D-observed
issue), the lib falls back to `ChangeDisplaySettingsExW` with a snapshot
of the OS-stored `DEVMODE` taken at Enable time.

Non-fatal: any LightBoost failure is logged and the lib continues without
the custom timing.

To override the embedded DB with your own file (e.g. for additional panel
support), set `InitParams::nvtimings_json_path`.

## Window ownership

Two modes, selected by `InitParams::host_hwnd`:

- **Library-owned (default, `host_hwnd = nullptr`)** — lib spawns its own
  FSE popup on the target monitor and pumps its message loop on a dedicated
  thread. Handles minimize / focus / `SC_SCREENSAVE` suppression and the
  AttachThreadInput foreground-lock bypass needed for FSE D3D9Ex.
- **Host-owned (`host_hwnd != nullptr`)** — lib subclasses the host's
  HWND and uses it as the D3D9Ex FSE device window. Preserves the host
  process's native audio routing + cursor. The host must agree not to
  fight the FSE state once Init returns.

## Consumer link requirements

NV3DLib is a static library; consumers add the appropriate `.lib` from
`NV3DLib/lib/<arch>/<config>/` to their linker input.

Additional libs the consumer's exe must link:
- `nvapi64.lib` (x64) or `nvapi.lib` (x86) from `external/nvapi/`
- `d3d9.lib`, `d3d11.lib`, `dxgi.lib`
- `d3d12.lib` (only if using the DX12 backend)
- `d3dcompiler.lib` (only if using the DX12 backend — for the swizzle shader)
- `opengl32.lib` (only if using the OpenGL backend)

Recommended: delay-load `nvapi64.dll` so the host degrades gracefully on
non-NVIDIA systems:

```
Linker > Input > Delay Loaded DLLs: nvapi64.dll
```

(`nvapi.dll` for x86.)

## Diagnostic logging

Set a log sink to capture the lib's progress + any failure details:

```cpp
NV3D::SetLogSink([](NV3D::LogLevel lvl, const wchar_t* msg, void*) {
    // route to your logger, file, OutputDebugString, etc.
}, nullptr);
```

Default sink is `OutputDebugStringW`. The four sample exes write to
`{dx11,dx12,ogl,vulkan}sample.log` next to themselves (opened with
`ccs=UTF-16LE` so wide-char log lines round-trip cleanly).

## Samples

- `samples/DX11Sample` — `D3D11_USAGE_DEFAULT | MISC_SHARED | BGRA8` texture, CPU-fills test pattern, `UpdateSubresource`, `SetInputTexture`, `Present`.
- `samples/DX12Sample` — `RGBA8 | HEAP_FLAG_SHARED | COMMON` resource, COMMON↔COPY_DEST barriers around `CopyTextureRegion` from an UPLOAD buffer, shared `FENCE_FLAG_SHARED` fence.
- `samples/OGLSample` — invisible WGL host context, GL texture sized 2W×H, `glTexImage2D` with BGRA-byte upload each frame.
- `samples/VulkanSample` — Vulkan 1.2 instance/device with `KHR_external_memory_win32` + `KHR_external_semaphore_win32` + timeline semaphores. Imports the lib's NT handles as `VkImage` + timeline `VkSemaphore`, CPU-fills a staging buffer, `vkCmdCopyBufferToImage` + layout transitions, signal-on-submit.

All four samples render a clearly stereoscopic test pattern (left eye =
red gradient, right eye = green gradient, with an animated white quad that
oscillates in depth) so visual correctness is obvious through the shutter
glasses.

## Hard-won lessons (so you don't relearn them)

- `NvAPI_Stereo_SetDriverMode(NVAPI_STEREO_DRIVER_MODE_DIRECT)` **must** be
  called before `Direct3DCreate9Ex`. Without it `SetActiveEye` is silently
  ignored and you get one-eye-only output with the emitter on.
- Cross-API shared resources (DX12 → DX11) **must** be in
  `D3D12_RESOURCE_STATE_COMMON` at the handoff, or use
  `ALLOW_SIMULTANEOUS_ACCESS`.
- `D3D12_RESOURCE_FLAG_NONE` + `RGBA8_UNORM` + `UNKNOWN` layout is the
  combo `D3D11On12::CreateWrappedResource` accepts; the plain
  `OpenSharedResource1` path is broken on the legacy 3D Vision driver
  regardless of flags.
- `IDXGIResource::GetSharedHandle` returns a **KMT** handle owned by the
  texture — **do not** `CloseHandle` it. The NT handles from
  `ID3D12Device::CreateSharedHandle` / `IDXGIResource1::CreateSharedHandle` /
  `ID3D11Fence::CreateSharedHandle` **do** need closing.
- HLSL component swizzles operate on color channels, not on memory byte
  order. Format conversion (RGBA8 → BGRA8) between SRV and RTV happens
  automatically; the shader is just a sample-and-return pass.
- D3D9Ex `Release()` on a TDR'd device blocks in the kernel-mode driver.
  The lib uses `Detach()` instead of `Reset()` when the device is dead
  to avoid wedging shutdown.
- `NvAPI_Unload` disables NVAPI process-wide, not per-init. Libraries
  shouldn't call it (would break any other NVAPI users in the host
  process); rely on process-exit cleanup instead.

## License

LGPL v3 — see [LICENSE](LICENSE). The vendored Khronos / NVIDIA SDK
submodules carry their own licenses.
