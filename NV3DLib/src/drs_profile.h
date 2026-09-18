/*
 * NVIDIA Driver Settings Repository (DRS) profile management.
 *
 * Currently one concern: a Fast Sync ("Vertical Sync = Fast") profile for a
 * named executable. Kept separate from d3d9_presenter because it touches
 * PERSISTENT system state that outlives any interface, and because a host
 * must be able to call it with no interface in existence at all — e.g. to
 * REMOVE the profile when the user switched to an output path that doesn't
 * want it.
 */
#pragma once

#include <NV3D.hpp>

namespace NV3D {

// Implementation behind the public SetFastSyncProfile(). See NV3D.hpp for the
// contract and the safety rules.
FastSyncResult ApplyFastSyncProfile(bool enable, const wchar_t* exe_name);

}  // namespace NV3D
