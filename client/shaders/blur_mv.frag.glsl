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

// Multiview twin of blur.frag.glsl: identical 5-tap Gaussian, reading
// the calling view's layer. Same push layout (24 bytes), so the
// multiview pipelines share the blur pipeline layout.
layout(set = 0, binding = 0) uniform sampler2DArray src;

layout(push_constant) uniform Push
{
	vec2 texel;
	vec2 dir;
	float spread;
	float pad;
}
pc;

void main()
{
	vec2 t = pc.dir * pc.texel * pc.spread;
	vec3 base_uv = vec3(uv, float(gl_ViewIndex));
	float a = texture(src, base_uv).r * 0.2514;
	a += texture(src, base_uv + vec3(t * 1.4072, 0)).r * 0.3743;
	a += texture(src, base_uv - vec3(t * 1.4072, 0)).r * 0.3743;
	out_color = vec4(a);
}
