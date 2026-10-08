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
#include <vector>

namespace xr
{

void fiducial_filter::configure(const tuning & t)
{
	cfg = t;
	cfg.window_size = std::max(2, cfg.window_size);
	cfg.min_samples = std::clamp(cfg.min_samples, 1, cfg.window_size);
	cfg.sigma_k = std::max(0.5f, cfg.sigma_k);
	reset();
}

void fiducial_filter::reset()
{
	window.clear();
	last_mean_weight = 1;
	rendered_valid = false;
	euro_init = false;
	omega_hat = 0;
	prev_resolved_valid = false;
}

void fiducial_filter::ingest(const glm::vec3 & pos, const glm::quat & quat, XrTime t)
{
	glm::quat q = quat;
	if (not window.empty() and glm::dot(q, window.front().quat) < 0)
		q = -q;
	window.push_back({pos, q, t});
	while ((int)window.size() > cfg.window_size)
		window.pop_front();
}

float fiducial_filter::median_of(float * v, size_t n)
{
	std::nth_element(v, v + n / 2, v + n);
	float m = v[n / 2];
	if ((n & 1) == 0)
	{
		std::nth_element(v, v + n / 2 - 1, v + n);
		m = (m + v[n / 2 - 1]) * 0.5f;
	}
	return m;
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
	if (window.empty())
		return std::nullopt;
	size_t n = window.size();

	// Per-axis median + MAD (robust sigma). Warmup: accept all at ~full
	// weight so the filter seeds instead of gating on thin statistics.
	std::vector<float> xs(n), ys(n), zs(n);
	for (size_t i = 0; i < n; ++i)
	{
		xs[i] = window[i].pos.x;
		ys[i] = window[i].pos.y;
		zs[i] = window[i].pos.z;
	}
	float mx = median_of(xs.data(), n);
	float my = median_of(ys.data(), n);
	float mz = median_of(zs.data(), n);
	float sx = 1e-6f, sy = 1e-6f, sz = 1e-6f;
	if (n >= 2)
	{
		for (size_t i = 0; i < n; ++i)
		{
			xs[i] = std::abs(window[i].pos.x - mx);
			ys[i] = std::abs(window[i].pos.y - my);
			zs[i] = std::abs(window[i].pos.z - mz);
		}
		sx = std::max(1e-6f, 1.4826f * median_of(xs.data(), n));
		sy = std::max(1e-6f, 1.4826f * median_of(ys.data(), n));
		sz = std::max(1e-6f, 1.4826f * median_of(zs.data(), n));
	}

	// Median orientation: weighted-sum reference is the first sample (all
	// are hemisphere-aligned at ingest, dispersions here are small).
	glm::quat qref = window.front().quat;
	glm::vec4 qacc(0);
	float wsum = 0;
	glm::dvec3 pacc(0);
	float wsum_pos = 0;
	float wsum_all = 0;
	bool warmup = (int)n < cfg.min_samples;
	for (const auto & s: window)
	{
		float dx = std::abs(s.pos.x - mx) / sx;
		float dy = std::abs(s.pos.y - my) / sy;
		float dz = std::abs(s.pos.z - mz) / sz;
		float d = std::max({dx, dy, dz});
		float w = warmup ? 1.f : 1.f / (1.f + (d / cfg.sigma_k) * (d / cfg.sigma_k));
		// Orientation weight from angular distance to reference.
		float ang = 2.f * std::acos(std::clamp(std::abs(glm::dot(s.quat, qref)), 0.f, 1.f));
		float wrot = warmup ? 1.f : 1.f / (1.f + (ang / 0.087f / cfg.sigma_k) * (ang / 0.087f / cfg.sigma_k));
		float wpos = w;
		wsum_pos += wpos;
		pacc += (double)wpos * glm::dvec3(s.pos);
		wsum += wrot;
		qacc += wrot * glm::vec4(s.quat.x, s.quat.y, s.quat.z, s.quat.w);
		wsum_all += std::min(wpos, wrot);
	}
	pose out;
	out.pos = wsum_pos > 0 ? glm::vec3(pacc / (double)wsum_pos) : glm::vec3(mx, my, mz);
	if (wsum > 0 and glm::length(glm::vec3(qacc)) > 1e-9f)
		out.quat = glm::normalize(glm::quat((float)qacc.w, (float)qacc.x, (float)qacc.y, (float)qacc.z));
	else
		out.quat = qref;
	last_mean_weight = n > 0 ? wsum_all / (float)n : 1;
	return out;
}

float fiducial_filter::euro_channel::filter(float x, double dt, float min_cutoff, float beta)
{
	if (not init)
	{
		init = true;
		x_prev = x_hat = x;
		dx_hat = 0;
		return x;
	}
	if (dt <= 0)
		return x_hat;
	float dx = (float)((x - x_prev) / dt);
	float a_d = alpha_for(1.0f, dt);
	dx_hat += a_d * (dx - dx_hat);
	float cutoff = min_cutoff + beta * std::abs(dx_hat);
	float a = alpha_for(cutoff, dt);
	x_hat += a * (x - x_hat);
	x_prev = x;
	return x_hat;
}

fiducial_filter::pose fiducial_filter::advance(double dt_s)
{
	auto target = resolve();
	if (not target)
		return smoothed;
	if (not rendered_valid)
	{
		snap();
		return smoothed;
	}
	double dt = std::clamp(dt_s, 1e-4, 0.25);

	// Stage 1 — One Euro on the resolved stream. Position runs the full
	// two-stage low-pass; orientation adapts its slerp rate from the
	// filtered angular speed (shared euro params, no extra knobs).
	float ex = euro[0].filter(target->pos.x, dt, cfg.euro_min_cutoff, cfg.euro_beta);
	float ey = euro[1].filter(target->pos.y, dt, cfg.euro_min_cutoff, cfg.euro_beta);
	float ez = euro[2].filter(target->pos.z, dt, cfg.euro_min_cutoff, cfg.euro_beta);
	glm::vec3 euro_pos(ex, ey, ez);

	float omega_inst = 0;
	if (prev_resolved_valid and dt > 0)
	{
		float c = std::clamp(std::abs(glm::dot(prev_resolved, target->quat)), 0.f, 1.f);
		omega_inst = (float)(2.0 * std::acos(c) / dt);
	}
	prev_resolved = target->quat;
	prev_resolved_valid = true;
	float a_w = alpha_for(1.0f, dt);
	omega_hat += a_w * (omega_inst - omega_hat);
	float rot_cutoff = cfg.euro_min_cutoff + cfg.euro_beta * omega_hat * 10.f;
	float rot_alpha = std::clamp(1.f - std::exp(-cfg.rot_gain * (float)dt * (0.35f + rot_cutoff)), 0.f, 1.f);
	glm::quat q_target = glm::slerp(smoothed.quat, target->quat, rot_alpha);
	if (glm::dot(q_target, smoothed.quat) < 0)
		q_target = -q_target;

	// Stage 2 — proportional follow with a soft knee on the residual.
	// Large errors step large; sub-knee residuals blend to zero instead of
	// sticking then jumping like a binary deadband.
	glm::vec3 delta = euro_pos - smoothed.pos;
	float dist_mm = glm::length(delta) * 1000.f;
	float inner = cfg.knee_inner_mm, outer = std::max(inner + 1e-3f, cfg.knee_outer_mm);
	float t = std::clamp((dist_mm - inner) / (outer - inner), 0.f, 1.f);
	t = t * t * (3 - 2 * t);
	float follow = std::clamp(1.f - std::exp(-cfg.pos_gain * (float)dt), 0.f, 1.f);
	smoothed.pos += delta * (follow * t);

	glm::quat dq = q_target * glm::conjugate(smoothed.quat);
	float ang = 2.f * std::acos(std::clamp(std::abs(dq.w), 0.f, 1.f));
	float ang_deg = ang * 57.29577951308232f;
	float ri = cfg.knee_inner_deg, ro = std::max(ri + 1e-4f, cfg.knee_outer_deg);
	float tr = std::clamp((ang_deg - ri) / (ro - ri), 0.f, 1.f);
	tr = tr * tr * (3 - 2 * tr);
	smoothed.quat = glm::slerp(smoothed.quat, q_target, std::clamp(rot_alpha * (0.2f + 0.8f * tr), 0.f, 1.f));
	smoothed.quat = glm::normalize(smoothed.quat);
	return smoothed;
}

bool fiducial_filter::snap()
{
	auto target = resolve();
	if (not target)
		return false;
	smoothed = *target;
	rendered_valid = true;
	euro[0].reset(target->pos.x);
	euro[1].reset(target->pos.y);
	euro[2].reset(target->pos.z);
	omega_hat = 0;
	prev_resolved = target->quat;
	prev_resolved_valid = true;
	return true;
}

} // namespace xr
