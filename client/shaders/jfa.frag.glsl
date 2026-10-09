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

// One Jump Flood step (fullscreen triangle from blur.vert): adopt the
// nearest seed among the 8 neighbors at +-step texels. Distances compare
// in float32 (squared); storage is float32 RG so coordinates are exact.
// INF seeds (1e10) square to 1e20, representable, never selected while any
// real seed is reachable.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec2 out_seed;

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push
{
	vec2 texel;
	float step;
	float pad;
}
pc;

void main()
{
	vec2 frag = gl_FragCoord.xy;
	vec2 best = texture(src, uv).xy;
	vec2 d0 = frag - best;
	float best_d = dot(d0, d0);
	vec2 off = pc.step * pc.texel;
	for (int j = -1; j <= 1; ++j)
	{
		for (int i = -1; i <= 1; ++i)
		{
			if (i == 0 && j == 0)
				continue;
			vec2 cand = texture(src, uv + vec2(i, j) * off).xy;
			vec2 d = frag - cand;
			float dd = dot(d, d);
			if (dd < best_d)
			{
				best_d = dd;
				best = cand;
			}
		}
	}
	out_seed = best;
}
