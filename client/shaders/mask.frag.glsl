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

// Flat silhouette: opacity replicated to all channels, so one body serves
// single-channel intermediates (.r) and RGBA8 outputs (.a; RGB is ignored
// downstream by the ZERO color blend factors).
layout(push_constant) uniform PushConstants
{
	mat4 mvp;
	float opacity;
}
pc;

layout(location = 0) out vec4 out_color;

void main()
{
	out_color = vec4(pc.opacity);
}
