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
// Two stages, both adaptive (no fixed gates, no binary deadband):
//
//  1. Resolve: a rolling window of mesh-target samples (marker pose *
//     config offset, world space) is robustly averaged. Each sample gets a
//     Cauchy soft-weight from its distance to the window median, scaled by
//     MAD-derived sigma: mild outliers count a little, wild ones ~zero.
//     During warmup (fewer than min_samples) everything counts at full
//     weight so the filter seeds; a genuine move migrates the median after
//     ~window/2 consistent samples while a lone spike cannot drag it.
//  2. Follow: the per-frame rendered pose approaches the resolved target
//     through a One Euro (velocity-adaptive) stage plus an exponential
//     catch-up with a soft-knee residual clamp. At rest the cutoff
//     collapses and jitter cancels; sustained motion (even slow drift)
//     carries velocity, opens the cutoff, and gets through — no
//     stick-slip step like a binary deadband would produce.
//
// All state lives on the render thread; ingest() runs at the live update
// rate (5-10Hz novel samples), advance() runs every frame.
namespace xr
{

class fiducial_filter
{
public:
	struct tuning
	{
		int window_size = 12;
		int min_samples = 4;
		float sigma_k = 3;
		float pos_gain = 3;
		float rot_gain = 3;
		float euro_min_cutoff = 0.4f;
		float euro_beta = 0.07f;
		float knee_inner_mm = 1;
		float knee_outer_mm = 5;
		float knee_inner_deg = 0.1f;
		float knee_outer_deg = 0.5f;
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

	// Ingest one mesh-target sample (world space). Duplicates/echoes must
	// be filtered by the caller (marker_tracker::sighting::novel).
	void ingest(const glm::vec3 & pos, const glm::quat & quat, XrTime t);

	bool has_target() const
	{
		return not window.empty();
	}
	size_t sample_count() const
	{
		return window.size();
	}
	// Mean Cauchy weight of the current window (1 = clean, ->0 = noisy).
	float mean_weight() const
	{
		return last_mean_weight;
	}

	// Robust resolved target from the current window.
	std::optional<pose> resolve() const;

	// Advance the smoothed rendered pose toward resolve() by dt seconds.
	// Proportional: large errors take large steps, converging without
	// overshoot; sub-knee residuals hold still.
	pose advance(double dt_s);

	// Snap the rendered pose to resolve() (first seed / manual re-seed).
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
		glm::quat quat; // hemisphere-aligned to the seed at ingest
		XrTime time;
	};

	// Scalar One Euro channel for the resolved-position stream.
	struct euro_channel
	{
		bool init = false;
		float x_prev = 0;
		float dx_hat = 0;
		float x_hat = 0;
		float filter(float x, double dt, float min_cutoff, float beta);
		void reset(float x)
		{
			init = true;
			x_prev = x;
			x_hat = x;
			dx_hat = 0;
		}
	};

	static float median_of(float * v, size_t n);
	static float alpha_for(float cutoff, double dt);

	tuning cfg;
	std::deque<sample> window;
	mutable float last_mean_weight = 1;

	pose smoothed{};
	bool rendered_valid = false;
	euro_channel euro[3];
	bool euro_init = false;
	// Filtered angular speed (rad/s) driving the adaptive rotation alpha.
	float omega_hat = 0;
	glm::quat prev_resolved{1, 0, 0, 0};
	bool prev_resolved_valid = false;
};

} // namespace xr
