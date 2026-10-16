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

// Custom boundary zones: headset-to-mesh signed distance (local-space
// single pass) + symmetric smoothstep + union combine. See boundary.h.

#include "xr/boundary.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>
#include <vector>

namespace xr
{
namespace
{
struct vec3
{
	float x, y, z;
};

vec3 operator-(const vec3 & a, const vec3 & b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vec3 operator+(const vec3 & a, const vec3 & b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vec3 operator*(const vec3 & a, float s)
{
	return {a.x * s, a.y * s, a.z * s};
}

float dot(const vec3 & a, const vec3 & b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3 cross(const vec3 & a, const vec3 & b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool finite(const vec3 & v)
{
	return std::isfinite(v.x) and std::isfinite(v.y) and std::isfinite(v.z);
}

// Ericson 5.1.5 closest-point-to-triangle, squared distance. Degenerate
// triangles (zero area) return +inf so they never win the minimum.
float sqdist_point_triangle(const vec3 & p, const vec3 & a, const vec3 & b, const vec3 & c)
{
	vec3 ab = b - a;
	vec3 ac = c - a;
	vec3 ap = p - a;
	float d1 = dot(ab, ap);
	float d2 = dot(ac, ap);
	if (d1 <= 0 and d2 <= 0)
		return dot(ap, ap); // vertex A
	vec3 bp = p - b;
	float d3 = dot(ab, bp);
	float d4 = dot(ac, bp);
	if (d3 >= 0 and d4 <= d3)
		return dot(bp, bp); // vertex B
	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 and d1 >= 0 and d3 <= 0)
	{
		float v = d1 / (d1 - d3);
		vec3 q = a + ab * v - p;
		return dot(q, q); // edge AB
	}
	vec3 cp = p - c;
	float d5 = dot(ab, cp);
	float d6 = dot(ac, cp);
	if (d6 >= 0 and d5 <= d6)
		return dot(cp, cp); // vertex C
	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 and d2 >= 0 and d6 <= 0)
	{
		float w = d2 / (d2 - d6);
		vec3 q = a + ac * w - p;
		return dot(q, q); // edge AC
	}
	float va = d3 * d6 - d5 * d4;
	float denom = va + vb + vc;
	if (denom == 0)
		return HUGE_VALF; // degenerate
	if (va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0)
	{
		float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		vec3 q = b + (c - b) * w - p;
		return dot(q, q); // edge BC
	}
	float inv = 1 / denom;
	float v = vb * inv;
	float w = vc * inv;
	vec3 q = a + ab * v + ac * w - p;
	return dot(q, q); // face interior
}

// Moller-Trumbore, no culling (parity needs both faces). Parallel and
// degenerate triangles miss. Shared edges/vertices produce identical hit
// distances, merged by the caller, so quad diagonals count once.
bool ray_hit_triangle(const vec3 & o, const vec3 & d, const vec3 & a, const vec3 & b, const vec3 & c, float * t_out)
{
	vec3 e1 = b - a;
	vec3 e2 = c - a;
	vec3 p = cross(d, e2);
	float det = dot(e1, p);
	if (std::fabs(det) < 1e-12f)
		return false;
	float inv = 1 / det;
	vec3 t = o - a;
	float u = dot(t, p) * inv;
	if (u < 0 or u > 1)
		return false;
	vec3 q = cross(t, e1);
	float v = dot(d, q) * inv;
	if (v < 0 or u + v > 1)
		return false;
	float tp = dot(e2, q) * inv;
	if (tp <= 1e-6f)
		return false;
	*t_out = tp;
	return true;
}

// Skewed parity-ray direction (deterministic, irrational-ish slopes):
// axis rays systematically strike quad diagonals twice (identical hit
// distance, but only when deduplicated) and graze box edges; the skew
// keeps hits transversal on axis-aligned zone boxes.
const vec3 & parity_ray_dir()
{
	static const vec3 d = [] {
		vec3 u{1, 0.314159f, 0.720478f};
		return u * (1 / std::sqrt(dot(u, u)));
	}();
	return d;
}
} // namespace

boundary_query boundary_signed_distance(
        std::span<const XrVector3f> vertices,
        std::span<const uint32_t> indices,
        const XrPosef & world_pose,
        float world_scale,
        float hx,
        float hy,
        float hz)
{
	boundary_query out;
	if (vertices.empty() or indices.size() < 3 or indices.size() % 3 != 0)
		return out;
	if (not std::isfinite(world_scale) or std::fabs(world_scale) < 1e-6f)
		return out;
	vec3 t{world_pose.position.x, world_pose.position.y, world_pose.position.z};
	vec3 h{hx, hy, hz};
	if (not finite(t) or not finite(h))
		return out;
	// Unit quaternion assumed (poses come from the runtime/fusion; a
	// non-unit input only skews the local frame, guarded below by the
	// finite checks on the soup).
	float qx = world_pose.orientation.x, qy = world_pose.orientation.y;
	float qz = world_pose.orientation.z, qw = world_pose.orientation.w;
	if (not(std::isfinite(qx) and std::isfinite(qy) and std::isfinite(qz) and std::isfinite(qw)))
		return out;
	// World -> local: p_l = R^T (p_w - t) / s. Rows of R from xyzw quat.
	float r00 = 1 - 2 * (qy * qy + qz * qz), r01 = 2 * (qx * qy - qz * qw), r02 = 2 * (qx * qz + qy * qw);
	float r10 = 2 * (qx * qy + qz * qw), r11 = 1 - 2 * (qx * qx + qz * qz), r12 = 2 * (qy * qz - qx * qw);
	float r20 = 2 * (qx * qz - qy * qw), r21 = 2 * (qy * qz + qx * qw), r22 = 1 - 2 * (qx * qx + qy * qy);
	vec3 d{h.x - t.x, h.y - t.y, h.z - t.z};
	vec3 p{(r00 * d.x + r10 * d.y + r20 * d.z) / world_scale,
	       (r01 * d.x + r11 * d.y + r21 * d.z) / world_scale,
	       (r02 * d.x + r12 * d.y + r22 * d.z) / world_scale};
	if (not finite(p))
		return out;

	float best2 = HUGE_VALF;
	size_t valid_tris = 0;
	// Hit distances along the parity ray, reused across calls (clear
	// keeps capacity). Sorted + merged below: shared edges/vertices
	// report the same geometric point, counted once.
	thread_local std::vector<float> hits;
	hits.clear();
	const vec3 & dir = parity_ray_dir();
	for (size_t i = 0; i + 2 < indices.size(); i += 3)
	{
		uint32_t ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
		if (ia >= vertices.size() or ib >= vertices.size() or ic >= vertices.size())
			continue;
		vec3 a{vertices[ia].x, vertices[ia].y, vertices[ia].z};
		vec3 b{vertices[ib].x, vertices[ib].y, vertices[ib].z};
		vec3 c{vertices[ic].x, vertices[ic].y, vertices[ic].z};
		if (not finite(a) or not finite(b) or not finite(c))
			continue;
		++valid_tris;
		float d2 = sqdist_point_triangle(p, a, b, c);
		if (d2 < best2)
			best2 = d2;
		float th;
		if (ray_hit_triangle(p, dir, a, b, c, &th))
			hits.push_back(th);
	}
	if (valid_tris == 0 or best2 == HUGE_VALF)
		return out;
	// Odd crossing count = inside. Merge window 0.1mm (local units):
	// shared-vertex float scatter sits far below it, while distinct
	// zone surfaces never pack that tight.
	std::sort(hits.begin(), hits.end());
	size_t crossings = 0;
	float last = 0;
	for (float th: hits)
	{
		if (crossings == 0 or th - last > 1e-4f)
		{
			++crossings;
			last = th;
		}
	}
	out.inside = crossings % 2 == 1;
	out.distance_m = std::sqrt(best2) * std::fabs(world_scale);
	out.signed_m = out.inside ? out.distance_m : -out.distance_m;
	out.valid = true;
	return out;
}

float boundary_smoothstep(float signed_m, float transition_m)
{
	if (not std::isfinite(signed_m))
		return 1; // fail open to game video
	if (transition_m <= 0)
		return signed_m >= 0 ? 1 : 0;
	float x = (signed_m + transition_m) / (2 * transition_m);
	x = std::clamp(x, 0.f, 1.f);
	return x * x * (3 - 2 * x);
}

void boundary_mix::add(float object_opacity, bool reversed)
{
	object_opacity = std::clamp(object_opacity, 0.f, 1.f);
	if (reversed)
		global_ = std::min(global_, object_opacity);
	else if (normals_ < 0)
		normals_ = object_opacity;
	else
		normals_ = std::max(normals_, object_opacity);
}

float boundary_mix::result() const
{
	float n = normals_ < 0 ? 1 : normals_;
	return std::min(n, global_);
}

bool boundary_selftest()
{
	// Unit cube, half-extent 1, centered on the origin.
	std::vector<XrVector3f> v = {
	        {-1, -1, -1},
	        {1, -1, -1},
	        {1, 1, -1},
	        {-1, 1, -1},
	        {-1, -1, 1},
	        {1, -1, 1},
	        {1, 1, 1},
	        {-1, 1, 1},
	};
	std::vector<uint32_t> idx = {
	        0,
	        3,
	        7,
	        0,
	        7,
	        4, // -x
	        1,
	        5,
	        6,
	        1,
	        6,
	        2, // +x
	        0,
	        4,
	        5,
	        0,
	        5,
	        1, // -y
	        3,
	        2,
	        6,
	        3,
	        6,
	        7, // +y
	        0,
	        1,
	        2,
	        0,
	        2,
	        3, // -z
	        4,
	        7,
	        6,
	        4,
	        6,
	        5, // +z
	};
	XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};

	bool pass = true;
	auto check = [&](const char * name, bool ok) {
		spdlog::info("boundary selftest: {} {}", name, ok ? "PASS" : "FAIL");
		pass = pass and ok;
	};

	boundary_query q = boundary_signed_distance(v, idx, identity, 1, 0, 0, 0);
	check("center inside", q.valid and q.inside and std::fabs(q.distance_m - 1) < 1e-3f);
	q = boundary_signed_distance(v, idx, identity, 1, 5, 0, 0);
	check("outside +x", q.valid and not q.inside and std::fabs(q.distance_m - 4) < 1e-3f);
	q = boundary_signed_distance(v, idx, identity, 1, 1, 0, 0);
	check("on surface", q.valid and std::fabs(q.distance_m) < 1e-3f);
	q = boundary_signed_distance(v, idx, identity, 1, 0, -7, 0);
	check("outside -y", q.valid and not q.inside and std::fabs(q.distance_m - 6) < 1e-3f);
	// Scaled + translated placement: pose t=(10,0,0) scale 2.
	XrPosef placed{{0, 0, 0, 1}, {10, 0, 0}};
	q = boundary_signed_distance(v, idx, placed, 2, 10, 0, 0);
	check("scaled center", q.valid and q.inside and std::fabs(q.distance_m - 2) < 1e-3f);
	q = boundary_signed_distance(v, idx, placed, 2, 14, 0, 0);
	check("scaled outside", q.valid and not q.inside and std::fabs(q.distance_m - 2) < 1e-3f);
	// Degenerate inputs fail closed (invalid, never a veil).
	check("empty soup", not boundary_signed_distance({}, {}, identity, 1, 0, 0, 0).valid);
	check("zero scale", not boundary_signed_distance(v, idx, identity, 0, 0, 0, 0).valid);

	check("smoothstep surface", std::fabs(boundary_smoothstep(0, 0.3f) - 0.5f) < 1e-6f);
	check("smoothstep inside", boundary_smoothstep(0.3f, 0.3f) == 1);
	check("smoothstep outside", boundary_smoothstep(-0.3f, 0.3f) == 0);
	check("smoothstep hard inside", boundary_smoothstep(1e-4f, 0) == 1);
	check("smoothstep hard outside", boundary_smoothstep(-1e-4f, 0) == 0);

	boundary_mix m;
	check("mix empty", m.result() == 1);
	m.add(1, false);
	m.add(0, true);
	check("mix exclusion wins", m.result() == 0);
	boundary_mix m2;
	m2.add(0, false);
	m2.add(1, true);
	check("mix outside normal", m2.result() == 0);
	boundary_mix m3;
	m3.add(0.5f, false);
	m3.add(0.8f, false);
	check("mix union max", std::fabs(m3.result() - 0.8f) < 1e-6f);

	spdlog::info("boundary selftest: overall {}", pass ? "PASS" : "FAIL");
	return pass;
}
} // namespace xr
