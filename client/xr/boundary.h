/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>
#include <span>
#include <openxr/openxr.h>

namespace xr
{
// Custom boundary zones (3d-boundary / 3d-boundary-reversed): a global
// scene fade from the headset distance to placed closed-volume meshes.
// One scalar per frame for both eyes (from the view-space head point);
// the mask submit lifts the flat union's floor by (1 - opacity), so no
// extra layers or swapchains in the common case.
//
// Meshes must be closed volumes: inside/outside comes from ray parity,
// which is undefined for open meshes (documented, not enforced).
struct boundary_query
{
	float signed_m = 0;   // + inside, - outside (world meters)
	float distance_m = 0; // unsigned distance to the closest surface point
	bool inside = false;
	bool valid = false; // false for empty/degenerate soups or bad inputs
};

// Signed distance from a world-space point to a placed uniform-scaled
// rigid soup (world = pose * (scale * local)). Single pass over the
// triangles: min point-triangle distance + 3-axis majority-vote parity.
// Cost is linear in triangle count: room-size boxes are noise, document
// a ~5k-triangle budget for detailed zones.
boundary_query boundary_signed_distance(
        std::span<const XrVector3f> vertices,
        std::span<const uint32_t> indices,
        const XrPosef & world_pose,
        float world_scale,
        float hx,
        float hy,
        float hz);

// Symmetric smoothstep over [-transition_m, +transition_m] of the signed
// distance (0.5 at the surface, full game at +t, full passthrough at -t
// for a normal zone); transition_m <= 0 is a hard step at the surface.
float boundary_smoothstep(float signed_m, float transition_m);

// Union combine: game visible iff inside ANY normal zone AND outside ALL
// reversed zones. Feed per-object opacities (already smoothstepped,
// reversed objects as 1 - ss); with no normal objects the normals term
// is 1 (reversed-only configs still work).
class boundary_mix
{
public:
	void add(float object_opacity, bool reversed);
	float result() const;

private:
	float normals_ = -1; // max over normal zones, -1 = none yet
	float global_ = 1;   // min with every reversed term
};

// In-app self-test (Passthrough tab): unit-cube inside/outside/surface,
// scaled placement, smoothstep shape, and combine truth table. Logs
// PASS/FAIL lines like the board solver self-test.
bool boundary_selftest();
} // namespace xr
