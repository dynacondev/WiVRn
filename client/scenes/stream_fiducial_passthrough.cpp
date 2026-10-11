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

// Phase 2 (ROADMAP.md): surface-projected passthrough mesh MVP.
// Loads the server-provided glB from the Phase 1 cache, flattens it to a
// runtime triangle mesh, and submits it as an overlay cutout layer.
// Placement is the config pose used directly as a world-space pose;
// marker-relative anchoring comes in Phase 4.

#include "stream.h"

#include "application.h"
#include "render/passthrough_mesh.h"
#include "render/ui_widgets.h"
#include "utils/i18n.h"

#ifdef __ANDROID__
#include "android/permissions.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <spdlog/spdlog.h>

#ifdef __ANDROID__
namespace
{
// USE_SCENE grant state. Written once from the permission callback (activity
// thread), read on the render thread for the Stats-tab hint; a torn read only
// delays the hint by a frame.
bool scene_permission_granted = false;
} // namespace
#endif

void scenes::stream::request_spatial_permissions()
{
#ifdef __ANDROID__
	static bool requested = false;
	if (requested)
		return;
	requested = true;

	// Canonical name first: the runtime gates context creation on the
	// com.oculus grant, while extension enumeration keys off the manifest
	// string. Requesting only the horizonos string auto-grants vacuously
	// ("already granted", no effective permission) and context creation
	// then fails with "spatial permission not granted".
	if (check_permission("com.oculus.permission.USE_SCENE"))
	{
		scene_permission_granted = true;
		spdlog::info("Scene permission already granted (com.oculus.permission.USE_SCENE)");
		return;
	}
	if (check_permission("horizonos.permission.USE_SCENE"))
		spdlog::info("horizonos Scene permission granted, still requesting the canonical one");

	// Canonical name on current Horizon OS first; the legacy name stays
	// declared in the manifest for older releases.
	spdlog::info("Requesting Scene permission for marker tracking");
	request_permission("com.oculus.permission.USE_SCENE", [](bool granted) {
		scene_permission_granted = granted;
		if (granted)
			spdlog::info("Scene permission granted");
		else
			spdlog::warn("Scene permission denied, marker tracking unavailable");
	});
#endif
}

bool scenes::stream::fiducial_passthrough_wanted()
{
	// Strict gating: nothing mesh-related happens until an instance
	// auto-anchors. Pre-placement behavior is stock upstream, so a grey
	// baseline with zero mesh lines in the log exonerates the mesh path.
	bool wanted = fiducial_passthrough.calibrated and instance.has_extension(XR_FB_TRIANGLE_MESH_EXTENSION_NAME);

	static bool last_wanted = true; // log the initial false once
	if (wanted != last_wanted)
	{
		last_wanted = wanted;
		spdlog::info("Fiducial passthrough wanted: {}", wanted);
	}
	return wanted;
}

namespace
{
// base * offset (translation unaffected by scale; scale applies later at
// the object). Offset quaternion stored xyzw.
XrPosef compose_pose(const XrPosef & base, const std::array<float, 3> & p, const std::array<float, 4> & q)
{
	glm::quat bq(base.orientation.w, base.orientation.x, base.orientation.y, base.orientation.z);
	glm::vec3 bp(base.position.x, base.position.y, base.position.z);
	glm::quat oq(q[3], q[0], q[1], q[2]);
	glm::vec3 op(p[0], p[1], p[2]);
	glm::quat rq = bq * oq;
	glm::vec3 rp = bp + bq * op;
	return {.orientation = {rq.x, rq.y, rq.z, rq.w}, .position = {rp.x, rp.y, rp.z}};
}

// Authored marker offsets are tag-in-board: position = tag center in
// fiducial meters, orientation = tag orientation relative to fiducial
// axes. Votes need the inverse (board-in-tag). Inverted here, once per
// sighting (nanoseconds, K tiny) — never hand-invert in config. Unit
// quaternions: conjugate is the inverse.
void effective_offset(const std::array<float, 3> & p, const std::array<float, 4> & q,
                      std::array<float, 3> & op, std::array<float, 4> & oq)
{
	glm::quat r = glm::normalize(glm::quat(q[3], q[0], q[1], q[2]));
	glm::vec3 t(p[0], p[1], p[2]);
	glm::quat ri = glm::conjugate(r);
	glm::vec3 ti = -(ri * t);
	op = {ti.x, ti.y, ti.z};
	oq = {ri.x, ri.y, ri.z, ri.w};
}
} // namespace

