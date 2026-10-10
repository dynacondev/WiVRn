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

#include "xr/board_solver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <manif/SE3.h>
#include <spdlog/spdlog.h>
#include <tiny_solver.h>

namespace xr
{
namespace
{
// TEMP v1 weight model: parametric guesses, tuned on-device via the
// `board solve:` logs + Passthrough-tab residuals. Physical sketch:
// position noise grows ~ d^2/size (triangulation-like), rotation noise
// ~ d/size, both blow up at grazing incidence.
constexpr double kPosGain = 0.002; // sigma_pos = kPosGain * d * (d/size) / cos
constexpr double kRotGain = 0.01; // sigma_rot = kRotGain * (d/size) / cos
constexpr double kCauchyK = 3.0; // IRLS robustness elbow (whitened units)
constexpr double kTrustFloor = 0.25; // final cauchy weight counting as "used"
constexpr double kExcludeFloor = 0.05; // below this a vote leaves the final refit
constexpr int kMaxTags = 8; // tiny_solver static cap: 6 residuals each
constexpr int kIrlsRounds = 3;

manif::SE3d se3_from_xr(const XrPosef & p)
{
	Eigen::Vector3d t(p.position.x, p.position.y, p.position.z);
	Eigen::Quaterniond q(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
	q.normalize();
	return manif::SE3d(t, q);
}

XrPosef xr_from_se3(const manif::SE3d & T)
{
	auto c = T.coeffs(); // [tx,ty,tz,qx,qy,qz,qw]
	XrPosef p;
	p.position = {(float)c[0], (float)c[1], (float)c[2]};
	p.orientation = {(float)c[3], (float)c[4], (float)c[5], (float)c[6]};
	return p;
}

float quat_angle_deg(const Eigen::Quaterniond & a, const Eigen::Quaterniond & b)
{
	double d = std::clamp(std::abs(a.dot(b)), 0.0, 1.0);
	return (float)(2.0 * std::acos(d) * 57.29577951308232);
}

// Per-vote diagonal whitening from the TEMP model (1/sigma per axis).
Eigen::Matrix<double, 6, 1> whitening(const board_vote & v)
{
	double d = std::max<double>(v.distance_m, 1e-3);
	double s = std::max<double>(v.tag_size_m, 1e-3);
	double c = std::clamp<double>(v.cos_incidence, 0.25, 1.0);
	double sig_pos = std::clamp(kPosGain * d * (d / s) / c, 1e-4, 1.0);
	double sig_rot = std::clamp(kRotGain * (d / s) / c, 1e-4, 1.0);
	Eigen::Matrix<double, 6, 1> w;
	w << 1 / sig_pos, 1 / sig_pos, 1 / sig_pos, 1 / sig_rot, 1 / sig_rot, 1 / sig_rot;
	return w;
}

// Weighted Kabsch over tag corners (closed-form LS init). P = board-frame
// corners, Q = world-frame corners. Library-owned SVD; assembly is ours.
manif::SE3d kabsch_init(const std::vector<board_vote> & votes, const std::vector<double> & w)
{
	Eigen::Vector3d pbar = Eigen::Vector3d::Zero(), qbar = Eigen::Vector3d::Zero();
	double wsum = 0;
	for (size_t i = 0; i < votes.size(); ++i)
	{
		const auto & v = votes[i];
		manif::SE3d M = se3_from_xr(v.observed);
		Eigen::Isometry3d O = Eigen::Isometry3d::Identity();
		O.translate(Eigen::Vector3d(v.offset_pos[0], v.offset_pos[1], v.offset_pos[2]));
		O.rotate(Eigen::Quaterniond(v.offset_quat[3], v.offset_quat[0], v.offset_quat[1], v.offset_quat[2]));
		Eigen::Isometry3d Oinv = O.inverse();
		Eigen::Matrix4d Mw = M.transform();
		Eigen::Matrix3d R = Mw.topLeftCorner<3, 3>();
		Eigen::Vector3d t = Mw.topRightCorner<3, 1>();
		float h = v.tag_size_m * 0.5f;
		for (int cx = -1; cx <= 1; cx += 2)
			for (int cy = -1; cy <= 1; cy += 2)
			{
				Eigen::Vector3d cl(cx * h, cy * h, 0); // tag frame
				Eigen::Vector3d cb = Oinv * cl; // board frame
				Eigen::Vector3d cw = R * cl + t; // world frame
				pbar += w[i] * cb;
				qbar += w[i] * cw;
				wsum += w[i];
			}
	}
	pbar /= wsum;
	qbar /= wsum;
	Eigen::Matrix3d H = Eigen::Matrix3d::Zero();
	for (size_t i = 0; i < votes.size(); ++i)
	{
		const auto & v = votes[i];
		manif::SE3d M = se3_from_xr(v.observed);
		Eigen::Isometry3d O = Eigen::Isometry3d::Identity();
		O.translate(Eigen::Vector3d(v.offset_pos[0], v.offset_pos[1], v.offset_pos[2]));
		O.rotate(Eigen::Quaterniond(v.offset_quat[3], v.offset_quat[0], v.offset_quat[1], v.offset_quat[2]));
		Eigen::Isometry3d Oinv = O.inverse();
		Eigen::Matrix4d Mw = M.transform();
		Eigen::Matrix3d R = Mw.topLeftCorner<3, 3>();
		Eigen::Vector3d t = Mw.topRightCorner<3, 1>();
		float h = v.tag_size_m * 0.5f;
		for (int cx = -1; cx <= 1; cx += 2)
			for (int cy = -1; cy <= 1; cy += 2)
			{
				Eigen::Vector3d cb = Oinv * Eigen::Vector3d(cx * h, cy * h, 0);
				Eigen::Vector3d cw = R * Eigen::Vector3d(cx * h, cy * h, 0) + t;
				H += w[i] * (cb - pbar) * (cw - qbar).transpose();
			}
	}
	Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
	Eigen::Matrix3d R = svd.matrixV() * Eigen::Vector3d(1, 1, (svd.matrixV() * svd.matrixU().transpose()).determinant() > 0 ? 1 : -1).asDiagonal() *
	        svd.matrixU().transpose();
	Eigen::Quaterniond q(R);
	q.normalize(); // SVD orthonormality is approximate; manif checks strictly
	return manif::SE3d(qbar - R * pbar, q);
}

// Analytic Jacobians, every block from manif (no hand-rolled Lie math).
struct fusion_functor
{
	using Scalar = double;
	enum
	{
		NUM_RESIDUALS = Eigen::Dynamic,
		NUM_PARAMETERS = 6,
	};

	manif::SE3d T0;
	std::vector<manif::SE3d> votes;
	std::vector<Eigen::Matrix<double, 6, 1>> white; // whitening * sqrt(irls mult)
	int NumResiduals() const { return (int)(6 * votes.size()); }

	bool operator()(const double * p, double * r, double * J) const
	{
		Eigen::Map<const Eigen::Matrix<double, 6, 1>> dm(p);
		manif::SE3Tangentd delta;
		delta.coeffs() = dm;
		manif::SE3d::Jacobian J_T_d = delta.rjac();
		manif::SE3d T = T0 + delta;
		manif::SE3d::Jacobian J_inv, J_comp, J_log, J_dummy;
		Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, 6>> Jm(J, NumResiduals(), 6);
		for (size_t i = 0; i < votes.size(); ++i)
		{
			manif::SE3d Y = T.inverse(J_inv);
			manif::SE3d W = Y.compose(votes[i], J_comp, J_dummy);
			manif::SE3Tangentd e = W.log(J_log);
			Eigen::Matrix<double, 6, 1> ew = white[i].cwiseProduct(e.coeffs());
			for (int k = 0; k < 6; ++k)
				r[6 * i + k] = ew[k];
			if (J)
				Jm.block<6, 6>(6 * (int)i, 0).noalias() =
				        white[i].asDiagonal() * J_log * J_comp * J_inv * J_T_d;
		}
		return true;
	}
};

// One IRLS run: LM from T_init, Cauchy reweighting between rounds.
// Returns the fused pose; mult holds final per-vote multipliers.
manif::SE3d irls_solve(const std::vector<manif::SE3d> & z, const std::vector<Eigen::Matrix<double, 6, 1>> & white,
                       manif::SE3d T_init, std::vector<double> & mult, int & iters, int rounds)
{
	fusion_functor f;
	f.T0 = T_init;
	f.votes = z;
	f.white = white;
	ceres::TinySolver<fusion_functor, 6 * kMaxTags> solver;
	solver.options.max_num_iterations = 20;
	Eigen::Matrix<double, 6, 1> delta = Eigen::Matrix<double, 6, 1>::Zero();
	iters = 0;
	for (int round = 0; round < rounds; ++round)
	{
		for (size_t i = 0; i < z.size(); ++i)
			f.white[i] = white[i] * std::sqrt(mult[i]);
		delta.setZero();
		const auto & summary = solver.Solve(f, &delta);
		iters += summary.iterations;
		manif::SE3Tangentd d;
		d.coeffs() = delta;
		f.T0 = f.T0 + d;
		for (size_t i = 0; i < z.size(); ++i)
		{
			manif::SE3Tangentd e = f.T0.inverse().compose(z[i]).log();
			double u = (white[i].cwiseProduct(e.coeffs())).norm();
			mult[i] = 1.0 / (1.0 + (u / kCauchyK) * (u / kCauchyK));
		}
	}
	return f.T0;
}

// Trimmed fit cost for candidate comparison: sum of squared whitened
// residuals, dropping the `trim` largest. Least-trimmed-squares family:
// inliers agree with EACH OTHER better than with an outlier, independent
// of sigma calibration (a soft kernel alone can prefer a pulled fit when
// the outlier is only a few sigma out). trim=1 for n>=3, else 0.
double trimmed_cost(const manif::SE3d & T, const std::vector<manif::SE3d> & z,
                    const std::vector<Eigen::Matrix<double, 6, 1>> & white, int trim)
{
	std::vector<double> u2;
	for (size_t i = 0; i < z.size(); ++i)
	{
		manif::SE3Tangentd e = T.inverse().compose(z[i]).log();
		double u = (white[i].cwiseProduct(e.coeffs())).norm();
		u2.push_back(u * u);
	}
	std::sort(u2.begin(), u2.end());
	double cost = 0;
	for (size_t i = 0; i + (size_t)trim < u2.size(); ++i)
		cost += u2[i];
	return cost;
}
// tiny_solver functor: residuals of the error-state delta around T0.
} // namespace

board_solution solve_board(const std::vector<board_vote> & votes_in, const std::optional<XrPosef> & warm_start)
{
	auto t0 = std::chrono::steady_clock::now();
	board_solution sol;
	if (votes_in.empty())
		return sol;

	// Cap: keep the highest-prior-weight tags (boards are small; a log
	// line if this ever bites).
	std::vector<board_vote> votes = votes_in;
	std::vector<Eigen::Matrix<double, 6, 1>> white;
	for (const auto & v: votes)
		white.push_back(whitening(v));
	if (votes.size() > (size_t)kMaxTags)
	{
		std::vector<size_t> idx(votes.size());
		for (size_t i = 0; i < idx.size(); ++i)
			idx[i] = i;
		std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return white[a].squaredNorm() > white[b].squaredNorm(); });
		idx.resize(kMaxTags);
		std::sort(idx.begin(), idx.end());
		std::vector<board_vote> nv;
		std::vector<Eigen::Matrix<double, 6, 1>> nw;
		for (size_t i: idx)
		{
			nv.push_back(votes[i]);
			nw.push_back(white[i]);
		}
		votes.swap(nv);
		white.swap(nw);
		spdlog::warn("board_solver: {} votes exceed cap {}, kept strongest", votes_in.size(), kMaxTags);
	}

