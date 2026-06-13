/*
 * NVIDIA 3D Vision behaviour suppressor — ported verbatim from VRto3D.
 *
 * Suppresses selected NVIDIA 3D Vision UMD (nvd3dumx.dll) behaviours that
 * otherwise inflict OSDs / hotkey hijacks on the host process once stereo is
 * activated. All suppressions share one MinHook lifecycle and one
 * Install/Uninstall, since they all live or die together with the D3D9Ex
 * device.
 *
 *   1. OSD warnings dispatcher (FUN_1802C4850 in current builds)
 *        Per-frame fan-out that iterates a 23-slot warnings bitmask at
 *        offset 600 of its argument struct. We clear two bits before
 *        letting the original run:
 *          - bit 0  — Depth Amount slider
 *          - bit 10 — "Warning: attempt to run Stereoscopic 3D in a
 *                     non-stereo display mode" red overlay
 *
 *   2. Rating / info overlay (FUN_180284160 in current builds)
 *        The function that composites the per-app rating header
 *        ("Rating: Excellent/Good/Fair/Not Recommended"), the
 *        "This application is not rated by NVIDIA Corp." red overlay
 *        when no profile matches, the "Press X to toggle this info"
 *        hint, the A-J known-issue list, and the "3D Compatibility
 *        mode on/off" notice. For exes without an NVIDIA-side
 *        compatibility profile, a flat no-op detour is equivalent to
 *        suppressing just the "not rated" combination.
 *
 *   3. Hotkey blocker on user32!GetAsyncKeyState
 *        NVIDIA's hotkey dispatchers (FUN_1802A9300, FUN_1802A9840, and
 *        siblings) poll specific Ctrl+F-key combos via GetAsyncKeyState
 *        every frame. We hook that API and, when the caller is inside
 *        nvd3dumx.dll's .text AND Ctrl is currently held, lie that the
 *        polled F-key isn't pressed for a small blocklist (F3, F4, F5,
 *        F6, F7, F10, F11). Calls from anywhere else in the process,
 *        and presses without Ctrl, pass through unmodified.
 */
#pragma once

namespace NV3D {

class Nv3DVisionSuppressor {
public:
    Nv3DVisionSuppressor() = default;
    ~Nv3DVisionSuppressor();

    Nv3DVisionSuppressor(const Nv3DVisionSuppressor&)            = delete;
    Nv3DVisionSuppressor& operator=(const Nv3DVisionSuppressor&) = delete;

    // Locates nvd3dumx.dll + user32.dll in the current process, resolves
    // each target, installs its MinHook detour. Returns true when at least
    // one hook landed. Safe to call when nvd3dumx.dll isn't loaded yet
    // (returns false). Idempotent — repeat Install() calls after success
    // are no-ops.
    bool Install();

    // Removes any installed detour(s) and releases the MinHook reference.
    // Safe to call when not installed (no-op).
    void Uninstall();

    bool IsInstalled() const { return installed_; }

    static constexpr int kHookCount = 3;

private:
    bool  installed_                          = false;
    void* installed_targets_[kHookCount]      = {};
};

}  // namespace NV3D
