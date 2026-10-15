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

#pragma once

#include "openxr/openxr.h"
#include "triangle_mesh.h"
#include "utils/handle.h"
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace xr
{
class instance;
class session;

class passthrough_layer_fb : public utils::handle<XrPassthroughLayerFB>
{
public:
	passthrough_layer_fb(instance &, session &, const XrPassthroughLayerCreateInfoFB &);
};

class passthrough_fb : public utils::handle<XrPassthroughFB>
{
	PFN_xrPassthroughStartFB xrPassthroughStartFB{};
	PFN_xrPassthroughPauseFB xrPassthroughPauseFB{};
	PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB{};
	PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB{};
	PFN_xrPassthroughLayerSetStyleFB xrPassthroughLayerSetStyleFB{};

	passthrough_layer_fb passthrough_layer;
	XrCompositionLayerPassthroughFB composition_layer;

	// Surface-projected passthrough (XR_FB_triangle_mesh, Quest only).
	// The projected layer shares the XrPassthroughFB above; the runtime
	// projects camera imagery onto the supplied triangle geometry instead
	// of doing full-environment reconstruction. Dormant until a mesh is
	// set (no content this branch).
	instance * inst = nullptr;
	session * sess = nullptr;
	bool projected_supported = false;
	std::optional<passthrough_layer_fb> projected_layer;
	XrCompositionLayerPassthroughFB projected_composition_layer{};
	std::optional<triangle_mesh_fb> projected_mesh;
	std::optional<geometry_instance_fb> projected_geometry;

public:
	passthrough_fb(instance &, session &);

	void start();
	void pause();
	XrCompositionLayerBaseHeader * layer()
	{
		return (XrCompositionLayerBaseHeader *)&composition_layer;
	}

	// Forget any chained structs from a previous frame's submit. Our
	// chains (alpha blend, settings) live in per-frame storage while this
	// layer lives in the session: a stale next pointer would otherwise
	// dangle after the frame ends.
	void reset_chain()
	{
		composition_layer.next = nullptr;
	}
	void reset_projected_chain()
	{
		projected_composition_layer.next = nullptr;
	}

	// True when the runtime supports surface-projected passthrough
	bool projected_mesh_supported() const
	{
		return projected_supported;
	}

	// (Re)create the projected mesh from baked world-space geometry and
	// bind it to the projected layer at the given initial transform
	void set_projected_mesh(
	        std::span<const XrVector3f> vertices,
	        std::span<const uint32_t> indices,
	        XrSpace base_space,
	        const XrPosef & pose,
	        const XrVector3f & scale);
	void clear_projected_mesh();

	bool has_projected_mesh() const
	{
		return projected_geometry.has_value();
	}

	void update_projected_transform(XrSpace base_space, XrTime time, const XrPosef & pose, const XrVector3f & scale);

	XrCompositionLayerBaseHeader * projected_layer_header()
	{
		return (XrCompositionLayerBaseHeader *)&projected_composition_layer;
	}

	// Per-object geometry instances on the projected layer (mask-path
	// surface projection: one mesh + instance per flagged object, all
	// sharing the layer above). Managed per frame from placed objects.
	struct projected_instance
	{
		std::optional<triangle_mesh_fb> mesh;
		std::optional<geometry_instance_fb> geometry;
	};
	void set_projected_instance_mesh(const std::string & key,
	                                 std::span<const XrVector3f> vertices,
	                                 std::span<const uint32_t> indices,
	                                 XrSpace base_space,
	                                 const XrPosef & pose,
	                                 const XrVector3f & scale);
	void remove_projected_instance(const std::string & key);
	void clear_projected_instances();
	bool has_projected_instance(const std::string & key) const;
	bool has_projected_instances() const;
	void update_projected_instance_transform(const std::string & key,
	                                         XrSpace base_space,
	                                         XrTime time,
	                                         const XrPosef & pose,
	                                         const XrVector3f & scale);

private:
	std::map<std::string, projected_instance> projected_instances;
};

class passthrough_htc : public utils::handle<XrPassthroughHTC>
{
	XrCompositionLayerPassthroughHTC composition_layer;

public:
	passthrough_htc(instance &, session &);
	XrCompositionLayerBaseHeader * layer()
	{
		return (XrCompositionLayerBaseHeader *)&composition_layer;
	}
};

class passthrough_alpha_blend
{
};

using passthrough = std::variant<std::monostate, passthrough_fb, passthrough_htc, passthrough_alpha_blend>;

} // namespace xr
