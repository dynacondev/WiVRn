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

#include <spdlog/spdlog.h>

bool scenes::stream::fiducial_passthrough_wanted()
{
	if (not instance.has_extension(XR_FB_TRIANGLE_MESH_EXTENSION_NAME))
		return false;

	std::error_code ec;
	auto entries = fiducial_entries.lock();
	for (const auto & entry: *entries)
	{
		if (entry.model_hash.empty())
			continue;
		if (std::filesystem::exists(fiducial_model_path(entry.model_hash), ec))
			return true;
	}
	return false;
}

void scenes::stream::update_fiducial_passthrough(XrTime predicted_display_time)
{
	XrSpace world_space = application::space(xr::spaces::world);
	auto & fp = fiducial_passthrough;

	// Fingerprint of the inputs: reset one-shot state when the map or the
	// cache contents change (new model pushed, download finished, ...)
	std::string key;
	std::optional<to_headset::fiducial_map_entry> entry;
	bool model_cached = false;
	{
		auto entries = fiducial_entries.lock();
		for (const auto & e: *entries)
		{
			key += e.model_hash;
			key += ';';
			if (not entry and not e.model_hash.empty())
				entry = e;
		}
	}
	if (entry)
	{
		std::error_code ec;
		model_cached = std::filesystem::exists(fiducial_model_path(entry->model_hash), ec);
		key += model_cached ? 'C' : 'D';
	}

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
		if (fp.ready)
		{
			session.clear_projected_passthrough_mesh();
			fp.ready = false;
		}
		if (not entry)
			fp.status = "no fiducial map from server";
		else if (not model_cached)
			fp.status = "downloading model " + entry->model_hash.substr(0, 8) + "...";
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

		// Phase 2: config pose is the world-space placement
		XrPosef pose{
		        .orientation = {entry->orientation[0], entry->orientation[1], entry->orientation[2], entry->orientation[3]},
		        .position = {entry->position[0], entry->position[1], entry->position[2]},
		};
		XrVector3f scale{entry->scale, entry->scale, entry->scale};

		session.set_projected_passthrough_mesh(soup.vertices, soup.indices, world_space, pose, scale);

		fp.ready = true;
		fp.model_hash = entry->model_hash;
		fp.world_pose = pose;
		fp.world_scale = scale;
		fp.vertex_count = soup.vertices.size();
		fp.triangle_count = soup.indices.size() / 3;
		fp.status = "projected";
		spdlog::info("Fiducial passthrough mesh live: {} vertices, {} triangles", fp.vertex_count, fp.triangle_count);

		session.update_projected_passthrough_transform(world_space, predicted_display_time, pose, scale);
		add_projected_passthrough_layer();
	}
	catch (std::exception & e)
	{
		fp.status = std::string("mesh error: ") + e.what();
		spdlog::warn("Fiducial passthrough mesh failed: {}", e.what());
	}
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
}
