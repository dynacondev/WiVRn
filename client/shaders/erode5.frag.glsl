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

// 5x5 box minimum (morphological erosion, ~2 texels): the wide sibling
// of erode3, selected when the blur support exceeds one texel. Same
// layout contract (src_texel push prefix only).
layout(push_constant) uniform Push
{
	vec2 src_texel;
}
pc;

void main()
{
	vec2 t = pc.src_texel;
	float m = texture(src, uv).r;
	m = min(m, texture(src, uv + vec2(-2.0 * t.x, -2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(-t.x, -2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(0.0, -2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(t.x, -2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(2.0 * t.x, -2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(-2.0 * t.x, -t.y)).r);
	m = min(m, texture(src, uv + vec2(-t.x, -t.y)).r);
	m = min(m, texture(src, uv + vec2(0.0, -t.y)).r);
	m = min(m, texture(src, uv + vec2(t.x, -t.y)).r);
	m = min(m, texture(src, uv + vec2(2.0 * t.x, -t.y)).r);
	m = min(m, texture(src, uv + vec2(-2.0 * t.x, 0.0)).r);
	m = min(m, texture(src, uv + vec2(-t.x, 0.0)).r);
	m = min(m, texture(src, uv + vec2(t.x, 0.0)).r);
	m = min(m, texture(src, uv + vec2(2.0 * t.x, 0.0)).r);
	m = min(m, texture(src, uv + vec2(-2.0 * t.x, t.y)).r);
	m = min(m, texture(src, uv + vec2(-t.x, t.y)).r);
	m = min(m, texture(src, uv + vec2(0.0, t.y)).r);
	m = min(m, texture(src, uv + vec2(t.x, t.y)).r);
	m = min(m, texture(src, uv + vec2(2.0 * t.x, t.y)).r);
	m = min(m, texture(src, uv + vec2(-2.0 * t.x, 2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(-t.x, 2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(0.0, 2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(t.x, 2.0 * t.y)).r);
	m = min(m, texture(src, uv + vec2(2.0 * t.x, 2.0 * t.y)).r);
	out_color = vec4(m);
}
