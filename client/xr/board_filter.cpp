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

#include "xr/board_filter.h"

#include <algorithm>
#include <cmath>
#include <random>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <spdlog/spdlog.h>

namespace xr
{
namespace
{
// Idioms salvaged from fiducial_filter.* (deleted): smoothstep shaping and
// hemisphere-aware quaternion handling. Its windowed single-tag architecture
// (12-frame memory, self-derived weights, no strength input) did not fit.
double sstep(double t)
{
	t = std::clamp(t, 0.0, 1.0);
	return t * t * (3 - 2 * t);
}

Eigen::Vector3d pos_of(const XrPosef & p)
{
	return Eigen::Vector3d(p.position.x, p.position.y, p.position.z);
}

Eigen::Quaterniond quat_of(const XrPosef & p)
{
	Eigen::Quaterniond q(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
	if (q.squaredNorm() < 1e-12)
		return Eigen::Quaterniond::Identity();
	return q.normalized();
}

XrPosef xr_of(const Eigen::Vector3d & t, const Eigen::Quaterniond & q)
{
	XrPosef p;
	p.position = {(float)t.x(), (float)t.y(), (float)t.z()};
	Eigen::Quaterniond qn = q.normalized();
	p.orientation = {(float)qn.x(), (float)qn.y(), (float)qn.z(), (float)qn.w()};
	return p;
}

double quat_deg(const Eigen::Quaterniond & a, const Eigen::Quaterniond & b)
{
	double d = std::clamp(std::abs(a.dot(b)), 0.0, 1.0);
	return 2.0 * std::acos(d) * 57.29577951308232;
}
} // namespace

double board_filter::smoothstep(double x, double floor, double full)
{
	if (full <= floor)
		return x > floor ? 1.0 : 0.0;
	return sstep((x - floor) / (full - floor));
}

float board_filter::strength(int tags_used, int tags_visible, float rms_mm, bool single)
{
	double used_frac = tags_visible > 0 ? (double)tags_used / tags_visible : 0.0;
	double elbow = 1.0 / (1.0 + (rms_mm / 4.0) * (rms_mm / 4.0));
	double k = single ? 0.5 : (tags_visible >= 3 ? 1.0 : 0.8);
	return (float)std::clamp(used_frac * elbow * k, 0.0, 1.0);
}

void board_filter::reset()
{
	valid = false;
	slow = XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
}

XrPosef board_filter::update(const XrPosef & measured, float strength, double dt_s, board_filter_diag * diag)
{
	board_filter_diag d;
	d.strength = std::clamp<double>(strength, 0.0, 1.0);
	if (not valid)
	{
		slow = measured;
		valid = true;
		d.alpha = 1.0;
		if (diag)
			*diag = d;
		return slow;
	}
	double dt = std::clamp(dt_s, 1e-4, 0.5);
	if (dt_s > snap_dt_s)
	{
		// Teleport-silence snap: silence-separated solutions are
		// re-acquisitions, not motion. Converge immediately (the pop line
		// with its dt records the jump).
		slow = measured;
		d.alpha = 1.0;
		d.snapped = true;
		d.innov_mm = 0; // unknown across the gap; call it a snap, not motion
		if (diag)
			*diag = d;
		return slow;
	}
	double w = d.strength;
	Eigen::Vector3d sp = pos_of(slow), mp = pos_of(measured);
	Eigen::Quaterniond sq = quat_of(slow), mq = quat_of(measured);
	if (sq.dot(mq) < 0)
		mq.coeffs() = -mq.coeffs(); // hemisphere align for slerp

	// Position channel: alpha senses the UNCAPPED deadzoned error (the cap
	// bounds the move, never the urgency — capping alpha instead would
	// blind carries). alpha = w*(base creep + Hill proportional + linear
	// tail); move = alpha*e_shaped, then slew-capped.
	Eigen::Vector3d e = mp - sp;
	double emm = e.norm() * 1000.0;
	d.innov_mm = emm;
	double sh = smoothstep(emm, cfg.dead_floor_mm, cfg.dead_full_mm);
	Eigen::Vector3d esh = e * sh;
	d.deadzoned = sh < 0.5;
	double eshm = esh.norm() * 1000.0;
	double hill = cfg.hill_sat * eshm * eshm / (cfg.hill_knee_mm * cfg.hill_knee_mm + eshm * eshm);
	double tail = eshm > cfg.e_lin_mm ? (eshm - cfg.e_lin_mm) / cfg.e_big_mm : 0.0;
	double alpha = std::clamp(w * (dt / cfg.tau_slow_s + hill + tail), 0.0, 1.0);
	d.alpha = alpha;
	Eigen::Vector3d move = alpha * esh;
	double cap = cfg.slew_cap_mm * std::clamp(2.0 * w, 0.15, 1.0) / 1000.0;
	if (move.norm() > cap)
	{
		move *= cap / move.norm();
		d.capped = true;
	}
	sp += move;

	// Orientation twin (degree-scaled constants; orientation is clean so
	// the deadzone does most of the work).
	double ang = quat_deg(sq, mq);
	d.innov_deg = ang;
	double sh_r = smoothstep(ang, cfg.dead_floor_deg, cfg.dead_full_deg);
	double ashm = ang * sh_r;
	double hill_r =
	        cfg.hill_sat * ashm * ashm / (cfg.hill_knee_deg * cfg.hill_knee_deg + ashm * ashm);
	double tail_r = ashm > cfg.e_lin_deg ? (ashm - cfg.e_lin_deg) / cfg.e_big_deg : 0.0;
	double alpha_r = std::clamp(w * (dt / cfg.tau_slow_s + hill_r + tail_r), 0.0, 1.0);
	double cap_r = cfg.slew_cap_deg * std::clamp(2.0 * w, 0.15, 1.0);
	double applied = std::min(alpha_r * ashm, cap_r);
	if (applied >= cap_r)
		d.capped = true;
	Eigen::Quaterniond qn = sq;
	if (ang > 1e-9)
		qn = sq.slerp(std::min(applied / ang, 1.0), mq);
	if (sh_r < 0.5)
		d.deadzoned = true;

	slow = xr_of(sp, qn);
	if (diag)
		*diag = d;
	return slow;
}

bool board_filter_selftest()
{
	// Deterministic: fixed seed. Truth static unless the phase moves it.
	std::mt19937 rng(7);
	std::normal_distribution<double> gauss(0, 1);
	const double dt = 1.0 / 90.0;
	auto noisy = [&](const Eigen::Vector3d & t, const Eigen::Quaterniond & q, double pmm, double rdeg) {
		Eigen::Vector3d tn(t.x() + pmm * gauss(rng) / 1000.0, t.y() + pmm * gauss(rng) / 1000.0,
		                   t.z() + pmm * gauss(rng) / 1000.0);
		Eigen::Vector3d ax(gauss(rng), gauss(rng), gauss(rng));
		if (ax.norm() < 1e-9)
			ax = Eigen::Vector3d::UnitX();
		ax.normalize();
		Eigen::Quaterniond nq(Eigen::AngleAxisd(rdeg * gauss(rng) / 57.29577951308232, ax));
		Eigen::Quaterniond qq = (q * nq).normalized();
		XrPosef p;
		p.position = {(float)tn.x(), (float)tn.y(), (float)tn.z()};
		p.orientation = {(float)qq.x(), (float)qq.y(), (float)qq.z(), (float)qq.w()};
		return p;
	};
	auto perr = [&](const XrPosef & p, const Eigen::Vector3d & t) {
		return (pos_of(p) - t).norm() * 1000.0;
	};
	bool pass = true;

	Eigen::Vector3d truth(1.0, 0.5, -2.0);
	Eigen::Quaterniond truthq(Eigen::Quaterniond(Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitY())).normalized());
	board_filter_params cfg; // defaults under test
	board_filter flt(cfg);

