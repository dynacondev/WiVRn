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

#pragma once

#include "render/scene_components.h"
#include <entt/entity/fwd.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <openxr/openxr.h>
#include <random>
#include <vector>

class scene;

namespace wivrn
{

// Small fleet of model planes for the kiosk waiting overlays, shared by the
// lobby and stream scenes. Each plane flies a frozen world-space Bezier path
// launched from behind the overlay panel, then fades out and respawns: later
// panel motion never perturbs a flight in progress. Create once the scene
// world exists (on_focused), update every frame the overlay may show, destroy
// with the scene (planes hold entities in the scene registry).
class kiosk_planes
{
public:
	kiosk_planes(::scene & scene, uint32_t layer_mask);

	// anchor is the overlay panel pose sampled this frame; only used when
	// (re)spawning. Inactive planes are hidden and frozen.
	void update(XrTime now, const glm::vec3 & anchor_pos, const glm::quat & anchor_orient, bool active);

private:
	struct flight
	{
		entt::entity root;
		std::vector<std::shared_ptr<renderer::material>> materials;
		glm::vec3 p0, p1, p2, p3;
		XrTime t0 = 0;
		XrDuration dur = 0;
		float phase = 0;
	};

	::scene & scene_;
	std::vector<flight> flights_;
	std::mt19937 rng_;
	bool was_active_ = false;

	void respawn(flight & f, XrTime now, const glm::vec3 & anchor_pos, const glm::quat & anchor_orient, double age_offset);
	static glm::vec3 bezier(const flight & f, double t);
	static glm::vec3 bezier_vel(const flight & f, double t);
};

} // namespace wivrn