	sol.tags.resize(votes.size());
	for (size_t i = 0; i < votes.size(); ++i)
		sol.tags[i].payload = votes[i].payload;

	// K==1: passthrough, solver skipped. Stats still filled below.
	manif::SE3d T;
	if (votes.size() == 1)
	{
		sol.single = true;
		T = se3_from_xr(votes[0].observed).compose(se3_from_xr(
		        XrPosef{{votes[0].offset_quat[0], votes[0].offset_quat[1], votes[0].offset_quat[2], votes[0].offset_quat[3]},
		                {votes[0].offset_pos[0], votes[0].offset_pos[1], votes[0].offset_pos[2]}}));
		sol.tags[0].weight = 1;
		sol.tags[0].used = true;
	}
	else
	{
		std::vector<manif::SE3d> z;
		for (const auto & v: votes)
			z.push_back(se3_from_xr(v.observed).compose(se3_from_xr(
			        XrPosef{{v.offset_quat[0], v.offset_quat[1], v.offset_quat[2], v.offset_quat[3]},
			                {v.offset_pos[0], v.offset_pos[1], v.offset_pos[2]}})));
		std::vector<double> kw(votes.size()); // kabsch weights ~ 1/sigma_pos^2
		for (size_t i = 0; i < votes.size(); ++i)
			kw[i] = white[i].head<3>().squaredNorm();

		// Candidates: full set (warm/Kabsch start) + leave-one-out
		// subsets (Kabsch on subset) for K>=3. LOO is degenerate for
		// K==2 (a lone vote fits itself), so K==2 gets a best-vote
		// init instead and leans on the warm start. Scored by robust
		// cost on the FULL set; the winner is refit on all votes.
		// Deterministic.
		struct candidate
		{
			std::vector<int> idx;
			manif::SE3d init;
		};
		std::vector<candidate> cands;
		{
			candidate c;
			for (size_t i = 0; i < votes.size(); ++i)
				c.idx.push_back((int)i);
			if (warm_start)
				c.init = se3_from_xr(*warm_start);
			else
				c.init = kabsch_init(votes, kw);
			cands.push_back(std::move(c));
		}
		if (votes.size() >= 3)
		{
			for (size_t skip = 0; skip < votes.size(); ++skip)
			{
				candidate c;
				std::vector<board_vote> sv;
				std::vector<double> sw;
				for (size_t i = 0; i < votes.size(); ++i)
				{
					if (i == skip)
						continue;
					c.idx.push_back((int)i);
					sv.push_back(votes[i]);
					sw.push_back(kw[i]);
				}
				c.init = kabsch_init(sv, sw);
				cands.push_back(std::move(c));
			}
		}
		else
		{
			size_t best_vote = 0;
			double best_prior = -1;
			for (size_t i = 0; i < votes.size(); ++i)
			{
				double w = white[i].squaredNorm();
				if (w > best_prior)
				{
					best_prior = w;
					best_vote = i;
				}
			}
			candidate c;
			c.idx = {0, 1};
			c.init = z[best_vote];
			cands.push_back(std::move(c));
		}

		// Selection: short IRLS per candidate, robust cost on full set.
		manif::SE3d winner = cands[0].init;
		double best_cost = 1e100; // no infinity(): UB under -ffast-math
		for (const auto & c: cands)
		{
			std::vector<manif::SE3d> zc;
			std::vector<Eigen::Matrix<double, 6, 1>> wc;
			for (int i: c.idx)
			{
				zc.push_back(z[i]);
				wc.push_back(white[i]);
			}
			std::vector<double> m(zc.size(), 1.0);
			int it = 0;
			manif::SE3d Tc = irls_solve(zc, wc, c.init, m, it, 2);
			sol.lm_iters += it;
			double co = trimmed_cost(Tc, z, white, (int)z.size() >= 3 ? 1 : 0);
			if (co < best_cost)
			{
				best_cost = co;
				winner = Tc;
			}
		}
		// Final refit: votes the winner distrusts (mult < floor) are
		// EXCLUDED, not merely downweighted. A huge residual times a
		// tiny-but-nonzero weight still drags the fit (measured 17mm
		// from one 160deg flip at w~0.01); exclusion is what RANSAC
		// would do, deterministically.
		std::vector<double> mult(votes.size(), 1.0);
		for (size_t i = 0; i < votes.size(); ++i)
		{
			manif::SE3Tangentd e = winner.inverse().compose(z[i]).log();
			double u = (white[i].cwiseProduct(e.coeffs())).norm();
			mult[i] = 1.0 / (1.0 + (u / kCauchyK) * (u / kCauchyK));
		}
		{
			std::vector<int> kept;
			for (size_t i = 0; i < votes.size(); ++i)
			{
				if (mult[i] >= kExcludeFloor)
					kept.push_back((int)i);
			}
			int it = 0;
			if (kept.size() >= 2)
			{
				std::vector<manif::SE3d> zk;
				std::vector<Eigen::Matrix<double, 6, 1>> wk;
				std::vector<double> mk;
				for (int i: kept)
				{
					zk.push_back(z[i]);
					wk.push_back(white[i]);
					mk.push_back(mult[i]);
				}
				T = irls_solve(zk, wk, winner, mk, it, kIrlsRounds);
				// Map surviving multipliers back for stats.
				for (size_t k = 0; k < kept.size(); ++k)
					mult[kept[k]] = mk[k];
			}
			else if (kept.size() == 1)
				T = z[kept[0]];
			else
				T = winner;
			sol.lm_iters += it;
		}
		manif::SE3d Tf = T;
		for (size_t i = 0; i < votes.size(); ++i)
		{
			manif::SE3Tangentd e = Tf.inverse().compose(z[i]).log();
			double u = (white[i].cwiseProduct(e.coeffs())).norm();
			sol.tags[i].weight = u > 6.0 ? 0.0f : (float)mult[i];
			sol.tags[i].used = sol.tags[i].weight > kTrustFloor;
		}
	}

