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

// Fiducial marker tracking (Quest only, see docs/configuration.md).
// Syncs one runtime tracker per (marker size, static) group from the server
// map, resolves sightings to fiducial frames, and smooths each frame with
// its fiducial's tuning. Runs every frame; the latched frames feed the
// status UI here and the passthrough consumers downstream.

#include "stream.h"

#include "application.h"
#include "imgui.h"
#include "render/ui_widgets.h"
#include "utils/i18n.h"

#ifdef __ANDROID__
#include "android/permissions.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <tuple>
#include <vector>

#ifdef __ANDROID__
namespace
{
// USE_SCENE grant state. Written once from the permission callback (activity
// thread), read on the render thread for the status-tab hint; a torn read
// only delays the hint by a frame.
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

namespace
{
// Filter tuning carried through from the fiducial, clamped to sane ranges.
// Outer hold knees are 5x inner internally (config carries inners only).
xr::fiducial_filter::tuning tuning_for(const to_headset::fiducial_entry & f)
{
	xr::fiducial_filter::tuning t;
	t.sigma_k = std::max(0.5f, f.sigma_k);
	t.euro_min_cutoff = std::max(0.05f, f.euro_min_cutoff);
	t.euro_beta = std::max(0.f, f.euro_beta);
	t.hold_inner_mm = std::max(0.f, f.hold_inner_mm);
	t.hold_inner_deg = std::max(0.f, f.hold_inner_deg);
	return t;
}

// Serialized tuning for change detection: re-configures the filter when the
// fiducial's tuning changes, without disturbing anything else.
std::string tuning_key_for(const to_headset::fiducial_entry & f)
{
	return std::to_string(f.sigma_k) + ',' + std::to_string(f.euro_min_cutoff) + ',' +
	        std::to_string(f.euro_beta) + ',' + std::to_string(f.hold_inner_mm) + ',' +
	        std::to_string(f.hold_inner_deg);
}

// Authored marker offsets are tag-in-board: position = tag center in
// fiducial meters, orientation = tag orientation relative to fiducial
// axes. Votes need the inverse (board-in-tag). Inverted here, once per
// sighting (nanoseconds, K tiny) — never hand-invert in config. Unit
// quaternions: conjugate is the inverse.
void effective_offset(const std::array<float, 3> & p, const std::array<float, 4> & q,
                      std::array<float, 3> & op, std::array<float, 4> & oq)
{
	glm::quat bq(q[3], q[0], q[1], q[2]);
	glm::vec3 bp(p[0], p[1], p[2]);
	glm::quat iq = glm::conjugate(bq);
	glm::vec3 ip = iq * -bp;
	oq = {iq.x, iq.y, iq.z, iq.w};
	op = {ip.x, ip.y, ip.z};
}

// Group key for the shared tracker: marker size is context-global, so one
// context serves all payloads with the same (size, static).
std::string group_key_for(float size_m, bool is_static)
{
	return std::to_string(size_m) + (is_static ? 'S' : 'M');
}
} // namespace

// No-config discovery vehicle payload: a sentinel no real QR payload will
// equal (leading control char), so every sighting through the vehicle
// reports unmatched (red). Never inserted into the map union.
const std::string kDiscoveryVehiclePayload = "\x01__wivrn_discovery__";

void scenes::stream::sync_fiducial_trackers(const to_headset::fiducial_map & map)
{
	// Group (size, static) -> {size, static}: one shared spatial context
	// per group, payloads match client-side. Payloads stay informational
	// here (union for diagnostics + ambiguity warnings).
	std::map<std::string, std::pair<float, bool>> groups;
	std::map<std::string, std::set<std::string>> payload_groups;
	for (const auto & f: map.fiducials)
	{
		for (const auto & m: f.markers)
		{
			std::string gk = group_key_for(m.marker_size_m, f.is_static);
			groups.emplace(gk, std::make_pair(m.marker_size_m, f.is_static));
			payload_groups[m.marker_data].insert(gk);
		}
	}
	std::string key;
	for (const auto & [gk, _]: groups)
	{
		key += gk;
		key += ';';
	}
	// No-config discovery vehicle: with an empty map there is nothing to
	// match against, yet the debug gizmos/cutouts report raw sightings
	// (all unmatched/red) whenever the debug toggle is on. One discovering
	// context suffices: every tracker already reports all codes in view,
	// and with configured trackers present this is redundant, so the
	// vehicle exists only while the union is empty. Folded into the
	// fingerprint so toggle/map transitions re-sync deterministically.
	bool want_vehicle = groups.empty() and debug_overlays;
	if (want_vehicle)
		key += "V;";

	if (key != last_tracker_key)
	{
		last_tracker_key = key;
		for (const auto & [payload, gks]: payload_groups)
		{
			if (gks.size() > 1)
				spdlog::warn("Fiducial tracking: payload \"{}\" configured with {} different (size, static) groups, each tracks it at its own scale",
				             payload.substr(0, 64), (unsigned)gks.size());
		}
		// Drop trackers for vanished groups (destroys their contexts).
		for (auto it = fiducial_trackers.begin(); it != fiducial_trackers.end();)
		{
			if (groups.contains(it->first))
				++it;
			else
				it = fiducial_trackers.erase(it);
		}
	}

	if (not xr::marker_tracker::supported(instance))
	{
		if (not marker_support_logged)
		{
			marker_support_logged = true;
			spdlog::info("Spatial marker tracking not supported by runtime");
		}
		return;
	}
	for (auto & [gk, ss]: groups)
	{
		auto [it, _] = fiducial_trackers.try_emplace(gk, instance, session, system);
		auto & [size, is_static] = ss;
		// Empty payload = enumerate-all; update_fiducial_passthrough
		// matches payloads against each fiducial's markers.
		std::string tag = "group " + gk;
		it->second.configure(size, "", tag, is_static);
	}
	if (want_vehicle)
	{
		// Discovery vehicle only (see above): size unspecified so no
		// fixed-size hint is chained (the runtime measures codes itself),
		// not static so no static-marker assumption either. The sentinel
		// never matches: every sighting stays unmatched (red), and the
		// resolver below finds no fiducials, so no fusion engages.
		auto [it, _] = fiducial_trackers.try_emplace(kDiscoveryVehiclePayload, instance, session, system);
		it->second.configure(0.f, kDiscoveryVehiclePayload, "discovery", false);
	}
	else
		fiducial_trackers.erase(kDiscoveryVehiclePayload);
}

// K==1 jump guard: the passthrough path is residual-blind (a lone
// vote fits itself), so a flipped singleton would display inverted.
// Frame jumps are physically impossible by hand — carry speeds run
// ~5mm/frame, so dpos 50mm (4.5m/s) / dang 30deg (2700deg/s) clear legit
// motion while sitting far below flips (70mm+/40deg+). On trigger, the
// caller skips ALL state writes: the frame becomes an occlusion frame
// downstream (hold-last) and the warm reference never advances to
// garbage. Release on resolve / K>=2 consensus / teleport-silence.
// First sighting (no reference) always accepts: unknowable without history.
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
                                         bool have_head, glm::vec3 head_pos)
{
	// `board solve:` is the tuning instrument (more logging better).
	static XrTime last_solve_log = 0;
	static std::map<std::string, std::string> last_sets;
	static std::map<std::string, XrPosef> last_board_pose;
	static std::map<std::string, int> last_board_n;
	// Viewpoint per sample (world-space head position, meters): residual
	// direction vs viewpoint separates tilt-type bias (rotates with view)
	// from depth noise (view-axis aligned) and translation bias (constant).
	char head_buf[64];
	if (have_head)
		snprintf(head_buf, sizeof(head_buf), " head(%.2f,%.2f,%.2f)", (double)head_pos.x,
		         (double)head_pos.y, (double)head_pos.z);
	else
		snprintf(head_buf, sizeof(head_buf), " head--");
	std::string head_str = head_buf;
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
		spdlog::info("board flip: fid \"{}\" K{}{}{}", fid, (int)bv.size(), tag_detail(), head_str);
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
			spdlog::info("board tags: fid \"{}\"{}{}", fid, tag_detail(), head_str);
	}
}

