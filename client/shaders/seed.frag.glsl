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

// SDF seed pass (vertex stage is mask.vert: pos transformed by pc.mvp).
// Inside every silhouette triangle, emit this pixel's own SDF-texel
// coordinate; outside stays at the INF clear value. The Jump Flood chain
// then propagates nearest-seed coordinates across the target.
layout(location = 0) out vec2 out_seed;

void main()
{
	out_seed = gl_FragCoord.xy;
}
