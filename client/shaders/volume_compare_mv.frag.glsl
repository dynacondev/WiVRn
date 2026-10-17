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
// The remap rides push constants as 6 NAMED vec4 members (push arrays
// and the fragment-stage UBO both read back zero on this driver;
// consecutive vec4 members lay out exactly like the proven mat4 columns
// of volume_push): per-eye homography columns, w unused, eye e uses
// h[3e..3e+2].
// MODE is a specialization constant (separate pipeline per mode,
// selected CPU-side):
// 0 = remap via push, 1 = hardcoded flip (no homography read),
// 2 = sensor raw, 3 = volume raw. Modes 2/3 bypass the comparison to
// localize an empty mask to the sensor input vs the volume raster.
layout(push_constant) uniform Push
{
	vec4 h0;
	vec4 h1;
	vec4 h2;
	vec4 h3;
	vec4 h4;
	vec4 h5;
}
pc;

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
		mat3 H = (gl_ViewIndex == 0) ? mat3(pc.h0, pc.h1, pc.h2)
		                             : mat3(pc.h3, pc.h4, pc.h5);
		vec3 p = H * vec3(uv, 1.0);
		if (p.z <= 1e-6)
		{
			out_color = vec4(0.0);
			return;
		}
		vec2 s = p.xy / p.z;
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
	if (MODE == 0 && uv.x > 0.5)
	{
		// Diagnostic split-screen (one build only): the right half
		// echoes push delivery as brightness (pc.h0.x ~= 1.0 white =
		// matrix delivered, 0.0 black = shader reads zero) while the
		// left half keeps the real remap result for comparison.
		out_color = vec4(vec3(pc.h0.x), 1.0);
		return;
	}
	out_color = vec4(inside);
}
