/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "kiosk_planes.h"

#include "scene.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <string>

namespace
{
// tuning knobs, all device-tunable: model is ~13.4m wingspan in file units
constexpr int plane_count = 4;
constexpr float model_scale = 0.00675f; // ~0.13m wingspan
constexpr double dur_min = 9.0;
constexpr double dur_max = 14.0;
constexpr double fade_in = 1.2;
constexpr double fade_out = 2.0;

// model nose points along +Z once the file's own root rotation (-90deg
// about X on "Air Plane") is applied; tweak below only if a re-export
// changes that convention
constexpr float model_yaw_adjust = 0.f;
// mirrored model X flips the apparent bank direction; flip once on device
// if planes bank out of their turns instead of into them
constexpr float bank_sign = 1.f;
// radians of bank per m/s^2 of lateral acceleration (coordinated-turn feel)
constexpr float bank_gain = 0.9f;
// hard bank limit, ~29 deg
constexpr float bank_max = 0.5f;

// half-angle of the exclusion cone around the anchor-to-user axis (+Z in
// anchor space): flights inside it cross right in front of the face
constexpr float exclusion_tan = 0.2679491924f; // tan(15 deg)

// Legality limits for control-point legs: no clustered points (which stall
// the plane into a pivot) and no ultra-steep turns between legs. Yaw is
// allowed more freedom than pitch, like a real aircraft.
constexpr float min_leg_len = 0.45f;
constexpr float max_yaw_turn = 1.3963f;    // 80 deg direction change per leg
constexpr float min_turn_radius = 0.4f;    // legs must span this much arc per radian of turn: big turns go wide
constexpr float max_flight_range = 5.0f;   // control points stay within this distance of the anchor
constexpr float launch_yaw_dev = 3.14159265f; // full circle: spawn headings cover all directions, cone-trimmed into flybys
constexpr float launch_pitch_dev = 0.5f;
// Homing: with this probability per leg, steer back toward the anchor so
// flights swing by the user instead of diffusing away forever. The
// exclusion cone still trims anything inside 15 deg into a near miss.
constexpr float homing_prob = 0.5f;
constexpr float homing_blend = 0.55f;
// Yaw sampling bias: legs always turn at least this fraction of the limit
// so flights wiggle constantly instead of sneaking in near-straight legs.
constexpr float min_turn_fraction = 0.25f;
constexpr float max_pitch_change = 0.5236f; // 30 deg climb/dive change per leg
constexpr float max_abs_pitch = 0.7854f;   // 45 deg absolute climb/dive

// Engine trail ribbon: resampled while moving, fades out by age.
constexpr size_t trail_points = 60;
constexpr double trail_sample_dt = 1. / 30.;
constexpr double trail_max_age = trail_points * trail_sample_dt;
constexpr float trail_width_head = 0.02f;  // ribbon half-width at the engine (m)
constexpr float trail_width_tail = 0.055f; // ... at the faded end
constexpr float trail_alpha_max = 0.15f;
constexpr glm::vec3 trail_tint{0.9f, 0.92f, 0.98f};
// Gap between the tail and the start of the ribbon (m).
constexpr float trail_gap = 0.05f;

double smoothstep(double e0, double e1, double x)
{
	double t = std::clamp((x - e0) / (e1 - e0), 0., 1.);
	return t * t * (3. - 2. * t);
}

// body paint jobs for the "Material" part, with per-channel variance;
// glass, emissive and dark/metal parts stay as authored
struct paint
{
	glm::vec3 base;
	float variance;
};
const paint paints[]{
        {{1.f, 1.f, 1.f}, 0.f},                        // white
        {{0.62f, 0.62f, 0.62f}, 0.12f},                // grey
        {{0.75f, 0.38f, 0.04f}, 0.14f},                // dark dynacon orange
        {{0.03f, 0.06f, 0.36f}, 0.10f},                // dark #0E1B8D navy
};

} // namespace