void scenes::stream::update_fiducial_passthrough(XrTime predicted_display_time)
{
	XrSpace world_space = application::space(xr::spaces::world);
	auto map = fiducial_map.lock();

	// Fingerprint: tracking identity. Tags excluded (display-only): a
	// rename must not disturb tracking. Any other change resets the
	// solved and smoothed frames.
	std::string map_key;
	for (const auto & f: map->fiducials)
	{
		map_key += 'F';
		map_key += f.id;
		map_key += f.is_static ? 'S' : 'M';
		map_key += ';';
		map_key += tuning_key_for(f);
		map_key += ';';
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
	// Trackers run every frame, independent of consumers (their state
	// feeds the status UI below).
	sync_fiducial_trackers(*map);
	XrTime now = instance.now();
	for (auto & [payload, tr]: fiducial_trackers)
	{
		(void)payload;
		tr.update(world_space, now, predicted_display_time);
	}

	// Fused poses built from stale votes reset with the map (trackers
	// already re-sync on their own key inside sync_fiducial_trackers).
	if (map_key != last_frame_key)
	{
		last_frame_key = map_key;
		fiducial_votes.clear();
		board_poses.clear();
		board_warm_start.clear();
		// `board config:` makes every future log self-describing:
		// marker sizes, offsets and tuning, so tuning sessions never
		// guess what geometry produced the data.
		for (const auto & f: map->fiducials)
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
	}

	// Resolver: one board hypothesis (vote) per (fiducial, marker,
	// entity) = observed pose plus the marker's size, offset and latch
	// time. ALL markers of a fiducial vote. Rebuilt every frame.
	fiducial_votes.clear();
	for (const auto & f: map->fiducials)
	{
		for (const auto & m: f.markers)
		{
			auto tr = fiducial_trackers.find(group_key_for(m.marker_size_m, f.is_static));
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
				v.extents = s.extents;
				v.tag_size_m = m.marker_size_m;
				v.offset_pos = ep;
				v.offset_quat = eq;
				v.latch_time = s.latch_time;
				v.time = s.time;
				v.novel = s.novel;
				fiducial_votes.push_back(std::move(v));
			}
		}
	}
	// Collapse duplicate votes: one payload is one identity, but the
	// runtime recycles entity ids (discovery re-latch vs update path),
	// which ghosts a frozen duplicate beside the live track — and would
	// double-weight the payload in the solve. Keep newest seen; assembly
	// order is deterministic (map order, then tracker order), so ties
	// resolve stably (no flicker).
	for (auto it = fiducial_votes.begin(); it != fiducial_votes.end();)
	{
		bool superseded = false;
		for (auto jt = fiducial_votes.begin(); jt != fiducial_votes.end(); ++jt)
		{
			if (jt == it or jt->payload != it->payload or jt->fiducial_id != it->fiducial_id)
				continue;
			if (jt->time > it->time)
			{
				superseded = true;
				break;
			}
		}
		if (superseded)
			it = fiducial_votes.erase(it);
		else
			++it;
	}

	// Viewpoint for the incidence/weight model (world-space head position).
	glm::vec3 head_pos{0};
	bool have_head = false;
	if (auto head = application::locate_controller(application::space(xr::spaces::view), world_space, predicted_display_time))
	{
		head_pos = head->first;
		have_head = true;
	}

	for (auto it = board_poses.begin(); it != board_poses.end();)
	{
		// Prune ids unseen for 60s (the runtime recycles long-gone
		// entity ids: without expiry a return would leave a frozen ghost
		// beside the fresh frame). Pruning drops the filter state, so a
		// returning fiducial re-seeds from the solver.
		if (it->second.time != 0 and now - it->second.time > 60'000'000'000LL)
			it = board_poses.erase(it);
		else
		{
			it->second.live = false;
			++it;
		}
	}
	for (const auto & f: map->fiducials)
	{
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
		auto & bp = board_poses[f.id];
		// Post-resolution smoothing (robust-Euro filter; multi and
		// single-tag flow through identically). Ingest fused poses with
		// novel content only (solver echo at frame rate would collapse
		// the filter's spread gate); advance coasts every frame.
		std::string tkey = tuning_key_for(f);
		xr::fiducial_filter::tuning t = tuning_for(f);
		if (tkey != bp.tuning_key)
		{
			bp.tuning_key = tkey;
			bp.filter.configure(t);
			bp.last_predicted = 0;
		}
		bool any_novel = false;
		for (const auto & v: fiducial_votes)
		{
			if (v.fiducial_id == f.id and v.novel)
			{
				any_novel = true;
				break;
			}
		}
		if (any_novel)
		{
			glm::vec3 tp(sol.board_pose.position.x, sol.board_pose.position.y, sol.board_pose.position.z);
			glm::quat tq(sol.board_pose.orientation.w, sol.board_pose.orientation.x, sol.board_pose.orientation.y,
			             sol.board_pose.orientation.z);
			bp.filter.ingest(tp, tq, now);
			bp.samples_ingested++;
		}
		double fdt = bp.time != 0 and now >= bp.time ? (now - bp.time) * 1e-9 : 1.0 / 72.0;
		double adt = bp.last_predicted > 0 ? (now - bp.last_predicted) * 1e-9 : fdt;
		bp.last_predicted = now;
		auto r = bp.filter.advance(std::clamp(adt, 0.0, 0.25));
		bp.pose = {
		        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
		        .position = {r.pos.x, r.pos.y, r.pos.z},
		};
		// Warm start follows the smoothed pose (solver init only).
		board_warm_start[f.id] = bp.pose;
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
		bp.last_votes = bv;
		// Tuning-instrument logging (heartbeat + set changes + pops + flips).
		double horizon_ms = (predicted_display_time - now) * 1e-6;
		log_board_tuning(f.id, bv, sol, bp, horizon_ms, now, solve_dt, have_head, head_pos);
	}
}

void scenes::stream::gui_passthrough()
{
	wivrn::ui::page_header(_S("Passthrough"), _S("QR marker tracking and alignment."));
	gui_fiducial_status();

	ImGui::SeparatorText(_S("Fiducial marker debugging"));
	ImGui::Checkbox(_S("Marker debug overlays"), &debug_overlays);
	ImGui::BeginDisabled(not debug_overlays);
	ImGui::Checkbox(_S("Matched markers (green)"), &debug_matched);
	ImGui::Checkbox(_S("Unmatched codes (red)"), &debug_unmatched);
	ImGui::Checkbox(_S("Corrected tags (orange)"), &debug_corrected);
	ImGui::Checkbox(_S("Axes gizmo (RGB=XYZ)"), &debug_axes);
	ImGui::Checkbox(_S("Origin tripod (offset boards)"), &debug_origin);
	ImGui::Checkbox(_S("Smoothed tripod (cyan, filtered pose)"), &debug_smoothed);
	float pct = debug_opacity * 100;
	if (ImGui::SliderFloat(_S("Debug overlay opacity"), &pct, 0, 100, "%.0f%%"))
		debug_opacity = std::clamp(pct / 100, 0.f, 1.f);
	if (ImGui::SliderInt(_S("Marker window cutout"), &debug_window_mm, -1, 512, debug_window_mm < 0 ? "Disabled" : "%d mm"))
		debug_window_mm = std::clamp(debug_window_mm, -1, 512);
	ImGui::EndDisabled();
}

void scenes::stream::gui_fiducial_status()
{
	auto map = fiducial_map.lock();

	if (map->fiducials.empty())
	{
		ImGui::Text("%s", _S("No fiducial map from server"));
	}
	if (fiducial_trackers.contains(kDiscoveryVehiclePayload))
	{
		ImGui::Text("%s", _S("Discovering QR codes (no map: all codes unmatched)"));
	}
	for (const auto & f: map->fiducials)
	{
		std::string label = f.tag.empty() ? f.id : f.tag;
		auto bpit = board_poses.find(f.id);
		bool live = bpit != board_poses.end() and bpit->second.live;
		// Sighted/total markers this frame: the configured count alone
		// reads as "tracked", so show the live fraction instead.
		size_t sighted = 0;
		for (const auto & v: fiducial_votes)
		{
			if (v.fiducial_id == f.id)
				sighted++;
		}
		// Live while solved this frame; otherwise fall back to the
		// latched (held) pose so the resolved 6DoF persists out of view.
		bool held = false;
		XrDuration held_age = 0;
		if (not live and bpit != board_poses.end())
		{
			held = true;
			held_age = instance.now() - bpit->second.time;
		}
		ImVec4 dot = live ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : held ? ImVec4{0.9f, 0.7f, 0.2f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
		ImGui::TextColored(dot, "%s", live ? "[o]" : held ? "[~]" : "[x]");
		ImGui::SameLine();
		if (held and not live)
			ImGui::Text("%s %.40s: %s (%.0fs ago), %zu/%zu marker(s)%s",
			            _S("Fiducial"),
			            label.c_str(),
			            _S("holding last pose"),
			            (double)held_age / 1e9,
			            sighted,
			            f.markers.size(),
			            f.is_static ? "" : " (moving)");
		else
			ImGui::Text("%s %.40s: %s, %zu/%zu marker(s)%s",
			            _S("Fiducial"),
			            label.c_str(),
			            live ? _S("tracking") : _S("seeking"),
			            sighted,
			            f.markers.size(),
			            f.is_static ? "" : " (moving)");
		if (bpit == board_poses.end())
			continue;
		const auto & bp = bpit->second;
		// Solution summary: how the fused pose was reached.
		if (bp.single)
			ImGui::Text("  %s: %s", _S("Solve"), _S("single tag passthrough"));
		else
		{
			char loo[96];
			if (bp.tag_stats.empty())
				snprintf(loo, sizeof(loo), "no solution yet");
			else if (bp.tags_visible >= 3 and bp.tags_used < bp.tags_visible)
				snprintf(loo, sizeof(loo), "LOO excluded outlier(s)");
			else
				snprintf(loo, sizeof(loo), "full set");
			ImGui::Text("  %s: %s, %d/%d %s, rms %.2fmm %.3fdeg, %d %s %.0fus",
			            _S("Solve"),
			            loo,
			            bp.tags_used,
			            bp.tags_visible,
			            _S("tags used"),
			            (double)bp.rms_mm,
			            (double)bp.rms_deg,
			            bp.lm_iters,
			            _S("iters"),
			            bp.solve_us);
		}
		// Fused (smoothed) pose: what consumers read.
		ImGui::Text("  %s: (%.3f, %.3f, %.3f)", _S("Board pos m"), (double)bp.pose.position.x,
		            (double)bp.pose.position.y, (double)bp.pose.position.z);
		// Filter state: innovation absorption in progress (rendered vs
		// held target), last robust weight, ingested novel samples.
		float lag_mm = 0;
		if (auto held_target = bp.filter.resolve())
		{
			float dx = held_target->pos.x - bp.pose.position.x;
			float dy = held_target->pos.y - bp.pose.position.y;
			float dz = held_target->pos.z - bp.pose.position.z;
			lag_mm = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.f;
		}
		ImGui::Text("  %s: w%.3f lag %.2fmm %llu %s", _S("Filter"), (double)bp.filter.mean_weight(),
		            (double)lag_mm, (unsigned long long)bp.samples_ingested, _S("samples"));
		// Per-tag rows: live sighting joined with the last accepted
		// solve's weight model inputs (range/incidence/settle) and
		// outputs (weight/residuals). Unseen tags show last-known stats.
		for (const auto & m: f.markers)
		{
			const fiducial_vote * vote = nullptr;
			for (const auto & v: fiducial_votes)
			{
				if (v.fiducial_id == f.id and v.payload == m.marker_data)
				{
					vote = &v;
					break;
				}
			}
			size_t tag_idx = 0;
			bool have_stat = false;
			for (; tag_idx < bp.tag_stats.size(); ++tag_idx)
			{
				if (bp.tag_stats[tag_idx].payload == m.marker_data)
				{
					have_stat = true;
					break;
				}
			}
			const xr::board_vote * lvote = nullptr;
			if (have_stat and tag_idx < bp.last_votes.size() and
			    bp.last_votes[tag_idx].payload == m.marker_data)
				lvote = &bp.last_votes[tag_idx];
			ImVec4 tdot = vote ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
			ImGui::TextColored(tdot, "%s", vote ? "[o]" : "[x]");
			ImGui::SameLine();
			if (vote and have_stat and lvote)
			{
				const auto & t = bp.tag_stats[tag_idx];
				double age_s = (instance.now() - vote->time) * 1e-9;
				ImGui::Text("%.32s: %s w%.3f r%.1fmm %.2fdeg d%.2fm cos%.2f s%.2f (%.1fs %s)",
				            m.marker_data.substr(0, 32).c_str(), t.used ? _S("used") : _S("down"),
				            (double)t.weight, (double)t.res_mm, (double)t.res_deg,
				            (double)lvote->distance_m, (double)lvote->cos_incidence, (double)t.settle,
				            age_s, _S("ago"));
			}
			else if (vote)
			{
				double age_s = (instance.now() - vote->time) * 1e-9;
				ImGui::Text("%.32s: %s (%.1fs %s)", m.marker_data.substr(0, 32).c_str(),
				            _S("sighted"), age_s, _S("ago"));
			}
			else if (have_stat)
				ImGui::Text("%.32s: %s w%.3f r%.1fmm %.2fdeg", m.marker_data.substr(0, 32).c_str(),
				            _S("unseen"), (double)bp.tag_stats[tag_idx].weight,
				            (double)bp.tag_stats[tag_idx].res_mm, (double)bp.tag_stats[tag_idx].res_deg);
			else
				ImGui::Text("%.32s: %s", m.marker_data.substr(0, 32).c_str(), _S("unseen"));
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
		if (not scene_permission_granted and (not map->fiducials.empty() or debug_overlays))
			ImGui::Text("%s", _S("On Quest, grant the Scene permission, then restart the app"));
#endif
	}
}
