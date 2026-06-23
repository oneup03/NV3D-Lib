#include "lightboost.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "log.h"
#include "nvtimings_embedded.h"

namespace NV3D {

namespace {

uint16_t json_u16(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing field: ") + name);
    auto x = j.at(name).get<int64_t>();
    if (x < 0 || x > 0xFFFF) throw std::runtime_error(std::string("Field out of range u16: ") + name);
    return static_cast<uint16_t>(x);
}

uint32_t json_u32(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing field: ") + name);
    auto x = j.at(name).get<int64_t>();
    if (x < 0 || x > 0xFFFFFFFFLL) throw std::runtime_error(std::string("Field out of range u32: ") + name);
    return static_cast<uint32_t>(x);
}

double json_double(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing field: ") + name);
    const auto& v = j.at(name);
    if (v.is_number_float())   return v.get<double>();
    if (v.is_number_integer()) return static_cast<double>(v.get<int64_t>());
    throw std::runtime_error(std::string("Field not numeric: ") + name);
}

const nlohmann::json& json_must(const nlohmann::json& j, const char* name) {
    if (!j.contains(name)) throw std::runtime_error(std::string("Missing object: ") + name);
    return j.at(name);
}

NV_TIMING ParseTiming(const nlohmann::json& jmon) {
    NV_TIMING t{};
    std::memset(&t, 0, sizeof(t));
    t.pclk = json_u32(jmon, "frequency_10khz");
    const auto& jh = json_must(jmon, "hor");
    t.HTotal      = json_u16(jh, "total");
    t.HVisible    = json_u16(jh, "visible");
    t.HBorder     = json_u16(jh, "border");
    t.HFrontPorch = json_u16(jh, "frontPorch");
    t.HSyncWidth  = json_u16(jh, "numSync");
    t.HSyncPol    = 0;
    const auto& jv = json_must(jmon, "ver");
    t.VTotal      = json_u16(jv, "total");
    t.VVisible    = json_u16(jv, "visible");
    t.VBorder     = json_u16(jv, "border");
    t.VFrontPorch = json_u16(jv, "frontPorch");
    t.VSyncWidth  = json_u16(jv, "numSync");
    t.VSyncPol    = 0;
    t.interlaced = 0;
    std::memset(&t.etc, 0, sizeof(t.etc));
    double rr = json_double(jmon, "refresh_hz");
    t.etc.rr    = static_cast<NvU16>(std::lround(rr));
    t.etc.rrx1k = static_cast<NvU32>(std::lround(rr * 1000.0));
    return t;
}

NvTimingsEntry ParseEntry(const nlohmann::json& jentry) {
    if (!jentry.contains("monitor_timings"))
        throw std::runtime_error("Entry missing 'monitor_timings'");
    NvTimingsEntry e;
    const auto& jmon = jentry.at("monitor_timings");
    e.timing     = ParseTiming(jmon);
    e.refresh_hz = static_cast<float>(json_double(jmon, "refresh_hz"));
    return e;
}

// EDID → "VENDOR_PRODUCT" key (e.g. "ACI_23F7").
std::wstring ParseMonitorEdid(NvU32 displayId) {
    NV_EDID_DATA edid_data{};
    edid_data.version = NV_EDID_DATA_VER;
    NvU8 buf[NV_EDID_DATA_SIZE_MAX] = {};
    edid_data.pEDID = buf;
    edid_data.sizeOfEDID = NV_EDID_DATA_SIZE_MAX;
    NV_EDID_FLAG flag = NV_EDID_FLAG_RAW;
    NvAPI_Status s = NvAPI_DISP_GetEdidData(displayId, &edid_data, &flag);
    if (s != NVAPI_OK || edid_data.sizeOfEDID < 128) {
        NV3D_LOG_WARN(L"LightBoost: NvAPI_DISP_GetEdidData failed s=%d size=%d",
                        static_cast<int>(s), edid_data.sizeOfEDID);
        return L"";
    }
    const NvU8* edid = edid_data.pEDID;
    uint16_t vendor_id = (static_cast<uint16_t>(edid[8]) << 8) | static_cast<uint16_t>(edid[9]);
    char vendor[4]{};
    vendor[0] = ((vendor_id >> 10) & 0x1F) + 'A' - 1;
    vendor[1] = ((vendor_id >> 5)  & 0x1F) + 'A' - 1;
    vendor[2] = ((vendor_id)       & 0x1F) + 'A' - 1;
    uint16_t product_id = (static_cast<uint16_t>(edid[11]) << 8) | static_cast<uint16_t>(edid[10]);
    std::wstringstream ss;
    ss << vendor << L"_" << std::uppercase << std::hex
       << std::setw(4) << std::setfill(L'0') << product_id;
    return ss.str();
}

}  // anonymous

// ---------------------------------------------------------------------------
// NvTimingsDb
// ---------------------------------------------------------------------------
NvTimingsDb NvTimingsDb::Load(const std::string& json_path) {
    std::ifstream ifs(json_path);
    if (!ifs) throw std::runtime_error("NvTimingsDb: failed to open: " + json_path);
    nlohmann::json j;
    ifs >> j;
    NvTimingsDb db;
    db.data_.reserve(j.size());
    for (auto it = j.begin(); it != j.end(); ++it)
        db.data_.emplace(it.key(), ParseEntry(it.value()));
    return db;
}

NvTimingsDb NvTimingsDb::LoadFromString(const char* data, size_t size) {
    nlohmann::json j = nlohmann::json::parse(data, data + size);
    NvTimingsDb db;
    db.data_.reserve(j.size());
    for (auto it = j.begin(); it != j.end(); ++it)
        db.data_.emplace(it.key(), ParseEntry(it.value()));
    return db;
}

NvTimingsDb NvTimingsDb::LoadEmbedded() {
    return LoadFromString(reinterpret_cast<const char*>(kNvTimingsJson),
                            kNvTimingsJsonSize);
}

std::optional<NvTimingsEntry> NvTimingsDb::FindExact(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

std::optional<NvTimingsEntry> NvTimingsDb::FindByBaseAndRefresh(
    const std::string& base, int rr_nearest) const {
    return FindExact(base + "_" + std::to_string(rr_nearest));
}

std::optional<NvTimingsEntry> NvTimingsDb::FindHighestRefreshForBase(
    const std::string& base) const {
    const std::string prefix = base + "_";
    int best_rr = -1;
    const NvTimingsEntry* best = nullptr;
    for (const auto& kv : data_) {
        if (kv.first.size() <= prefix.size()) continue;
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string suffix = kv.first.substr(prefix.size());
        if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), ::isdigit)) continue;
        int rr = std::stoi(suffix);
        if (rr > best_rr) { best_rr = rr; best = &kv.second; }
    }
    if (!best) return std::nullopt;
    NvTimingsEntry r = *best;
    r.refresh_int = static_cast<NvU16>(best_rr);
    return r;
}

