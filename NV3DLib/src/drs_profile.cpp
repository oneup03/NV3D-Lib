#include "drs_profile.h"

#include <excpt.h>
#include <delayimp.h>
#include <string>

#include "log.h"

#include <nvapi.h>
#include <NvApiDriverSettings.h>

#ifndef VcppException
#define VcppException(sev, err) ((sev) | 0x40000000 | (FACILITY_VISUALCPP << 16) | (err))
#endif

namespace NV3D {

namespace {

// The only profile name this module will ever create, edit, or delete.
constexpr wchar_t kProfileName[] = L"NV3DLib Fast Sync";

// SEH guard around NvAPI_Initialize — same pattern as d3d9_presenter's
// TryNvAPIInitializeSEH. In the usual link (nvapi64.lib's static thunks) a
// missing DLL surfaces as NVAPI_LIBRARY_NOT_FOUND, because the lib LoadLibrary's
// nvapi64.dll itself and never puts it in the import table. The filter is here
// for the build where it IS delay-imported, which faults instead of returning.
bool NvApiReady() {
    __try {
        return NvAPI_Initialize() == NVAPI_OK;
    } __except (
        (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
         GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
            ? EXCEPTION_EXECUTE_HANDLER
            : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// NvAPI_UnicodeString is NvU16[2048]; wchar_t is 16-bit on Windows, so this is
// a straight element copy into the fixed-size field.
void SetNvStr(NvU16* dst, const wchar_t* src) {
    size_t i = 0;
    for (; src[i] && i < NVAPI_UNICODE_STRING_MAX - 1; ++i) {
        dst[i] = static_cast<NvU16>(src[i]);
    }
    dst[i] = 0;
}

std::wstring FromNvStr(const NvU16* src) {
    std::wstring out;
    for (size_t i = 0; i < NVAPI_UNICODE_STRING_MAX && src[i]; ++i) {
        out.push_back(static_cast<wchar_t>(src[i]));
    }
    return out;
}

// Basename of this process's image, e.g. L"vrserver.exe". DRS matches
// applications on the leaf file name, not the full path.
std::wstring SelfExeName() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const wchar_t* leaf = wcsrchr(path, L'\\');
    return leaf ? leaf + 1 : path;
}

struct SessionScope {
    NvDRSSessionHandle h = nullptr;
    ~SessionScope() { if (h) NvAPI_DRS_DestroySession(h); }
};

}  // namespace


FastSyncResult ApplyFastSyncProfile(bool enable, const wchar_t* exe_name) {
    if (!NvApiReady()) {
        NV3D_LOG_INFO(L"FastSync: NVAPI unavailable — no driver profile change");
        return FastSyncResult::Unavailable;
    }

    const std::wstring exe = (exe_name && *exe_name) ? exe_name : SelfExeName();
    if (exe.empty()) {
        NV3D_LOG_WARN(L"FastSync: could not determine executable name");
        return FastSyncResult::Failed;
    }

    SessionScope ses;
    NvAPI_Status s = NvAPI_DRS_CreateSession(&ses.h);
    if (s != NVAPI_OK || !ses.h) {
        NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_CreateSession failed code=%d", static_cast<int>(s));
        return FastSyncResult::Failed;
    }
    s = NvAPI_DRS_LoadSettings(ses.h);
    if (s != NVAPI_OK) {
        NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_LoadSettings failed code=%d", static_cast<int>(s));
        return FastSyncResult::Failed;
    }

    // Our profile, if a previous run left one behind.
    NvAPI_UnicodeString wanted_name{};
    SetNvStr(wanted_name, kProfileName);
    NvDRSProfileHandle ours = nullptr;
    const bool have_ours =
        (NvAPI_DRS_FindProfileByName(ses.h, wanted_name, &ours) == NVAPI_OK) && ours;

    // ---- Removal path -----------------------------------------------------
    // Deleting the whole profile is safe precisely BECAUSE we created it: it
    // holds nothing but our own application entry and our own setting, so its
    // removal cannot disturb anything the user or NVIDIA configured.
    if (!enable) {
        if (!have_ours) return FastSyncResult::NotPresent;
        s = NvAPI_DRS_DeleteProfile(ses.h, ours);
        if (s != NVAPI_OK) {
            NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_DeleteProfile failed code=%d", static_cast<int>(s));
            return FastSyncResult::Failed;
        }
        s = NvAPI_DRS_SaveSettings(ses.h);
        if (s != NVAPI_OK) {
            NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_SaveSettings failed code=%d", static_cast<int>(s));
            return FastSyncResult::Failed;
        }
        NV3D_LOG_INFO(L"FastSync: removed profile [%s] (was for %s)", kProfileName, exe.c_str());
        return FastSyncResult::Removed;
    }

    // ---- Enable path ------------------------------------------------------
    // Refuse to fight for the executable. DRS allows an application to belong
    // to exactly one profile, and NVIDIA ships predefined profiles covering a
    // great many executables. If anything other than our own profile claims
    // this one, say which and change nothing — editing a profile we do not own
    // means silently overwriting the user's (or NVIDIA's) vsync choice with no
    // record of what it was.
    {
        NvAPI_UnicodeString app_name{};
        SetNvStr(app_name, exe.c_str());
        NvDRSProfileHandle owner = nullptr;
        NVDRS_APPLICATION found{};
        found.version = NVDRS_APPLICATION_VER;
        if (NvAPI_DRS_FindApplicationByName(ses.h, app_name, &owner, &found) == NVAPI_OK &&
            owner && owner != ours) {
            NVDRS_PROFILE pi{};
            pi.version = NVDRS_PROFILE_VER;
            std::wstring owner_name = L"<unknown>";
            if (NvAPI_DRS_GetProfileInfo(ses.h, owner, &pi) == NVAPI_OK) {
                owner_name = FromNvStr(pi.profileName);
            }
            NV3D_LOG_WARN(L"FastSync: %s already belongs to driver profile [%s] — leaving it "
                          L"alone. Set Vertical Sync = Fast there by hand if you want Fast "
                          L"Sync for it.",
                          exe.c_str(), owner_name.c_str());
            return FastSyncResult::Conflict;
        }
    }

    if (!have_ours) {
        NVDRS_PROFILE pi{};
        pi.version = NVDRS_PROFILE_VER;
        SetNvStr(pi.profileName, kProfileName);
        s = NvAPI_DRS_CreateProfile(ses.h, &pi, &ours);
        if (s != NVAPI_OK || !ours) {
            NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_CreateProfile failed code=%d", static_cast<int>(s));
            return FastSyncResult::Failed;
        }
    }

    // Idempotent: CreateApplication on an entry we already own returns
    // NVAPI_EXECUTABLE_ALREADY_IN_USE, which at this point can only mean
    // "already in OUR profile" — the conflict check above ruled out any other
    // owner.
    {
        NVDRS_APPLICATION app{};
        app.version      = NVDRS_APPLICATION_VER;
        app.isPredefined = 0;
        SetNvStr(app.appName, exe.c_str());
        SetNvStr(app.userFriendlyName, exe.c_str());
        s = NvAPI_DRS_CreateApplication(ses.h, ours, &app);
        if (s != NVAPI_OK && s != NVAPI_EXECUTABLE_ALREADY_IN_USE) {
            NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_CreateApplication(%s) failed code=%d",
                          exe.c_str(), static_cast<int>(s));
            return FastSyncResult::Failed;
        }
    }

    // Was the value already in place? This is the difference between "Fast
    // Sync is live in THIS process" and "Fast Sync will be live next launch":
    // the driver samples a profile when a process/device initializes, so a
    // value written now generally does not apply to the already-running
    // process that wrote it.
    bool already = false;
    {
        NVDRS_SETTING cur{};
        cur.version = NVDRS_SETTING_VER;
        if (NvAPI_DRS_GetSetting(ses.h, ours, VSYNCMODE_ID, &cur) == NVAPI_OK) {
            already = (cur.u32CurrentValue == VSYNCMODE_VIRTUAL);
        }
    }

    if (already) {
        NV3D_LOG_INFO(L"FastSync: profile [%s] already sets Vertical Sync = Fast for %s — "
                      L"active for this process",
                      kProfileName, exe.c_str());
        return FastSyncResult::AlreadyActive;
    }

    NVDRS_SETTING set{};
    set.version         = NVDRS_SETTING_VER;
    set.settingId       = VSYNCMODE_ID;
    set.settingType     = NVDRS_DWORD_TYPE;
    set.u32CurrentValue = VSYNCMODE_VIRTUAL;   // "Vertical Sync = Fast"
    s = NvAPI_DRS_SetSetting(ses.h, ours, &set);
    if (s != NVAPI_OK) {
        NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_SetSetting(VSYNCMODE) failed code=%d",
                      static_cast<int>(s));
        return FastSyncResult::Failed;
    }
    s = NvAPI_DRS_SaveSettings(ses.h);
    if (s != NVAPI_OK) {
        NV3D_LOG_WARN(L"FastSync: NvAPI_DRS_SaveSettings failed code=%d", static_cast<int>(s));
        return FastSyncResult::Failed;
    }
    NV3D_LOG_INFO(L"FastSync: profile [%s] now sets Vertical Sync = Fast for %s. The driver "
                  L"samples profiles at process start, so this most likely takes effect on "
                  L"the NEXT launch, not this one.",
                  kProfileName, exe.c_str());
    return FastSyncResult::Applied;
}


extern "C" FastSyncResult SetFastSyncProfile(bool enable, const wchar_t* exe_name) {
    return ApplyFastSyncProfile(enable, exe_name);
}

}  // namespace NV3D
