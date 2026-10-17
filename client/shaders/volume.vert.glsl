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

layout(location = 0) in vec3 pos;

// Volume raster push block: model MVP plus the depth frustum the
// output must match (sensor-compatible D, same near/far the compare
// pass tests against). The vertex stage only reads mvp; the fragment
// stage only reads near_z/far_z. One layout, one range (72 bytes).
layout(push_constant) uniform PushConstants
{
	mat4 mvp;
	float near_z;
	float far_z;
}
pc;

layout(location = 0) out float eye_depth;

void main()
{
	vec4 clip = pc.mvp * vec4(pos, 1.0);
	gl_Position = clip;
	// Planar eye distance (-z view): clip.w for the perspective
	// projection the MVP came from. The fragment stage converts it
	// to sensor-compatible D.
	eye_depth = clip.w;
}
