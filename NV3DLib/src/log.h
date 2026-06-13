#pragma once

#include "NV3D.hpp"

namespace NV3D {

void LogV(LogLevel level, const wchar_t* fmt, ...);

#define NV3D_LOG_DEBUG(...) ::NV3D::LogV(::NV3D::LogLevel::Debug,   __VA_ARGS__)
#define NV3D_LOG_INFO(...)  ::NV3D::LogV(::NV3D::LogLevel::Info,    __VA_ARGS__)
#define NV3D_LOG_WARN(...)  ::NV3D::LogV(::NV3D::LogLevel::Warning, __VA_ARGS__)
#define NV3D_LOG_ERROR(...) ::NV3D::LogV(::NV3D::LogLevel::Error,   __VA_ARGS__)

}  // namespace NV3D
