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
#include <algorithm>
#include <cmath>
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
	xrPassthroughLayerSetStyleFB = inst.get_proc<PFN_xrPassthroughLayerSetStyleFB>("xrPassthroughLayerSetStyleFB");

#ifdef XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME
	if (inst.has_extension(XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME))
	{
		try
		{
			xrCreatePassthroughColorLutMETA = inst.get_proc<PFN_xrCreatePassthroughColorLutMETA>("xrCreatePassthroughColorLutMETA");
			xrDestroyPassthroughColorLutMETA = inst.get_proc<PFN_xrDestroyPassthroughColorLutMETA>("xrDestroyPassthroughColorLutMETA");
			xrUpdatePassthroughColorLutMETA = inst.get_proc<PFN_xrUpdatePassthroughColorLutMETA>("xrUpdatePassthroughColorLutMETA");
			lut_supported = xrCreatePassthroughColorLutMETA && xrDestroyPassthroughColorLutMETA;
			if (lut_supported)
				spdlog::info("Passthrough color LUT functions loaded");
		}
		catch (std::exception & e)
		{
			spdlog::warn("Passthrough color LUT proc lookup failed: {}", e.what());
			lut_supported = false;
		}
	}
#endif

	composition_layer = XrCompositionLayerPassthroughFB{
	        .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
	        .next = nullptr,
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
		        .next = nullptr,
		        // Non-zero flags are required by the composition-layer
		        // VUID (unlike the layer-create struct, where 0 is
		        // legal), and signal that the layer carries meaningful
		        // alpha: without it the compositor may treat the layer
		        // as opaque, hiding video behind off-geometry pixels.
		        // The flat layer keeps flags=0 (proven tolerated); do
		        // not "harmonize" this.
		        .flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
		        .space = XR_NULL_HANDLE,
		        .layerHandle = *projected_layer,
		};
		// Explicit style: the spec states no default, so pin full
		// opacity with edge rendering disabled (alpha 0) rather than
		// relying on whatever the runtime assumes for an unstyled layer.
		XrPassthroughStyleFB projected_style{
		        .type = XR_TYPE_PASSTHROUGH_STYLE_FB,
		        .next = nullptr,
		        .textureOpacityFactor = 1.0f,
		        .edgeColor = {0.f, 0.f, 0.f, 0.f},
		};
		// Warn-only: a throw here inside enable_passthrough()'s emplace
		// would leave the passthrough variant valueless_by_exception and
		// poison every later frame (bad_variant_access skips end_frame,
		// surfacing as CALL_ORDER_INVALID). Flat default below is
		// already warn-only; keep both consistent.
		if (auto res = xrPassthroughLayerSetStyleFB(*projected_layer, &projected_style); res != XR_SUCCESS)
			spdlog::warn("Default projected passthrough style failed: {}", (int)res);
	}
	else
		spdlog::info("XR_FB_triangle_mesh not available, surface-projected passthrough disabled");

	// Pin flat layer to the same explicit default so later style swaps
	// have a known baseline on both layers.
	{
		XrPassthroughStyleFB flat_style{
		        .type = XR_TYPE_PASSTHROUGH_STYLE_FB,
		        .next = nullptr,
		        .textureOpacityFactor = 1.0f,
		        .edgeColor = {0.f, 0.f, 0.f, 0.f},
		};
		if (auto res = xrPassthroughLayerSetStyleFB(passthrough_layer, &flat_style); res != XR_SUCCESS)
			spdlog::warn("Default flat passthrough style failed: {}", (int)res);
	}

	start();
}

xr::passthrough_fb::~passthrough_fb()
{
#ifdef XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME
	if (xrDestroyPassthroughColorLutMETA)
	{
		for (auto & [key, entry]: lut_cache)
		{
			if (entry.handle != XR_NULL_HANDLE)
				xrDestroyPassthroughColorLutMETA(entry.handle);
		}
	}
#endif
	lut_cache.clear();
}

