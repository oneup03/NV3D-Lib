/*
 * Ported from VRto3D — see header for the full rationale of each hook.
 */
#include "nv_3dvision_suppressor.h"

#include <cstdint>
#include <cstring>

#include <Windows.h>
#include <intrin.h>

#include <MinHook.h>

#include "log.h"

#pragma intrinsic(_ReturnAddress)

namespace NV3D {

namespace {

// Walk the PE headers of an already-loaded module to find the .text section.
bool GetTextSection(HMODULE mod, uint8_t** out_base, size_t* out_size) {
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text\0\0\0", 8) == 0) {
            *out_base = base + sec->VirtualAddress;
            *out_size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

uint8_t* ScanSignature(uint8_t* start, size_t size,
                        const uint8_t* sig, const bool* mask, size_t sig_len) {
    if (size < sig_len) return nullptr;
    const size_t end = size - sig_len;
    for (size_t i = 0; i <= end; ++i) {
        bool match = true;
        for (size_t j = 0; j < sig_len; ++j) {
            if (mask[j] && start[i + j] != sig[j]) { match = false; break; }
        }
        if (match) return start + i;
    }
    return nullptr;
}

const uint8_t* g_nvd3dumx_text_start = nullptr;
const uint8_t* g_nvd3dumx_text_end   = nullptr;

// ---------------------------------------------------------------------------
// Hook 1: OSD warnings dispatcher (FUN_1802C4850 in current driver builds).
// ---------------------------------------------------------------------------
constexpr uint8_t kSigDispatcher[] = {
    0x40, 0x55, 0x56,
    0x48, 0x8D, 0xAC, 0x24, 0x78, 0xFC, 0xFF, 0xFF,
    0x48, 0x81, 0xEC, 0x88, 0x04, 0x00, 0x00,
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x33, 0xC4,
    0x48, 0x89, 0x85, 0x50, 0x03, 0x00, 0x00,
    0x48, 0x8B, 0xF1,
    0x48, 0x8D, 0x4C, 0x24, 0x38,
    0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x48, 0xB8, 0x4B, 0x59, 0x86, 0x38,
    0xD6, 0xC5, 0x6D, 0x34,
};
constexpr bool kMaskDispatcher[] = {
    true, true, true,
    true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true,
    true, true, true,
    false, false, false, false,
    true, true, true,
    true, true, true, true, true, true, true,
    true, true, true,
    true, true, true, true, true,
    true, true,
    false, false, false, false,
    true, true, true, true, true, true,
    true, true, true, true,
};
static_assert(sizeof(kSigDispatcher) == sizeof(kMaskDispatcher) / sizeof(bool),
              "Dispatcher sig/mask length mismatch");

using NvOsdDispatch_t = void(__fastcall*)(LONGLONG);
NvOsdDispatch_t g_original_dispatcher = nullptr;

// Bit 0  — Depth Amount slider
// Bit 10 — "non-stereo display mode" warning
constexpr uint32_t kSuppressedOsdBits = (1u << 0) | (1u << 10);

void __fastcall NvOsdDispatchDetour(LONGLONG param_1) {
    if (param_1) {
        auto& warning_bits = *reinterpret_cast<uint32_t*>(param_1 + 600);
        warning_bits &= ~kSuppressedOsdBits;
    }
    if (g_original_dispatcher) g_original_dispatcher(param_1);
}

// ---------------------------------------------------------------------------
// Hook 2: rating / info overlay (FUN_180284160 in current driver builds).
// ---------------------------------------------------------------------------
constexpr uint8_t kSigRating[] = {
    0x48, 0x89, 0x5C, 0x24, 0x20,
    0x55,
    0x56,
    0x57,
    0x41, 0x54,
    0x41, 0x55,
    0x41, 0x56,
    0x41, 0x57,
    0x48, 0x8D, 0xAC, 0x24, 0x60, 0xED, 0xFF, 0xFF,
    0xB8, 0xA0, 0x13, 0x00, 0x00,
    0xE8, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x2B, 0xE0,
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x33, 0xC4,
    0x48, 0x89, 0x85, 0x90, 0x12, 0x00, 0x00,
    0x80, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00,
};
constexpr bool kMaskRating[] = {
    true, true, true, true, true,
    true,
    true,
    true,
    true, true,
    true, true,
    true, true,
    true, true,
    true, true, true, true, true, true, true, true,
    true, true, true, true, true,
    true, false, false, false, false,
    true, true, true,
    true, true, true, false, false, false, false,
    true, true, true,
    true, true, true, true, true, true, true,
    true, true, false, false, false, false, true,
};
static_assert(sizeof(kSigRating) == sizeof(kMaskRating) / sizeof(bool),
              "Rating sig/mask length mismatch");

using NvRatingOverlay_t = void(__fastcall*)(void*, void*, unsigned int);
NvRatingOverlay_t g_original_rating = nullptr;

void __fastcall NvRatingOverlayDetour(void*, void*, unsigned int) {
    // Intentionally empty — drop the overlay.
}

// ---------------------------------------------------------------------------
// Hook 3: user32!GetAsyncKeyState hotkey blocker.
// ---------------------------------------------------------------------------
constexpr int kHotkeyBlocklist[] = {
    VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F10, VK_F11,
};

using GetAsyncKeyState_t = SHORT (WINAPI*)(int);
GetAsyncKeyState_t g_original_GetAsyncKeyState = nullptr;

SHORT WINAPI NvGetAsyncKeyStateDetour(int vKey) {
    GetAsyncKeyState_t orig = g_original_GetAsyncKeyState;
    if (!orig) return 0;
    auto* ret = static_cast<const uint8_t*>(_ReturnAddress());
    const bool from_nvd3dumx =
        g_nvd3dumx_text_start && g_nvd3dumx_text_end &&
        ret >= g_nvd3dumx_text_start && ret < g_nvd3dumx_text_end;
    if (from_nvd3dumx) {
        for (int v : kHotkeyBlocklist) {
            if (vKey != v) continue;
            SHORT ctrl = orig(VK_CONTROL);
            if (ctrl & 0x8000) return 0;
            break;
        }
    }
    return orig(vKey);
}

struct HookSpec {
    const char*    name;
    const uint8_t* sig;
    const bool*    mask;
    size_t         sig_len;
    const wchar_t* api_module;
    const char*    api_proc;
    void*  detour;
    void** original_out;
};

const HookSpec kHooks[] = {
    {
        "OSD warnings dispatcher",
        kSigDispatcher, kMaskDispatcher, sizeof(kSigDispatcher),
        nullptr, nullptr,
        reinterpret_cast<void*>(NvOsdDispatchDetour),
        reinterpret_cast<void**>(&g_original_dispatcher),
    },
    {
        "rating / info overlay",
        kSigRating, kMaskRating, sizeof(kSigRating),
        nullptr, nullptr,
        reinterpret_cast<void*>(NvRatingOverlayDetour),
        reinterpret_cast<void**>(&g_original_rating),
    },
    {
        "GetAsyncKeyState hotkey blocker",
        nullptr, nullptr, 0,
        L"user32.dll", "GetAsyncKeyState",
        reinterpret_cast<void*>(NvGetAsyncKeyStateDetour),
        reinterpret_cast<void**>(&g_original_GetAsyncKeyState),
    },
};
static_assert(sizeof(kHooks) / sizeof(kHooks[0])
                  == Nv3DVisionSuppressor::kHookCount,
              "Hook spec count must match kHookCount in the header.");

}  // namespace

Nv3DVisionSuppressor::~Nv3DVisionSuppressor() {
    Uninstall();
}

bool Nv3DVisionSuppressor::Install() {
    if (installed_) return true;

    HMODULE nv_mod = GetModuleHandleW(L"nvd3dumx.dll");
    if (!nv_mod) {
        NV3D_LOG_INFO(L"Nv3DVisionSuppressor: nvd3dumx.dll not loaded yet — hooks skipped");
        return false;
    }
    uint8_t* nv_text_base = nullptr;
    size_t   nv_text_size = 0;
    if (!GetTextSection(nv_mod, &nv_text_base, &nv_text_size)) {
        NV3D_LOG_ERROR(L"Nv3DVisionSuppressor: could not locate .text in nvd3dumx.dll");
        return false;
    }
    g_nvd3dumx_text_start = nv_text_base;
    g_nvd3dumx_text_end   = nv_text_base + nv_text_size;

    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            NV3D_LOG_ERROR(L"Nv3DVisionSuppressor: MH_Initialize failed status=%d", s);
            return false;
        }
    }

