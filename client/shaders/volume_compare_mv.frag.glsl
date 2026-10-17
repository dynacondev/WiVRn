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
// The remap rides a uniform texel buffer (6 vec4 texels): sampler-unit
// reads are the one path proven live in this shader (fragment UBO and
// push reads both return zero on this driver). Updated per volume
// record with the same memcpy+flush idiom as the depth overlay.
// Per-eye homography columns: eye e uses texels [3e..3e+2].
layout(set = 0, binding = 2) uniform samplerBuffer h_tex;
// MODE is a plain constant (no specialization: spec-constant pipelines
// were also eliminated as a suspect on this build):
// 0 = remap via texel buffer, 1 = hardcoded flip (no homography read),
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
		vec4 h0 = texelFetch(h_tex, 0);
		vec4 h1 = texelFetch(h_tex, 1);
		vec4 h2 = texelFetch(h_tex, 2);
		vec4 h3 = texelFetch(h_tex, 3);
		vec4 h4 = texelFetch(h_tex, 4);
		vec4 h5 = texelFetch(h_tex, 5);
		mat3 H = (gl_ViewIndex == 0) ? mat3(h0, h1, h2)
		                             : mat3(h3, h4, h5);
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
	if (MODE == 0)
	{
		// CONTROL (one build only): hardcoded 1.0 full-canvas, no reads
		// at all. The merge only transfers the object-footprint box, so
		// a half-canvas echo can miss it entirely; full-canvas must stamp
		// the box wherever it is. Patch at the object = apparatus valid;
		// still VR = the echo path itself is broken downstream.
		out_color = vec4(1.0);
		return;
	}
	out_color = vec4(inside);
}