// K==1 jump guard: the passthrough path is residual-blind (a lone vote fits
// itself), so a flipped singleton displays inverted (run-E: user-visible
// inverts through the flip storm). Frame jumps are physically impossible by
// hand — carry speeds run ~5mm/frame, so dpos 50mm (4.5m/s) / dang 30deg
// (2700deg/s) clear legit motion including 42mm bistable swaps (~1deg) while
// sitting far below flips (70mm+/40deg+). Returns true when the frame must be
// skipped: the caller `continue`s, making it an occlusion frame downstream
// (hold-last) whose warm reference never advances to garbage. Release on
// resolve / K>=2 consensus / teleport-silence. No frame timeout: a rig yanked
// mid-visibility with no silence and no set change is vanishingly rare, and
// set changes / silence release first. First sighting (no reference) always
// accepts: unknowable without history.
bool scenes::stream::guard_k1_jump(const std::string & fid, const xr::board_solution & sol,
                                      const std::optional<XrPosef> & warm, XrTime now, double & dt_out)
{
	static std::map<std::string, XrTime> last_solve_t;
	static std::map<std::string, bool> holding;
	double solve_dt = 0;
	if (auto ltit0 = last_solve_t.find(fid); ltit0 != last_solve_t.end())
		solve_dt = (now - ltit0->second) * 1e-9;
	last_solve_t[fid] = now; // cadence advances even when held
	dt_out = solve_dt;
	bool held = false;
	if (sol.single and warm)
	{
		float dx = sol.board_pose.position.x - warm->position.x;
		float dy = sol.board_pose.position.y - warm->position.y;
		float dz = sol.board_pose.position.z - warm->position.z;
		float dpos = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.f;
		float dot = std::abs(sol.board_pose.orientation.w * warm->orientation.w +
		                     sol.board_pose.orientation.x * warm->orientation.x +
		                     sol.board_pose.orientation.y * warm->orientation.y +
		                     sol.board_pose.orientation.z * warm->orientation.z);
		float dang = 2.f * std::acos(std::clamp(dot, 0.f, 1.f)) * 57.29577951308232f;
		bool teleport = solve_dt > 0.25; // silence => the rig may have moved
		if (not teleport and (dpos > 50.f or dang > 30.f))
		{
			held = true;
			if (not holding[fid])
			{
				holding[fid] = true;
				spdlog::info("board hold: fid \"{}\" K1 jump {:.2f}mm {:.2f}deg dt{:.2f}s (holding last good)",
				             fid, (double)dpos, (double)dang, solve_dt);
			}
		}
		else if (holding[fid])
		{
			holding[fid] = false;
			spdlog::info("board hold: fid \"{}\" resumed ({:.2f}mm{})", fid, (double)dpos,
			             teleport ? ", teleport accepted" : "");
		}
	}
	else if (holding[fid])
	{
		holding[fid] = false;
		spdlog::info("board hold: fid \"{}\" consensus resumed", fid);
	}
	return held;
}

