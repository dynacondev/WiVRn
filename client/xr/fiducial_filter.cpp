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

#include "fiducial_filter.h"

#include <algorithm>
#include <cmath>

namespace xr
{

void fiducial_filter::configure(const tuning & t)
{
	cfg = t;
	cfg.euro_min_cutoff = std::max(0.05f, cfg.euro_min_cutoff);
	cfg.euro_beta = std::max(0.f, cfg.euro_beta);
	cfg.sigma_k = std::max(0.5f, cfg.sigma_k);
	cfg.hold_inner_mm = std::max(0.f, cfg.hold_inner_mm);
	cfg.hold_inner_deg = std::max(0.f, cfg.hold_inner_deg);
	reset();
}

void fiducial_filter::reset()
{
	held.reset();
	held_time = 0;
	recent.clear();
	last_weight = 1;
	rendered_valid = false;
	reseat_trust = false;
	reseat_frames = 0;
	vel_hat = glm::vec3(0);
	omega_hat = 0;
	prev_target_valid = false;
}

void fiducial_filter::begin_reseat()
{
	// Keep held/smoothed (the glide starts from the live estimate, no
	// pop); drop everything that would read the relocalized truth as a
	// spike: predecessor spread, stale velocity, and the last weight.
	recent.clear();
	last_weight = 1;
	vel_hat = glm::vec3(0);
	omega_hat = 0;
	prev_target_valid = false;
	reseat_trust = true;
	reseat_frames = 0;
}

static float angle_between(const glm::quat & a, const glm::quat & b)
{
	float c = std::clamp(std::abs(glm::dot(a, b)), 0.f, 1.f);
	return 2.f * std::acos(c);
}

void fiducial_filter::ingest(const glm::vec3 & pos, const glm::quat & quat, XrTime t)
{
	// Hemisphere-align to the smoothed pose when seeded, else to the most
	// recent sample. Aligning to the oldest window entry (previous design)
	// let a stale outlier own the reference; the smoothed pose is the live
	// estimate and the correct reference.
	glm::quat q = quat;
	if (rendered_valid)
	{
		if (glm::dot(q, smoothed.quat) < 0)
			q = -q;
	}
	else if (not recent.empty())
	{
		if (glm::dot(q, recent.back().quat) < 0)
			q = -q;
	}

	const float sigma_pos_m = std::max(1e-6f, cfg.hold_inner_mm / 1000.f);
	constexpr float deg_to_rad = 3.14159265358979323846f / 180.f;
	const float sigma_rot = std::max(1e-6f, cfg.hold_inner_deg * deg_to_rad);

	// Stage the sample in the recent history first; the predecessor spread
	// (all but the new back entry) adapts the outlier scale below.
	recent.push_back({pos, q, t});
	while (recent.size() > recent_depth)
		recent.pop_front();

	// Max pairwise spread of the predecessors (excludes the new sample):
	// tight means the past is certain, wide means motion is underway.
	float spread_pos = 0, spread_rot = 0;
	if (recent.size() >= 3)
	{
		for (size_t i = 0; i + 1 < recent.size() - 1; ++i)
		{
			for (size_t j = i + 1; j < recent.size() - 1; ++j)
			{
				spread_pos = std::max(spread_pos, glm::distance(recent[i].pos, recent[j].pos));
				spread_rot = std::max(spread_rot, angle_between(recent[i].quat, recent[j].quat));
			}
		}
	}
	float eff_pos = std::max(sigma_pos_m, spread_pos / spread_divisor);
	float eff_rot = std::max(sigma_rot, spread_rot / spread_divisor);

	// Accepted when close to smoothed (or motion already underway, which
	// widens the scale above). Velocity (which opens the Euro cutoff)
	// updates only on accepted samples so a spike neither moves the pose
	// nor widens the gate for itself.
	bool accept_vel = not rendered_valid;
	if (rendered_valid)
	{
		float d_pos = glm::distance(pos, smoothed.pos);
		float d_rot = angle_between(q, smoothed.quat);
		float n_pos = d_pos / eff_pos / std::max(1e-6f, cfg.sigma_k);
		float n_rot = d_rot / eff_rot / std::max(1e-6f, cfg.sigma_k);
		float w_pos = 1.f / (1.f + n_pos * n_pos);
		float w_rot = 1.f / (1.f + n_rot * n_rot);
		accept_vel = w_pos > 0.3f or w_rot > 0.3f;
	}
	if (prev_target_valid and accept_vel)
	{
		double dt_n = (t - prev_target_time) * 1e-9;
		if (dt_n > 1e-6 and dt_n < 5.0)
		{
			glm::vec3 v_inst = (pos - prev_target_pos) / (float)dt_n;
			float w_inst = 0;
			if (dt_n > 0)
			{
				glm::quat dq = q * glm::conjugate(prev_target_quat);
				w_inst = 2.f * std::acos(std::clamp(std::abs(dq.w), 0.f, 1.f)) / (float)dt_n;
				if (not std::isfinite(w_inst))
					w_inst = 0;
			}
			if (std::isfinite(v_inst.x) and std::isfinite(v_inst.y) and std::isfinite(v_inst.z) and std::isfinite(w_inst))
			{
				float a_d = alpha_for(1.0f, dt_n);
				vel_hat += a_d * (v_inst - vel_hat);
				omega_hat += a_d * (w_inst - omega_hat);
			}
		}
	}
	else if (not rendered_valid)
	{
		vel_hat = glm::vec3(0);
		omega_hat = 0;
	}

	prev_target_pos = pos;
	prev_target_quat = q;
	prev_target_time = t;
	prev_target_valid = true;
	held = pose{pos, q};
	held_time = t;
}

float fiducial_filter::alpha_for(float cutoff, double dt)
{
	if (cutoff <= 0 or dt <= 0)
		return 1;
	constexpr float pi = 3.14159265358979323846f;
	float tau = 1.f / (2.f * pi * cutoff);
	return (float)(1.0 / (1.0 + tau / dt));
}

std::optional<fiducial_filter::pose> fiducial_filter::resolve() const
{
	if (not held)
		return std::nullopt;
	return held;
}

fiducial_filter::pose fiducial_filter::advance(double dt_s)
{
	if (not held)
		return smoothed;
	if (not rendered_valid)
	{
		snap();
		return smoothed;
	}
	double dt = std::clamp(dt_s, 1e-4, 0.25);

	// One Euro cutoffs from the accepted-target speed. Shared params for
	// position and rotation; angular speeds are numerically smaller so
	// rotation keeps the historical x10 weight (no extra knob). The
	// reseat trust window raises the floor so the correction glides
	// briskly instead of at the at-rest cutoff.
	float base_cutoff = reseat_trust ? std::max(cfg.euro_min_cutoff, reseat_cutoff_hz) : cfg.euro_min_cutoff;
	float cutoff_pos = base_cutoff + cfg.euro_beta * glm::length(vel_hat);
	float alpha_pos = alpha_for(cutoff_pos, dt);
	float cutoff_rot = base_cutoff + cfg.euro_beta * omega_hat * 10.f;
	float alpha_rot = alpha_for(cutoff_rot, dt);

	glm::vec3 dpos = held->pos - smoothed.pos;
	float dist_m = glm::length(dpos);
	const float sigma_pos_m = std::max(1e-6f, cfg.hold_inner_mm / 1000.f);
	constexpr float deg_to_rad = 3.14159265358979323846f / 180.f;
	constexpr float rad_to_deg = 180.f / 3.14159265358979323846f;
	const float sigma_rot = std::max(1e-6f, cfg.hold_inner_deg * deg_to_rad);

	// Predecessor spread adapts the Cauchy scale (same basis as ingest):
	// consistent motion widens its own gate, a lone spike faces the tight
	// nominal floor.
	float spread_pos = 0, spread_rot = 0;
	if (recent.size() >= 3)
	{
		for (size_t i = 0; i + 1 < recent.size() - 1; ++i)
		{
			for (size_t j = i + 1; j < recent.size() - 1; ++j)
			{
				spread_pos = std::max(spread_pos, glm::distance(recent[i].pos, recent[j].pos));
				spread_rot = std::max(spread_rot, angle_between(recent[i].quat, recent[j].quat));
			}
		}
	}
	float eff_pos = std::max(sigma_pos_m, spread_pos / spread_divisor);
	float eff_rot = std::max(sigma_rot, spread_rot / spread_divisor);
	float n_pos = dist_m / eff_pos / std::max(1e-6f, cfg.sigma_k);
	float wpos = 1.f / (1.f + n_pos * n_pos);

	glm::quat qheld = held->quat;
	if (glm::dot(qheld, smoothed.quat) < 0)
		qheld = -qheld;
	float ang = angle_between(qheld, smoothed.quat);
	float n_rot = ang / eff_rot / std::max(1e-6f, cfg.sigma_k);
	float wrot = 1.f / (1.f + n_rot * n_rot);

	// Soft-knee hold: sub-inner residuals freeze, outer (5x inner) and
	// beyond pass fully, smoothstep between. Kills sub-perceptual jitter
	// without the stick-slip step of a binary deadband.
	float inner_mm = cfg.hold_inner_mm;
	float outer_mm = inner_mm * hold_outer_ratio;
	float dist_mm = dist_m * 1000.f;
	float tpos = 1;
	if (outer_mm > inner_mm + 1e-6f)
		tpos = std::clamp((dist_mm - inner_mm) / (outer_mm - inner_mm), 0.f, 1.f);
	else
		tpos = dist_mm <= inner_mm ? 0.f : 1.f;
	tpos = tpos * tpos * (3 - 2 * tpos);

	float inner_deg = cfg.hold_inner_deg;
	float outer_deg = inner_deg * hold_outer_ratio;
	float ang_deg = ang * rad_to_deg;
	float trot = 1;
	if (outer_deg > inner_deg + 1e-6f)
		trot = std::clamp((ang_deg - inner_deg) / (outer_deg - inner_deg), 0.f, 1.f);
	else
		trot = ang_deg <= inner_deg ? 0.f : 1.f;
	trot = trot * trot * (3 - 2 * trot);

	float fpos = std::clamp(alpha_pos * wpos * tpos, 0.f, 1.f);
	float frot = std::clamp(alpha_rot * wrot * trot, 0.f, 1.f);
	if (reseat_trust)
	{
		// Relocalized truth: glide with the brisk cutoff only (unity
		// weights, no Cauchy/hold gating), so the correction always
		// completes instead of sticking part-way like a gated spike.
		fpos = std::clamp(alpha_pos, 0.f, 1.f);
		frot = std::clamp(alpha_rot, 0.f, 1.f);
	}
	smoothed.pos += dpos * fpos;
	smoothed.quat = glm::normalize(glm::slerp(smoothed.quat, qheld, frot));
	last_weight = reseat_trust ? 1 : std::min(wpos, wrot);
	if (reseat_trust)
	{
		// Adaptive end: residual back inside the outer hold knee. Frame
		// cap terminates the window even if it never settles.
		float post_mm = glm::length(held->pos - smoothed.pos) * 1000.f;
		float post_deg = angle_between(qheld, smoothed.quat) * rad_to_deg;
		++reseat_frames;
		if ((post_mm <= std::max(outer_mm, 1e-3f) and post_deg <= std::max(outer_deg, 1e-3f)) or
		    reseat_frames >= reseat_frame_cap)
			reseat_trust = false;
	}
	return smoothed;
}

bool fiducial_filter::snap()
{
	if (not held)
		return false;
	smoothed = *held;
	rendered_valid = true;
	reseat_trust = false;
	reseat_frames = 0;
	vel_hat = glm::vec3(0);
	omega_hat = 0;
	last_weight = 1;
	return true;
}

} // namespace xr
