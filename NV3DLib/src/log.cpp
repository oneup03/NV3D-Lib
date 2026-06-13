#include "log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>

namespace NV3D {

namespace {
LogSinkFn g_sink = nullptr;
void*     g_sink_user = nullptr;

void DefaultSink(LogLevel level, const wchar_t* message, void* /*user*/) {
    const wchar_t* prefix = L"[NV3D] ";
    switch (level) {
        case LogLevel::Debug:   prefix = L"[NV3D][D] "; break;
        case LogLevel::Info:    prefix = L"[NV3D][I] "; break;
        case LogLevel::Warning: prefix = L"[NV3D][W] "; break;
        case LogLevel::Error:   prefix = L"[NV3D][E] "; break;
    }
    OutputDebugStringW(prefix);
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}
}  // anonymous

void LogV(LogLevel level, const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    (void)n;
    if (g_sink) g_sink(level, buf, g_sink_user);
    else        DefaultSink(level, buf, nullptr);
}

extern "C" void SetLogSink(LogSinkFn sink, void* user) {
    g_sink = sink;
    g_sink_user = user;
}

}  // namespace NV3D