namespace
{
// Clamp helpers for effect ranges (spec ranges, GUI may overshoot).
float clamp_opacity(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}
float clamp_brightness(float v)
{
	return std::clamp(v, -100.0f, 100.0f);
}
float clamp_nonneg(float v)
{
	return std::max(v, 0.0f);
}

uint8_t clamp_u8(float v)
{
	return (uint8_t)std::clamp<int>((int)std::lround(v), 0, 255);
}

std::string lut_key(xr::passthrough_lut_preset preset, uint32_t res)
{
	return std::to_string((int)preset) + "_" + std::to_string(res);
}

// LUT data order per spec/docs: lut[(R + G*res + B*res*res)*bytesPerColor],
// generated b-outer, g-mid, r-inner like Meta's sample.
std::vector<uint8_t> make_lut_data(xr::passthrough_lut_preset preset, uint32_t res)
{
	std::vector<uint8_t> data;
	data.reserve((size_t)res * res * res * 3);
	for (uint32_t b = 0; b < res; b++)
	{
		for (uint32_t g = 0; g < res; g++)
		{
			for (uint32_t r = 0; r < res; r++)
			{
				float rn = res > 1 ? (float)r / (float)(res - 1) : 0;
				float gn = res > 1 ? (float)g / (float)(res - 1) : 0;
				float bn = res > 1 ? (float)b / (float)(res - 1) : 0;
				float in_r = rn * 255.0f, in_g = gn * 255.0f, in_b = bn * 255.0f;
				float o_r = in_r, o_g = in_g, o_b = in_b;
				switch (preset)
				{
					case xr::passthrough_lut_preset::identity:
						break;
					case xr::passthrough_lut_preset::sepia:
						o_r = 0.393f * in_r + 0.769f * in_g + 0.189f * in_b;
						o_g = 0.349f * in_r + 0.686f * in_g + 0.168f * in_b;
						o_b = 0.272f * in_r + 0.534f * in_g + 0.131f * in_b;
						break;
					case xr::passthrough_lut_preset::invert:
						o_r = 255.0f - in_r;
						o_g = 255.0f - in_g;
						o_b = 255.0f - in_b;
						break;
					case xr::passthrough_lut_preset::high_contrast:
						o_r = (in_r - 128.0f) * 1.8f + 128.0f;
						o_g = (in_g - 128.0f) * 1.8f + 128.0f;
						o_b = (in_b - 128.0f) * 1.8f + 128.0f;
						break;
					case xr::passthrough_lut_preset::night_vision: {
						float lum = 0.299f * in_r + 0.587f * in_g + 0.114f * in_b;
						o_r = lum * 0.2f;
						o_g = std::min(255.0f, lum * 1.2f);
						o_b = lum * 0.2f;
						break;
					}
				}
				data.push_back(clamp_u8(o_r));
				data.push_back(clamp_u8(o_g));
				data.push_back(clamp_u8(o_b));
			}
		}
	}
	return data;
}

void fill_mono_rgba(xr::passthrough_mono_preset preset, XrColor4f out[256])
{
	for (int i = 0; i < 256; i++)
	{
		float t = (float)i / 255.0f;
		switch (preset)
		{
			case xr::passthrough_mono_preset::gray:
				out[i] = {t, t, t, 1.0f};
				break;
			case xr::passthrough_mono_preset::thermal: {
				// 4-stop thermal: deep blue -> red -> orange -> pale yellow
				float r, g, b;
				if (t < 0.33f)
				{
					float k = t / 0.33f;
					r = 0.1f + 0.7f * k;
					g = 0.0f + 0.1f * k;
					b = 0.4f * (1.0f - k) + 0.1f * k;
				}
				else if (t < 0.66f)
				{
					float k = (t - 0.33f) / 0.33f;
					r = 0.8f + 0.2f * k;
					g = 0.1f + 0.5f * k;
					b = 0.1f * (1.0f - k);
				}
				else
				{
					float k = (t - 0.66f) / 0.34f;
					r = 1.0f;
					g = 0.6f + 0.4f * k;
					b = 0.0f + 0.8f * k;
				}
				out[i] = {r, g, b, 1.0f};
				break;
			}
			case xr::passthrough_mono_preset::posterize: {
				int level = (i * 4) / 256; // 0..3
				static const float cols[4][3] = {{0, 0, 0}, {0.2f, 0.4f, 1.0f}, {1.0f, 0.55f, 0.1f}, {1, 1, 1}};
				out[i] = {cols[level][0], cols[level][1], cols[level][2], 1.0f};
				break;
			}
		}
	}
}
} // namespace

