#include "backends/ogl_backend.h"

#ifndef NV3DLIB_DISABLE_OGL

#include <GL/gl.h>

#include "log.h"

namespace NV3D {

namespace {

// We inline the GL/WGL extension declarations we need so the lib doesn't
// pull in glcorearb.h / wglext.h. Consumers only need to link opengl32.lib.
using GLsizeiptr = ptrdiff_t;
constexpr GLenum kGL_FRAMEBUFFER             = 0x8D40;
constexpr GLenum kGL_READ_FRAMEBUFFER        = 0x8CA8;
constexpr GLenum kGL_DRAW_FRAMEBUFFER        = 0x8CA9;
constexpr GLenum kGL_COLOR_ATTACHMENT0       = 0x8CE0;
constexpr GLenum kGL_FRAMEBUFFER_COMPLETE    = 0x8CD5;
constexpr GLenum kGL_TEXTURE_2D              = 0x0DE1;
constexpr GLenum kGL_RGBA                    = 0x1908;
constexpr GLenum kGL_RGBA8                   = 0x8058;
constexpr GLenum kGL_UNSIGNED_BYTE           = 0x1401;
constexpr GLenum kGL_NEAREST                 = 0x2600;
constexpr GLenum kGL_LINEAR                  = 0x2601;
constexpr GLenum kGL_TEXTURE_MIN_FILTER      = 0x2801;
constexpr GLenum kGL_TEXTURE_MAG_FILTER      = 0x2800;
constexpr GLbitfield kGL_COLOR_BUFFER_BIT    = 0x00004000;
constexpr GLenum kWGL_ACCESS_WRITE_DISCARD_NV = 0x00000002;

// WGL_NV_DX_interop2 prototypes
using PFN_wglDXOpenDeviceNV       = HANDLE (WINAPI*)(void*);
using PFN_wglDXCloseDeviceNV      = BOOL   (WINAPI*)(HANDLE);
using PFN_wglDXRegisterObjectNV   = HANDLE (WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PFN_wglDXUnregisterObjectNV = BOOL   (WINAPI*)(HANDLE, HANDLE);
using PFN_wglDXLockObjectsNV      = BOOL   (WINAPI*)(HANDLE, GLint, HANDLE*);
using PFN_wglDXUnlockObjectsNV    = BOOL   (WINAPI*)(HANDLE, GLint, HANDLE*);

// FBO / blit prototypes
using PFN_glGenFramebuffers       = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteFramebuffers    = void (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBindFramebuffer       = void (APIENTRY*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D  = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glBlitFramebuffer       = void (APIENTRY*)(GLint, GLint, GLint, GLint,
                                                       GLint, GLint, GLint, GLint,
                                                       GLbitfield, GLenum);
using PFN_glCheckFramebufferStatus = GLenum (APIENTRY*)(GLenum);

PROC ResolveGL(HMODULE m, const char* name) {
    PROC p = wglGetProcAddress(name);
    if ((!p || reinterpret_cast<intptr_t>(p) == 1 || reinterpret_cast<intptr_t>(p) == 2 ||
         reinterpret_cast<intptr_t>(p) == 3 || reinterpret_cast<intptr_t>(p) == -1) && m) {
        p = GetProcAddress(m, name);
    }
    return p;
}

}  // anonymous

struct OGLBackendState {
    // GL/WGL function pointers (resolved at Init under the host's context).
    PFN_wglDXOpenDeviceNV       wglDXOpenDeviceNV       = nullptr;
    PFN_wglDXCloseDeviceNV      wglDXCloseDeviceNV      = nullptr;
    PFN_wglDXRegisterObjectNV   wglDXRegisterObjectNV   = nullptr;
    PFN_wglDXUnregisterObjectNV wglDXUnregisterObjectNV = nullptr;
    PFN_wglDXLockObjectsNV      wglDXLockObjectsNV      = nullptr;
    PFN_wglDXUnlockObjectsNV    wglDXUnlockObjectsNV    = nullptr;
    PFN_glGenFramebuffers       glGenFramebuffers       = nullptr;
    PFN_glDeleteFramebuffers    glDeleteFramebuffers    = nullptr;
    PFN_glBindFramebuffer       glBindFramebuffer       = nullptr;
    PFN_glFramebufferTexture2D  glFramebufferTexture2D  = nullptr;
    PFN_glBlitFramebuffer       glBlitFramebuffer       = nullptr;
    PFN_glCheckFramebufferStatus glCheckFramebufferStatus = nullptr;

    // Interop state.
    HANDLE dxgl_device = nullptr;
    HANDLE dxgl_object = nullptr;

    // GL-side aliased texture + the source/dest FBOs used by the blit.
    GLuint aliased_tex = 0;
    GLuint fbo_src     = 0;   // host_tex bound
    GLuint fbo_dst     = 0;   // aliased_tex bound

    // D3D9 RT that backs aliased_tex.
    Microsoft::WRL::ComPtr<IDirect3DTexture9> d3d9_target_tex;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> d3d9_target_sfc;
    uint32_t target_w = 0;
    uint32_t target_h = 0;
};

namespace {

// RAII helper for save/restore of the host GL context — Present() may be
// called from a thread where some other context is current.
struct ScopedGLContext {
    HDC   prev_dc  = nullptr;
    HGLRC prev_ctx = nullptr;
    bool  switched = false;
    ScopedGLContext(HDC target_dc, HGLRC target_ctx) {
        prev_dc  = wglGetCurrentDC();
        prev_ctx = wglGetCurrentContext();
        if (prev_dc != target_dc || prev_ctx != target_ctx) {
            switched = wglMakeCurrent(target_dc, target_ctx) ? true : false;
            if (!switched) {
                NV3D_LOG_ERROR(L"OGLBackend: wglMakeCurrent failed (GLE=%lu)", GetLastError());
            }
        }
    }
    ~ScopedGLContext() {
        if (switched) wglMakeCurrent(prev_dc, prev_ctx);
    }
};

bool ResolveAllFunctions(OGLBackendState& s) {
    HMODULE opengl32 = GetModuleHandleW(L"opengl32.dll");

    #define R(field, name) \
        s.field = reinterpret_cast<decltype(s.field)>(ResolveGL(opengl32, #name)); \
        if (!s.field) { NV3D_LOG_ERROR(L"OGLBackend: %hs unresolved", #name); return false; }

    R(wglDXOpenDeviceNV,       wglDXOpenDeviceNV)
    R(wglDXCloseDeviceNV,      wglDXCloseDeviceNV)
    R(wglDXRegisterObjectNV,   wglDXRegisterObjectNV)
    R(wglDXUnregisterObjectNV, wglDXUnregisterObjectNV)
    R(wglDXLockObjectsNV,      wglDXLockObjectsNV)
    R(wglDXUnlockObjectsNV,    wglDXUnlockObjectsNV)
    R(glGenFramebuffers,       glGenFramebuffers)
    R(glDeleteFramebuffers,    glDeleteFramebuffers)
    R(glBindFramebuffer,       glBindFramebuffer)
    R(glFramebufferTexture2D,  glFramebufferTexture2D)
    R(glBlitFramebuffer,       glBlitFramebuffer)
    R(glCheckFramebufferStatus, glCheckFramebufferStatus)
    #undef R
    return true;
}

}  // anonymous

HRESULT OGLBackend::Init(HGLRC ctx, HDC dc, const InitParams& params) {
    if (!ctx || !dc) return E_POINTER;
    params_   = params;
    host_ctx_ = ctx;
    host_dc_  = dc;
    state_    = std::make_unique<OGLBackendState>();

    // Bring up the present window + D3D9 device first; we need D3D9Ex to
    // hand to wglDXOpenDeviceNV.
    window_ = std::make_unique<PresentWindow>();
    PresentWindowConfig wcfg{};
    wcfg.target_monitor = params.target_monitor;
    wcfg.host_hwnd      = params.host_hwnd;
    wcfg.on_top         = params.on_top;
    wcfg.title          = L"NV3DLib (OpenGL)";
    if (!window_->Init(wcfg)) {
        NV3D_LOG_ERROR(L"OGLBackend: PresentWindow::Init failed");
        return E_FAIL;
    }
    presenter_ = std::make_unique<D3D9Presenter>();
    if (!presenter_->Init(window_.get(), params)) {
        NV3D_LOG_ERROR(L"OGLBackend: D3D9Presenter::Init failed");
        return E_FAIL;
    }

    // Resolve extensions under the host's GL context.
    {
        ScopedGLContext _(host_dc_, host_ctx_);
        if (!ResolveAllFunctions(*state_)) {
            NV3D_LOG_ERROR(L"OGLBackend: WGL_NV_DX_interop2 / FBO functions missing — "
                            L"driver does not advertise the required extensions");
            return E_FAIL;
        }
        state_->dxgl_device = state_->wglDXOpenDeviceNV(presenter_->Device());
        if (!state_->dxgl_device) {
            NV3D_LOG_ERROR(L"OGLBackend: wglDXOpenDeviceNV failed (GLE=%lu)", GetLastError());
            return E_FAIL;
        }
    }

    if (params_.enable_suppressor) suppressor_.Install();

    // Split-async present worker (only the D3D9 Present runs off-thread —
    // the GL-interop work above it stays on the calling thread because the
    // host's GL context can only be current on one thread at a time).
    async_.SetOnSeh([this](DWORD /*code*/) {
        if (presenter_) presenter_->CheckAndMarkD3D9Dead(D3DERR_DEVICEHUNG, "async worker SEH");
    });
    async_.Start();
    return S_OK;
}

HRESULT OGLBackend::SetInputTexture(NV3DGLuint sbs_tex, int width, int height, bool flip_y) {
    if (sbs_tex == 0 || width <= 0 || height <= 0) return E_INVALIDARG;
    input_tex_ = sbs_tex;
    input_w_   = width;
    input_h_   = height;
    flip_y_    = flip_y;
    return S_OK;
}

HRESULT OGLBackend::Present() {
    if (!presenter_ || presenter_->IsDead()) return E_FAIL;
    if (input_tex_ == 0) return E_NOT_VALID_STATE;

    ScopedGLContext _(host_dc_, host_ctx_);

    // Lazily build / resize the D3D9 RT + GL alias on first frame or input
    // size change.
    if (!state_->d3d9_target_sfc || state_->target_w != static_cast<uint32_t>(input_w_)
        || state_->target_h != static_cast<uint32_t>(input_h_)) {
        // Tear down old alias (with the GL ctx current).
        if (state_->dxgl_object && state_->dxgl_device) {
            state_->wglDXUnregisterObjectNV(state_->dxgl_device, state_->dxgl_object);
            state_->dxgl_object = nullptr;
        }
        if (state_->aliased_tex) {
            glDeleteTextures(1, &state_->aliased_tex);
            state_->aliased_tex = 0;
        }
        state_->d3d9_target_sfc.Reset();
        state_->d3d9_target_tex.Reset();

        // Create the D3D9 RT at host dimensions (BGRA8 / X8R8G8B8).
        HRESULT hr = presenter_->Device()->CreateTexture(
            static_cast<uint32_t>(input_w_), static_cast<uint32_t>(input_h_),
            1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
            &state_->d3d9_target_tex, nullptr);
        if (FAILED(hr)) {
            NV3D_LOG_ERROR(L"OGLBackend: D3D9 CreateTexture(RT) failed hr=0x%08X", hr);
            return hr;
        }
        hr = state_->d3d9_target_tex->GetSurfaceLevel(0, &state_->d3d9_target_sfc);
        if (FAILED(hr)) return hr;

        // Generate GL alias and register it.
        glGenTextures(1, &state_->aliased_tex);
        glBindTexture(kGL_TEXTURE_2D, state_->aliased_tex);
        glTexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, kGL_LINEAR);
        glTexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, kGL_LINEAR);
        glBindTexture(kGL_TEXTURE_2D, 0);

        state_->dxgl_object = state_->wglDXRegisterObjectNV(
            state_->dxgl_device,
            state_->d3d9_target_tex.Get(),
            state_->aliased_tex,
            kGL_TEXTURE_2D,
            kWGL_ACCESS_WRITE_DISCARD_NV);
        if (!state_->dxgl_object) {
            NV3D_LOG_ERROR(L"OGLBackend: wglDXRegisterObjectNV failed (GLE=%lu)", GetLastError());
            return E_FAIL;
        }

        // FBOs (one for source/host tex, one for dest/aliased).
        if (!state_->fbo_src) state_->glGenFramebuffers(1, &state_->fbo_src);
        if (!state_->fbo_dst) state_->glGenFramebuffers(1, &state_->fbo_dst);

        state_->target_w = input_w_;
        state_->target_h = input_h_;
        NV3D_LOG_INFO(L"OGLBackend: aliased %dx%d D3D9 RT via WGL_NV_DX_interop2", input_w_, input_h_);
    }

    // Acquire ownership of the aliased texture from D3D9 to GL.
    if (!state_->wglDXLockObjectsNV(state_->dxgl_device, 1, &state_->dxgl_object)) {
        NV3D_LOG_ERROR(L"OGLBackend: wglDXLockObjectsNV failed (GLE=%lu)", GetLastError());
        return E_FAIL;
    }

    // Blit host_tex → aliased_tex. Y is flipped via the destination rect
    // (dy0 > dy1) when flip_y=true.
    state_->glBindFramebuffer(kGL_READ_FRAMEBUFFER, state_->fbo_src);
    state_->glFramebufferTexture2D(kGL_READ_FRAMEBUFFER,
                                    kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D,
                                    input_tex_, 0);
    state_->glBindFramebuffer(kGL_DRAW_FRAMEBUFFER, state_->fbo_dst);
    state_->glFramebufferTexture2D(kGL_DRAW_FRAMEBUFFER,
                                    kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D,
                                    state_->aliased_tex, 0);
    const GLint dy0 = flip_y_ ? input_h_ : 0;
    const GLint dy1 = flip_y_ ? 0 : input_h_;
    state_->glBlitFramebuffer(0, 0, input_w_, input_h_,
                                0, dy0, input_w_, dy1,
                                kGL_COLOR_BUFFER_BIT, kGL_NEAREST);
    // Detach + unbind so we don't leak refs to host_tex / aliased_tex.
    state_->glFramebufferTexture2D(kGL_READ_FRAMEBUFFER,
                                    kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D, 0, 0);
    state_->glFramebufferTexture2D(kGL_DRAW_FRAMEBUFFER,
                                    kGL_COLOR_ATTACHMENT0, kGL_TEXTURE_2D, 0, 0);
    state_->glBindFramebuffer(kGL_READ_FRAMEBUFFER, 0);
    state_->glBindFramebuffer(kGL_DRAW_FRAMEBUFFER, 0);

    // Return ownership to D3D9. Failure here is fatal — leaving GL holding
    // the alias would block the D3D9 StretchRect that's about to read it.
    if (!state_->wglDXUnlockObjectsNV(state_->dxgl_device, 1, &state_->dxgl_object)) {
        NV3D_LOG_ERROR(L"OGLBackend: wglDXUnlockObjectsNV failed (GLE=%lu) — D3D9 read will race",
                        GetLastError());
        return E_FAIL;
    }

    // SPLIT ASYNC. Everything above ran on the host's GL thread (the
    // ScopedGLContext is about to go out of scope and restore whatever the
    // host had current). Only the D3D9 StretchRect + vsync'd PresentEx
    // moves to the worker — the D3D9 device is ours (MULTITHREADED), so
    // the worker can safely drive it from a different thread.
    Microsoft::WRL::ComPtr<IDirect3DSurface9> snap_sfc = state_->d3d9_target_sfc;
    const uint32_t snap_w = static_cast<uint32_t>(input_w_);
    const uint32_t snap_h = static_cast<uint32_t>(input_h_);
    return async_.Submit([this, snap_sfc, snap_w, snap_h]() {
        return presenter_->Present(snap_sfc.Get(), snap_w, snap_h);
    });
}

void OGLBackend::Delete() {
    // Teardown ordering mirrors DX11 backend: drain worker, suppressor
    // first, GL/D3D9 child resources next, presenter, then window.
    async_.Stop();
    suppressor_.Uninstall();

    const bool dead = presenter_ && presenter_->IsDead();

    if (state_) {
        // GL-side cleanup needs the host context current.
        // If presenter is dead the wglDX* calls might still work (they're
        // CPU-side bookkeeping), but be defensive and Detach D3D9 refs.
        ScopedGLContext _(host_dc_, host_ctx_);
        if (state_->dxgl_object && state_->dxgl_device && state_->wglDXUnregisterObjectNV) {
            state_->wglDXUnregisterObjectNV(state_->dxgl_device, state_->dxgl_object);
            state_->dxgl_object = nullptr;
        }
        if (state_->aliased_tex) {
            glDeleteTextures(1, &state_->aliased_tex);
            state_->aliased_tex = 0;
        }
        if (state_->fbo_src && state_->glDeleteFramebuffers) {
            state_->glDeleteFramebuffers(1, &state_->fbo_src);
            state_->fbo_src = 0;
        }
        if (state_->fbo_dst && state_->glDeleteFramebuffers) {
            state_->glDeleteFramebuffers(1, &state_->fbo_dst);
            state_->fbo_dst = 0;
        }
        if (state_->dxgl_device && state_->wglDXCloseDeviceNV) {
            state_->wglDXCloseDeviceNV(state_->dxgl_device);
            state_->dxgl_device = nullptr;
        }
        if (dead) {
            (void)state_->d3d9_target_sfc.Detach();
            (void)state_->d3d9_target_tex.Detach();
        } else {
            state_->d3d9_target_sfc.Reset();
            state_->d3d9_target_tex.Reset();
        }
    }

    presenter_.reset();
    window_.reset();
    state_.reset();
    delete this;
}

extern "C" HRESULT CreateInterfaceOGL(HGLRC gl_context, HDC gl_dc,
                                       const InitParams* params,
                                       InterfaceOGL** out) {
    if (!gl_context || !gl_dc || !params || !out) return E_POINTER;
    auto* impl = new OGLBackend();
    HRESULT hr = impl->Init(gl_context, gl_dc, *params);
    if (FAILED(hr)) {
        impl->Delete();
        return hr;
    }
    *out = impl;
    return S_OK;
}

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_OGL