std::string NvTimingsDb::ToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                         out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// LightBoost
// ---------------------------------------------------------------------------
NvU32 LightBoost::ResolveNvDisplayId(const std::string& gdi_device) {
    if (gdi_device.empty()) return 0;
    char narrow[NVAPI_SHORT_STRING_MAX] = {};
    size_t n = (std::min)(gdi_device.size(), sizeof(narrow) - 1);
    std::memcpy(narrow, gdi_device.data(), n);
    NvU32 id = 0;
    NvAPI_Status s = NvAPI_DISP_GetDisplayIdByDisplayName(narrow, &id);
    if (s != NVAPI_OK) {
        NV3D_LOG_WARN(L"LightBoost: GetDisplayIdByDisplayName('%hs') failed s=%d", narrow, s);
        return 0;
    }
    return id;
}

void LightBoost::WaitForModesetSettle(DWORD ms) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
}

bool LightBoost::WaitForTimingChange(NvU32 displayId, const NV_TIMING& previous,
                                       DWORD timeout_ms) {
    if (!displayId) { WaitForModesetSettle(timeout_ms); return false; }
    const DWORD start = GetTickCount();
    while (true) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        NV_TIMING live{};
        NV_TIMING_INPUT ti{};
        ti.version = NV_TIMING_INPUT_VER;
        NvAPI_Status s = NvAPI_DISP_GetTiming(displayId, &ti, &live);
        if (s == NVAPI_OK &&
            (live.HTotal != previous.HTotal || live.VTotal != previous.VTotal)) {
            return true;
        }
        if (GetTickCount() - start >= timeout_ms) return false;
        Sleep(50);
    }
}

