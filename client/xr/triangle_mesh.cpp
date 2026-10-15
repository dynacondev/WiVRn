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

#include "triangle_mesh.h"
#include "openxr/openxr.h"
#include "session.h"
#include "xr/check.h"
#include "xr/instance.h"

xr::triangle_mesh_fb::triangle_mesh_fb(instance & inst, session & s, std::span<const XrVector3f> vertices, std::span<const uint32_t> indices, XrWindingOrderFB winding) :
        handle(inst.get_proc<PFN_xrDestroyTriangleMeshFB>("xrDestroyTriangleMeshFB"))
{
	if (vertices.empty() or indices.empty() or indices.size() % 3 != 0)
		throw std::runtime_error("triangle_mesh_fb: need non-empty vertex/index buffers with a multiple of 3 indices");

	PFN_xrCreateTriangleMeshFB xrCreateTriangleMeshFB = inst.get_proc<PFN_xrCreateTriangleMeshFB>("xrCreateTriangleMeshFB");

	XrTriangleMeshCreateInfoFB info{
	        .type = XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB,
	        .flags = 0,
	        .windingOrder = winding,
	        .vertexCount = (uint32_t)vertices.size(),
	        .vertexBuffer = vertices.data(),
	        .triangleCount = (uint32_t)(indices.size() / 3),
	        .indexBuffer = indices.data(),
	};

	CHECK_XR(xrCreateTriangleMeshFB(s, &info, &id));
}

xr::geometry_instance_fb::geometry_instance_fb(instance & inst, session & s, XrPassthroughLayerFB layer, XrTriangleMeshFB mesh, XrSpace base_space, const XrPosef & pose, const XrVector3f & scale) :
        handle(inst.get_proc<PFN_xrDestroyGeometryInstanceFB>("xrDestroyGeometryInstanceFB"))
{
	PFN_xrCreateGeometryInstanceFB xrCreateGeometryInstanceFB = inst.get_proc<PFN_xrCreateGeometryInstanceFB>("xrCreateGeometryInstanceFB");
	xrGeometryInstanceSetTransformFB = inst.get_proc<PFN_xrGeometryInstanceSetTransformFB>("xrGeometryInstanceSetTransformFB");

	XrGeometryInstanceCreateInfoFB info{
	        .type = XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB,
	        .layer = layer,
	        .mesh = mesh,
	        .baseSpace = base_space,
	        .pose = pose,
	        .scale = scale,
	};

	CHECK_XR(xrCreateGeometryInstanceFB(s, &info, &id));
}

void xr::geometry_instance_fb::set_transform(XrSpace base_space, XrTime time, const XrPosef & pose, const XrVector3f & scale)
{
	XrGeometryInstanceTransformFB transform{
	        .type = XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB,
	        .baseSpace = base_space,
	        .time = time,
	        .pose = pose,
	        .scale = scale,
	};

	CHECK_XR(xrGeometryInstanceSetTransformFB(id, &transform));
}
