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
// Syncs one runtime tracker per configured QR payload from the server map,
// resolves sightings to fiducial frames, and smooths each frame with its
// fiducial's tuning. Runs every frame; the solved frames feed the status
// UI here and the passthrough consumers downstream.

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
xr::fiducial_filter::tuning tuning_for(const to_headset::fiducial_entry & f)
{
	xr::fiducial_filter::tuning t;
	t.window_size = std::max(2, f.window_size);
	t.min_samples = std::clamp(f.min_samples, 1, t.window_size);
	t.sigma_k = std::max(0.5f, f.sigma_k);
	t.pos_gain = std::max(0.1f, f.pos_gain);
	t.rot_gain = std::max(0.1f, f.rot_gain);
	t.euro_min_cutoff = std::max(0.05f, f.euro_min_cutoff);
	t.euro_beta = std::max(0.f, f.euro_beta);
	t.knee_inner_mm = f.knee_inner_mm;
	t.knee_outer_mm = std::max(t.knee_inner_mm, f.knee_outer_mm);
	t.knee_inner_deg = f.knee_inner_deg;
	t.knee_outer_deg = std::max(t.knee_inner_deg, f.knee_outer_deg);
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
	return std::to_string(f.window_size) + ',' + std::to_string(f.min_samples) + ',' +
	        std::to_string(f.sigma_k) + ',' + std::to_string(f.pos_gain) + ',' + std::to_string(f.rot_gain) + ',' +
	        std::to_string(f.euro_min_cutoff) + ',' + std::to_string(f.euro_beta) + ',' +
	        std::to_string(f.knee_inner_mm) + ',' + std::to_string(f.knee_outer_mm) + ',' +
	        std::to_string(f.knee_inner_deg) + ',' + std::to_string(f.knee_outer_deg);
}
} // namespace

void scenes::stream::sync_fiducial_trackers(const to_headset::fiducial_map & map)
{
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
		for (const auto & f: map.fiducials)
		{
			if (f.markers.size() > 1)
				spdlog::warn("Fiducial \"{}\": multi-marker resolve not implemented, using first of {} markers",
				             f.id, (unsigned)f.markers.size());
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

	// Resolver (single-marker): solved fiducial frame per (fiducial,
	// entity) = observed * marker offset. Rebuilt every frame.
	fiducial_sightings.clear();
	for (const auto & f: map->fiducials)
	{
		if (f.markers.empty())
			continue;
		const auto & m = f.markers[0];
		auto tr = fiducial_trackers.find(m.marker_data);
		if (tr == fiducial_trackers.end())
			continue;
		for (const auto & s: tr->second.sightings())
		{
			if (s.payload != m.marker_data)
				continue;
			fiducial_sightings[{f.id, s.entity_id}] = {
			        .fiducial_id = f.id,
			        .payload = s.payload,
			        .solved = compose_pose(s.pose, m.position, m.orientation),
			        .extents = s.extents,
			        .time = s.time,
			};
		}
	}

	// Smoothed frames: ingest every live sighting, predict forward for
	// render, prune ids unseen for 60s (the runtime recycles long-gone
	// entity ids: without expiry a return would leave a frozen ghost
	// beside the fresh frame).
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
		glm::vec3 tp(fs.solved.position.x, fs.solved.position.y, fs.solved.position.z);
		glm::quat tq(fs.solved.orientation.w, fs.solved.orientation.x, fs.solved.orientation.y, fs.solved.orientation.z);
		frame.filter.ingest(tp, tq, fs.time);
		frame.samples_ingested++;
		frame.last_seen = now;
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
		// Live if any current sighting resolves through this fiducial.
		bool live = false;
		for (const auto & [key, fs]: fiducial_sightings)
		{
			(void)key;
			if (fs.fiducial_id == f.id)
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
