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
#include "utils/i18n.h"

#ifdef __ANDROID__
#include "android/permissions.h"
#endif

#include <cmath>
#include <glm/gtc/quaternion.hpp>
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

	// Either namespace counts: the grant survives reinstalls, so a returning
	// user on either old or new Horizon OS skips the prompt entirely.
	if (check_permission("horizonos.permission.USE_SCENE") or
	    check_permission("com.oculus.permission.USE_SCENE"))
	{
		scene_permission_granted = true;
		spdlog::info("Scene permission already granted");
		return;
	}

	// Canonical name on current Horizon OS first; the legacy name stays
	// declared in the manifest for older releases.
	spdlog::info("Requesting Scene permission for marker tracking");
	request_permission("horizonos.permission.USE_SCENE", [](bool granted) {
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
	// Strict gating: nothing mesh-related happens until the user presses
	// Calibrate. Pre-calibration behavior is stock upstream, so a grey
	// baseline with zero mesh lines in the log exonerates the mesh path.
	bool wanted = false;
	if (fiducial_passthrough.calibrated and instance.has_extension(XR_FB_TRIANGLE_MESH_EXTENSION_NAME))
	{
		std::error_code ec;
		auto entries = fiducial_entries.lock();
		for (const auto & entry: *entries)
		{
			if (entry.model_hash.empty())
				continue;
			if (std::filesystem::exists(fiducial_model_path(entry.model_hash), ec))
			{
				wanted = true;
				break;
			}
		}
	}

	static bool last_wanted = true; // log the initial false once
	if (wanted != last_wanted)
	{
		last_wanted = wanted;
		spdlog::info("Fiducial passthrough wanted: {}", wanted);
	}
	return wanted;
}

void scenes::stream::update_fiducial_passthrough(XrTime predicted_display_time)
{
	XrSpace world_space = application::space(xr::spaces::world);
	auto & fp = fiducial_passthrough;

	// Fingerprints: map_key covers the server map identity (any entry change
	// invalidates a calibration); the cache flag only re-arms the upload
	// (a download finishing must NOT wipe a calibration made before it).
	std::string map_key;
	std::optional<to_headset::fiducial_map_entry> entry;
	bool model_cached = false;
	{
		auto entries = fiducial_entries.lock();
		for (const auto & e: *entries)
		{
			map_key += e.model_hash;
			map_key += ';';
			map_key += std::to_string(e.marker_id);
			map_key += ';';
			map_key += std::to_string(e.marker_size_m);
			map_key += ';';
			for (float v: e.position)
				map_key += std::to_string(v) + ',';
			map_key += ';';
			for (float v: e.orientation)
				map_key += std::to_string(v) + ',';
			map_key += ';';
			map_key += std::to_string(e.scale);
			map_key += ';';
			if (not entry and not e.model_hash.empty())
				entry = e;
		}
	}
	if (entry)
	{
		std::error_code ec;
		model_cached = std::filesystem::exists(fiducial_model_path(entry->model_hash), ec);
	}
	std::string key = map_key + (model_cached ? 'C' : 'D');

	// Marker tracking (Phase 3): same map entry, independent of mesh state
	if (not entry)
		marker_tracker.reset();
	else if (entry->marker_size_m > 0)
	{
		if (not marker_tracker)
		{
			if (xr::marker_tracker::supported(instance))
				marker_tracker.emplace(instance, session, system);
			else if (not fp.marker_support_logged)
			{
				fp.marker_support_logged = true;
				spdlog::info("Spatial marker tracking not supported by runtime");
			}
		}
		if (marker_tracker)
		{
			marker_tracker->configure(entry->marker_id, entry->marker_size_m);
			marker_tracker->update(world_space, instance.now(), predicted_display_time);
		}
	}

	if (key != fp.last_key)
	{
		fp.last_key = key;
		fp.attempted = false;
		fp.marker_support_logged = false;
		// Full reset only when the map itself changed: a download finishing
		// (cache D->C flip) preserves a calibration made before it.
		if (map_key != fp.last_map_key)
		{
			fp.last_map_key = map_key;
			fp.calibrated = false;
			fp.calibrated_marker = -1;
			if (fp.ready)
			{
				spdlog::info("Fiducial map changed, clearing projected mesh");
				session.clear_projected_passthrough_mesh();
				fp.ready = false;
			}
		}
		if (not entry)
			fp.status = "no fiducial map from server";
		else if (not model_cached)
			fp.status = "downloading model " + entry->model_hash.substr(0, 8) + "...";
	}

	// Strict gating: marker tracking above runs unconditionally (needed for
	// the in-view dot and the Calibrate button), but nothing mesh-related
	// happens until calibrated.
	if (not fp.calibrated)
		return;

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

	if (fp.attempted or not entry or not model_cached)
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
		auto soup = passthrough_mesh::flatten_gltf(fiducial_model_path(entry->model_hash));

		// Placement always comes from calibrate_to_marker() (strict gating
		// above guarantees fp.calibrated here).
		session.set_projected_passthrough_mesh(soup.vertices, soup.indices, world_space, fp.world_pose, fp.world_scale);

		fp.ready = true;
		fp.model_hash = entry->model_hash;
		fp.vertex_count = soup.vertices.size();
		fp.triangle_count = soup.indices.size() / 3;
		fp.status = "projected";
		spdlog::info("Fiducial passthrough mesh live: {} vertices, {} triangles", fp.vertex_count, fp.triangle_count);

		session.update_projected_passthrough_transform(world_space, predicted_display_time, fp.world_pose, fp.world_scale);
		add_projected_passthrough_layer();
	}
	catch (std::exception & e)
	{
		fp.status = std::string("mesh error: ") + e.what();
		spdlog::warn("Fiducial passthrough mesh failed: {}", e.what());
	}
}

