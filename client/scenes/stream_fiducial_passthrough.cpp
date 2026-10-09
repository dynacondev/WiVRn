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
// Filter tuning carried through from the fiducial (resolver + smoothing
// move are future work; the ops stay at the instance filter for now).
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
} // namespace

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
		for (const auto & f: map.fiducials)
		{
			if (f.markers.size() > 1)
				spdlog::warn("Fiducial \"{}\": multi-marker resolve not implemented, using first of {} markers",
				             f.id, (unsigned)f.markers.size());
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
	for (auto & [payload, tr]: fiducial_trackers)
	{
		(void)payload;
		tr.update(world_space, now, predicted_display_time);
	}

	// Resolver (single-marker): solved fiducial frame per (fiducial,
	// entity) = observed * marker offset. Rebuilt every frame.
	fiducial_sightings.clear();
	for (const auto & f: map.fiducials)
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
			fiducial_sightings.clear();
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

	// ---- behavior objects: instances per (object, fiducial, entity) ----
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
		// Instances: every referenced fiducial, every live sighting. Same
		// QR seen twice places twice; several fiducials place per pairing.
		for (const auto & fid: def.fiducial)
		{
			auto fdef = std::ranges::find(map.fiducials, fid, &to_headset::fiducial_entry::id);
			if (fdef == map.fiducials.end())
				continue;
			xr::fiducial_filter::tuning t = tuning_for(*fdef);
			for (const auto & [skey, fs]: fiducial_sightings)
			{
				if (fs.fiducial_id != fid)
					continue;
				auto [iit, fresh] = ost.instances.try_emplace(skey);
				auto & inst = iit->second;
				if (fresh)
					inst.filter.configure(t);
				XrPosef target = compose_pose(fs.solved, def.position, def.orientation);
				glm::quat tq(target.orientation.w, target.orientation.x, target.orientation.y, target.orientation.z);
				glm::vec3 tp(target.position.x, target.position.y, target.position.z);
				inst.filter.ingest(tp, tq, fs.time);
				inst.samples_ingested++;
				inst.last_seen = now;
				if (not inst.anchored)
				{
					// Auto-anchor: the single allowed snap (only behavior).
					inst.filter.snap();
					auto r = inst.filter.rendered();
					inst.world_pose = {
					        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
					        .position = {r.pos.x, r.pos.y, r.pos.z},
					};
					inst.world_scale = {def.scale, def.scale, def.scale};
					inst.anchored = true;
					inst.anchored_at = now;
					inst.last_predicted = predicted_display_time;
					inst.fade_start = predicted_display_time;
					spdlog::info("Passthrough object \"{}\" auto-anchored to fiducial \"{}\"", def.id, fid);
				}
			}
		}
		// Follow every anchored instance; prune ids unseen for 60s (the
		// runtime recycles long-gone entity ids: without expiry a return
		// would leave a frozen ghost beside the fresh instance).
		for (auto iit = ost.instances.begin(); iit != ost.instances.end();)
		{
			auto & inst = iit->second;
			if (now - inst.last_seen > 60'000'000'000LL)
			{
				iit = ost.instances.erase(iit);
				continue;
			}
			++iit;
			if (not inst.anchored or not inst.filter.has_target())
				continue;
			double dt = inst.last_predicted > 0 ? (predicted_display_time - inst.last_predicted) * 1e-9 : 1.0 / 72.0;
			inst.last_predicted = predicted_display_time;
			auto r = inst.filter.advance(std::clamp(dt, 0.0, 0.25));
			inst.world_pose = {
			        .orientation = {r.quat.x, r.quat.y, r.quat.z, r.quat.w},
			        .position = {r.pos.x, r.pos.y, r.pos.z},
			};
			inst.world_scale = {def.scale, def.scale, def.scale};
			if (auto target = inst.filter.resolve())
			{
				inst.target_render_err_mm = glm::length(target->pos - r.pos) * 1000.f;
				float c = std::clamp(std::abs(glm::dot(target->quat, r.quat)), 0.f, 1.f);
				inst.target_render_err_deg = 2.f * std::acos(c) * 57.29577951308232f;
			}
		}
	}

	// ---- render shim: first live instance drives the single render path
	// (per-feather groups replace this) ----
	fp.calibrated = false;
	bool shimmed = false;
	const passthrough_object_state * shim_ost = nullptr;
	for (auto & [oid, ost]: passthrough_objects)
	{
		(void)oid;
		if (ost.def.type != "3d-passthrough" or not ost.soup_ready)
			continue;
		for (auto & [skey, inst]: ost.instances)
		{
			(void)skey;
			if (not inst.anchored)
				continue;
			fp.calibrated = true;
			fp.world_pose = inst.world_pose;
			fp.world_scale = inst.world_scale;
			fp.status = ost.status;
			shim_ost = &ost;
			shimmed = true;
			break;
		}
		if (shimmed)
			break;
	}

	// Strict gating: tracking above runs unconditionally (status UI and
	// debug overlays), but nothing mesh-related submits until an instance
	// auto-anchors.
	if (not fp.calibrated)
		return;

	// Mask groups record in render() (per-feather stacks); nothing to
	// upload here. The binary fallback below keeps the first live
	// instance only: the projected layer type has no per-object layers.

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
		// guarantees fp.calibrated here). First instance only: the binary
		// fallback has no per-object layers.
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
	auto map = fiducial_map.lock();

	if (map->fiducials.empty() and map->objects.empty())
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
	for (const auto & [oid, ost]: passthrough_objects)
	{
		(void)oid;
		std::string label = ost.def.tag.empty() ? ost.def.id : ost.def.tag;
		size_t anchored = 0;
		for (const auto & [key, inst]: ost.instances)
		{
			(void)key;
			anchored += inst.anchored;
		}
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
		ImGui::Text("%s %.40s: %zu tris, %zu %s", _S("Object"), label.c_str(), ost.triangle_count,
		            anchored, anchored == 1 ? _S("instance") : _S("instances"));
		size_t shown = 0;
		for (const auto & [key, inst]: ost.instances)
		{
			if (shown >= 8)
			{
				ImGui::Text("  ... +%zu", ost.instances.size() - shown);
				break;
			}
			++shown;
			double age_s = (instance.now() - inst.anchored_at) * 1e-9;
			ImGui::Text("  %s %.1fs %s, err %.1fmm %.2fdeg, %llu %s",
			            _S("placed"), age_s, _S("ago"),
			            (double)inst.target_render_err_mm, (double)inst.target_render_err_deg,
			            (unsigned long long)inst.samples_ingested, _S("samples"));
			(void)key;
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
