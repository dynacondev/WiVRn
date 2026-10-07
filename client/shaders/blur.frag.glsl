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

// Bilinear-gather 9-tap Gaussian, sigma = 2 texels. Adjacent tap pairs
// share one linear-filtered fetch at the weighted offset (hardware lerp
// mixes each pair in exact proportion), so 9 taps cost 5 fetches:
// center 0.2042, pairs (1,2) at 1.4072 weight 0.3040, pairs (3,4) at
// 3.2939 weight 0.0939. The tap offsets scale with spread, so feather-px
// maps to band width without changing the kernel. Only alpha carries
// information downstream.
void main()
{
	vec2 t = pc.dir * pc.texel * pc.spread;
	float a = texture(src, uv).a * 0.2042;
	a += texture(src, uv + t * 1.4072).a * 0.3040;
	a += texture(src, uv - t * 1.4072).a * 0.3040;
	a += texture(src, uv + t * 3.2939).a * 0.0939;
	a += texture(src, uv - t * 3.2939).a * 0.0939;
	out_color = vec4(1.0, 1.0, 1.0, a);
}
