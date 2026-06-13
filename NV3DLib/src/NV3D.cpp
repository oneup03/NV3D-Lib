/*
 * NV3DLib — library translation unit. Per-backend factories live in their
 * own .cpp files (backends/*.cpp), each gated by NV3DLIB_DISABLE_<API>.
 * This TU exists so the library always has at least one object file even
 * when all backends are disabled at build time.
 */
#include "NV3D.hpp"