bool LightBoost::Enable(const std::wstring& gdi_device_w,
                          const std::wstring& json_path_w) {
    if (enabled_) return true;

    const std::string gdi_device = NvTimingsDb::ToUtf8(gdi_device_w);

    NV3D_LOG_INFO(L"LightBoost: target gdi='%s'", gdi_device_w.c_str());

    const NvU32 target_id = ResolveNvDisplayId(gdi_device);
    if (!target_id) {
        NV3D_LOG_WARN(L"LightBoost: could not resolve target displayId — skipped");
        return false;
    }

    NvPhysicalGpuHandle gpu_handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32 gpu_count = 0;
    if (NvAPI_EnumPhysicalGPUs(gpu_handles, &gpu_count) != NVAPI_OK || gpu_count == 0) {
        NV3D_LOG_WARN(L"LightBoost: EnumPhysicalGPUs failed — skipped");
        return false;
    }

    NvTimingsDb db;
    try {
        if (json_path_w.empty()) {
            NV3D_LOG_INFO(L"LightBoost: using embedded nvtimings DB");
            db = NvTimingsDb::LoadEmbedded();
        } else {
            std::string path = NvTimingsDb::ToUtf8(json_path_w);
            NV3D_LOG_INFO(L"LightBoost: loading nvtimings.json from '%s'", json_path_w.c_str());
            db = NvTimingsDb::Load(path);
        }
    } catch (const std::exception& ex) {
        NV3D_LOG_WARN(L"LightBoost: nvtimings load failed: %hs -- skipped", ex.what());
        return false;
    }

    // Snapshot OS-stored DEVMODE for fallback revert later.
    has_original_devmode_ = false;
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(gdi_device_w.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) {
            original_devmode_     = dm;
            has_original_devmode_ = true;
            NV3D_LOG_INFO(L"LightBoost: snapshotted DEVMODE %ux%u@%uHz",
                            dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        }
    }

    // Walk GPUs to find the target display & its EDID.
    bool target_found = false;
    bool already_good = false;
    display_ids_.clear();
    has_original_target_timing_ = false;
    for (NvU32 gi = 0; gi < gpu_count && !target_found; ++gi) {
        NV_GPU_DISPLAYIDS dids[NVAPI_MAX_DISPLAYS]{};
        NvU32 dcount = NVAPI_MAX_DISPLAYS;
        for (NvU32 i = 0; i < dcount; ++i) dids[i].version = NV_GPU_DISPLAYIDS_VER;
        if (NvAPI_GPU_GetConnectedDisplayIds(gpu_handles[gi], dids, &dcount, 0) != NVAPI_OK)
            continue;
        for (NvU32 d = 0; d < dcount; ++d) {
            const NvU32 id = dids[d].displayId;
            if (id != target_id) continue;
            std::wstring edid_key = ParseMonitorEdid(id);
            if (edid_key.empty()) continue;
            NV_TIMING       cur{};
            NV_TIMING_INPUT ti{}; ti.version = NV_TIMING_INPUT_VER;
            if (NvAPI_DISP_GetTiming(id, &ti, &cur) != NVAPI_OK) continue;
            NV3D_LOG_INFO(L"LightBoost: target display EDID=%s rr=%u VTotal=%u HTotal=%u",
                            edid_key.c_str(), cur.etc.rr, cur.VTotal, cur.HTotal);
            std::string base = NvTimingsDb::ToUtf8(edid_key);
            auto entry = db.FindByBaseAndRefresh(base, cur.etc.rr);
            if (!entry) entry = db.FindHighestRefreshForBase(base);
            if (!entry) {
                NV3D_LOG_INFO(L"LightBoost: EDID '%hs' not in DB — no LightBoost", base.c_str());
                target_found = true;
                break;
            }
            target_found = true;
            display_ids_.push_back(id);
            matched_ = *entry;
            matched_.refresh_int  = entry->refresh_int ? entry->refresh_int : cur.etc.rr;
            matched_.monitor_EDID = base;
            original_target_timing_     = cur;
            has_original_target_timing_ = true;
            const bool h_eq = (cur.HTotal == entry->timing.HTotal);
            const bool v_eq = (cur.VTotal == entry->timing.VTotal);
            already_good = h_eq && v_eq;
            break;
        }
    }
    if (!target_found || display_ids_.empty() || already_good) {
        if (already_good)
            NV3D_LOG_INFO(L"LightBoost: timings already match DB — no change");
        return already_good;
    }

    // Surround/Mosaic incompatibility.
    {
        NV_MOSAIC_TOPO_BRIEF tb{};      tb.version = NVAPI_MOSAIC_TOPO_BRIEF_VER;
        NV_MOSAIC_DISPLAY_SETTING ds{}; ds.version = NVAPI_MOSAIC_DISPLAY_SETTING_VER;
        NvS32 ox = 0, oy = 0;
        if (NvAPI_Mosaic_GetCurrentTopo(&tb, &ds, &ox, &oy) == NVAPI_OK && tb.enabled) {
            NV3D_LOG_WARN(L"LightBoost: Surround/Mosaic active — skipped");
            return false;
        }
    }

    // Merge DB-sourced timing with live polarity / etc fields.
    NV_TIMING merged = matched_.timing;
    {
        NV_TIMING       live{};
        NV_TIMING_INPUT ti{}; ti.version = NV_TIMING_INPUT_VER;
        if (NvAPI_DISP_GetTiming(display_ids_.front(), &ti, &live) == NVAPI_OK) {
            merged.HSyncPol   = live.HSyncPol;
            merged.VSyncPol   = live.VSyncPol;
            merged.interlaced = live.interlaced;
            merged.etc        = live.etc;
            merged.etc.rr     = matched_.timing.etc.rr;
            merged.etc.rrx1k  = matched_.timing.etc.rrx1k;
        }
    }

    NV_CUSTOM_DISPLAY cd{};
    cd.version      = NV_CUSTOM_DISPLAY_VER;
    cd.timing       = merged;
    cd.srcPartition = { 0.0f, 0.0f, 1.0f, 1.0f };
    cd.width        = merged.HVisible;
    cd.height       = merged.VVisible;
    cd.colorFormat  = NV_FORMAT_A8R8G8B8;
    cd.xRatio       = 1.0f;
    cd.yRatio       = 1.0f;
    cd.depth        = 32;

    std::vector<NV_CUSTOM_DISPLAY> arr{ cd };
    NvAPI_Status s = NvAPI_DISP_TryCustomDisplay(display_ids_.data(),
                                                   static_cast<NvU32>(display_ids_.size()),
                                                   arr.data());
    if (s != NVAPI_OK) {
        NV3D_LOG_WARN(L"LightBoost: TryCustomDisplay failed s=%d — reverting", s);
        NvAPI_DISP_RevertCustomDisplayTrial(display_ids_.data(),
                                              static_cast<NvU32>(display_ids_.size()));
        WaitForModesetSettle(500);
        return false;
    }

    WaitForModesetSettle(200);
    if (has_original_target_timing_)
        WaitForTimingChange(display_ids_.front(), original_target_timing_, 10000);

    // Commit the validated trial timing to NVCP's persistent custom-display
    // registry. Without this, the timing would revert on reboot (or on any
    // RevertCustomDisplayTrial call). Saving has several payoffs:
    //   * LightBoost stays applied across reboots — users typically want this
    //     on their 3DV panel permanently rather than re-enabling each session.
    //   * No modeset / flicker on host shutdown (Disable is a no-op).
    //   * Removes the post-D3D9-release ChangeDisplaySettingsExW fallback path
    //     that was a contributor to the post-quit display-freeze pattern.
    // (true, true) = per-output-id + per-monitor-id, matching the NvAPI SDK
    // CustomTiming sample's parameters. To remove the saved timing, the user
    // can delete the entry from NVIDIA Control Panel's custom resolutions.
    NvAPI_Status saved = NvAPI_DISP_SaveCustomDisplay(
        display_ids_.data(),
        static_cast<NvU32>(display_ids_.size()),
        /*isThisOutputIdOnly=*/  TRUE,
        /*isThisMonitorIdOnly=*/ TRUE);
    if (saved != NVAPI_OK) {
        NV3D_LOG_WARN(L"LightBoost: SaveCustomDisplay s=%d — timing applied for this session "
                       L"only (will revert on reboot)", saved);
    } else {
        NV3D_LOG_INFO(L"LightBoost: SaveCustomDisplay OK — timing persisted to NVCP");
    }

    // Sync GDI's display mode to the new wire timing.
    if (has_original_devmode_ && matched_.refresh_int > 0) {
        DEVMODEW dm = original_devmode_;
        dm.dmDisplayFrequency = matched_.refresh_int;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
        LONG rc = ChangeDisplaySettingsExW(gdi_device_w.c_str(), &dm,
                                             nullptr, CDS_FULLSCREEN, nullptr);
        if (rc == DISP_CHANGE_SUCCESSFUL) WaitForModesetSettle(500);
        else NV3D_LOG_WARN(L"LightBoost: ChangeDisplaySettingsExW rc=%ld", rc);
    }

    enabled_ = true;
    NV3D_LOG_INFO(L"LightBoost: applied custom timing");
    return true;
}

}  // namespace NV3D
