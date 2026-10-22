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
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace xr
{
class instance;
class session;

// Demo passthrough color effects (Quest, XR_FB_passthrough +
// XR_META_passthrough_color_lut). Session-scoped desired state, applied to
// both flat (reconstruction) and projected layers via
// xrPassthroughLayerSetStyleFB. Only one color struct in the Style next
// chain at a time (runtime validation), so the GUI exposes a mode radio.
enum class passthrough_effect_mode
{
	off,
	bcs,
	mono_rgba,
	lut,
	interpolated_lut,
};

enum class passthrough_mono_preset
{
	gray,
	thermal,
	posterize,
};

enum class passthrough_lut_preset
{
	identity,
	sepia,
	invert,
	high_contrast,
	night_vision,
};

struct passthrough_effects
{
	bool enabled = false; // fullscreen demo passthrough (like depth overlay)
	float opacity = 1.0f; // XrPassthroughStyleFB::textureOpacityFactor [0,1]
	float edge_alpha = 0.0f; // edge overlay strength [0,1], 0 disables
	// Edge tint rgb (alpha comes from edge_alpha above)
	float edge_r = 1.0f;
	float edge_g = 1.0f;
	float edge_b = 1.0f;

	float brightness = 0.0f; // BCS: [-100,100] def 0
	float contrast = 1.0f; // BCS: [0,inf] def 1
	float saturation = 1.0f; // BCS: [0,inf] def 1

	passthrough_effect_mode mode = passthrough_effect_mode::bcs;
	passthrough_mono_preset mono_preset = passthrough_mono_preset::thermal;

	passthrough_lut_preset lut_preset = passthrough_lut_preset::sepia;
	passthrough_lut_preset lut_preset_b = passthrough_lut_preset::invert;
	float lut_weight = 1.0f; // Lut: blend original->LUT [0,1]
	float lut_mix = 0.5f; // Interpolated: blend LUT A->B [0,1]
};

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

#ifdef XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME
	PFN_xrCreatePassthroughColorLutMETA xrCreatePassthroughColorLutMETA{};
	PFN_xrDestroyPassthroughColorLutMETA xrDestroyPassthroughColorLutMETA{};
#endif
	bool lut_supported = false;

	struct lut_entry
	{
		XrPassthroughColorLutMETA handle = XR_NULL_HANDLE;
		std::vector<uint8_t> data;
		uint32_t resolution = 0;
	};
	std::map<std::string, lut_entry> lut_cache;
	bool lut_warn_once = false;

	XrPassthroughColorLutMETA get_or_create_lut(passthrough_lut_preset preset, uint32_t resolution);
	// Build + apply XrPassthroughStyleFB chain for both layers.
	// LUT handles are cached per preset; missing LUT ext falls back to
	// unstyled with a one-shot warn.
	void apply_style(const passthrough_effects & fx);

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
	~passthrough_fb();
	passthrough_fb(passthrough_fb &&) noexcept = default;
	passthrough_fb & operator=(passthrough_fb &&) noexcept = default;
	passthrough_fb(const passthrough_fb &) = delete;
	passthrough_fb & operator=(const passthrough_fb &) = delete;

	void start();
	void pause();

	// Demo color effects: apply desired style to flat + projected layers.
	// Safe to call when LUT ext is missing (LUT modes fall back to plain
	// style with a warn). Called by session on enable + on GUI change.
	void set_effects(const passthrough_effects & fx);
	bool color_lut_supported() const
	{
		return lut_supported;
	}

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
