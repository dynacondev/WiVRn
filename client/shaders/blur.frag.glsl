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

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push
{
	vec2 texel;
	vec2 dir;
	float spread;
	float pad;
}
pc;

// Fixed 9-tap Gaussian, sigma = 2 texels (normalized weights). The tap
// offsets scale with spread, so feather-px maps to band width without
// changing the kernel. Only alpha carries information downstream.
const float W[9] = float[9](0.0276, 0.0663, 0.1238, 0.1802, 0.2042, 0.1802, 0.1238, 0.0663, 0.0276);

void main()
{
	float a = 0.0;
	for (int i = -4; i <= 4; ++i)
		a += texture(src, uv + pc.dir * pc.texel * (float(i) * pc.spread)).a * W[i + 4];
	out_color = vec4(1.0, 1.0, 1.0, a);
}
