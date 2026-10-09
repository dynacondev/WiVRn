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
#include <cmath>
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
			// Identity is payload + size; the tag is display-only and
			// deliberately excluded so a rename doesn't wipe calibration.
			map_key += e.marker_data;
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
			// Mode + live tuning ride the same fingerprint: any of them
			// changing re-seeds the filter/mesh like any other map change.
			map_key += e.mode;
			map_key += ';';
			map_key += e.is_static ? 'S' : 'M';
			map_key += ';';
			map_key += std::to_string(e.window_size) + ',' +
			        std::to_string(e.min_samples) + ',' + std::to_string(e.sigma_k) + ',' +
			        std::to_string(e.pos_gain) + ',' + std::to_string(e.rot_gain) + ',' +
			        std::to_string(e.euro_min_cutoff) + ',' + std::to_string(e.euro_beta) + ',' +
			        std::to_string(e.knee_inner_mm) + ',' + std::to_string(e.knee_outer_mm) + ',' +
			        std::to_string(e.knee_inner_deg) + ',' + std::to_string(e.knee_outer_deg) + ';';
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
			marker_tracker->configure(entry->marker_size_m, entry->marker_data, entry->tag, entry->is_static);
			marker_tracker->update(world_space, instance.now(), predicted_display_time);
			// Debug hold-store: every sighted code at its raw instant
			// pose; entries freeze (SLAM hold) when unseen. Runs even
			// before calibration so raw positions are always visible.
			for (const auto & s: marker_tracker->sightings())
			{
				auto & h = fp.held_codes[s.entity_id];
				h.payload = s.payload.substr(0, 64);
				h.pose = s.pose;
				h.extents = s.extents;
				h.last_seen = s.time;
				h.matched = s.matched;
			}
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
			fp.calibrated_tag.clear();
			fp.filter.reset();
			fp.continuous = false;
			fp.novel_ingested = 0;
			fp.fade_start = 0;
			fp.held_codes.clear();
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

	// Fade duration is render-only (like feather-px): tracked live, never
	// part of the calibration fingerprint above.
	if (entry)
		fp.fade_dur_ms = std::max(0.f, entry->fade_in_ms);

	// Strict gating: marker tracking above runs unconditionally (needed for
	// the in-view dot and the Calibrate button), but nothing mesh-related
	// happens until calibrated. Continuous mode self-seeds that flag on
	// first sighting (auto-anchor); one-shot waits for the button.
	if (entry and entry->mode == "continuous" and marker_tracker)
	{
		fp.continuous = true;
		xr::fiducial_filter::tuning t;
		t.window_size = std::max(2, entry->window_size);
		t.min_samples = std::clamp(entry->min_samples, 1, t.window_size);
		t.sigma_k = std::max(0.5f, entry->sigma_k);
		t.pos_gain = std::max(0.1f, entry->pos_gain);
		t.rot_gain = std::max(0.1f, entry->rot_gain);
		t.euro_min_cutoff = std::max(0.05f, entry->euro_min_cutoff);
		t.euro_beta = std::max(0.f, entry->euro_beta);
		t.knee_inner_mm = entry->knee_inner_mm;
		t.knee_outer_mm = std::max(t.knee_inner_mm, entry->knee_outer_mm);
		t.knee_inner_deg = entry->knee_inner_deg;
		t.knee_outer_deg = std::max(t.knee_inner_deg, entry->knee_outer_deg);
		// Tuning rides the map fingerprint above, so a tuning change lands
		// here with a fresh filter: apply once per map (not per key — the
		// key's cache flag flips when a download finishes, which must not
		// wipe a live filter). configure() resets the window.
		if (map_key != fp.applied_tuning_key)
		{
			fp.applied_tuning_key = map_key;
			fp.filter.configure(t);
		}
		if (not fp.calibrated)
		{
			// Not seeded yet: the mesh transform below needs a pose, but
			// nothing is submitted until calibrated, so just keep feeding
			// the filter (auto-anchor happens on novel sightings below).
			fp.world_scale = {entry->scale, entry->scale, entry->scale};
		}
		auto sighting = marker_tracker->latest();
		if (sighting.tracked and sighting.novel)
		{
			// Same mesh-target math as calibrate_to_marker(), but as a
			// filter sample instead of a snap: meshTarget =
			// observedMarker * offset (translation unaffected by scale).
			glm::quat marker_quat(sighting.pose.orientation.w, sighting.pose.orientation.x, sighting.pose.orientation.y, sighting.pose.orientation.z);
			glm::vec3 marker_pos(sighting.pose.position.x, sighting.pose.position.y, sighting.pose.position.z);
			glm::quat offset_quat(entry->orientation[3], entry->orientation[0], entry->orientation[1], entry->orientation[2]);
			glm::vec3 offset_pos(entry->position[0], entry->position[1], entry->position[2]);
			glm::quat mesh_quat = marker_quat * offset_quat;
			glm::vec3 mesh_pos = marker_pos + marker_quat * offset_pos;
			fp.filter.ingest(mesh_pos, mesh_quat, sighting.time);
			fp.novel_ingested++;
			fp.last_novel_at = instance.now();
			if (not fp.calibrated)
			{
				// Auto-anchor: the single allowed snap in continuous mode.
				fp.filter.snap();
				auto r = fp.filter.rendered();
				fp.world_pose = {
				        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
				        .position = {r.pos.x, r.pos.y, r.pos.z},
				};
				fp.world_scale = {entry->scale, entry->scale, entry->scale};
				fp.calibrated = true;
				fp.calibrated_at = fp.last_novel_at;
				fp.calibrated_tag = entry->tag.empty() ? entry->marker_data : entry->tag;
				fp.last_predicted = predicted_display_time;
				fp.fade_start = predicted_display_time;
				spdlog::info("Continuous: auto-anchored to marker \"{}\"", fp.calibrated_tag);
			}
		}
		if (fp.calibrated and fp.filter.has_target())
		{
			// Per-frame proportional follow (large error = large step).
			// On marker loss no novel samples arrive: advance() coasts on
			// the current window, i.e. permanent SLAM hold.
			double dt = fp.last_predicted > 0 ? (predicted_display_time - fp.last_predicted) * 1e-9 : 1.0 / 72.0;
			fp.last_predicted = predicted_display_time;
			auto r = fp.filter.advance(std::clamp(dt, 0.0, 0.25));
			fp.world_pose = {
			        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
			        .position = {r.pos.x, r.pos.y, r.pos.z},
			};
			fp.world_scale = {entry->scale, entry->scale, entry->scale};
			if (auto target = fp.filter.resolve())
			{
				fp.target_render_err_mm = glm::length(target->pos - r.pos) * 1000.f;
				float c = std::clamp(std::abs(glm::dot(target->quat, r.quat)), 0.f, 1.f);
				fp.target_render_err_deg = 2.f * std::acos(c) * 57.29577951308232f;
			}
		}
	}
	else if (entry)
	{
		fp.continuous = entry->mode == "continuous";
	}

	if (not fp.calibrated)
		return;

	// Feathered mask-blend path (replaces the binary triangle-mesh cutout
	// when the alpha-blend extension is present): no runtime mesh upload,
	// no geometry-instance transform. The soup + raster resources are
	// (re)built here; raster record and layer submit happen in render().
	if (composition_layer_alpha_blend_supported)
	{
		if (entry and model_cached)
		{
			// Live feather value: changing it needs no recalibration.
			fp.feather_px = entry->feather_px;
			fp.model_hash = entry->model_hash;
			if (fp.mask_hash != entry->model_hash)
			{
				fp.mask_hash = entry->model_hash;
				fp.mask_ready = false;
				fp.mask_active = false;
				try
				{
					fp.mask_soup = passthrough_mesh::flatten_gltf(fiducial_model_path(entry->model_hash));
					if (not fp.mask_renderer)
						fp.mask_renderer = std::make_unique<feather_mask_renderer>(device, physical_device, swapchain_format);
					fp.mask_renderer->set_soup(fp.mask_soup);
					fp.mask_ready = fp.mask_renderer->has_mesh();
					fp.status = "feathered";
					spdlog::info("Fiducial mask mesh ready: {} triangles, feather {}px",
					             fp.mask_soup.indices.size() / 3, entry->feather_px);
				}
				catch (std::exception & e)
				{
					fp.status = std::string("mask error: ") + e.what();
					spdlog::warn("Fiducial mask mesh failed: {}", e.what());
				}
			}
			fp.mask_active = fp.mask_ready;
			if (fp.mask_active)
				return;
			// Else fall through to the binary mesh path below (mask
			// unavailable: no soup, no renderer, or no model).
		}
		else
		{
			fp.mask_active = false;
		}
	}

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

	if (entry->mode == "continuous")
	{
		// Re-seed: the single allowed snap in continuous mode. Clears the
		// window, ingests the current sighting at full weight, and snaps
		// the rendered pose to it; follow resumes from there.
		glm::quat marker_quat(sighting.pose.orientation.w, sighting.pose.orientation.x, sighting.pose.orientation.y, sighting.pose.orientation.z);
		glm::vec3 marker_pos(sighting.pose.position.x, sighting.pose.position.y, sighting.pose.position.z);
		glm::quat offset_quat(entry->orientation[3], entry->orientation[0], entry->orientation[1], entry->orientation[2]);
		glm::vec3 offset_pos(entry->position[0], entry->position[1], entry->position[2]);
		fp.filter.reset();
		fp.filter.ingest(marker_pos + marker_quat * offset_pos, marker_quat * offset_quat, sighting.time);
		fp.filter.snap();
		auto r = fp.filter.rendered();
		fp.world_pose = {
		        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
		        .position = {r.pos.x, r.pos.y, r.pos.z},
		};
		fp.world_scale = {entry->scale, entry->scale, entry->scale};
		fp.calibrated = true;
		fp.calibrated_at = now;
		fp.calibrated_tag = entry->tag.empty() ? entry->marker_data : entry->tag;
		fp.fade_start = now;
		spdlog::info("Continuous: re-seeded to marker \"{}\"", fp.calibrated_tag);
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
	// Display label: the tag, or the payload when untagged.
	fp.calibrated_tag = entry->tag.empty() ? entry->marker_data : entry->tag;
	fp.fade_dur_ms = std::max(0.f, entry->fade_in_ms);
	fp.fade_start = now;

	// Deliberately no world-origin change: shifting the client origin moves
	// it out from under the server-rendered video (the game is rendered
	// against the session-start origin), displacing the video quad. The
	// mesh is placed purely as an object in the stable SLAM frame, which
	// the headset holds drift-free. Virtual-world moves belong server-side
	// (see ROADMAP.md future phase), never as a client origin shift.

	spdlog::info("Calibrated to marker \"{}\": observed at ({:.2f}, {:.2f}, {:.2f}), mesh at ({:.2f}, {:.2f}, {:.2f}), sighting {}ms old",
	             fp.calibrated_tag, marker_pos.x, marker_pos.y, marker_pos.z, mesh_pos.x, mesh_pos.y, mesh_pos.z,
	             (long long)((now - sighting.time) / 1'000'000));
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
	ImGui::BeginDisabled(true);
	ImGui::Text("%s", _S("Multi-code corrected position (orange) — coming later"));
	ImGui::EndDisabled();
	if (not fp.held_codes.empty())
	{
		ImGui::Text("%s (%zu):", _S("Codes in view"), fp.held_codes.size());
		ImGui::BeginChild("debug_codes", {0, 140});
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
	auto & fp = fiducial_passthrough;
	if (fp.ready)
		ImGui::Text("%s: %zu tris (%s)", _S("Passthrough mesh"), fp.triangle_count, fp.model_hash.substr(0, 8).c_str());
	else
		ImGui::Text("%s: %s", _S("Passthrough mesh"), fp.status.c_str());

	if (marker_tracker)
	{
		auto sighting = marker_tracker->latest();
		auto [novel, total] = marker_tracker->update_stats();
		int dup_pct = total > 0 ? (int)((total - novel) * 100 / total) : 0;
		ImVec4 dot = sighting.tracked ? ImVec4{0.2f, 0.9f, 0.3f, 1.0f} : ImVec4{0.9f, 0.25f, 0.2f, 1.0f};
		ImGui::TextColored(dot, "%s", sighting.tracked ? "[o]" : "[x]");
		ImGui::SameLine();
		if (sighting.tracked)
		{
			double age_s = (instance.now() - sighting.time) * 1e-9;
			ImGui::Text("%s %.40s (%.0fcm): %s, %.1fs ago%s%s",
			            _S("Marker"),
			            marker_tracker->label().c_str(),
			            marker_tracker->configured_size() * 100,
			            _S("tracked"),
			            age_s,
			            marker_tracker->configured_static() ? "" : " (moving)",
			            total > 0 ? (" upd " + std::to_string((int)novel) + "/" + std::to_string((int)total) + " " + std::to_string(dup_pct) + "%dup").c_str() : "");
		}
		else
		{
			ImGui::Text("%s %.40s: %s",
			            _S("Marker"),
			            marker_tracker->label().c_str(),
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
		ImGui::Text("%s %.40s, %.0fs %s%s",
		            _S("Aligned to marker"),
		            fp.calibrated_tag.c_str(),
		            age_s,
		            _S("ago"),
		            fp.continuous ? " (continuous)" : "");
		if (fp.continuous)
			ImGui::Text("Follow: %zu samples, w=%.2f, err %.1fmm %.2fdeg, %llu novel",
			            fp.filter.sample_count(),
			            fp.filter.mean_weight(),
			            fp.target_render_err_mm,
			            fp.target_render_err_deg,
			            (unsigned long long)fp.novel_ingested);
		if (float fade = fp.fade_factor(instance.now()); fade < 1)
			ImGui::Text("Fade-in: %.0f%%", fade * 100);
	}

	bool can_calibrate = marker_tracker && marker_tracker->latest().tracked;
	ImGui::BeginDisabled(!can_calibrate);
	if (ImGui::Button(_S(fp.continuous ? "Re-seed to marker" : "Calibrate to marker")))
		calibrate_to_marker();
	ImGui::EndDisabled();
	if (not can_calibrate)
	{
		ImGui::SameLine();
		ImGui::Text("%s", _S("needs marker in view"));
	}
}
