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

// One axis of a separable box-minimum erosion (morphological erosion):
// minimum over [-radius, +radius] working texels along dir. Two passes
// (dir = (1,0) then (0,1)) compose an exact (2r+1)^2 box erosion for any
// radius, so the blur band below lands inside the silhouette at any
// feather width. Push layout matches the blur shader (24 bytes), so all
// pipelines share one layout object.
layout(push_constant) uniform Push
{
	vec2 texel;
	vec2 dir;
	float radius;
	float pad;
}
pc;

void main()
{
	int r = int(pc.radius + 0.5);
	float m = texture(src, uv).r;
	for (int i = -8; i <= 8; ++i)
	{
		if (i == 0 || abs(i) > r)
			continue;
		m = min(m, texture(src, uv + pc.dir * pc.texel * float(i)).r);
	}
	out_color = vec4(m);
}