	// 1. Static hold, noisy (2mm/0.3deg), w=0.8, 6.7s: shimmer attenuated
	// (output std << input sigma), mean unbiased (no chasing).
	{
		flt.reset();
		std::vector<double> errs;
		Eigen::Vector3d acc(0, 0, 0);
		for (int i = 0; i < 600; ++i)
		{
			XrPosef m = noisy(truth, truthq, 2.0, 0.3);
			XrPosef o = flt.update(m, 0.8f, dt);
			if (i >= 500)
			{
				errs.push_back(perr(o, truth));
				acc += pos_of(o);
			}
		}
		double mean = 0, var = 0;
		for (double x: errs)
			mean += x;
		mean /= errs.size();
		for (double x: errs)
			var += (x - mean) * (x - mean);
		double sd = std::sqrt(var / errs.size());
		double bias = (acc / (double)errs.size() - truth).norm() * 1000.0;
		bool ok = sd < 0.5 and bias < 1.0;
		spdlog::info("board filter selftest: static shimmer sd {:.3f}mm bias {:.3f}mm {}", sd, bias,
		             ok ? "PASS" : "FAIL");
		pass = pass and ok;
	}
	// 2. Pop: truth steps +8mm, w=0.8: absorbed (~1-2s), no jump.
	{
		Eigen::Vector3d t2 = truth + Eigen::Vector3d(0.008, 0, 0);
		double maxstep = 0;
		Eigen::Vector3d prev = pos_of(flt.current());
		for (int i = 0; i < 180; ++i)
		{
			XrPosef m = noisy(t2, truthq, 2.0, 0.3);
			XrPosef o = flt.update(m, 0.8f, dt);
			maxstep = std::max(maxstep, (pos_of(o) - prev).norm() * 1000.0);
			prev = pos_of(o);
		}
		double fin = perr(flt.current(), t2);
		bool ok = fin < 2.0 and maxstep < 2.0;
		spdlog::info("board filter selftest: pop final {:.2f}mm maxstep {:.2f}mm {}", fin, maxstep,
		             ok ? "PASS" : "FAIL");
		pass = pass and ok;
	}
	// 3. Flip spike: single +80mm frame (w=0.9), then clean: bounded move
	// (<6mm: cap + shaped alpha reject ~93%), washout over ~3s (the state
	// cannot tell "absorbed spike" from "truth moved" — washout speed is
	// absorption physics). (Upstream gates own single-spike rejection; the
	// filter bounds what slips through.)
	{
		Eigen::Vector3d before = pos_of(flt.current());
		XrPosef spike = noisy(truth + Eigen::Vector3d(0.08, 0, 0), truthq, 0.1, 0.01);
		flt.update(spike, 0.9f, dt);
		double moved = (pos_of(flt.current()) - before).norm() * 1000.0;
		for (int i = 0; i < 270; ++i)
			flt.update(noisy(truth, truthq, 2.0, 0.3), 0.8f, dt);
		// Return is measured vs clean truth (not vs pre-spike state: phase 2
		// left the filter at the popped pose, which is correct behavior).
		double back = (pos_of(flt.current()) - truth).norm() * 1000.0;
		bool ok = moved < 6.0 and back < 1.5;
		spdlog::info("board filter selftest: spike moved {:.2f}mm return {:.2f}mm {}", moved, back,
		             ok ? "PASS" : "FAIL");
		pass = pass and ok;
	}
	// 4. Carry: truth ramps 6m at 0.9m/s (10mm/frame, realistic fast carry),
	// w=0.9: slew-saturated tracking (continuous, lag-bounded), converges
	// after stop. Teleport-silence snaps are tested on-device (dt path).
	{
		Eigen::Vector3d t = truth;
		for (int i = 0; i < 600; ++i)
		{
			t += Eigen::Vector3d(0.01, 0, 0);
			flt.update(noisy(t, truthq, 2.0, 0.3), 0.9f, dt);
		}
		double track = (pos_of(flt.current()) - t).norm() * 1000.0;
		for (int i = 0; i < 60; ++i)
			flt.update(noisy(t, truthq, 2.0, 0.3), 0.9f, dt);
		double conv = (pos_of(flt.current()) - t).norm() * 1000.0;
		bool ok = track < 200.0 and conv < 50.0;
		spdlog::info("board filter selftest: carry lag {:.1f}mm converge {:.1f}mm {}", track, conv,
		             ok ? "PASS" : "FAIL");
		pass = pass and ok;
		truth = t; // subsequent phases continue from the carried pose
	}
	// 5. Slow drift 0.5mm/s for 20s, w=0.8: tracks within 3mm, no steps
	// (max frame delta < 0.5mm — smoothstep, never a cliff).
	{
		Eigen::Vector3d t = truth;
		double maxstep = 0;
		Eigen::Vector3d prev = pos_of(flt.current());
		for (int i = 0; i < 1800; ++i)
		{
			t += Eigen::Vector3d(0.5 / 90.0 / 1000.0, 0, 0);
			XrPosef o = flt.update(noisy(t, truthq, 2.0, 0.3), 0.8f, dt);
			maxstep = std::max(maxstep, (pos_of(o) - prev).norm() * 1000.0);
			prev = pos_of(o);
		}
		double fin = (pos_of(flt.current()) - t).norm() * 1000.0;
		bool ok = fin < 3.0 and maxstep < 0.5;
		spdlog::info("board filter selftest: drift final {:.2f}mm maxstep {:.3f}mm {}", fin, maxstep,
		             ok ? "PASS" : "FAIL");
		pass = pass and ok;
	}
	spdlog::info("board filter selftest: {}", pass ? "ALL PASS" : "FAILURES PRESENT");
	return pass;
}
} // namespace xr