// Tuning-instrument logging: heartbeat per fiducial + visible-set changes
// (`board solve:` / `board tags:` / `board pop:` / `board flip:` lines).
// Pure code motion out of the per-fiducial loop (no behavior change):
// per-fid state lives in function statics, exactly as before.
void scenes::stream::log_board_tuning(const std::string & fid, const std::vector<xr::board_vote> & bv,
                                         const xr::board_solution & sol, const board_fused & bp,
                                         double horizon_ms, XrTime now, double dt,
                                         const xr::board_filter_diag & fdiag)
{
	// `board solve:` is the tuning instrument (more logging better).
	static XrTime last_solve_log = 0;
	static std::map<std::string, std::string> last_sets;
	static std::map<std::string, XrPosef> last_board_pose;
	static std::map<std::string, int> last_board_n;
	std::string set;
	for (const auto & v: bv)
	{
		set += v.payload.substr(0, 32);
		set += ',';
	}
	auto lsit = last_sets.find(fid);
	if (lsit == last_sets.end() or lsit->second != set)
	{
		// Attribute the transition: which payloads entered/exited
		// (set strings are comma-joined truncated payloads). Tests the
		// grazing-entry hypothesis: big pops should name entering tags
		// that the `board tags:` line then shows at low cos / high u.
		std::string entered, exited;
		if (lsit != last_sets.end())
		{
			auto split = [](const std::string & s) {
				std::vector<std::string> out;
				size_t b = 0;
				while (b < s.size())
				{
					size_t c = s.find(',', b);
					if (c == std::string::npos)
						break;
					if (c > b)
						out.push_back(s.substr(b, c - b));
					b = c + 1;
				}
				return out;
			};
			auto old_tags = split(lsit->second);
			auto new_tags = split(set);
			for (const auto & t: new_tags)
			{
				if (std::ranges::find(old_tags, t) == old_tags.end())
					entered += t + ',';
			}
			for (const auto & t: old_tags)
			{
				if (std::ranges::find(new_tags, t) == new_tags.end())
					exited += t + ',';
			}
		}
		last_sets[fid] = set;
		spdlog::info("board solve: fid \"{}\" visible set -> {}", fid, set);
		// `board pop:` measures the felt artifact directly: how far
		// the fused pose jumps when the visible set changes. Fires on
		// transitions only (~0.5/s), not every frame. dt since the
		// previous solve separates true pops (dt~frame) from
		// teleport-recovery after silence (rig move / total loss).
		auto lpit = last_board_pose.find(fid);
		auto lnit = last_board_n.find(fid);
		if (lpit != last_board_pose.end() and lnit != last_board_n.end())
		{
			float dx = sol.board_pose.position.x - lpit->second.position.x;
			float dy = sol.board_pose.position.y - lpit->second.position.y;
			float dz = sol.board_pose.position.z - lpit->second.position.z;
			float dpos = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.f;
			float dot = std::abs(sol.board_pose.orientation.w * lpit->second.orientation.w +
			                     sol.board_pose.orientation.x * lpit->second.orientation.x +
			                     sol.board_pose.orientation.y * lpit->second.orientation.y +
			                     sol.board_pose.orientation.z * lpit->second.orientation.z);
			float dang = 2.f * std::acos(std::clamp(dot, 0.f, 1.f)) * 57.29577951308232f;
			std::string moves;
			if (not entered.empty())
				moves += " enter " + entered.substr(0, entered.size() - 1);
			if (not exited.empty())
				moves += " exit " + exited.substr(0, exited.size() - 1);
			spdlog::info("board pop: fid \"{}\" {}->{} dpos {:.2f}mm dang {:.3f}deg dt{:.2f}s{}", fid,
			             lnit->second, (int)bv.size(), (double)dpos, (double)dang, dt, moves);
		}
	}
	// Stash every accepted solve (held frames `continue` before this call,
	// so the pop reference never advances to garbage either).
	last_board_pose[fid] = sol.board_pose;
	last_board_n[fid] = (int)bv.size();
	// Per-tag detail builder, shared by the heartbeat `board tags:`
	// line and the immediate `board flip:` line below.
	auto tag_detail = [&]() {
		std::string detail;
		for (size_t i = 0; i < bv.size() and i < bp.tag_stats.size(); ++i)
		{
			const auto & t = bp.tag_stats[i];
			char buf[256];
			snprintf(buf, sizeof(buf), " [%s r(%.1f,%.1f,%.1f) |%.2f|mm %.2fdeg u%.2f d%.2fm cos%.2f w%.3f s%.2f %s]",
			         t.payload.substr(0, 32).c_str(), (double)t.res_vec[0], (double)t.res_vec[1],
			         (double)t.res_vec[2], (double)t.res_mm, (double)t.res_deg, (double)t.white_u,
			         (double)bv[i].distance_m, (double)bv[i].cos_incidence, (double)t.weight,
			         (double)t.settle, t.used ? "used" : "down");
			detail += buf;
		}
		return detail;
	};
	// `board flip:` immediate forensics: any tag beyond the report
	// thresholds (50mm/10deg: far above noise, far below real flips).
	// Unthrottled but fires rarely; single-frame flips never survive
	// to the 2s heartbeat, and this is the only per-frame flip record.
	// Residual-blind at K==1 (passthrough fits itself): K==1 flips show
	// as `board pop:` dang instead.
	if (sol.has_flip)
		spdlog::info("board flip: fid \"{}\" K{}{}", fid, (int)bv.size(), tag_detail());
	if (now - last_solve_log > 2'000'000'000LL)
	{
		last_solve_log = now;
		std::string down;
		for (const auto & t: bp.tag_stats)
		{
			if (not t.used)
			{
				down += t.payload.substr(0, 32);
				down += '(';
				down += std::to_string(t.weight).substr(0, 4);
				down += ')';
			}
		}
		// LOO outcome rides the heartbeat when the solver ran (!single):
		// full = full set won, drop:J = LOO subset without tag J won,
		// N/A = K==2 (runner-up is an init variant, not a subset).
		std::string loo;
		if (not bp.single)
		{
			char buf[128];
			if (sol.loo_dropped >= 0)
				snprintf(buf, sizeof(buf), " loo drop:%d cost:%.2f margin:%.2f", sol.loo_dropped,
				         (double)sol.select_cost, (double)sol.select_margin);
			else
				snprintf(buf, sizeof(buf), " loo N/A cost:%.2f margin:%.2f", (double)sol.select_cost,
				         (double)sol.select_margin);
			loo = buf;
		}
		spdlog::info("board solve: fid \"{}\" tags {}/{} used rms {:.2f}mm {:.3f}deg iters {} {:.0f}us horizon {:.1f}ms at ({:.2f},{:.2f},{:.2f}){}{}",
		             fid, bp.tags_used, bp.tags_visible, (double)bp.rms_mm, (double)bp.rms_deg,
		             bp.lm_iters, bp.solve_us, horizon_ms, (double)sol.board_pose.position.x,
		             (double)sol.board_pose.position.y, (double)sol.board_pose.position.z,
		             down.empty() ? "" : std::string(" down ") + down, loo);
		// `board tags:` per-tag detail for the weight-model fit: residual
		// vector (board frame: constant = systematic, wandering = noise),
		// whitened norm u (Cauchy input: plateau/elbow/rejected), range,
		// incidence, final weight. K>=2 only; K==1 is trivially exact.
		if (bv.size() >= 2)
			spdlog::info("board tags: fid \"{}\"{}", fid, tag_detail());
		// `board smooth:` filter state: innovation (raw vs smoothed),
		// applied alpha, caller strength, shaping activity (dz = deadzone
		// holding, cap = slew bound, snap = teleport re-acquisition).
		std::string shaping;
		if (fdiag.snapped)
			shaping = "snap";
		else
		{
			if (fdiag.deadzoned)
				shaping += "dz";
			if (fdiag.capped)
				shaping += "cap";
			if (shaping.empty())
				shaping = "ok";
		}
		spdlog::info("board smooth: fid \"{}\" innov {:.2f}mm {:.3f}deg alpha {:.4f} w {:.3f} {}", fid,
		             fdiag.innov_mm, fdiag.innov_deg, fdiag.alpha, fdiag.strength, shaping);
	}
}

void scenes::stream::sync_fiducial_trackers(const to_headset::fiducial_map & map)
{
	auto & fp = fiducial_passthrough;

	// Union payload -> {size, static, tag}: first wins, conflicts warn.
	std::map<std::string, std::tuple<float, bool, std::string>> uni;
	std::string key;
	for (const auto & f: map.fiducials)
	{
		for (const auto & m: f.markers)
		{
			key += m.marker_data;
			key += ';';
			key += std::to_string(m.marker_size_m);
			key += f.is_static ? 'S' : 'M';
			key += ';';
			auto [it, fresh] = uni.emplace(m.marker_data, std::make_tuple(m.marker_size_m, f.is_static, f.tag));
			if (not fresh)
			{
				auto & [size, st, tag] = it->second;
				if (size != m.marker_size_m or st != f.is_static)
					spdlog::warn("Fiducial tracking: payload \"{}\" configured inconsistently, first wins",
					             m.marker_data.substr(0, 64));
				(void)tag;
			}
		}
	}
	configured_marker_payloads.clear();
	for (const auto & [payload, _]: uni)
		configured_marker_payloads.insert(payload);

	if (key != last_tracker_key)
	{
		last_tracker_key = key;
		// Drop trackers for vanished payloads (destroys their contexts).
		for (auto it = fiducial_trackers.begin(); it != fiducial_trackers.end();)
		{
			if (uni.contains(it->first))
				++it;
			else
				it = fiducial_trackers.erase(it);
		}
	}

	if (not xr::marker_tracker::supported(instance))
	{
		if (not fp.marker_support_logged)
		{
			fp.marker_support_logged = true;
			spdlog::info("Spatial marker tracking not supported by runtime");
		}
		return;
	}
	for (auto & [payload, ss]: uni)
	{
		auto [it, _] = fiducial_trackers.try_emplace(payload, instance, session, system);
		auto & [size, is_static, tag] = ss;
		it->second.configure(size, payload, tag, is_static);
	}
}

void scenes::stream::update_fiducial_passthrough(XrTime predicted_display_time)
{
	XrSpace world_space = application::space(xr::spaces::world);
	auto & fp = fiducial_passthrough;
	// C0 cost split (clocks only, no behavior change): sync churn (map
	// copy, fingerprint strings, exists() syscalls, tracker sync, resolver,
	// hold-store) vs tracker XR work (snapshot + query per tracker).
	auto t_sync0 = std::chrono::steady_clock::now();
	// Snapshot, never held across model file loads below: the network
	// thread serves chunks through the same lock.
	to_headset::fiducial_map map = *fiducial_map.lock();

	// Fingerprint: tracking/render identity. Tags excluded (display-only);
	// feather/fade excluded (live render-only). Any other change wipes
	// instances; model-cache flips only re-arm uploads (below).
	std::string map_key;
	for (const auto & f: map.fiducials)
	{
		map_key += 'F';
		map_key += f.id;
		map_key += f.is_static ? 'S' : 'M';
		map_key += ';';
		map_key += std::to_string(f.window_size) + ',' + std::to_string(f.min_samples) + ',' +
		        std::to_string(f.sigma_k) + ',' + std::to_string(f.pos_gain) + ',' + std::to_string(f.rot_gain) + ',' +
		        std::to_string(f.euro_min_cutoff) + ',' + std::to_string(f.euro_beta) + ',' +
		        std::to_string(f.knee_inner_mm) + ',' + std::to_string(f.knee_outer_mm) + ',' +
		        std::to_string(f.knee_inner_deg) + ',' + std::to_string(f.knee_outer_deg) + ';';
		for (const auto & m: f.markers)
		{
			map_key += m.marker_data;
			map_key += ';';
			map_key += std::to_string(m.marker_size_m);
			map_key += ';';
			for (float v: m.position)
				map_key += std::to_string(v) + ',';
			map_key += ';';
			for (float v: m.orientation)
				map_key += std::to_string(v) + ',';
			map_key += ';';
		}
	}
	for (const auto & o: map.objects)
	{
		map_key += 'O';
		map_key += o.type;
		map_key += ';';
		map_key += o.id;
		map_key += ';';
		for (const auto & fid: o.fiducial)
		{
			map_key += fid;
			map_key += ',';
		}
		map_key += ';';
		map_key += o.model_hash;
		map_key += ';';
		for (float v: o.position)
			map_key += std::to_string(v) + ',';
		map_key += ';';
		for (float v: o.orientation)
			map_key += std::to_string(v) + ',';
		map_key += ';';
		map_key += std::to_string(o.scale);
		map_key += ';';
	}
	std::string key = map_key;
	for (const auto & o: map.objects)
	{
		if (o.model_hash.empty())
			continue;
		std::error_code ec;
		bool cached = std::filesystem::exists(fiducial_model_path(o.model_hash), ec);
		key += o.model_hash.substr(0, 8);
		key += cached ? 'C' : 'D';
		key += ';';
	}

	// Trackers run every frame, independent of objects/meshes (their state
	// feeds the status UI and the debug overlays pre-placement).
	sync_fiducial_trackers(map);
	XrTime now = instance.now();
	auto t_track0 = std::chrono::steady_clock::now();
	for (auto & [payload, tr]: fiducial_trackers)
	{
		(void)payload;
		tr.update(world_space, now, predicted_display_time);
	}
	auto t_track1 = std::chrono::steady_clock::now();

	// Resolver: one board hypothesis (vote) per (fiducial, marker,
	// entity) = observed * effective offset. ALL markers of a fiducial
	// vote now (was: markers[0] only). Rebuilt every frame.
	fiducial_votes.clear();
	for (const auto & f: map.fiducials)
	{
		for (const auto & m: f.markers)
		{
			auto tr = fiducial_trackers.find(m.marker_data);
			if (tr == fiducial_trackers.end())
				continue;
			std::array<float, 3> ep;
			std::array<float, 4> eq;
			effective_offset(m.position, m.orientation, ep, eq);
			for (const auto & s: tr->second.sightings())
			{
				if (s.payload != m.marker_data)
					continue;
				fiducial_vote v;
				v.fiducial_id = f.id;
				v.payload = s.payload;
				v.observed = s.pose;
				v.tag_size_m = m.marker_size_m;
				v.offset_pos = ep;
				v.offset_quat = eq;
				v.latch_time = s.latch_time;
				fiducial_votes.push_back(std::move(v));
			}
		}
	}

	// Debug hold-store: every sighted code at its raw instant pose (green
	// when some fiducial lists the payload, red otherwise); entries freeze
	// (SLAM hold) when unseen. Runs even before any placement.
	for (auto & [payload, tr]: fiducial_trackers)
	{
		(void)payload;
		for (const auto & s: tr.sightings())
		{
			auto & h = fp.held_codes[{s.payload, s.entity_id}];
			h.payload = s.payload.substr(0, 64);
			h.pose = s.pose;
			h.extents = s.extents;
			h.last_seen = s.time;
			h.matched = configured_marker_payloads.contains(s.payload);
		}
	}
	// Collapse duplicate tracks: one payload is one identity, but the
	// runtime recycles entity ids (discovery re-latch vs update path),
	// which ghosts a frozen duplicate beside the live track — duplicate
	// list rows and double quads. Keep newest seen; tie-break by smaller
	// entity id so concurrent tracks pick a stable survivor (no flicker).
	for (auto it = fp.held_codes.begin(); it != fp.held_codes.end();)
	{
		bool superseded = false;
		for (auto jt = fp.held_codes.begin(); jt != fp.held_codes.end(); ++jt)
		{
			if (jt == it or jt->first.first != it->first.first)
				continue;
			if (jt->second.last_seen > it->second.last_seen or
			    (jt->second.last_seen == it->second.last_seen and jt->first.second < it->first.second))
			{
				superseded = true;
				break;
			}
		}
		if (superseded)
			it = fp.held_codes.erase(it);
		else
			++it;
	}
	auto t_sync1 = std::chrono::steady_clock::now();
	fp.last_tracker_ms = std::chrono::duration<double, std::milli>(t_track1 - t_track0).count();
	fp.last_sync_ms = std::chrono::duration<double, std::milli>(t_sync1 - t_sync0).count() - fp.last_tracker_ms;

	if (key != fp.last_key)
	{
		fp.last_key = key;
		fp.attempted = false;
		fp.marker_support_logged = false;
		// Full reset only when the map itself changed: a download finishing
		// (cache flip) preserves live instances, just re-arms uploads.
		if (map_key != fp.last_map_key)
		{
			fp.last_map_key = map_key;
			fp.calibrated = false;
			fp.held_codes.clear();
			passthrough_objects.clear();
			fiducial_votes.clear();
			board_poses.clear();
			board_warm_start.clear();
			board_filters.clear();
			fp.corrected_tags.clear();
			// `board config:` makes every future log self-describing:
			// authored tag-in-board offsets + sizes + object links, so
			// tuning sessions never guess what geometry produced the data.
			for (const auto & f: map.fiducials)
			{
				std::string cfg;
				for (const auto & m: f.markers)
				{
					cfg += " [";
					cfg += m.marker_data.substr(0, 32);
					char buf[192];
					snprintf(buf, sizeof(buf), " %.0fmm pos(%.1f,%.1f,%.1f) quat(%.3f,%.3f,%.3f,%.3f)]",
					         (double)(m.marker_size_m * 1000.f), (double)(m.position[0] * 1000.f),
					         (double)(m.position[1] * 1000.f), (double)(m.position[2] * 1000.f),
					         (double)m.orientation[0], (double)m.orientation[1], (double)m.orientation[2],
					         (double)m.orientation[3]);
					cfg += buf;
				}
				spdlog::info("board config: fid \"{}\" {} markers{}",
				             f.id, f.is_static ? "static" : "moving", cfg);
			}
			for (const auto & o: map.objects)
			{
				std::string fids;
				for (const auto & fid: o.fiducial)
				{
					fids += fid;
					fids += ',';
				}
				spdlog::info("board config: object \"{}\" type \"{}\" fids -> {}", o.id, o.type, fids);
			}
			for (const auto & o: map.objects)
			{
				if (o.type != "3d-passthrough")
					spdlog::info("Passthrough object \"{}\": type \"{}\" not implemented, skipping", o.id, o.type);
			}
			if (fp.ready)
			{
				spdlog::info("Fiducial map changed, clearing projected mesh");
				session.clear_projected_passthrough_mesh();
				fp.ready = false;
			}
		}
		if (map.fiducials.empty() and map.objects.empty())
			fp.status = "no fiducial map from server";
	}

	// ---- fused board poses: one solve per fiducial ----
	// Head pose once per frame for the weight model (distance/incidence).
	// Neutral fallback (face-on at 1m) when untrackable: geometry still fuses.
	glm::vec3 head_pos{0};
	bool have_head = false;
	if (auto head = application::locate_controller(application::space(xr::spaces::view), world_space, predicted_display_time))
	{
		head_pos = head->first;
		have_head = true;
	}
	fp.last_horizon_ms = (predicted_display_time - now) * 1e-6;
	fp.last_solve_ms = 0;
	// Corrected tags persist across frames (stamped below on live
	// solves) so the overlay fades instead of popping; prune past 10s.
	for (auto it = fp.corrected_tags.begin(); it != fp.corrected_tags.end();)
	{
		if (now - it->second.last_seen > 10'000'000'000LL)
			it = fp.corrected_tags.erase(it);
		else
			++it;
	}
	for (auto & [fid, bp]: board_poses)
	{
		(void)fid;
		bp.live = false;
	}
	for (const auto & f: map.fiducials)
	{
		// Origin-tripod inputs: a non-trivial marker offset puts the
		// board origin apart from the tags. Position threshold 2mm,
		// angle threshold 1deg (both far below real rig offsets, far
		// above float/config noise).
		bool has_offset = false;
		float tag_size = 0;
		for (const auto & m: f.markers)
		{
			tag_size = std::max(tag_size, m.marker_size_m);
			float pp = m.position[0] * m.position[0] + m.position[1] * m.position[1] + m.position[2] * m.position[2];
			float qa = 2.f * std::acos(std::clamp(std::abs(m.orientation[3]), 0.f, 1.f)) * 57.29577951308232f;
			if (pp > 0.002f * 0.002f or qa > 1.f)
			{
				has_offset = true;
				break;
			}
		}
		std::vector<xr::board_vote> bv;
		for (const auto & v: fiducial_votes)
		{
			if (v.fiducial_id != f.id)
				continue;
			xr::board_vote b;
			b.payload = v.payload;
			b.observed = v.observed;
			for (int k = 0; k < 3; ++k)
				b.offset_pos[k] = v.offset_pos[k];
			for (int k = 0; k < 4; ++k)
				b.offset_quat[k] = v.offset_quat[k];
			b.tag_size_m = v.tag_size_m;
			// Settle from latch age (fail open: unknown latch = settled).
			// Latch stamps mix snapshot clocks (discovery: predicted_time,
			// update: now; ~20ms basis skew, negligible vs tau=0.4s).
			b.settle = 1;
			if (v.latch_time != 0 and now >= v.latch_time)
				b.settle = (float)xr::settle_from_age((now - v.latch_time) * 1e-9);
			glm::vec3 tp(v.observed.position.x, v.observed.position.y, v.observed.position.z);
			glm::quat tq(v.observed.orientation.w, v.observed.orientation.x, v.observed.orientation.y,
			             v.observed.orientation.z);
			float d = have_head ? glm::length(tp - head_pos) : 1.0f;
			b.distance_m = d;
			if (have_head and d > 1e-6f)
			{
				glm::vec3 n = tq * glm::vec3(0, 0, 1);
				b.cos_incidence = std::abs(glm::dot(n, (tp - head_pos) / d));
			}
			else
				b.cos_incidence = 1.0f;
			bv.push_back(std::move(b));
		}
		if (bv.empty())
			continue;
		std::optional<XrPosef> warm;
		if (auto it = board_warm_start.find(f.id); it != board_warm_start.end())
			warm = it->second;
		xr::board_solution sol = xr::solve_board(bv, warm);
		if (not sol.ok)
			continue;
		// Residual-blind singletons hold last-good instead of displaying
		// flips (guard_k1_jump above); held frames skip all state writes.
		double solve_dt = 0;
		if (guard_k1_jump(f.id, sol, warm, now, solve_dt))
			continue;
		board_warm_start[f.id] = sol.board_pose;
		auto & bp = board_poses[f.id];
		// Post-resolution smoothing (D-hybrid rigid filter; multi and
		// single-tag flow through identically). Strength from fusion stats;
		// dt from the last accepted solve (holds/total-loss freeze the
		// clock, so returns snap via the teleport path when stale).
		int used = 0;
		for (const auto & t: sol.tags)
			used += t.used;
		double fdt = 0;
		if (bp.time != 0 and now >= bp.time)
			fdt = (now - bp.time) * 1e-9;
		float w = xr::board_filter::strength(used, (int)sol.tags.size(), sol.rms_mm, sol.single);
		xr::board_filter_diag fdiag;
		XrPosef smoothed = board_filters[f.id].update(sol.board_pose, w, fdt, &fdiag);
		board_warm_start[f.id] = smoothed;
		bp.pose = smoothed;
		bp.raw_pose = sol.board_pose;
		bp.time = now;
		bp.live = true;
		bp.rms_mm = sol.rms_mm;
		bp.rms_deg = sol.rms_deg;
		bp.solve_us = sol.solve_us;
		bp.lm_iters = sol.lm_iters;
		bp.single = sol.single;
		bp.tags_used = 0;
		for (const auto & t: sol.tags)
			bp.tags_used += t.used;
		bp.tags_visible = (int)sol.tags.size();
		bp.tag_stats = std::move(sol.tags);
		bp.has_offset = has_offset;
		bp.tag_size_m = tag_size;
		fp.last_solve_ms = std::max(fp.last_solve_ms, sol.solve_us * 1e-3);
		// Orange overlay inputs: each tag re-projected from the SMOOTHED
		// board pose with its AUTHORED offset (predicted = smoothed *
		// authored: the inverse of the vote path, so no extra math).
		// Green raw vs orange corrected shows disagreement at a glance.
		for (const auto & m: f.markers)
			fp.corrected_tags[m.marker_data] = {compose_pose(smoothed, m.position, m.orientation), m.marker_size_m, f.id, now};
		// Tuning-instrument logging (heartbeat + set changes + pops + flips).
		log_board_tuning(f.id, bv, sol, bp, fp.last_horizon_ms, now, solve_dt, fdiag);
	}

	// ---- behavior objects: first referenced board pose wins ----
	for (auto oit = passthrough_objects.begin(); oit != passthrough_objects.end();)
	{
		if (std::ranges::any_of(map.objects, [&](const to_headset::passthrough_object & o) { return o.id == oit->first; }))
			++oit;
		else
			oit = passthrough_objects.erase(oit);
	}
	for (const auto & def: map.objects)
	{
		if (def.type != "3d-passthrough")
			continue;
		auto & ost = passthrough_objects[def.id];
		ost.def = def;
		// Model geometry upload (shared across this object's instances).
		if (not def.model_hash.empty())
		{
			std::error_code ec;
			if (std::filesystem::exists(fiducial_model_path(def.model_hash), ec) and ost.soup_hash != def.model_hash)
			{
				ost.soup_hash = def.model_hash;
				ost.soup_ready = false;
				try
				{
					ost.soup = passthrough_mesh::flatten_gltf(fiducial_model_path(def.model_hash));
					ost.soup_ready = true;
					ost.vertex_count = ost.soup.vertices.size();
					ost.triangle_count = ost.soup.indices.size() / 3;
					ost.status = "mesh ready";
					spdlog::info("Passthrough object \"{}\": mesh ready: {} triangles", def.id, ost.triangle_count);
				}
				catch (std::exception & e)
				{
					ost.status = std::string("mesh error: ") + e.what();
					spdlog::warn("Passthrough object \"{}\": mesh failed: {}", def.id, e.what());
				}
			}
		}
		// Placement: first referenced fiducial with a pose (live or
		// held) wins. Held poses keep rendering through occlusion
		// (hold-last); output smoothing is a separate track
		// (client/xr/fiducial_filter.*), so this is the raw fused output.
		for (const auto & fid: def.fiducial)
		{
			auto bpit = board_poses.find(fid);
			if (bpit == board_poses.end())
				continue;
			auto & fu = ost.fused;
			bool gap = fu.last_solve > 0 and (now - fu.last_solve) > 1'000'000'000LL;
			fu.world_pose = compose_pose(bpit->second.pose, def.position, def.orientation);
			fu.world_scale = {def.scale, def.scale, def.scale};
			if (not fu.placed)
			{
				fu.placed = true;
				fu.placed_at = now;
				fu.fade_start = predicted_display_time;
				spdlog::info("Passthrough object \"{}\" placed on fiducial \"{}\"", def.id, fid);
			}
			if (bpit->second.live)
			{
				fu.last_solve = now;
				if (gap)
					fu.fade_start = predicted_display_time; // reacquire fade
			}
			break;
		}
	}

	// ---- render shim: first placed object drives the single render path
	// (per-feather groups replace this) ----
	fp.calibrated = false;
	const passthrough_object_state * shim_ost = nullptr;
	for (auto & [oid, ost]: passthrough_objects)
	{
		(void)oid;
		if (ost.def.type != "3d-passthrough" or not ost.soup_ready)
			continue;
		if (not ost.fused.placed)
			continue;
		fp.calibrated = true;
		fp.world_pose = ost.fused.world_pose;
		fp.world_scale = ost.fused.world_scale;
		fp.status = ost.status;
		shim_ost = &ost;
		break;
	}

	// Strict gating: tracking above runs unconditionally (status UI and
	// debug overlays), but nothing mesh-related submits until an object
	// is placed on a fused board pose.
	if (not fp.calibrated)
		return;

	// Mask groups record in render() (per-feather stacks); nothing to
	// upload here. The binary fallback below runs solely where the mask
	// path is unavailable (no alpha-blend ext): the projected layer type
	// has no per-object layers, and its first-frame presentation flashes
	// fullscreen, so it stays out of the mask stack's way entirely.
	if (not composition_layer_alpha_blend_supported)
	{
		// Re-upload if the runtime lost the mesh (e.g. passthrough re-created)
		if (fp.ready and not session.has_projected_passthrough_mesh())
		{
			fp.ready = false;
			fp.attempted = false;
			fp.status = "runtime mesh lost, re-uploading";
		}

		if (fp.ready)
		{
			session.update_projected_passthrough_transform(world_space, predicted_display_time, fp.world_pose, fp.world_scale);
			add_projected_passthrough_layer();
			return;
		}

		if (fp.attempted or not shim_ost)
			return;

		if (not instance.has_extension(XR_FB_TRIANGLE_MESH_EXTENSION_NAME))
		{
			fp.attempted = true;
			fp.status = "XR_FB_triangle_mesh not supported by runtime";
			return;
		}

		fp.attempted = true;
		try
		{
			// Placement comes from the live instance (strict gating above
			// guarantees fp.calibrated here).
			session.set_projected_passthrough_mesh(shim_ost->soup.vertices, shim_ost->soup.indices, world_space, fp.world_pose, fp.world_scale);

			fp.ready = true;
			fp.status = "projected";
			spdlog::info("Fiducial passthrough mesh live: {} vertices, {} triangles",
			             shim_ost->vertex_count, shim_ost->triangle_count);

			session.update_projected_passthrough_transform(world_space, predicted_display_time, fp.world_pose, fp.world_scale);
			add_projected_passthrough_layer();
		}
		catch (std::exception & e)
		{
			fp.status = std::string("mesh error: ") + e.what();
			spdlog::warn("Fiducial passthrough mesh failed: {}", e.what());
		}
	}
}

void scenes::stream::gui_passthrough()
{
	wivrn::ui::page_header(_S("Passthrough"), _S("QR-anchored passthrough meshes, marker tracking and alignment."));
	gui_fiducial_status();

	auto & fp = fiducial_passthrough;
	ImGui::SeparatorText(_S("Fiducial marker debugging"));
	ImGui::Checkbox(_S("Marker debug overlays"), &fp.debug_overlays);
	ImGui::BeginDisabled(not fp.debug_overlays);
	ImGui::Checkbox(_S("Matched markers (green)"), &fp.debug_matched);
	ImGui::Checkbox(_S("Unmatched codes (red)"), &fp.debug_unmatched);
	float pct = fp.debug_opacity * 100;
	if (ImGui::SliderFloat(_S("Debug overlay opacity"), &pct, 0, 100, "%.0f%%"))
		fp.debug_opacity = std::clamp(pct / 100, 0.f, 1.f);
	if (ImGui::SliderInt(_S("Marker window cutout"), &fp.debug_window_mm, -1, 512, fp.debug_window_mm < 0 ? "Disabled" : "%d mm"))
		fp.debug_window_mm = std::clamp(fp.debug_window_mm, -1, 512);
	ImGui::Checkbox(_S("Corrected tags (orange)"), &fp.debug_corrected);
	ImGui::Checkbox(_S("Axes gizmo (RGB=XYZ)"), &fp.debug_axes);
	ImGui::Checkbox(_S("Origin tripod (offset boards)"), &fp.debug_origin);
	ImGui::Checkbox(_S("Smoothed tripod (cyan, filtered pose)"), &fp.debug_smoothed);
	if (ImGui::Button(_S("Run board solver self-test")))
	{
		xr::board_solver_selftest();
		xr::board_filter_selftest();
	}
	if (not fp.held_codes.empty())
	{
		ImGui::Text("%s (%zu):", _S("Codes in view"), fp.held_codes.size());
		ImGui::BeginChild("debug_codes", {0, 280});
		for (const auto & [id, h]: fp.held_codes)
		{
			(void)id;
			ImVec4 dot = h.matched ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
			double age_s = (instance.now() - h.last_seen) * 1e-9;
			ImGui::TextColored(dot, "%s", h.matched ? "[o]" : "[x]");
			ImGui::SameLine();
			ImGui::Text("%.40s (%.0fx%.0fmm, %.1fs ago)",
			            h.payload.c_str(),
			            (double)(h.extents.width * 1000),
			            (double)(h.extents.height * 1000),
			            age_s);
		}
		ImGui::EndChild();
	}
	ImGui::EndDisabled();
}

void scenes::stream::gui_fiducial_status()
{
	auto map = fiducial_map.lock();

	if (map->fiducials.empty() and map->objects.empty())
	{
		ImGui::Text("%s", _S("No fiducial map from server"));
	}
	for (const auto & f: map->fiducials)
	{
		std::string label = f.tag.empty() ? f.id : f.tag;
		// Live if any current vote comes through this fiducial.
		bool live = false;
		for (const auto & v: fiducial_votes)
		{
			if (v.fiducial_id == f.id)
			{
				live = true;
				break;
			}
		}
		ImVec4 dot = live ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
		ImGui::TextColored(dot, "%s", live ? "[o]" : "[x]");
		ImGui::SameLine();
		ImGui::Text("%s %.40s: %s, %zu marker(s)%s",
		            _S("Fiducial"),
		            label.c_str(),
		            live ? _S("tracking") : _S("seeking"),
		            f.markers.size(),
		            f.is_static ? "" : " (moving)");
	}
	for (const auto & [oid, ost]: passthrough_objects)
	{
		(void)oid;
		std::string label = ost.def.tag.empty() ? ost.def.id : ost.def.tag;
		if (ost.def.type != "3d-passthrough")
		{
			ImGui::Text("%s %.40s: %s \"%s\" (%s)",
			            _S("Object"), label.c_str(), _S("type"), ost.def.type.c_str(), _S("not implemented"));
			continue;
		}
		if (not ost.soup_ready)
		{
			ImGui::Text("%s %.40s: %s", _S("Object"), label.c_str(), ost.status.c_str());
			continue;
		}
		if (not ost.fused.placed)
		{
			ImGui::Text("%s %.40s: %s", _S("Object"), label.c_str(), _S("seeking"));
			continue;
		}
		double stale_s = (instance.now() - ost.fused.last_solve) * 1e-9;
		ImGui::Text("%s %.40s: %zu tris, %s (stale %.1fs)",
		            _S("Object"), label.c_str(), ost.triangle_count, _S("placed"), stale_s);
		// Per-fiducial solve stats for placed objects.
		for (const auto & fid: ost.def.fiducial)
		{
			auto bpit = board_poses.find(fid);
			if (bpit == board_poses.end())
				continue;
			const auto & bp = bpit->second;
			ImGui::Text("  %s %.40s: %d/%d tags rms %.1fmm %.2fdeg %.0fus%s",
			            _S("board"), fid.c_str(), bp.tags_used, bp.tags_visible,
			            (double)bp.rms_mm, (double)bp.rms_deg, bp.solve_us,
			            bp.single ? " (single)" : "");
			size_t shown = 0;
			for (const auto & t: bp.tag_stats)
			{
				if (shown >= 8)
				{
					ImGui::Text("  ... +%zu", bp.tag_stats.size() - shown);
					break;
				}
				++shown;
				ImGui::TextColored(t.used ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f},
				                   "  %.32s res %.1fmm %.2fdeg w %.2f",
				                   t.payload.c_str(), (double)t.res_mm, (double)t.res_deg, (double)t.weight);
			}
		}
	}
	if (fiducial_trackers.empty())
	{
		ImGui::Text("%s: %s", _S("Marker tracking"), _S("unavailable"));
#ifdef __ANDROID__
		// Trackers only exist when the runtime exposes the spatial
		// extensions. On Quest that means the manifest permissions (fixed) and
		// the USE_SCENE runtime grant; extensions enumerate at instance
		// creation, so a late grant needs an app restart to take effect.
		if (not scene_permission_granted and (not map->fiducials.empty() or not map->objects.empty()))
			ImGui::Text("%s", _S("On Quest, grant the Scene permission, then restart the app"));
#endif
	}
}
