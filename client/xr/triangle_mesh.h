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

#include "openxr/openxr.h"
#include "utils/handle.h"
#include <span>

namespace xr
{
class instance;
class session;

// RAII wrapper for XrTriangleMeshFB (XR_FB_triangle_mesh).
// The mesh holds baked world-space vertices; the per-frame pose is applied
// through the geometry instance, not by updating the mesh.
class triangle_mesh_fb : public utils::handle<XrTriangleMeshFB>
{
public:
	triangle_mesh_fb(instance &, session &, std::span<const XrVector3f> vertices, std::span<const uint32_t> indices, XrWindingOrderFB winding = XR_WINDING_ORDER_CCW_FB);
};

// RAII wrapper for XrGeometryInstanceFB: binds a triangle mesh to a
// projected passthrough layer with a base space + pose + scale.
// The transform is updated every frame via set_transform().
class geometry_instance_fb : public utils::handle<XrGeometryInstanceFB>
{
	PFN_xrGeometryInstanceSetTransformFB xrGeometryInstanceSetTransformFB{};

public:
	geometry_instance_fb(instance &, session &, XrPassthroughLayerFB layer, XrTriangleMeshFB mesh, XrSpace base_space, const XrPosef & pose, const XrVector3f & scale);

	void set_transform(XrSpace base_space, XrTime time, const XrPosef & pose, const XrVector3f & scale);
};
} // namespace xr
