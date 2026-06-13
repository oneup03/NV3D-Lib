// OGLSample: minimal NV3DLib OpenGL consumer.
//
// Creates an invisible OpenGL context, uploads a side-by-side test pattern
// into a GL texture each frame, and hands it to NV3DLib. Identical visual
// contract to the DX11 sample — left eye red gradient, right eye green
// gradient, centered animated quad with depth disparity.

#include <Windows.h>
#include <GL/gl.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "NV3D.hpp"

#pragma comment(lib, "opengl32.lib")

namespace {

constexpr int kWidth  = 2560;
constexpr int kHeight = 720;

void FillTestPattern(uint8_t* px, int frame) {
    const int per_eye = kWidth / 2;
    const int cx = per_eye / 2;
    const int cy = kHeight / 2;
    const int disparity = static_cast<int>(20 + 15 * sinf(frame * 0.04f));
    const int quad_half = 40;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            uint8_t* p = px + (y * kWidth + x) * 4;
            const bool right_eye = (x >= per_eye);
            const int lx = right_eye ? (x - per_eye) : x;
            // GL_BGRA upload order means we write B,G,R,A here.
            if (!right_eye) {
                p[0] = 0; p[1] = 0;
                p[2] = static_cast<uint8_t>(lx * 255 / per_eye);
                p[3] = 0xFF;
            } else {
                p[0] = 0;
                p[1] = static_cast<uint8_t>(lx * 255 / per_eye);
                p[2] = 0; p[3] = 0xFF;
            }
            int qx = lx - cx + (right_eye ? -disparity : disparity);
            int qy = y - cy;
            if (qx >= -quad_half && qx <= quad_half &&
                qy >= -quad_half && qy <= quad_half) {
                p[0] = p[1] = p[2] = 0xFF;
            }
        }
    }
}

FILE* g_log_file = nullptr;

void LogSink(NV3D::LogLevel level, const wchar_t* msg, void*) {
    const wchar_t* lvl = L"";
    switch (level) {
        case NV3D::LogLevel::Debug:   lvl = L"D"; break;
        case NV3D::LogLevel::Info:    lvl = L"I"; break;
        case NV3D::LogLevel::Warning: lvl = L"W"; break;
        case NV3D::LogLevel::Error:   lvl = L"E"; break;
    }
    wprintf(L"[NV3D][%s] %s\n", lvl, msg);
    fflush(stdout);
    if (g_log_file) {
        fwprintf(g_log_file, L"[NV3D][%s] %s\n", lvl, msg);
        fflush(g_log_file);
    }
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

constexpr GLenum kGL_BGRA            = 0x80E1;
constexpr GLenum kGL_UNSIGNED_BYTE   = 0x1401;
constexpr GLenum kGL_TEXTURE_2D      = 0x0DE1;
constexpr GLenum kGL_RGBA8           = 0x8058;
constexpr GLenum kGL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum kGL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum kGL_LINEAR          = 0x2601;

}  // anonymous

int wmain(int, wchar_t**) {
    _wfopen_s(&g_log_file, L"oglsample.log", L"w, ccs=UTF-16LE");
    NV3D::SetLogSink(LogSink, nullptr);

    // Invisible host window + GL context.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DummyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NV3DLib_OGLSample_Host";
    RegisterClassExW(&wc);
    HWND host_hwnd = CreateWindowExW(0, wc.lpszClassName, L"NV3DLib OGLSample (host, hidden)",
                                       WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                                       nullptr, nullptr, wc.hInstance, nullptr);
    HDC host_dc = GetDC(host_hwnd);

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.iLayerType   = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(host_dc, &pfd);
    if (!pf || !SetPixelFormat(host_dc, pf, &pfd)) {
        wprintf(L"OGLSample: ChoosePixelFormat / SetPixelFormat failed\n");
        return 1;
    }
    HGLRC host_ctx = wglCreateContext(host_dc);
    if (!host_ctx || !wglMakeCurrent(host_dc, host_ctx)) {
        wprintf(L"OGLSample: wglCreateContext / wglMakeCurrent failed\n");
        return 1;
    }

    // Upload-target texture.
    GLuint sbs_tex = 0;
    glGenTextures(1, &sbs_tex);
    glBindTexture(kGL_TEXTURE_2D, sbs_tex);
    glTexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MIN_FILTER, kGL_LINEAR);
    glTexParameteri(kGL_TEXTURE_2D, kGL_TEXTURE_MAG_FILTER, kGL_LINEAR);
    std::vector<uint8_t> staging(static_cast<size_t>(kWidth) * kHeight * 4u);
    glTexImage2D(kGL_TEXTURE_2D, 0, kGL_RGBA8, kWidth, kHeight, 0,
                  kGL_BGRA, kGL_UNSIGNED_BYTE, staging.data());

    // Init NV3DLib.
    NV3D::InitParams p{};
    p.enable_lightboost = true;
    p.on_top            = true;
    NV3D::InterfaceOGL* nv3d = nullptr;
    HRESULT hr = NV3D::CreateInterfaceOGL(host_ctx, host_dc, &p, &nv3d);
    if (FAILED(hr) || !nv3d) {
        wprintf(L"CreateInterfaceOGL failed hr=0x%08X\n", hr);
        return 1;
    }

    wprintf(L"OGLSample: running. Ctrl+C to exit.\n");
    fflush(stdout);

    auto next = std::chrono::steady_clock::now();
    for (int frame = 0; ; ++frame) {
        next += std::chrono::milliseconds(16);

        FillTestPattern(staging.data(), frame);
        glBindTexture(kGL_TEXTURE_2D, sbs_tex);
        // BGRA upload: byte order matches our CPU fill above.
        // glTexImage2D recreates the storage — slower but simpler than glTexSubImage2D.
        glTexImage2D(kGL_TEXTURE_2D, 0, kGL_RGBA8, kWidth, kHeight, 0,
                      kGL_BGRA, kGL_UNSIGNED_BYTE, staging.data());

        nv3d->SetInputTexture(sbs_tex, kWidth, kHeight, /*flip_y=*/false);
        HRESULT phr = nv3d->Present();
        if (FAILED(phr)) {
            wprintf(L"Present failed hr=0x%08X\n", phr);
            break;
        }

        std::this_thread::sleep_until(next);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { nv3d->Delete(); return 0; }
        }
    }

    nv3d->Delete();
    return 0;
}
