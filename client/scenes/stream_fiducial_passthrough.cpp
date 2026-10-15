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
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <tuple>

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

// base * offset (translation unaffected by scale). Offset quaternion stored xyzw.
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

// Serialized tuning for change detection: re-configures the filter when the
// fiducial's tuning changes, without disturbing anything else.
std::string tuning_key_for(const to_headset::fiducial_entry & f)
{
	return std::to_string(f.sigma_k) + ',' + std::to_string(f.euro_min_cutoff) + ',' +
	        std::to_string(f.euro_beta) + ',' + std::to_string(f.hold_inner_mm) + ',' +
	        std::to_string(f.hold_inner_deg);
}

// Group key for the shared tracker: marker size is context-global, so one
// context serves all payloads with the same (size, static).
std::string group_key_for(float size_m, bool is_static)
{
	return std::to_string(size_m) + (is_static ? 'S' : 'M');
}
} // namespace

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
	configured_marker_payloads.clear();
	for (const auto & [payload, _]: payload_groups)
		configured_marker_payloads.insert(payload);

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

	// Frames built from stale sightings reset with the map (trackers
	// already re-sync on their own key inside sync_fiducial_trackers).
	if (map_key != last_frame_key)
	{
		last_frame_key = map_key;
		fiducial_sightings.clear();
		fiducial_frames.clear();
	}

	// Resolver (all markers): solved fiducial frame per (fiducial,
	// entity) = observed * marker offset. Rebuilt every frame from live
	// sightings only (ephemeral).
	fiducial_sightings.clear();
	for (const auto & f: map->fiducials)
	{
		for (const auto & m: f.markers)
		{
			auto tr = fiducial_trackers.find(group_key_for(m.marker_size_m, f.is_static));
			if (tr == fiducial_trackers.end())
				continue;
			for (const auto & s: tr->second.sightings())
			{
				if (s.payload != m.marker_data)
					continue;
				fiducial_sightings.emplace(
				        std::make_pair(f.id, s.entity_id),
				        fiducial_sighting{
				                .fiducial_id = f.id,
				                .payload = s.payload,
				                .solved = compose_pose(s.pose, m.position, m.orientation),
				                .extents = s.extents,
				                .time = s.time,
				                .novel = s.novel,
				        });
			}
		}
	}

	// Smoothed (latched) frames: the held 6DoF persists when codes leave
	// view — consumers read these, never the ephemeral sightings above.
	// Novel samples ingest; every frame (live or blind) advances the
	// robust-Euro filter toward its held target for render. Prune ids
	// unseen for 60s (the runtime recycles long-gone entity ids: without
	// expiry a return would leave a frozen ghost beside the fresh frame).
	for (auto it = fiducial_frames.begin(); it != fiducial_frames.end();)
	{
		if (fiducial_sightings.contains(it->first) or now - it->second.last_seen <= 60'000'000'000LL)
			++it;
		else
			it = fiducial_frames.erase(it);
	}
	for (const auto & [skey, fs]: fiducial_sightings)
	{
		auto fdef = std::ranges::find(map->fiducials, fs.fiducial_id, &to_headset::fiducial_entry::id);
		if (fdef == map->fiducials.end())
			continue;
		auto [it, fresh] = fiducial_frames.try_emplace(skey);
		auto & frame = it->second;
		std::string tkey = tuning_key_for(*fdef);
		if (fresh or tkey != frame.tuning_key)
		{
			frame.tuning_key = tkey;
			frame.filter.configure(tuning_for(*fdef));
			frame.last_predicted = 0;
		}
		if (fs.novel)
		{
			glm::vec3 tp(fs.solved.position.x, fs.solved.position.y, fs.solved.position.z);
			glm::quat tq(fs.solved.orientation.w, fs.solved.orientation.x, fs.solved.orientation.y, fs.solved.orientation.z);
			frame.filter.ingest(tp, tq, fs.time);
			frame.samples_ingested++;
		}
		frame.last_seen = now;
	}
	for (auto & [skey, frame]: fiducial_frames)
	{
		(void)skey;
		double dt = frame.last_predicted > 0 ? (predicted_display_time - frame.last_predicted) * 1e-9 : 1.0 / 72.0;
		frame.last_predicted = predicted_display_time;
		auto r = frame.filter.advance(std::clamp(dt, 0.0, 0.25));
		frame.pose = {
		        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
		        .position = {r.pos.x, r.pos.y, r.pos.z},
		};
	}
}

void scenes::stream::gui_passthrough()
{
	wivrn::ui::page_header(_S("Passthrough"), _S("QR marker tracking and alignment."));
	gui_fiducial_status();
}

void scenes::stream::gui_fiducial_status()
{
	auto map = fiducial_map.lock();

	if (map->fiducials.empty())
	{
		ImGui::Text("%s", _S("No fiducial map from server"));
	}
	for (const auto & f: map->fiducials)
	{
		std::string label = f.tag.empty() ? f.id : f.tag;
		// Live while sighted this frame; otherwise fall back to the latched
		// (held) frame so the resolved 6DoF persists out of view.
		bool live = false;
		bool held = false;
		XrDuration held_age = 0;
		for (const auto & [key, fs]: fiducial_sightings)
		{
			(void)key;
			if (fs.fiducial_id == f.id)
			{
				live = true;
				break;
			}
		}
		if (not live)
		{
			XrTime now = instance.now();
			for (const auto & [key, fr]: fiducial_frames)
			{
				if (key.first == f.id)
				{
					held = true;
					XrDuration age = now - fr.last_seen;
					if (held_age == 0 or age < held_age)
						held_age = age;
				}
			}
		}
		ImVec4 dot = live ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : held ? ImVec4{0.9f, 0.7f, 0.2f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
		ImGui::TextColored(dot, "%s", live ? "[o]" : held ? "[~]" : "[x]");
		ImGui::SameLine();
		if (held and not live)
			ImGui::Text("%s %.40s: %s (%.0fs ago), %zu marker(s)%s",
			            _S("Fiducial"),
			            label.c_str(),
			            _S("holding last pose"),
			            (double)held_age / 1e9,
			            f.markers.size(),
			            f.is_static ? "" : " (moving)");
		else
			ImGui::Text("%s %.40s: %s, %zu marker(s)%s",
			            _S("Fiducial"),
			            label.c_str(),
			            live ? _S("tracking") : _S("seeking"),
			            f.markers.size(),
			            f.is_static ? "" : " (moving)");
	}
	if (fiducial_trackers.empty())
	{
		ImGui::Text("%s: %s", _S("Marker tracking"), _S("unavailable"));
#ifdef __ANDROID__
		// Trackers only exist when the runtime exposes the spatial
		// extensions. On Quest that means the manifest permissions (fixed) and
		// the USE_SCENE runtime grant; extensions enumerate at instance
		// creation, so a late grant needs an app restart to take effect.
		if (not scene_permission_granted and not map->fiducials.empty())
			ImGui::Text("%s", _S("On Quest, grant the Scene permission, then restart the app"));
#endif
	}
}