XrPassthroughColorLutMETA xr::passthrough_fb::get_or_create_lut(passthrough_lut_preset preset, uint32_t resolution)
{
#ifdef XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME
	if (!lut_supported || !xrCreatePassthroughColorLutMETA)
		return XR_NULL_HANDLE;
	std::string key = lut_key(preset, resolution);
	if (auto it = lut_cache.find(key); it != lut_cache.end() && it->second.handle != XR_NULL_HANDLE)
		return it->second.handle;

	std::vector<uint8_t> data = make_lut_data(preset, resolution);
	XrPassthroughColorLutDataMETA lut_data{
	        .bufferSize = (uint32_t)data.size(),
	        .buffer = data.data(),
	};
	XrPassthroughColorLutCreateInfoMETA create_info{
	        .type = XR_TYPE_PASSTHROUGH_COLOR_LUT_CREATE_INFO_META,
	        .next = nullptr,
	        .channels = XR_PASSTHROUGH_COLOR_LUT_CHANNELS_RGB_META,
	        .resolution = resolution,
	        .data = lut_data,
	};
	XrPassthroughColorLutMETA handle = XR_NULL_HANDLE;
	XrResult res = xrCreatePassthroughColorLutMETA(id, &create_info, &handle);
	if (res != XR_SUCCESS)
	{
		spdlog::warn("xrCreatePassthroughColorLutMETA failed (preset {} res {}): {}", (int)preset, resolution, (int)res);
		return XR_NULL_HANDLE;
	}
	lut_cache[key] = lut_entry{.handle = handle, .data = std::move(data), .resolution = resolution};
	spdlog::info("Created passthrough color LUT preset {} res {}", (int)preset, resolution);
	return handle;
#else
	(void)preset;
	(void)resolution;
	return XR_NULL_HANDLE;
#endif
}

void xr::passthrough_fb::apply_style(const passthrough_effects & fx)
{
	float opacity = clamp_opacity(fx.opacity);
	XrColor4f edge{fx.edge_r, fx.edge_g, fx.edge_b, clamp_opacity(fx.edge_alpha)};

	XrPassthroughStyleFB style{
	        .type = XR_TYPE_PASSTHROUGH_STYLE_FB,
	        .next = nullptr,
	        .textureOpacityFactor = opacity,
	        .edgeColor = edge,
	};

	XrPassthroughBrightnessContrastSaturationFB bcs{
	        .type = XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB,
	        .next = nullptr,
	        .brightness = clamp_brightness(fx.brightness),
	        .contrast = clamp_nonneg(fx.contrast),
	        .saturation = clamp_nonneg(fx.saturation),
	};

	XrPassthroughColorMapMonoToRgbaFB mono{
	        .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_MONO_TO_RGBA_FB,
	        .next = nullptr,
	        .textureColorMap = {},
	};
	XrPassthroughColorMapLutMETA lut{
	        .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_LUT_META,
	        .next = nullptr,
	        .colorLut = XR_NULL_HANDLE,
	        .weight = 0.0f,
	};
	XrPassthroughColorMapInterpolatedLutMETA interp{
	        .type = XR_TYPE_PASSTHROUGH_COLOR_MAP_INTERPOLATED_LUT_META,
	        .next = nullptr,
	        .sourceColorLut = XR_NULL_HANDLE,
	        .targetColorLut = XR_NULL_HANDLE,
	        .weight = 0.0f,
	};

	// Only one color struct may be chained; mode selects it.
	switch (fx.mode)
	{
		case passthrough_effect_mode::off:
			break;
		case passthrough_effect_mode::bcs:
			style.next = &bcs;
			break;
		case passthrough_effect_mode::mono_rgba:
			fill_mono_rgba(fx.mono_preset, mono.textureColorMap);
			style.next = &mono;
			break;
		case passthrough_effect_mode::lut: {
#ifdef XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME
			if (!lut_supported)
			{
				if (!lut_warn_once)
				{
					lut_warn_once = true;
					spdlog::warn("Passthrough LUT requested but XR_META_passthrough_color_lut not available, ignoring");
				}
				break;
			}
			XrPassthroughColorLutMETA h = get_or_create_lut(fx.lut_preset, 32);
			if (h == XR_NULL_HANDLE)
				break;
			lut.colorLut = h;
			lut.weight = clamp_opacity(fx.lut_weight);
			style.next = &lut;
#else
			break;
#endif
		}
		case passthrough_effect_mode::interpolated_lut: {
#ifdef XR_META_PASSTHROUGH_COLOR_LUT_EXTENSION_NAME
			if (!lut_supported)
			{
				if (!lut_warn_once)
				{
					lut_warn_once = true;
					spdlog::warn("Passthrough interpolated LUT requested but LUT ext not available, ignoring");
				}
				break;
			}
			XrPassthroughColorLutMETA ha = get_or_create_lut(fx.lut_preset, 32);
			XrPassthroughColorLutMETA hb = get_or_create_lut(fx.lut_preset_b, 32);
			if (ha == XR_NULL_HANDLE || hb == XR_NULL_HANDLE)
				break;
			interp.sourceColorLut = ha;
			interp.targetColorLut = hb;
			interp.weight = clamp_opacity(fx.lut_mix);
			style.next = &interp;
#else
			break;
#endif
		}
	}

	auto apply_one = [&](XrPassthroughLayerFB layer, const char * name) {
		XrResult res = xrPassthroughLayerSetStyleFB(layer, &style);
		if (res != XR_SUCCESS)
			spdlog::warn("xrPassthroughLayerSetStyleFB({}) failed mode={}: {}", name, (int)fx.mode, (int)res);
		else
			spdlog::debug("Passthrough style {}: opacity {:.2f} edge_a {:.2f} mode {}", name, opacity, (double)edge.a, (int)fx.mode);
	};
	apply_one(passthrough_layer, "flat");
	if (projected_layer)
		apply_one(*projected_layer, "projected");
}

