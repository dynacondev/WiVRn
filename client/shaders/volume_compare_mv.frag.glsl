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

#extension GL_EXT_multiview : require

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// Depth-volume flatten with rotation reprojection: the sensor tap is
// remapped through the capture<->display relative rotation so the
// comparison tests the same world ray on both sides. Identity
// reproduces the direct tap. Strict sensor < render, far / no-data
// hidden, out-of-frustum hidden.
//
// The remap rides a tiny sampled image (8x1 RGBA32F, texels 0..5 =
// H columns): sampler reads are the read unit proven live in this
// shader (fragment UBO, push and texel-buffer reads all return zero on
// this driver). Same descriptor type as the two working bindings.
// Per-eye homography columns: eye e uses texels [3e..3e+2].
layout(set = 0, binding = 2) uniform sampler2D h_img;
// MODE is a plain constant (no specialization: spec-constant pipelines
// were also eliminated as a suspect on this build):
// 0 = remap via sampled image, 1 = hardcoded flip (no homography read),
// 2 = sensor raw, 3 = volume raw. Modes 2/3 bypass the comparison to
// localize an empty mask to the sensor input vs the volume raster.

const int MODE = 0; // Hardcoded: no specialization (spec-constant
// pipelines misread push/UBO data on this driver, so the mode is a
// plain constant and behavior is selected by which build is installed).

layout(set = 0, binding = 0) uniform sampler2DArray sensor_tex;
layout(set = 0, binding = 1) uniform sampler2DArray volume_tex;

void main()
{
	vec2 suv = vec2(uv.x, 1.0 - uv.y);
	if (MODE == 0)
	{
		// Scalar-spelled homography: no mat3 constructor, no mat3
		// ternary. (The Adreno compiler miscompiles mat3-from-vec4s in
		// this pass, which also gated every prior transport echo behind
		// mat3-derived guards and made them all read "black".)
		// Eye base without branching: gl_ViewIndex is 0/1.
		int e = gl_ViewIndex * 3;
		vec4 c0 = texelFetch(h_img, ivec2(e + 0, 0), 0);
		vec4 c1 = texelFetch(h_img, ivec2(e + 1, 0), 0);
		vec4 c2 = texelFetch(h_img, ivec2(e + 2, 0), 0);
		float px = c0.x * uv.x + c1.x * uv.y + c2.x;
		float py = c0.y * uv.x + c1.y * uv.y + c2.y;
		float pz = c0.z * uv.x + c1.z * uv.y + c2.z;
		if (pz <= 1e-6)
		{
			out_color = vec4(0.0);
			return;
		}
		vec2 s = vec2(px, py) / pz;
		if (s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0)
		{
			out_color = vec4(0.0);
			return;
		}
		suv = s;
	}
	float s = texture(sensor_tex, vec3(suv, float(gl_ViewIndex))).r;
	float v = texture(volume_tex, vec3(uv, float(gl_ViewIndex))).r;
	if (MODE == 2)
	{
		// Footprint-gated: raw sensor only where the volume raster drew
		// (v > 0). Full-canvas sensor (~1 for far scenes) would MAX the
		// whole tier seed to 1 and wipe the working silhouette mask.
		out_color = vec4(v > 0.0 ? vec3(s) : vec3(0.0), 1.0);
		return;
	}
	if (MODE == 3)
	{
		// Already footprint-safe by construction (v == 0 outside the raster).
		out_color = vec4(vec3(v), 1.0);
		return;
	}
	float inside = (s < v && s < 1.0 && v > 0.0) ? 1.0 : 0.0;
	out_color = vec4(inside);
}
