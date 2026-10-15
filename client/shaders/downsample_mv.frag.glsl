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

// Multiview twin of downsample.frag.glsl: identical 2x2 box prefilter,
// reading the calling view's layer. Same push prefix, so it shares the
// blur pipeline layout. Keeps the linear sampler (see the single-view
// note on ±0.5 tie-breaks).
layout(set = 0, binding = 0) uniform sampler2DArray src;

layout(push_constant) uniform Push
{
	vec2 src_texel;
}
pc;

void main()
{
	vec2 t = pc.src_texel;
	vec3 base_uv = vec3(uv, float(gl_ViewIndex));
	float a = texture(src, base_uv + vec3(vec2(-0.5, -0.5) * t, 0)).r;
	a += texture(src, base_uv + vec3(vec2(0.5, -0.5) * t, 0)).r;
	a += texture(src, base_uv + vec3(vec2(-0.5, 0.5) * t, 0)).r;
	a += texture(src, base_uv + vec3(vec2(0.5, 0.5) * t, 0)).r;
	out_color = vec4(a * 0.25);
}
