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

// 2x2 box downsample: exact prefilter for halving (each output texel is
// the mean of its 2x2 source block), unlike naive bilinear decimation
// which aliases thin features. Push layout matches the blur shader (only
// src_texel is read), so both pipelines share one layout object.
layout(push_constant) uniform Push
{
	vec2 src_texel;
}
pc;

void main()
{
	vec2 t = pc.src_texel;
	float a = texture(src, uv + vec2(-0.5, -0.5) * t).r;
	a += texture(src, uv + vec2(0.5, -0.5) * t).r;
	a += texture(src, uv + vec2(-0.5, 0.5) * t).r;
	a += texture(src, uv + vec2(0.5, 0.5) * t).r;
	out_color = vec4(a * 0.25);
}
