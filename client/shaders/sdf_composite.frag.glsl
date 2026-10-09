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

#version 450

// Analytic SDF composite (fullscreen triangle from blur.vert): the single
// full-resolution pass. The flood field (linear-filtered, sub-texel smooth)
// gives distance to the silhouette; the untouched seed image (nearest)
// gives inside/outside. Band is outside-only: interiors stay pixel-exact
// to the rasterized edge, the ramp runs 0 -> feather_px outward.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D field;
layout(set = 0, binding = 1) uniform sampler2D seed;

layout(push_constant) uniform Push
{
	vec2 sdf_dim;
	float feather_px;
	float opacity;
	float px_per_texel;
	float inside_thresh;
	float viz;
}
pc;

void main()
{
	vec2 frag = uv * pc.sdf_dim;
	vec2 s = texture(field, uv).xy;
	// Clamp before smoothstep: unreached pixels hold INF seeds, and
	// strict drivers need not produce clean +inf through length().
	float raw_px = length(frag - s) * pc.px_per_texel;
	float inside = texture(seed, uv).x < pc.inside_thresh ? 1.0 : 0.0;
	if (pc.viz > 0.5)
	{
		// Iso-contours every 4px over live video. The mask layer blends
		// by alpha only (RGB is ignored), so the field is drawn as
		// opaque contour lines on video: smooth concentric lines =
		// healthy cascade, wavy/blocky lines pinpoint errors, and the
		// reality/video boundary is the sign edge under test. Far field
		// (past 2x feather) stays video; INF-safe by construction.
		float line = fract(raw_px * 0.25) < 0.18 ? 1.0 : 0.0;
		float a = inside > 0.5 ? 1.0 : (raw_px > pc.feather_px * 2.0 ? 0.0 : line);
		out_color = vec4(1.0, 1.0, 1.0, a);
		return;
	}
	float dist_px = min(raw_px, pc.feather_px);
	float band = 1.0 - smoothstep(0.0, pc.feather_px, dist_px);
	out_color = vec4(1.0, 1.0, 1.0, mix(band, 1.0, inside) * pc.opacity);
}
