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

#include <openxr/openxr.h>

// Post-resolution 6DoF smoother (D-hybrid): strength-adaptive exponential
// blend with a soft deadzone + slew cap. Replaces fiducial_filter.* (deleted:
// windowed single-tag design, no strength input, wrong granularity).
//
// Design contract (tuning-phase brief):
// - Long memory: weak measurements barely move the state (tens of seconds
//   effective at low strength); the board is quasi-static, so lag only
//   matters on deliberate rig moves, which announce themselves with huge
//   innovations that saturate alpha.
// - Strength-weighted: alpha scales with the caller's resolution strength
//   w (from K, rms, tag weights). Blind long averaging would drag flips;
//   strength is what makes long memory safe.
// - Soft deadzone (smoothstep, never a cliff): sub-floor corrections fade
//   to zero, so persistent noise is not chased and slow drifts never step.
// - Proportional + capped convergence: mid-size errors absorb in ~1s,
//   huge errors slew at the cap (continuous, never a jump).
// - No velocity state (deliberate): the board is static; proportional alpha
//   tracks drifts with smooth lag, and there is no overshoot state to go
//   stale across holds. Call update() only on accepted solves; skip held /
//   total-loss frames (state freezes, which for a static board is correct).
// - K==1 and fused multi-tag flow through identically (caller passes the
//   resolved pose either way); weak singletons naturally get longer memory.
// - Bias-preserving: low-pass keeps static offsets intact. No per-tag bias
//   states in v1 (rigid-only, per brief).
// - Silent library (no logging inside): per-update diagnostics are returned
//   for the caller's `board smooth:` line. Eigen inside, XrPosef I/O.
namespace xr
{
struct board_filter_params
{
	// Long-memory creep: weak full-strength alpha floor ~= dt/tau.
	double tau_slow_s = 18.0;
	// Proportional engine: Hill(x) = hill_sat * x^2/(hill_knee^2+x^2).
	// Linear alpha provably cannot separate "absorb 8mm pops in ~1s" from
	// "reject 80mm spikes" (needs ~50x alpha ratio over 10x error); the
	// Hill knee gives flat-at-origin, steep-mid, flat-top response.
	// Knee from run-F: transients live <0.1s-age, agreeing tags settled by
	// ~0.5s; sat/linear-tail sized so pops absorb ~1-2s, spikes move <6mm,
	// carries saturate alpha (cap-bound tracking). TUNE-ME as a set.
	double hill_sat = 0.07;
	double hill_knee_mm = 13.0;
	// Linear tail past e_lin_mm (huge-error urgency so carries saturate
	// alpha instead of crawling at Hill-sat rates).
	double e_lin_mm = 100.0;
	double e_big_mm = 200.0;
	double dead_floor_mm = 1.0; // smoothstep deadzone floor...
	double dead_full_mm = 3.0; // ...to full response (noise floor 1-3mm)
	double slew_cap_mm = 20.0; // max travel per update (frame)
	double dead_floor_deg = 0.05; // orientation twin, degree-scaled
	double dead_full_deg = 0.3;
	double slew_cap_deg = 1.5;
	double hill_knee_deg = 1.5;
	double e_lin_deg = 10.0;
	double e_big_deg = 20.0;
};

struct board_filter_diag
{
	double innov_mm = 0; // raw innovation size (pre-shaping)
	double innov_deg = 0;
	double alpha = 0; // applied blend factor (position channel)
	double strength = 0; // caller strength echo
	bool deadzoned = false; // deadzone shaped this update (>50% cut)
	bool capped = false; // slew cap bound this update
	bool snapped = false; // teleport-silence snap (dt > threshold)
};

class board_filter
{
public:
	explicit board_filter(const board_filter_params & p = {}) : cfg(p) {}

	void reset();

	bool initialized() const
	{
		return valid;
	}

	// Update toward the resolved pose with strength w in [0,1]; dt_s is
	// seconds since the last update (clamped internally). First call seeds
	// directly. A dt beyond snap_dt_s snaps (teleport re-acquisition, not
	// motion: silence-separated solutions converge immediately).
	// Returns the smoothed pose. diag may be null.
	XrPosef update(const XrPosef & measured, float strength, double dt_s,
	               board_filter_diag * diag = nullptr);

	XrPosef current() const
	{
		return slow;
	}

	// Silence threshold for teleport snaps (mirrors the K1-guard value:
	// continuous tracking never gaps this long).
	static constexpr double snap_dt_s = 0.25;

	// Resolution-strength map from fusion stats (used-fraction, rms elbow
	// ~4mm knee, K-factor). TUNE-ME: initial shape from tuning-phase data.
	static float strength(int tags_used, int tags_visible, float rms_mm, bool single);

private:
	static double smoothstep(double x, double floor, double full);

	board_filter_params cfg;
	XrPosef slow{{0, 0, 0, 1}, {0, 0, 0}};
	bool valid = false;
};

// Deterministic synthetic check (fixed seed): noisy static hold (shimmer
// attenuation + no chasing), injected pop (absorption ~1s), planted flip
// spike (bounded response), 6m carry (saturating convergence), slow drift
// (step-free tracking). Logs measurements, returns overall pass.
bool board_filter_selftest();
} // namespace xr
