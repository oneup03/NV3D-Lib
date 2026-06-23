/*
 * LightBoost — apply NVIDIA custom display timings to drive 3D Vision panels
 * in low-persistence (strobed backlight) mode. Originally ported from VRto3D.
 *
 * Enable():
 *   1. Looks up the target panel's EDID in nvtimings.json.
 *   2. Calls NvAPI_DISP_TryCustomDisplay to validate the timing on the live
 *      hardware.
 *   3. Calls NvAPI_DISP_SaveCustomDisplay to commit the validated timing to
 *      NVCP's persistent custom-display registry — it survives reboot and
 *      stays applied across host sessions.
 *   4. Syncs GDI's DEVMODE via ChangeDisplaySettingsExW so apps see the new
 *      refresh.
 *
 * There is no Disable. We deliberately leave the saved timing in place: most
 * users want LightBoost on their 3DV panel permanently, and reverting on
 * shutdown caused a modeset right after D3D9 release (during the DWM-reclaim
 * window) that contributed to post-quit display freezes. To remove the
 * saved timing the user deletes the entry from NVIDIA Control Panel →
 * Change resolution → Customize.
 *
 * Non-fatal: any failure logs + leaves the panel untouched. Calling Enable()
 * on a panel that's already in the target timing is a no-op.
 */
#pragma once

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvapi.h>

namespace NV3D {

struct NvTimingsEntry {
    std::string monitor_EDID;
    NV_TIMING   timing{};
    float       refresh_hz{};
    NvU16       refresh_int{};
};

class NvTimingsDb {
public:
    // Throws on file open / parse error.
    static NvTimingsDb Load(const std::string& json_path);
    // Parse from in-memory JSON buffer. Throws on parse error.
    static NvTimingsDb LoadFromString(const char* data, size_t size);
    // Load the JSON blob embedded into NV3DLib at build time
    // (tools/embed_nvtimings.ps1 generates nvtimings_embedded.h).
    static NvTimingsDb LoadEmbedded();

    std::optional<NvTimingsEntry> FindExact(const std::string& key) const;
    std::optional<NvTimingsEntry> FindByBaseAndRefresh(const std::string& base, int rr_nearest) const;
    std::optional<NvTimingsEntry> FindHighestRefreshForBase(const std::string& base) const;

    static std::string ToUtf8(const std::wstring& ws);

private:
    std::unordered_map<std::string, NvTimingsEntry> data_;
};

class LightBoost {
public:
    LightBoost()  = default;
    ~LightBoost() = default;

    LightBoost(const LightBoost&)            = delete;
    LightBoost& operator=(const LightBoost&) = delete;

    // Apply LightBoost to the GDI device (e.g. L"\\\\.\\DISPLAY3"). The
    // timings DB resolution:
    //   - explicit json_path used if provided
    //   - else the embedded DB (nvtimings_embedded.h, generated at lib
    //     build time from nvtimings.json)
    // Returns true if a custom timing was applied or was already active.
    // Returns false on any failure (logged) -- non-fatal.
    bool Enable(const std::wstring& gdi_device_w,
                  const std::wstring& json_path = L"");

    bool IsEnabled() const { return enabled_; }

private:
    static NvU32 ResolveNvDisplayId(const std::string& gdi_device);
    static bool  WaitForTimingChange(NvU32 displayId, const NV_TIMING& previous,
                                       DWORD timeout_ms);
    static void  WaitForModesetSettle(DWORD ms);

    bool                       enabled_ = false;
    std::vector<NvU32>         display_ids_;
    NvTimingsEntry             matched_{};
    NV_TIMING                  original_target_timing_{};
    bool                       has_original_target_timing_ = false;
    DEVMODEW                   original_devmode_{};
    bool                       has_original_devmode_ = false;
};

}  // namespace NV3D
