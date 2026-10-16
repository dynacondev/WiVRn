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

// Environment-depth debug false-color: green at nearZ, red at farZ.
//
// The runtime D16 array texture is sampled directly per eye layer at
// the framebuffer UV (no reprojection, no history, no smoothing).
// NEAREST filtering keeps texel edges exact.
//
// Depth values are Vulkan D in [0,1] (D16_UNORM, same semantics as
// XR_KHR_composition_layer_depth): d = near*far/(far - D*(far-near)).
// Unbounded farZ (+inf, what the Quest runtime reports) or
// farZ < nearZ selects the infinite-projection branch.

layout(location = 0) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2DArray depth_tex;

layout(set = 0, binding = 1) uniform params
{
	vec2 near_far;
	float opacity;
	float pad;
} u;

layout(push_constant) uniform pc
{
	int eye;
} push;

// Depth values are Vulkan D in [0,1] (D16_UNORM, same semantics as
// XR_KHR_composition_layer_depth): d = near*far/(far - D*(far-near)).
// The runtime reports farZ = +inf when it has no far bound (or
// farZ < nearZ for the classic infinite projection): both select the
// infinite branch below. Without the infinity check the finite math
// divides inf/inf = NaN and the whole viz goes transparent.
float linearize_depth(float d_norm, float n, float f)
{
	if (!(f > n) || f > 1e9)
	{
		// Infinite far plane. D=0 -> near, D=1 -> far.
		return n / max(1.0 - d_norm, 1e-6);
	}
	return (n * f) / max(f - d_norm * (f - n), 1e-6);
}

void main()
{
	float n = u.near_far.x;
	float f = u.near_far.y;

	// Y-flip: the framebuffer UV runs top-down against the depth
	// texture's bottom-up rows; sample flipped or the viz renders
	// upside down.
	float d_norm = texture(depth_tex, vec3(v_uv.x, 1.0 - v_uv.y, float(push.eye))).r;

	float d = linearize_depth(d_norm, n, f);
	// Infinite far (unbounded or classic): normalize against the
	// reconstructed distance itself (t = 1 - near/d).
	bool infinite = !(f > n) || f > 1e9;
	float denom = infinite ? max(d, 1e-6) : (f - n);
	float t = clamp((d - n) / denom, 0.0, 1.0);
	vec3 col = mix(vec3(0.1, 0.9, 0.2), vec3(0.9, 0.15, 0.1), t);

	out_color = vec4(col, u.opacity);
}
