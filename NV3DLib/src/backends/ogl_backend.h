#pragma once

#ifndef NV3DLIB_DISABLE_OGL

#include "NV3D.hpp"
#include "async_presenter.h"
#include "d3d9_presenter.h"
#include "nv_3dvision_suppressor.h"
#include "present_window.h"

#include <memory>

namespace NV3D {

struct OGLBackendState;  // hides GL/WGL typedefs from the header

// OpenGL → D3D9 DIRECT via WGL_NV_DX_interop2.
//
// Init:
//   - PresentWindow + D3D9Presenter.
//   - Resolve WGL_NV_DX_interop2 extension pointers via wglGetProcAddress.
//   - wglDXOpenDeviceNV(d3d9_device).
//
// First SetInputTexture (or on dim change):
//   - Create D3D9 RT surface sized to host SbS (BGRA, A8R8G8B8).
//   - glGenTextures → register the D3D9 surface against the GL name via
//     wglDXRegisterObjectNV.
//
// Per Present:
//   - wglDXLockObjectsNV (GL takes ownership of the aliased texture).
//   - Use an FBO blit (glBlitFramebuffer) to copy host_tex → aliased tex,
//     applying Y-flip if `flip_y` was set.
//   - wglDXUnlockObjectsNV (D3D9 takes back ownership).
//   - presenter->Present(d3d9_surface).
class OGLBackend final : public InterfaceOGL {
public:
    HRESULT Init(HGLRC ctx, HDC dc, const InitParams& params);
    HRESULT SetInputTexture(NV3DGLuint sbs_tex, int width, int height, bool flip_y) override;
    HRESULT Present() override;
    void    Delete() override;

private:
    InitParams params_{};
    HGLRC      host_ctx_ = nullptr;
    HDC        host_dc_  = nullptr;
    std::unique_ptr<PresentWindow> window_;
    std::unique_ptr<D3D9Presenter> presenter_;
    Nv3DVisionSuppressor           suppressor_;

    NV3DGLuint input_tex_ = 0;
    int        input_w_   = 0;
    int        input_h_   = 0;
    bool       flip_y_    = true;

    std::unique_ptr<OGLBackendState> state_;

    // SPLIT ASYNC. Only the D3D9Presenter::Present call runs on the worker;
    // the GL-interop work (wglDXLockObjectsNV → glBlitFramebuffer →
    // wglDXUnlockObjectsNV) stays on the caller's thread because a GL
    // context can be current on at most one thread at a time and the host
    // owns theirs.
    AsyncPresenter                  async_;
};

}  // namespace NV3D

#endif  // !NV3DLIB_DISABLE_OGL