void scenes::stream::calibrate_to_marker()
{
	auto & fp = fiducial_passthrough;

	std::optional<to_headset::fiducial_map_entry> entry;
	{
		auto entries = fiducial_entries.lock();
		for (const auto & e: *entries)
		{
			if (not e.model_hash.empty())
			{
				entry = e;
				break;
			}
		}
	}
	if (not entry)
	{
		spdlog::warn("Calibrate: no fiducial map entry");
		return;
	}
	if (not marker_tracker)
	{
		spdlog::warn("Calibrate: marker tracking unsupported by runtime");
		return;
	}

	// Sighting timestamps use predicted display time and can sit slightly in
	// the future; only reject clearly stale data.
	XrTime now = instance.now();
	auto sighting = marker_tracker->latest();
	if (not sighting.tracked or sighting.time == 0 or (now - sighting.time) > 3'000'000'000)
	{
		spdlog::warn("Calibrate: no fresh marker sighting");
		return;
	}

	// Mesh anchor: meshClientPose = observedMarkerPose * markerToMeshOffset.
	// The offset translation is in meters and is not affected by the scale.
	glm::quat marker_quat(sighting.pose.orientation.w, sighting.pose.orientation.x, sighting.pose.orientation.y, sighting.pose.orientation.z);
	glm::vec3 marker_pos(sighting.pose.position.x, sighting.pose.position.y, sighting.pose.position.z);
	glm::quat offset_quat(entry->orientation[3], entry->orientation[0], entry->orientation[1], entry->orientation[2]);
	glm::vec3 offset_pos(entry->position[0], entry->position[1], entry->position[2]);
	glm::quat mesh_quat = marker_quat * offset_quat;
	glm::vec3 mesh_pos = marker_pos + marker_quat * offset_pos;
	fp.world_pose = {
	        .orientation = {mesh_quat.x, mesh_quat.y, mesh_quat.z, mesh_quat.w},
	        .position = {mesh_pos.x, mesh_pos.y, mesh_pos.z},
	};
	fp.world_scale = {entry->scale, entry->scale, entry->scale};
	fp.calibrated = true;
	fp.calibrated_at = now;
	fp.calibrated_marker = entry->marker_id;

	// World origin: yaw + XZ from the marker (marker-as-origin: afterwards the
	// marker reports at XZ origin with identity yaw). Y keeps following the
	// height setting, composed in the tracking loop.
	const auto & q = sighting.pose.orientation;
	float yaw = std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.x * q.x));
	{
		auto calib = tracking_origin_calibration.lock();
		calib->active = true;
		calib->yaw = yaw;
		calib->x = sighting.pose.position.x;
		calib->z = sighting.pose.position.z;
	}

	spdlog::info("Calibrated to marker {}: observed at ({:.2f}, {:.2f}, {:.2f}), mesh at ({:.2f}, {:.2f}, {:.2f}), origin yaw {:.1f}deg, sighting {}ms old",
	             entry->marker_id, marker_pos.x, marker_pos.y, marker_pos.z, mesh_pos.x, mesh_pos.y, mesh_pos.z, glm::degrees(yaw),
	             (long long)((now - sighting.time) / 1'000'000));
}

void scenes::stream::gui_fiducial_status()
{
	auto & fp = fiducial_passthrough;
	if (fp.ready)
		ImGui::Text("%s: %zu tris (%s)", _S("Passthrough mesh"), fp.triangle_count, fp.model_hash.substr(0, 8).c_str());
	else
		ImGui::Text("%s: %s", _S("Passthrough mesh"), fp.status.c_str());

	if (marker_tracker)
	{
		auto sighting = marker_tracker->latest();
		ImVec4 dot = sighting.tracked ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
		ImGui::TextColored(dot, "%s", sighting.tracked ? "[o]" : "[x]");
		ImGui::SameLine();
		if (sighting.tracked)
		{
			double age_s = (instance.now() - sighting.time) * 1e-9;
			ImGui::Text("%s %d (%.0fcm): %s, %.1fs ago",
			            _S("Marker"),
			            marker_tracker->configured_marker(),
			            marker_tracker->configured_size() * 100,
			            _S("tracked"),
			            age_s);
		}
		else
		{
			ImGui::Text("%s %d: %s",
			            _S("Marker"),
			            marker_tracker->configured_marker(),
			            marker_tracker->status().c_str());
		}
	}
	else
	{
		ImGui::Text("%s: %s", _S("Marker"), _S("tracking unavailable"));
#ifdef __ANDROID__
		// The tracker only exists when the runtime exposes the spatial
		// extensions. On Quest that means the manifest permissions (fixed) and
		// the USE_SCENE runtime grant; extensions enumerate at instance
		// creation, so a late grant needs an app restart to take effect.
		if (not scene_permission_granted and not fiducial_entries.lock()->empty())
			ImGui::Text("%s", _S("On Quest, grant the Scene permission, then restart the app"));
#endif
	}

	if (fp.calibrated)
	{
		double age_s = (instance.now() - fp.calibrated_at) * 1e-9;
		ImGui::Text("%s %d, %.0fs %s", _S("Aligned to marker"), fp.calibrated_marker, age_s, _S("ago"));
	}

	bool can_calibrate = marker_tracker && marker_tracker->latest().tracked;
	ImGui::BeginDisabled(!can_calibrate);
	if (ImGui::Button(_S("Calibrate to marker")))
		calibrate_to_marker();
	ImGui::EndDisabled();
	if (not can_calibrate)
	{
		ImGui::SameLine();
		ImGui::Text("%s", _S("needs marker in view"));
	}
}