namespace
{
struct trail_vertex
{
	glm::vec4 position;
	glm::vec4 color;
};

constexpr size_t trail_verts_per_plane = trail_points * 5;
constexpr size_t trail_indices_per_plane = (trail_points - 1) * 4 * 6;
constexpr size_t trail_max_verts = size_t(plane_count) * trail_verts_per_plane;
constexpr size_t trail_max_indices = size_t(plane_count) * trail_indices_per_plane;
constexpr size_t trail_vert_bytes = trail_max_verts * sizeof(trail_vertex);
} // namespace

namespace wivrn
{

kiosk_planes::kiosk_planes(::scene & scene, uint32_t layer_mask, uint32_t trail_layer_mask) :
        scene_(scene), rng_(std::random_device{}())
{
	for (int i = 0; i < plane_count; ++i)
	{
		auto && [entity, node] = scene_.add_gltf("assets://plane.glb", layer_mask);
		node.name = "kiosk-plane-" + std::to_string(i);
		node.scale = {model_scale, model_scale, model_scale};
		node.visible = false;

		// one material clone per source material so each plane fades
		// independently; textures stay shared, only flags and the uniform
		// staging are per-plane. The white body part gets a per-plane
		// paint job from the palette above.
		flight f;
		f.root = entity;
		const paint & paint = paints[std::uniform_int_distribution<size_t>(0, std::size(paints) - 1)(rng_)];
		std::uniform_real_distribution<float> jitter(-paint.variance, paint.variance);
		const glm::vec3 body_tint{
		        std::clamp(paint.base.x + jitter(rng_), 0.f, 1.f),
		        std::clamp(paint.base.y + jitter(rng_), 0.f, 1.f),
		        std::clamp(paint.base.z + jitter(rng_), 0.f, 1.f),
		};
		// add_gltf shares mesh objects (and their materials) across
		// instantiations: deep-copy each mesh so per-plane material clones
		// don't leak into the other planes. GPU buffers stay shared.
		std::map<std::shared_ptr<renderer::mesh>, std::shared_ptr<renderer::mesh>> mesh_copies;
		std::map<std::shared_ptr<renderer::material>, std::shared_ptr<renderer::material>> clones;
		std::function<void(entt::entity)> visit = [&](entt::entity e) {
			if (auto * n = scene_.world.try_get<components::node>(e))
			{
				if (n->mesh)
				{
					auto [mesh_it, mesh_new] = mesh_copies.try_emplace(n->mesh, nullptr);
					if (mesh_new)
					{
						mesh_it->second = std::make_shared<renderer::mesh>();
						mesh_it->second->buffer = n->mesh->buffer;
						mesh_it->second->primitives = n->mesh->primitives;
						for (auto & primitive: mesh_it->second->primitives)
						{
							auto [it, inserted] = clones.try_emplace(primitive.material_, nullptr);
							if (inserted)
							{
								auto src = primitive.material_;
								it->second = scene_.create_material([src, body_tint](renderer::material & m) {
									m.staging = src->staging;
									m.base_color_texture = src->base_color_texture;
									m.metallic_roughness_texture = src->metallic_roughness_texture;
									m.occlusion_texture = src->occlusion_texture;
									m.emissive_texture = src->emissive_texture;
									m.normal_texture = src->normal_texture;
									m.double_sided = src->double_sided;
									m.fragment_shader = src->fragment_shader;
									m.name = src->name + "-kiosk";
									if (src->name == "Material")
										m.staging.base_color_factor = glm::vec4(body_tint, 1.f);
									m.blend_enable = true;
									m.depth_write_enable = false;
								});
								f.materials.push_back(it->second);
								f.base_colors.push_back(it->second->staging.base_color_factor);
								f.base_emissives.push_back(glm::vec3(it->second->staging.base_emissive_factor));
							}
							primitive.material_ = it->second;
						}
					}
					n->mesh = mesh_it->second;
				}
				for (auto && [child, child_node]: scene_.world.view<components::node>().each())
				{
					if (child_node.parent == e)
						visit(child);
				}
			}
		};
		visit(entity);
		flights_.push_back(std::move(f));
	}

	// Measure the model OBB in plane-root space for the trail emitter: walk
	// the first flight's subtree accumulating node transforms (the file's
	// own rotations live on child nodes, so the raw primitive OBBs alone
	// point the wrong way).
	glm::vec3 obb_min{std::numeric_limits<float>::max()};
	glm::vec3 obb_max{std::numeric_limits<float>::lowest()};
	std::function<void(entt::entity, const glm::mat4 &)> measure = [&](entt::entity e, const glm::mat4 & parent) {
		const auto & n = scene_.world.get<components::node>(e);
		glm::mat4 m = parent * (glm::translate(glm::mat4(1), n.position) * glm::mat4(n.orientation) * glm::scale(glm::mat4(1), n.scale));
		if (n.mesh)
		{
			for (auto & prim: n.mesh->primitives)
			{
				for (int i = 0; i < 8; ++i)
				{
					glm::vec3 corner{
					        (i & 1) ? prim.obb_max.x : prim.obb_min.x,
					        (i & 2) ? prim.obb_max.y : prim.obb_min.y,
					        (i & 4) ? prim.obb_max.z : prim.obb_min.z,
					};
					glm::vec3 w = m * glm::vec4(corner, 1);
					obb_min.x = std::min(obb_min.x, w.x);
					obb_min.y = std::min(obb_min.y, w.y);
					obb_min.z = std::min(obb_min.z, w.z);
					obb_max.x = std::max(obb_max.x, w.x);
					obb_max.y = std::max(obb_max.y, w.y);
					obb_max.z = std::max(obb_max.z, w.z);
				}
			}
		}
		for (auto && [child, child_node]: scene_.world.view<components::node>().each())
		{
			if (child_node.parent == e)
				measure(child, m);
		}
	};
	measure(flights_.front().root, glm::mat4(1));
	// Nose points along +Z: emit at the tail center.
	tail_root = {(obb_min.x + obb_max.x) * 0.5f, (obb_min.y + obb_max.y) * 0.5f, obb_min.z};

	// Shared trail ribbon: one node/primitive holding every flight's
	// camera-facing strip (debug_draw shaders: unlit position+color).
	trail_entity = scene_.world.create();
	auto & trail_node = scene_.world.emplace<components::node>(trail_entity);
	trail_node.name = "kiosk-trails";
	trail_node.layer_mask = trail_layer_mask;
	trail_node.visible = false;

	trail_mesh = std::make_shared<renderer::mesh>();
	auto & prim = trail_mesh->primitives.emplace_back();
	prim.indexed = true;
	prim.index_type = vk::IndexType::eUint16;
	prim.index_offset = trail_vert_bytes;
	prim.topology = vk::PrimitiveTopology::eTriangleList;
	prim.cull_mode = vk::CullModeFlagBits::eNone;
	prim.vertex_shader = "debug_draw.vert";
	prim.layout.add_vertex_attribute("Position", vk::Format::eR32G32B32A32Sfloat, 0, 0);
	prim.layout.add_vertex_attribute("Color", vk::Format::eR32G32B32A32Sfloat, 0, 1);
	prim.obb_min = {-10.f, -10.f, -10.f};
	prim.obb_max = {10.f, 10.f, 10.f};
	prim.material_ = scene_.create_material([](renderer::material & m) {
		m.name = "kiosk-trail";
		m.fragment_shader = "debug_draw.frag";
		m.blend_enable = true;
		// Additive: overlapping trail segments sum order-independently
		// instead of slicing each other out with sorted over-blends.
		m.blend_additive = true;
		m.depth_write_enable = false;
	});

	trail_buffer = std::make_shared<buffer_allocation>(
	        scene_.device,
	        vk::BufferCreateInfo{
	                .size = trail_vert_bytes + trail_max_indices * sizeof(uint16_t),
	                .usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer,
	        },
	        VmaAllocationCreateInfo{
	                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
	                .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
	        },
	        "kiosk_planes::trails");

	// Fixed strip topology: one independent 5-wide quad strip per flight.
	// The across-width alpha profile (0 at the edges) makes the ribbon
	// soft on all sides; length fade is written per-vertex each frame.
	auto * index = reinterpret_cast<uint16_t *>(trail_buffer->map()) + trail_vert_bytes / sizeof(uint16_t);
	for (int p = 0; p < plane_count; ++p)
	{
		for (size_t j = 0; j < trail_points - 1; ++j)
		{
			for (size_t k = 0; k < 4; ++k)
			{
				uint16_t a = uint16_t(p * trail_verts_per_plane + 5 * j + k);
				*index++ = a;
				*index++ = a + 1;
				*index++ = a + 5;
				*index++ = a + 1;
				*index++ = a + 6;
				*index++ = a + 5;
			}
		}
	}

	prim.vertex_offset.push_back(0);
	prim.vertex_count = uint32_t(trail_max_verts);
	prim.index_count = uint32_t(trail_max_indices);
	trail_mesh->buffer = trail_buffer;
	trail_node.mesh = trail_mesh;
}

glm::vec3 kiosk_planes::bezier(const flight & f, double t)
{
	double u = 1. - t;
	return f.p0 * float(u * u * u) + f.p1 * float(3. * u * u * t) + f.p2 * float(3. * u * t * t) + f.p3 * float(t * t * t);
}

glm::vec3 kiosk_planes::bezier_vel(const flight & f, double t)
{
	double u = 1. - t;
	return (f.p1 - f.p0) * float(3. * u * u) + (f.p2 - f.p1) * float(6. * u * t) + (f.p3 - f.p2) * float(3. * t * t);
}

glm::vec3 kiosk_planes::bezier_acc(const flight & f, double t)
{
	double u = 1. - t;
	return 6.f * float(u) * (f.p2 - 2.f * f.p1 + f.p0) + 6.f * float(t) * (f.p3 - 2.f * f.p2 + f.p1);
}

void kiosk_planes::respawn(flight & f, XrTime now, const glm::vec3 & anchor_pos, const glm::quat & anchor_orient, double age_offset)
{
	glm::mat3 R = glm::mat3_cast(anchor_orient);
	std::uniform_real_distribution<float> lateral(-0.3f, 0.3f);
	std::uniform_real_distribution<float> vertical(-0.2f, 0.2f);
	std::uniform_real_distribution<float> near_dist(0.6f, 1.0f);
	std::uniform_real_distribution<float> mid_dist(0.8f, 1.3f);
	std::uniform_real_distribution<float> far_dist(1.5f, 2.2f);
	std::uniform_real_distribution<double> dur_dist(dur_min, dur_max);

	auto clamp_height = [&](glm::vec3 v) {
		v.y = std::clamp(v.y, anchor_pos.y - 0.6f, anchor_pos.y + 0.9f);
		return v;
	};

	// launch from just behind the overlay panel; the anchor is frozen here
	// so later panel motion never perturbs the flight
	f.p0 = anchor_pos + R * glm::vec3(lateral(rng_), vertical(rng_), -0.2f);

	// Keep flights out of the exclusion cone around the anchor-to-user
	// axis: paths crossing right in front of the face are startling (and
	// alias badly). Offending control points move to the nearest cone edge
	// with a small margin; the fade-in covers the spawn behind the panel.
	// p0 stays exempt: it sits behind the panel by construction.
	// (Inverse-rotate by the anchor orientation: R is orthonormal, so this
	// is the world-to-anchor transform without needing a transpose.)
	const glm::quat Rinv = glm::inverse(anchor_orient);
	auto push_outside_cone = [&](const glm::vec3 & world) {
		glm::vec3 local = Rinv * (world - anchor_pos);
		if (local.z > 0.f and std::abs(local.x) < local.z * exclusion_tan and
		    std::abs(local.y) < local.z * exclusion_tan)
		{
			const float bx = local.z * exclusion_tan * 1.2f;
			const float by = local.z * exclusion_tan * 1.2f;
			if (std::abs(local.x) / bx < std::abs(local.y) / by)
				local.x = local.x >= 0.f ? bx : -bx;
			else
				local.y = local.y >= 0.f ? by : -by;
		}
		return anchor_pos + R * local;
	};
	auto leg_pitch = [](const glm::vec3 & d) {
		float len = glm::length(d);
		return len > 1e-6f ? std::asin(std::clamp(d.y / len, -1.f, 1.f)) : 0.f;
	};
	auto yaw_change = [](const glm::vec3 & d0, const glm::vec3 & d1) {
		glm::vec2 a(d0.x, d0.z), b(d1.x, d1.z);
		float la = glm::length(a), lb = glm::length(b);
		if (la < 1e-4f or lb < 1e-4f)
			return 999.f; // near-vertical leg: force a resample
		return std::acos(std::clamp(glm::dot(a / la, b / lb), -1.f, 1.f));
	};

	// Sample each control point chained tip-to-tail off the previous point,
	// steering around the previous leg direction: the achieved turn then
	// matches the sampled one by construction (the old code sampled around
	// the anchor, so the turn check measured a different vector than the
	// sampler produced and the fallback kept invalid legs). Cone push and
	// height clamp are verified after the fact with a few retries; the
	// fallback flies straight, which always satisfies the turn limits.
	auto sample_point = [&](const glm::vec3 & base, const glm::vec3 & prev2, const glm::vec3 & prev1, bool check_turn, auto & dist_rng, float yaw_lim, float pitch_lim, const glm::vec3 * launch_dir = nullptr) {
		auto gen = [&]() {
			glm::vec3 pd = launch_dir ? *launch_dir : prev1 - prev2;
			float pp = launch_dir ? 0.f : leg_pitch(pd);
			float p = std::clamp(pp + std::uniform_real_distribution<float>(-pitch_lim, pitch_lim)(rng_), -max_abs_pitch, max_abs_pitch);
			float u = std::uniform_real_distribution<float>(-1.f, 1.f)(rng_);
			float y = std::atan2(pd.x, pd.z) + (u >= 0.f ? 1.f : -1.f) * (min_turn_fraction + (1.f - min_turn_fraction) * std::abs(u)) * yaw_lim;
			glm::vec3 d{std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
			if (not launch_dir and std::uniform_real_distribution<float>(0.f, 1.f)(rng_) < homing_prob)
			{
				glm::vec3 to_anchor = anchor_pos - base;
				if (glm::length(to_anchor) > 1e-3f)
					d = glm::normalize(glm::mix(d, to_anchor / glm::length(to_anchor), homing_blend));
			}
			return base + d * dist_rng(rng_);
		};
		auto leg_ok = [&](const glm::vec3 & cand) {
			glm::vec3 d1 = cand - prev1;
			if (glm::length(d1) < min_leg_len or std::abs(leg_pitch(d1)) > max_abs_pitch)
				return false;
			if (glm::length(cand - anchor_pos) > max_flight_range)
				return false;
			if (check_turn)
			{
				glm::vec3 d0 = prev1 - prev2;
				if (glm::length(d0) < min_leg_len)
					return false;
				float yaw = yaw_change(d0, d1);
				if (yaw > max_yaw_turn)
					return false;
				// Wide banks only: a large direction change needs long legs,
				// otherwise it is a lightning-fast pivot, not a turn.
				if (std::min(glm::length(d0), glm::length(d1)) < min_turn_radius * yaw)
					return false;
				if (std::abs(leg_pitch(d1) - leg_pitch(d0)) > max_pitch_change)
					return false;
			}
			return true;
		};
		glm::vec3 p = clamp_height(push_outside_cone(gen()));
		for (int attempt = 0; attempt < 12 and not leg_ok(p); ++attempt)
			p = clamp_height(push_outside_cone(gen()));
		if (not leg_ok(p))
		{
			glm::vec3 pd = launch_dir ? *launch_dir : prev1 - prev2;
			float pl = glm::length(pd);
			glm::vec3 dn = pl > 1e-6f ? pd / pl : glm::vec3{0.f, 0.f, -1.f};
			p = clamp_height(push_outside_cone(base + dn * dist_rng.max()));
		}
		return p;
	};
	const glm::vec3 launch_dir = R * glm::vec3{0.f, 0.f, -1.f};
	f.p1 = sample_point(f.p0, f.p0, f.p0, false, near_dist, launch_yaw_dev, launch_pitch_dev, &launch_dir);
	f.p2 = sample_point(f.p1, f.p0, f.p1, true, mid_dist, max_yaw_turn, max_pitch_change);
	f.p3 = sample_point(f.p2, f.p1, f.p2, true, far_dist, max_yaw_turn, max_pitch_change);

	// Arc-length table for constant-speed flight: with clustered control
	// points gone the curve is well-behaved, and walking it by distance
	// (not time) removes the slow-down into what is left of the turns.
	f.total_len = 0.f;
	glm::vec3 prev = bezier(f, 0.);
	f.arclen[0] = 0.f;
	for (size_t i = 1; i < f.arclen.size(); ++i)
	{
		glm::vec3 p = bezier(f, double(i) / double(f.arclen.size() - 1));
		f.total_len += glm::distance(p, prev);
		f.arclen[i] = f.total_len;
		prev = p;
	}

	f.dur = XrDuration(dur_dist(rng_) * 1e9);
	double off = std::clamp(age_offset, 0., double(f.dur) * 1e-9);
	f.t0 = now - XrTime(off * 1e9);
	f.trail.clear();
	f.trail_t.clear();
	f.last_sample_t = 0;
	f.dist_flown = 0.f;
	f.prev_now = 0;
}

void kiosk_planes::update(XrTime now, const glm::vec3 & anchor_pos, const glm::quat & anchor_orient, bool active, const std::optional<glm::vec3> & camera)
{
	if (active && !was_active_)
	{
		// (re)opening: only (re)spawn planes that never flew or expired
		// while hidden, spread evenly with jitter. Untouched planes keep
		// their own clocks, so a flickering overlay can't regroup the
		// fleet into synchronized bundles.
		std::uniform_real_distribution<double> jitter(0., 1.);
		size_t slot = 0;
		for (auto & f: flights_)
		{
			double age = f.dur == 0 ? 1. : double(now - f.t0) / double(f.dur);
			if (f.dur != 0 && age < 1.)
				continue;
			respawn(f, now, anchor_pos, anchor_orient, 0.);
			double frac = (double(slot++) + jitter(rng_)) / double(flights_.size());
			f.t0 = now - XrTime(double(f.dur) * frac);
		}
	}
	was_active_ = active;

	for (auto & f: flights_)
	{
		auto * node = scene_.world.try_get<components::node>(f.root);
		if (not node)
			continue;
		node->visible = active;
		if (not active)
			continue;

		double age = double(now - f.t0) * 1e-9;
		double dur = double(f.dur) * 1e-9;
		if (age < 0.)
			age = 0.;
		if (age >= dur or f.dist_flown >= f.total_len)
		{
			respawn(f, now, anchor_pos, anchor_orient, 0.);
			continue;
		}
		// Arc-length walk with climb-coupled airspeed: the plane works
		// uphill and swoops downhill instead of holding exact constant
		// speed, which reads as natural velocity variation without any
		// pivoting. Degenerate paths fall back to raw time.
		auto arc_param = [&](float dist) {
			float s = std::clamp(dist, 0.f, f.total_len);
			size_t seg = 1;
			while (seg < f.arclen.size() - 1 and f.arclen[seg] < s)
				++seg;
			float s0 = f.arclen[seg - 1], s1 = f.arclen[seg];
			return (float(seg - 1) + (s1 > s0 ? (s - s0) / (s1 - s0) : 0.f)) / float(f.arclen.size() - 1);
		};
		float tt = f.total_len > 1e-6f ? arc_param(f.dist_flown) : float(age / std::max(dur, 1e-3));

		glm::vec3 vel_probe = bezier_vel(f, tt);
		float probe_len = glm::length(vel_probe);
		float slope = probe_len > 1e-6f ? vel_probe.y / probe_len : 0.f;
		float v_cruise = f.total_len / float(std::max(dur, 1e-3));
		double dt = f.prev_now == 0 ? 1. / 60. : std::clamp(double(now - f.prev_now) * 1e-9, 0.001, 0.1);
		f.prev_now = now;
		float v = v_cruise * std::clamp(1.f - 0.35f * slope, 0.7f, 1.35f);
		f.dist_flown = std::min(f.dist_flown + v * float(dt), f.total_len);
		tt = f.total_len > 1e-6f ? arc_param(f.dist_flown) : tt;

		glm::vec3 vel = bezier_vel(f, tt);
		glm::vec3 fwd{0.f, 0.f, 1.f};
		bool have_fwd = false;
		if (glm::length(vel) > 1e-6f)
		{
			fwd = glm::normalize(vel);
			have_fwd = true;
			// model nose (+Z after the file's root rotation) onto the
			// flight direction; mirrored X is absorbed by bank_sign
			glm::vec3 up0{0.f, 1.f, 0.f};
			glm::vec3 right = glm::cross(up0, fwd);
			if (glm::length(right) < 1e-4f)
				right = {1.f, 0.f, 0.f};
			else
				right = glm::normalize(right);
			glm::vec3 up = glm::cross(fwd, right);
			glm::quat orient = glm::quat_cast(glm::mat3(right, up, fwd));

			// Coordinated-turn bank from path curvature instead of the old
			// sine wobble: lateral acceleration over speed squared, scaled to
			// an arcade feel and clamped. Positive bank about the nose rolls
			// into the turn; flip bank_sign once on device if they bank out.
			glm::vec3 acc = bezier_acc(f, tt);
			float v2 = glm::dot(vel, vel);
			float kappa = glm::dot(glm::cross(vel, acc), up0) / std::max(v2 * std::sqrt(v2), 1e-6f);
			float bank = std::clamp(bank_sign * -kappa * v * v * bank_gain, -bank_max, bank_max);
			orient = glm::angleAxis(bank, fwd) * orient;

			if (model_yaw_adjust != 0.f)
				orient = orient * glm::angleAxis(model_yaw_adjust, glm::vec3(0.f, 1.f, 0.f));
			node->orientation = orient;
		}

		glm::vec3 pos = bezier(f, tt);
		node->position = pos;

		float alpha = float(smoothstep(0., fade_in, age) * (1. - smoothstep(dur - fade_out, dur, age)));
		for (size_t i = 0; i < f.materials.size(); ++i)
		{
			auto & material = f.materials[i];
			// the transparent pipeline blends premultiplied (ONE,
			// ONE_MINUS_SRC_ALPHA) while lit.frag outputs straight alpha,
			// which made alpha fades no-ops: premultiply on the CPU
			material->staging.base_color_factor = glm::vec4(glm::vec3(f.base_colors[i]) * alpha, alpha);
			material->staging.base_emissive_factor = glm::vec4(f.base_emissives[i] * alpha, 0.f);
			std::memcpy(material->buffer->map(), &material->staging, sizeof(material->staging));
			material->buffer->unmap();
		}

		// Record the engine position for the trail ribbon: the measured
		// tail offset is already in world units at the construction
		// scale, so only the orientation applies (uniform scale). The
		// sample hangs a little behind the tail so the ribbon never
		// touches the body. A long stall (hidden overlay, hitches) clears
		// the trail instead of streaking across the scene on resume.
		glm::vec3 tail = pos + node->orientation * tail_root - (have_fwd ? fwd * trail_gap : glm::vec3{0.f});
		if (f.last_sample_t != 0 and double(now - f.last_sample_t) * 1e-9 > 0.5)
		{
			f.trail.clear();
			f.trail_t.clear();
		}
		if (f.last_sample_t == 0 or double(now - f.last_sample_t) * 1e-9 >= trail_sample_dt)
		{
			f.trail.push_back(tail);
			f.trail_t.push_back(now);
			if (f.trail.size() > trail_points)
			{
				f.trail.erase(f.trail.begin());
				f.trail_t.erase(f.trail_t.begin());
			}
			f.last_sample_t = now;
		}
		if (auto * trail_node = scene_.world.try_get<components::node>(trail_entity))
			trail_node->visible = active;
		f.trail_alpha = alpha;
	}

	if (active)
		build_trail(camera, now);
}

void kiosk_planes::build_trail(const std::optional<glm::vec3> & camera, XrTime now)
{
	auto * verts = reinterpret_cast<trail_vertex *>(trail_buffer->map());
	const glm::vec3 up{0.f, 1.f, 0.f};
	constexpr double trail_max_age = trail_points * trail_sample_dt;

	for (int p = 0; p < plane_count; ++p)
	{
		const flight & f = flights_[size_t(p)];
		trail_vertex * out = verts + size_t(p) * trail_verts_per_plane;

		// Slot j=0 is the newest sample; pad short trails with the oldest
		// point (written at zero alpha below) so the fixed index buffer
		// always has valid data. Fade keys off real sample age, not slot
		// index, so a young trail never shows a hard end.
		glm::vec3 ring[trail_points];
		float ring_age[trail_points]{};
		const size_t n = f.trail.size();
		const bool live = n >= 2;
		for (size_t j = 0; j < trail_points; ++j)
		{
			const size_t src = live ? n - 1 - std::min(j, n - 1) : 0;
			ring[j] = live ? f.trail[src] : glm::vec3(0.f);
			ring_age[j] = live ? float(double(now - f.trail_t[src]) * 1e-9) : 0.f;
		}

		glm::vec3 last_side{1.f, 0.f, 0.f};
		for (size_t j = 0; j < trail_points; ++j)
		{
			const float t = float(j) / float(trail_points - 1); // 0 = engine, 1 = faded end
			const glm::vec3 & pt = ring[j];

			glm::vec3 dir = ring[std::min(j + 1, trail_points - 1)] - ring[j > 0 ? j - 1 : j];
			glm::vec3 side = last_side;
			if (glm::length(dir) > 1e-6f)
			{
				glm::vec3 dir_n = dir / glm::length(dir);
				glm::vec3 axis = camera ? (*camera - pt) : up;
				if (glm::length(axis) > 1e-6f)
				{
					glm::vec3 s = glm::cross(dir_n, axis / glm::length(axis));
					if (glm::length(s) > 1e-4f)
						side = s / glm::length(s);
					else
					{
						s = glm::cross(dir_n, up);
						if (glm::length(s) > 1e-4f)
							side = s / glm::length(s);
					}
				}
				last_side = side;
			}

			const float width = trail_width_head + (trail_width_tail - trail_width_head) * t;
			// Gradual age falloff (long tail, not a hard end) times a head
			// ramp over the first segments, so a young trail can never show
			// a bright leading edge no matter the sample age. The tail ramp
			// keys off the live point count (not the slot index): padding
			// duplicates the oldest sample, so without it a young trail
			// would end in a bright streak cut off at the last live point.
			const float age_frac = std::clamp(ring_age[j] / float(trail_max_age), 0.f, 1.f);
			const float envelope = std::pow(1.f - age_frac, 1.5f);
			const float head_ramp = float(smoothstep(0., 8., double(j)));
			const float tail_ramp = j < n ? 1.f - float(smoothstep(double(n > 4 ? n - 4 : 0), double(n > 1 ? n - 1 : 0), double(j))) : 0.f;
			const float a = live ? trail_alpha_max * f.trail_alpha * envelope * head_ramp * tail_ramp : 0.f;

			// Soft across-width profile: transparent edges, bright core.
			constexpr float across[5] = {-1.f, -0.5f, 0.f, 0.5f, 1.f};
			constexpr float across_alpha[5] = {0.f, 0.6f, 1.f, 0.6f, 0.f};
			for (size_t k = 0; k < 5; ++k)
			{
				const float ak = a * across_alpha[k];
				out[5 * j + k] = {
				        glm::vec4(pt + side * (across[k] * width * 0.5f), 1.f),
				        glm::vec4(trail_tint * ak, ak), // premultiplied
				};
			}
		}
	}
}

} // namespace wivrn