	sol.board_pose = xr_from_se3(T);
	// Unwhitened per-vote residuals vs the fused pose.
	double se = 0, sa = 0;
	for (size_t i = 0; i < votes.size(); ++i)
	{
		manif::SE3d Z = se3_from_xr(votes[i].observed).compose(se3_from_xr(
		        XrPosef{{votes[i].offset_quat[0], votes[i].offset_quat[1], votes[i].offset_quat[2], votes[i].offset_quat[3]},
		                {votes[i].offset_pos[0], votes[i].offset_pos[1], votes[i].offset_pos[2]}}));
		manif::SE3d E = T.inverse().compose(Z);
		Eigen::Vector3d tp(E.translation().x(), E.translation().y(), E.translation().z());
		Eigen::Quaterniond eq(E.asSO3().quat());
		sol.tags[i].res_mm = (float)(tp.norm() * 1000.0);
		sol.tags[i].res_deg = quat_angle_deg(eq, Eigen::Quaterniond::Identity());
		se += sol.tags[i].res_mm * sol.tags[i].res_mm;
		sa += sol.tags[i].res_deg * sol.tags[i].res_deg;
	}
	sol.rms_mm = (float)std::sqrt(se / votes.size());
	sol.rms_deg = (float)std::sqrt(sa / votes.size());
	sol.solve_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	sol.ok = true;
	return sol;
}