    int installed_count = 0;
    for (int i = 0; i < kHookCount; ++i) {
        const HookSpec& spec = kHooks[i];
        void* target = nullptr;
        if (spec.sig) {
            target = ScanSignature(nv_text_base, nv_text_size,
                                     spec.sig, spec.mask, spec.sig_len);
        } else if (spec.api_proc) {
            HMODULE m = GetModuleHandleW(spec.api_module);
            if (m) target = reinterpret_cast<void*>(GetProcAddress(m, spec.api_proc));
        }
        if (!target) {
            NV3D_LOG_WARN(L"Nv3DVisionSuppressor: target %hs not resolved", spec.name);
            continue;
        }
        MH_STATUS s = MH_CreateHook(target, spec.detour,
                                      reinterpret_cast<LPVOID*>(spec.original_out));
        if (s != MH_OK) {
            NV3D_LOG_WARN(L"Nv3DVisionSuppressor: MH_CreateHook(%hs) failed status=%d", spec.name, s);
            continue;
        }
        s = MH_EnableHook(target);
        if (s != MH_OK) {
            NV3D_LOG_WARN(L"Nv3DVisionSuppressor: MH_EnableHook(%hs) failed status=%d", spec.name, s);
            MH_RemoveHook(target);
            *spec.original_out = nullptr;
            continue;
        }
        installed_targets_[i] = target;
        ++installed_count;
        NV3D_LOG_INFO(L"Nv3DVisionSuppressor: hooked %hs at %p", spec.name, target);
    }

    if (installed_count == 0) {
        g_nvd3dumx_text_start = nullptr;
        g_nvd3dumx_text_end   = nullptr;
        MH_Uninitialize();
        return false;
    }

    installed_ = true;
    NV3D_LOG_INFO(L"Nv3DVisionSuppressor: %d of %d suppression hooks active",
                    installed_count, kHookCount);
    return true;
}

void Nv3DVisionSuppressor::Uninstall() {
    if (!installed_) return;
    for (int i = 0; i < kHookCount; ++i) {
        if (installed_targets_[i]) {
            MH_DisableHook(installed_targets_[i]);
            MH_RemoveHook(installed_targets_[i]);
            installed_targets_[i] = nullptr;
        }
        *kHooks[i].original_out = nullptr;
    }
    g_nvd3dumx_text_start = nullptr;
    g_nvd3dumx_text_end   = nullptr;
    installed_ = false;
    MH_Uninitialize();
    NV3D_LOG_INFO(L"Nv3DVisionSuppressor: suppression hooks removed");
}

}  // namespace NV3D
