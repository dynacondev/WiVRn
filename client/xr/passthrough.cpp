/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "passthrough.h"
#include "openxr/openxr.h"
#include "session.h"
#include "xr/check.h"
#include "xr/instance.h"
#include <spdlog/spdlog.h>

xr::passthrough_layer_fb::passthrough_layer_fb(instance & inst, session & s, const XrPassthroughLayerCreateInfoFB & info) :
        handle(inst.get_proc<PFN_xrDestroyPassthroughLayerFB>("xrDestroyPassthroughLayerFB"))
{
	PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB = inst.get_proc<PFN_xrCreatePassthroughLayerFB>("xrCreatePassthroughLayerFB");
	CHECK_XR(xrCreatePassthroughLayerFB(s, &info, &id));
}

static XrPassthroughFB create_passthrough_fb(xr::instance & inst, xr::session & s)
{
	PFN_xrCreatePassthroughFB xrCreatePassthroughFB = inst.get_proc<PFN_xrCreatePassthroughFB>("xrCreatePassthroughFB");

	XrPassthroughFB id;
	XrPassthroughCreateInfoFB info{
	        .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB,
	        .flags = 0,
	};

	CHECK_XR(xrCreatePassthroughFB(s, &info, &id));
	return id;
}

xr::passthrough_fb::passthrough_fb(instance & inst, session & s) :
        handle(create_passthrough_fb(inst, s), inst.get_proc<PFN_xrDestroyPassthroughFB>("xrDestroyPassthroughFB")),
        passthrough_layer(inst, s, XrPassthroughLayerCreateInfoFB{
                                           .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB,
                                           .passthrough = id,
                                           .flags = 0,
                                           .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB,
                                   }),
        inst(&inst),
        sess(&s),
        projected_supported(inst.has_extension(XR_FB_TRIANGLE_MESH_EXTENSION_NAME))
{
	xrPassthroughStartFB = inst.get_proc<PFN_xrPassthroughStartFB>("xrPassthroughStartFB");
	xrPassthroughPauseFB = inst.get_proc<PFN_xrPassthroughPauseFB>("xrPassthroughPauseFB");
	xrPassthroughLayerPauseFB = inst.get_proc<PFN_xrPassthroughLayerPauseFB>("xrPassthroughLayerPauseFB");
	xrPassthroughLayerResumeFB = inst.get_proc<PFN_xrPassthroughLayerResumeFB>("xrPassthroughLayerResumeFB");

	composition_layer = XrCompositionLayerPassthroughFB{
	        .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
	        .flags = 0,
	        .space = XR_NULL_HANDLE,
	        .layerHandle = passthrough_layer,
	};

	if (projected_supported)
	{
		projected_layer.emplace(inst, s, XrPassthroughLayerCreateInfoFB{
		                                        .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB,
		                                        .passthrough = id,
		                                        .flags = 0,
		                                        .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB,
		                                });
		projected_composition_layer = XrCompositionLayerPassthroughFB{
		        .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
		        .flags = 0,
		        .space = XR_NULL_HANDLE,
		        .layerHandle = *projected_layer,
		};
	}
	else
		spdlog::info("XR_FB_triangle_mesh not available, surface-projected passthrough disabled");

	start();
}

void xr::passthrough_fb::start()
{
	CHECK_XR(xrPassthroughStartFB(id));
	CHECK_XR(xrPassthroughLayerResumeFB(passthrough_layer));
	if (projected_layer)
		CHECK_XR(xrPassthroughLayerResumeFB(*projected_layer));
}

void xr::passthrough_fb::pause()
{
	if (projected_layer)
		CHECK_XR(xrPassthroughLayerPauseFB(*projected_layer));
	CHECK_XR(xrPassthroughLayerPauseFB(passthrough_layer));
	CHECK_XR(xrPassthroughPauseFB(id));
}

void xr::passthrough_fb::set_projected_mesh(
        std::span<const XrVector3f> vertices,
        std::span<const uint32_t> indices,
        XrSpace base_space,
        const XrPosef & pose,
        const XrVector3f & scale)
{
	if (not projected_supported or not projected_layer)
		throw std::runtime_error("Surface-projected passthrough not supported by the runtime");

	// Destroy the old geometry first: the instance references the mesh
	projected_geometry.reset();
	projected_mesh.reset();

	projected_mesh.emplace(*inst, *sess, vertices, indices);
	projected_geometry.emplace(*inst, *sess, *projected_layer, *projected_mesh, base_space, pose, scale);

	spdlog::info("Projected passthrough mesh set: {} vertices, {} triangles", vertices.size(), indices.size() / 3);
}

void xr::passthrough_fb::clear_projected_mesh()
{
	projected_geometry.reset();
	projected_mesh.reset();
}

void xr::passthrough_fb::update_projected_transform(XrSpace base_space, XrTime time, const XrPosef & pose, const XrVector3f & scale)
{
	if (projected_geometry)
		projected_geometry->set_transform(base_space, time, pose, scale);
}

xr::passthrough_htc::passthrough_htc(instance & inst, session & s) :
        handle(inst.get_proc<PFN_xrDestroyPassthroughHTC>("xrDestroyPassthroughHTC"))
{
	PFN_xrCreatePassthroughHTC xrCreatePassthroughHTC = inst.get_proc<PFN_xrCreatePassthroughHTC>("xrCreatePassthroughHTC");

	XrPassthroughCreateInfoHTC info{
	        .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_HTC,
	        .form = XR_PASSTHROUGH_FORM_PLANAR_HTC,
	};

	CHECK_XR(xrCreatePassthroughHTC(s, &info, &id));

	composition_layer = XrCompositionLayerPassthroughHTC{
	        .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC,
	        .layerFlags = 0,
	        .space = XR_NULL_HANDLE,
	        .passthrough = id,
	        .color = {
	                .type = XR_TYPE_PASSTHROUGH_COLOR_HTC,
	                .alpha = 1,
	        },
	};
}
