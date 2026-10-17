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
// comparison tests the same world ray on both sides. H is the per-eye
// homography (3x3 columns, w unused) mapping eye UV to sensor UV,
// computed on the CPU as S_uv * A_d^-1 * D * R * S * A_e * E1 (exact
// for rotation, any fov pair; identity reproduces the direct tap).
// Strict sensor < render, far / no-data hidden, out-of-frustum hidden.
layout(push_constant) uniform Push
{
	vec4 h[6];
}
pc;

layout(set = 0, binding = 0) uniform sampler2DArray sensor_tex;
layout(set = 0, binding = 1) uniform sampler2DArray volume_tex;

void main()
{
	// Static branch (constant push indices per side): dynamically
	// indexing the push array reads as zero on some drivers. The
	// condition is dynamically uniform, so both sides stay legal.
	mat3 H = (gl_ViewIndex == 0) ? mat3(pc.h[0], pc.h[1], pc.h[2])
	                            : mat3(pc.h[3], pc.h[4], pc.h[5]);
	vec3 p = H * vec3(uv, 1.0);
	float inside = 0.0;
	if (p.z > 1e-6)
	{
		vec2 suv = p.xy / p.z;
		if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0)
		{
			float s = texture(sensor_tex, vec3(suv, float(gl_ViewIndex))).r;
			float v = texture(volume_tex, vec3(uv, float(gl_ViewIndex))).r;
			inside = (s < v && s < 1.0 && v > 0.0) ? 1.0 : 0.0;
		}
	}
	out_color = vec4(inside);
}
