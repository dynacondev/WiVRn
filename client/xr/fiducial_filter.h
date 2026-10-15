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

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <openxr/openxr.h>

// Continuous-mode pose filter for fiducial-anchored passthrough meshes.
//
// Fused robust One Euro (no window, no median delay): each novel detector
// sample (marker pose * config offset, world space) updates a held target,
// and the per-frame rendered pose approaches it with a velocity-adaptive
// (One Euro) alpha multiplied by a Cauchy soft-weight and a soft-knee hold
// for sub-perceptual residuals. At rest the cutoff collapses and jitter
// cancels; sustained motion opens the cutoff and gets through. The Cauchy
// scale adapts to the recent target spread (excluding the current sample):
// tight history means spikes get ~zero weight and cannot drag the pose,
// while consistent motion widens the scale so genuine moves migrate within
// a few novel intervals instead of sticking.
//
// All state lives on the render thread; ingest() runs at the live novel
// rate (5-10Hz), advance() runs every frame against the held target.
namespace xr
{

class fiducial_filter
{
public:
	struct tuning
	{
		float euro_min_cutoff = 0.4f;
		float euro_beta = 0.07f;
		float sigma_k = 3;
		float hold_inner_mm = 1;
		float hold_inner_deg = 0.1f;
	};

	struct pose
	{
		glm::vec3 pos{0};
		glm::quat quat{1, 0, 0, 0};
	};

	fiducial_filter() = default;
	explicit fiducial_filter(const tuning & t) : cfg(t) {}

	void configure(const tuning & t);
	void reset();

	// Ingest one mesh-target sample (world space). Only novel detector
	// output: the caller must filter on marker_tracker::sighting::novel,
	// echoes (repeated poses) carry no information and must not enter.
	void ingest(const glm::vec3 & pos, const glm::quat & quat, XrTime t);

	bool has_target() const
	{
		return held.has_value();
	}
	size_t sample_count() const
	{
		return recent.size();
	}
	// Last robust weight (min of position/rotation, 1 = clean, ->0 = rejected).
	float mean_weight() const
	{
		return last_weight;
	}

	// Latest held novel target (null when nothing ingested yet).
	std::optional<pose> resolve() const;

	// Advance the smoothed rendered pose toward the held target by dt
	// seconds. Robust One Euro: large errors take large steps, converging
	// without overshoot; sub-hold residuals blend to still.
	pose advance(double dt_s);

	// Snap the rendered pose to the held target (first seed / re-seed).
	// Returns false when there is nothing to seed from.
	bool snap();

	pose rendered() const
	{
		return smoothed;
	}
	bool has_rendered() const
	{
		return rendered_valid;
	}

private:
	struct sample
	{
		glm::vec3 pos;
		glm::quat quat; // hemisphere-aligned at ingest
		XrTime time;
	};

	static float alpha_for(float cutoff, double dt);

	// Outer hold knee is a fixed multiple of the inner (config carries the
	// inner only): sub-inner residuals hold, outer and beyond pass fully.
	static constexpr float hold_outer_ratio = 5.f;
	// Adaptive-scale divisor: the Cauchy scale is the recent predecessor
	// spread / this (floored at the nominal noise floor), so consistent
	// motion widens its own gate. ~1.5 migrates a continuing step with
	// weight ~0.5 on its third sample.
	static constexpr float spread_divisor = 1.5f;
	static constexpr size_t recent_depth = 4;

	tuning cfg;
	// Held novel target + short history for move-vs-spike disambiguation.
	std::optional<pose> held;
	XrTime held_time = 0;
	std::deque<sample> recent;
	mutable float last_weight = 1;

	pose smoothed{};
	bool rendered_valid = false;
	// Filtered target speed from accepted novel samples (spikes frozen out).
	glm::vec3 vel_hat{0};
	float omega_hat = 0;
	// Previous novel target for velocity differencing.
	glm::vec3 prev_target_pos{0};
	glm::quat prev_target_quat{1, 0, 0, 0};
	XrTime prev_target_time = 0;
	bool prev_target_valid = false;
};

} // namespace xr