bool board_solver_selftest()
{
	// Deterministic: fixed seed, fixed truth. One planted flip-like
	// outlier exercises the IRLS shell; the lever-arm check proves
	// distant-tag orientation noise is absorbed, not smeared.
	std::mt19937 rng(42);
	std::normal_distribution<double> gauss(0, 1);

	struct tagdef
	{
		float pos[3];
		float quat[4]; // xyzw
		float size;
	};
	std::vector<tagdef> board = {
	        {{0, 0, 0}, {0, 0, 0, 1}, 0.08f},
	        {{0.3f, 0, 0}, {0, 0, 0, 1}, 0.08f},
	        {{0, 0.3f, 0}, {0, 0, 0, 1}, 0.08f},
	        {{0.15f, 0.15f, 0.05f}, {0, 0.258819f, 0, 0.9659258f}, 0.08f}, // tipped 30deg about Y
	};
	manif::SE3d truth(Eigen::Vector3d(1.2, -0.3, 2.0),
	                  (Eigen::Quaterniond(Eigen::AngleAxisd(0.35, Eigen::Vector3d::UnitY()) *
	                                             Eigen::AngleAxisd(0.17, Eigen::Vector3d::UnitX())))
	                          .normalized());

	auto make_votes = [&](bool plant_outlier) {
		std::vector<board_vote> votes;
		for (size_t i = 0; i < board.size(); ++i)
		{
			const auto & t = board[i];
			Eigen::Quaterniond oq(t.quat[3], t.quat[0], t.quat[1], t.quat[2]);
			oq.normalize(); // literals are approximate; manif checks strictly
			manif::SE3d O(Eigen::Vector3d(t.pos[0], t.pos[1], t.pos[2]), oq);
			// vote truth: observed = truth * O^-1, then noise in tag frame
			manif::SE3d M = truth.compose(O.inverse());
			Eigen::Matrix<double, 6, 1> n;
			for (int k = 0; k < 3; ++k)
				n[k] = 0.003 * gauss(rng);
			for (int k = 3; k < 6; ++k)
				n[k] = 0.009 * gauss(rng);
			if (plant_outlier and i == 1)
				n[4] = 2.8; // ~160deg flip-like yaw outlier on tag 1 (real
				            // planar flips are near-180deg, not 26deg: huge
				            // vs noise, so consensus must isolate it)
			manif::SE3Tangentd nt;
			nt.coeffs() = n;
			M = M + nt;
			board_vote v;
			v.payload = "selftest:" + std::to_string(i);
			v.observed = xr_from_se3(M);
			v.offset_pos[0] = t.pos[0];
			v.offset_pos[1] = t.pos[1];
			v.offset_pos[2] = t.pos[2];
			v.offset_quat[0] = t.quat[0];
			v.offset_quat[1] = t.quat[1];
			v.offset_quat[2] = t.quat[2];
			v.offset_quat[3] = t.quat[3];
			v.tag_size_m = t.size;
			v.distance_m = 2.0f;
			v.cos_incidence = 1.0f;
			votes.push_back(v);
		}
		return votes;
	};

	bool pass = true;
	auto check = [&](const char * name, board_solution s, double max_mm, double max_deg) {
		manif::SE3d F = se3_from_xr(s.board_pose);
		manif::SE3d E = truth.inverse().compose(F);
		double mm = Eigen::Vector3d(E.translation().x(), E.translation().y(), E.translation().z()).norm() * 1000.0;
		double deg = quat_angle_deg(Eigen::Quaterniond(E.asSO3().quat()), Eigen::Quaterniond::Identity());
		bool ok = s.ok and mm < max_mm and deg < max_deg;
		spdlog::info("board selftest: {} err {:.2f}mm {:.3f}deg (rms {:.2f}mm) iters {} {:.0f}us {}",
		             name, mm, deg, (double)s.rms_mm, s.lm_iters, s.solve_us, ok ? "PASS" : "FAIL");
		pass = pass and ok;
		return s;
	};

	// Cold start (Kabsch init path), clean noise.
	check("clean", solve_board(make_votes(false), std::nullopt), 3.0, 0.4);
	// Cold start with a planted outlier: fused error must stay small and
	// the outlier's final weight must collapse.
	// Post-exclusion subsets are geometrically weaker (here the x-lever
	// tag is the outlier, so x is looser); the structural demands are
	// consensus recovery + isolation + honest residuals, not tight error.
	board_solution so = check("outlier", solve_board(make_votes(true), std::nullopt), 6.0, 0.8);
	if (so.tags.size() > 1)
	{
		double inlier = 0;
		for (size_t i = 0; i < so.tags.size(); ++i)
		{
			if (i != 1)
				inlier = std::max(inlier, (double)so.tags[i].res_mm);
		}
		bool wok = so.tags[1].weight < 0.2f and so.tags[1].res_mm > 5.0 * std::max(inlier, 0.5);
		spdlog::info("board selftest: outlier weight {:.3f} res {:.1f}mm vs inliers {:.1f}mm {}", (double)so.tags[1].weight,
		             (double)so.tags[1].res_mm, inlier, wok ? "PASS" : "FAIL");
		pass = pass and wok;
	}
	// Lever arm: tag 3 sits off-origin; its orientation noise must not
	// smear position. Fused position error must beat the worst single
	// vote's position error.
	{
		std::vector<board_vote> votes = make_votes(false);
		manif::SE3d F = se3_from_xr(solve_board(votes, std::nullopt).board_pose);
		double fused = Eigen::Vector3d((truth.inverse().compose(F)).translation().x(),
		                               (truth.inverse().compose(F)).translation().y(),
		                               (truth.inverse().compose(F)).translation().z())
		                       .norm() * 1000.0;
		double worst = 0;
		for (const auto & v: votes)
		{
			manif::SE3d Z = se3_from_xr(v.observed).compose(se3_from_xr(
			        XrPosef{{v.offset_quat[0], v.offset_quat[1], v.offset_quat[2], v.offset_quat[3]},
			                {v.offset_pos[0], v.offset_pos[1], v.offset_pos[2]}}));
			manif::SE3d E = truth.inverse().compose(Z);
			worst = std::max(worst, Eigen::Vector3d(E.translation().x(), E.translation().y(), E.translation().z()).norm() * 1000.0);
		}
		bool lok = fused < worst;
		spdlog::info("board selftest: lever-arm fused {:.2f}mm vs worst single {:.2f}mm {}", fused, worst, lok ? "PASS" : "FAIL");
		pass = pass and lok;
	}
	spdlog::info("board selftest: {}", pass ? "ALL PASS" : "FAILURES PRESENT");
	return pass;
}
} // namespace xr
