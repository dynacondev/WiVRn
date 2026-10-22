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
#include <iterator>
#include <map>
#include <numbers>
#include <string>

namespace
{
// tuning knobs, all device-tunable: model is ~13.4m wingspan in file units
constexpr int plane_count = 4;
constexpr float model_scale = 0.00675f; // ~0.13m wingspan
constexpr double dur_min = 5.0;
constexpr double dur_max = 8.0;
constexpr double fade_in = 1.2;
constexpr double fade_out = 2.0;

// model nose points along +Z once the file's own root rotation (-90deg
// about X on "Air Plane") is applied; tweak below only if a re-export
// changes that convention
constexpr float model_yaw_adjust = 0.f;
// mirrored model X flips the apparent bank direction; flip once on device
// if planes roll out of their turns instead of into them
constexpr float bank_sign = 1.f;
constexpr float bank_amount = 0.25f;

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

glm::vec3 random_unit(std::mt19937 & rng)
{
	std::uniform_real_distribution<float> dist(-1.f, 1.f);
	glm::vec3 v(dist(rng), dist(rng), dist(rng));
	if (glm::length(v) < 1e-3f)
		return {0.f, 0.f, 1.f};
	return glm::normalize(v);
}
} // namespace

namespace wivrn
{

kiosk_planes::kiosk_planes(::scene & scene, uint32_t layer_mask) :
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

void kiosk_planes::respawn(flight & f, XrTime now, const glm::vec3 & anchor_pos, const glm::quat & anchor_orient, double age_offset)
{
	glm::mat3 R = glm::mat3_cast(anchor_orient);
	std::uniform_real_distribution<float> lateral(-0.3f, 0.3f);
	std::uniform_real_distribution<float> vertical(-0.2f, 0.2f);
	std::uniform_real_distribution<float> near_dist(0.8f, 1.4f);
	std::uniform_real_distribution<float> mid_dist(1.2f, 2.2f);
	std::uniform_real_distribution<float> far_dist(3.0f, 4.5f);
	std::uniform_real_distribution<double> dur_dist(dur_min, dur_max);
	std::uniform_real_distribution<float> phase_dist(0.f, 2.f * std::numbers::pi_v<float>);

	auto clamp_height = [&](glm::vec3 v) {
		v.y = std::clamp(v.y, anchor_pos.y - 0.6f, anchor_pos.y + 0.9f);
		return v;
	};

	// launch from just behind the overlay panel; the anchor is frozen here
	// so later panel motion never perturbs the flight
	f.p0 = anchor_pos + R * glm::vec3(lateral(rng_), vertical(rng_), -0.2f);
	f.p1 = clamp_height(f.p0 + random_unit(rng_) * near_dist(rng_));
	f.p2 = clamp_height(anchor_pos + random_unit(rng_) * mid_dist(rng_));
	f.p3 = clamp_height(anchor_pos + random_unit(rng_) * far_dist(rng_));
	f.dur = XrDuration(dur_dist(rng_) * 1e9);
	f.phase = phase_dist(rng_);
	double off = std::clamp(age_offset, 0., double(f.dur) * 1e-9);
	f.t0 = now - XrTime(off * 1e9);
}

void kiosk_planes::update(XrTime now, const glm::vec3 & anchor_pos, const glm::quat & anchor_orient, bool active)
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
		if (age >= dur)
		{
			respawn(f, now, anchor_pos, anchor_orient, 0.);
			continue;
		}
		double t = age / dur;

		glm::vec3 vel = bezier_vel(f, t);
		if (glm::length(vel) > 1e-6f)
		{
			glm::vec3 fwd = glm::normalize(vel);
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
			float bank = bank_sign * bank_amount * std::sin(2. * std::numbers::pi * t + f.phase);
			orient = glm::angleAxis(bank, fwd) * orient;
			if (model_yaw_adjust != 0.f)
				orient = orient * glm::angleAxis(model_yaw_adjust, glm::vec3(0.f, 1.f, 0.f));
			node->orientation = orient;
		}

		glm::vec3 pos = bezier(f, t);
		pos.y += 0.015f * std::sin(2. * std::numbers::pi * age * 0.7 + f.phase);
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
	}
}

} // namespace wivrn