void xr::passthrough_fb::set_effects(const passthrough_effects & fx)
{
	apply_style(fx);
}

void xr::passthrough_fb::reset_effects()
{
	passthrough_effects def{};
	def.mode = passthrough_effect_mode::off;
	def.opacity = 1.0f;
	def.edge_alpha = 0.0f;
	apply_style(def);
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

void xr::passthrough_fb::set_projected_instance_mesh(const std::string & key,
                                                     std::span<const XrVector3f> vertices,
                                                     std::span<const uint32_t> indices,
                                                     XrSpace base_space,
                                                     const XrPosef & pose,
                                                     const XrVector3f & scale)
{
	if (not projected_supported or not projected_layer)
		throw std::runtime_error("Surface-projected passthrough not supported by the runtime");

	auto & pin = projected_instances[key];
	// Destroy the old geometry first: the instance references the mesh
	pin.geometry.reset();
	pin.mesh.reset();

	pin.mesh.emplace(*inst, *sess, vertices, indices);
	pin.geometry.emplace(*inst, *sess, *projected_layer, *pin.mesh, base_space, pose, scale);

	spdlog::info("Projected passthrough instance \"{}\": {} vertices, {} triangles", key, vertices.size(), indices.size() / 3);
}

void xr::passthrough_fb::remove_projected_instance(const std::string & key)
{
	projected_instances.erase(key);
}

void xr::passthrough_fb::clear_projected_instances()
{
	projected_instances.clear();
}

bool xr::passthrough_fb::has_projected_instance(const std::string & key) const
{
	return projected_instances.contains(key);
}

bool xr::passthrough_fb::has_projected_instances() const
{
	return not projected_instances.empty();
}

void xr::passthrough_fb::update_projected_instance_transform(const std::string & key,
                                                             XrSpace base_space,
                                                             XrTime time,
                                                             const XrPosef & pose,
                                                             const XrVector3f & scale)
{
	if (auto it = projected_instances.find(key); it != projected_instances.end() and it->second.geometry)
		it->second.geometry->set_transform(base_space, time, pose, scale);
}
