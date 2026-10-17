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

// Depth-volume flatten: strict sensor < render per texel, per eye.
// Binary R8 output (1 = sensor resolves inside or in front of the
// volume's far side, 0 otherwise). Raw D compare, no epsilon: the
// low-res texel + feather absorb the boundary. Far / no-data never
// reveals (s >= 1 -> 0); empty volume texels (v <= 0) never reveal.
// The sensor tap carries the depth-side v-flip (bottom-up rows vs the
// top-down framebuffer UV, same as depth_viz); the volume raster was
// rendered in framebuffer space, so it samples unflipped.
layout(set = 0, binding = 0) uniform sampler2DArray sensor_tex;
layout(set = 0, binding = 1) uniform sampler2DArray volume_tex;

void main()
{
	float s = texture(sensor_tex, vec3(uv.x, 1.0 - uv.y, float(gl_ViewIndex))).r;
	float v = texture(volume_tex, vec3(uv, float(gl_ViewIndex))).r;
	float inside = (s < v && s < 1.0 && v > 0.0) ? 1.0 : 0.0;
	out_color = vec4(inside);
}
