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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <openxr/openxr.h>

namespace xr
{
// Multi-tag board pose fusion: N tag votes (each an independent board
// pose hypothesis from one sighted code) collapse into one rigid board
// pose. Single-node SE(3) MAP:
//
//	x* = argmin Sum_i rho(|| Log(T^-1 z_i) ||^2_{Omega_i})
//
// Library-owned math: manif (SE(3) exp/log/compose, analytic Jacobians,
// adjoint transport) + tiny_solver (dense LM loop). Ours: residual
// assembly, the v1 weight model, the Cauchy IRLS shell.
//
// Render thread only. Stateless except for the warm-start pose the caller
// passes in (solver init, not output smoothing).
struct board_vote
{
	std::string payload; // log/GUI identity only, never matched on here
	XrPosef observed{{0, 0, 0, 1}, {0, 0, 0}}; // world-space tag pose, predicted to display time
	float offset_pos[3] = {0, 0, 0}; // marker->board offset: vote = observed * offset
	float offset_quat[4] = {0, 0, 0, 1}; // xyzw
	float tag_size_m = 0;
	float distance_m = 0; // head -> tag center
	float cos_incidence = 1; // |tag normal . view dir|, 0..1
};

struct board_tag_stat
{
	std::string payload;
	float weight = 0; // final total weight (prior * cauchy), ~0..1
	float res_mm = 0; // unwhitened position residual vs fused pose
	float res_deg = 0; // unwhitened rotation residual vs fused pose
	bool used = false; // weight above the trust floor
};

struct board_solution
{
	bool ok = false; // a board pose came out (single-tag passthrough counts)
	bool single = false; // K==1: solver skipped, the only vote used directly
	XrPosef board_pose{{0, 0, 0, 1}, {0, 0, 0}};
	std::vector<board_tag_stat> tags; // same order as the input votes
	float rms_mm = 0;
	float rms_deg = 0;
	int lm_iters = 0; // total tiny_solver iterations across IRLS rounds
	double solve_us = 0;
};

board_solution solve_board(const std::vector<board_vote> & votes,
                           const std::optional<XrPosef> & warm_start);

// Deterministic synthetic check (fixed seed): known board + Gaussian vote
// noise + one planted flip-like outlier. Logs fused error vs truth, the
// outlier's final weight, and a lever-arm check (a distant tag's
// orientation noise must appear as position variance absorbed by fusion).
// True when all thresholds pass. Runs on-device from the debug UI.
bool board_solver_selftest();
} // namespace xr
