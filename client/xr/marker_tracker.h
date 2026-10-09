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

#include "utils/handle.h"
#include <cstdint>
#include <map>
#include <openxr/openxr.h>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace xr
{
class instance;
class session;
class system;

// QR-code fiducial tracking via the Khronos spatial entity framework
// (XR_EXT_future + XR_EXT_spatial_entity + XR_EXT_spatial_anchor +
// XR_EXT_spatial_marker_tracking). Quest only; everything else degrades to
// "unsupported" status, never a crash. The runtime reports markerId 0 for QR
// codes, so sightings match on the exact decoded payload string. Poses come
// from the BOUNDED_2D component (the ANCHOR component is not allowed for
// marker entities on this runtime).
//
// Driven from the render thread (one update() per frame): async context
// creation, throttled discovery snapshots for acquisition, then a
// synchronous update snapshot every frame for all latched entities whose
// MARKER + BOUNDED_2D components yield poses in the given base space.
class marker_tracker
{
public:
	struct sighting
	{
		bool tracked = false;
		XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrTime time = 0;
		// True when this sighting carries a pose not previously reported
		// (novel detector output). False on repeats: the runtime's QR
		// detector runs slower than the query rate, so update snapshots
		// often echo the previous pose. Consumers (continuous filter)
		// should only ingest novel samples.
		bool novel = false;
	};

	// All four extensions must be enabled; check supported() first.
	marker_tracker(instance &, session &, system &);
	static bool supported(instance &);

	// (Re)configure for a marker; kicks off async context creation.
	// No-op when already configured for the same size/payload/static, and
	// throttled by the failure backoff (no per-frame re-attempt spam).
	// Payload is required: QR runtimes report markerId 0, so identity is
	// the payload alone. Tag is display-only (UI/logs) and never gates:
	// a rename updates the label live without restarting tracking.
	// is_static maps to optimizeForStaticMarker: toggling it recreates the
	// spatial context (brief tracking hitch). Poses for all known codes
	// refresh every frame via update snapshots (no timers); the detector
	// is slower, so repeats are flagged novel=false.
	void configure(float marker_size_m, std::string marker_payload, std::string tag, bool is_static = true);

	// Advance the async state machine + throttled discovery.
	// Render thread only. predicted_time stamps the discovery snapshot.
	void update(XrSpace world_space, XrTime now, XrTime predicted_time);

	sighting latest() const
	{
		return current;
	}
	const std::string & status() const
	{
		return status_text;
	}
	const std::string & configured_tag() const
	{
		return marker_tag;
	}
	// Display label for UI/logs: the tag, or the payload when untagged.
	const std::string & label() const
	{
		return marker_tag.empty() ? marker_payload : marker_tag;
	}
	float configured_size() const
	{
		return marker_size_m;
	}
	bool configured_static() const
	{
		return marker_static;
	}
	// Every code sighted by the latest snapshot pass (matched entry and
	// unmatched alike), refreshed every frame. Consumers hold their own
	// SLAM state from this; entries vanish when unseen (no hold here).
	struct code_sighting
	{
		std::string payload;
		XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrExtent2Df extents{0, 0};
		XrSpatialEntityIdEXT entity_id = XR_NULL_SPATIAL_ENTITY_ID_EXT;
		XrTime time = 0;
		bool matched = false;
		bool novel = false;
	};
	const std::vector<code_sighting> & sightings() const
	{
		return all_sightings;
	}
	// (novel, total) update-snapshot counts for dup% diagnostics.
	std::pair<uint64_t, uint64_t> update_stats() const
	{
		return {update_queries_novel, update_queries_total};
	}

private:
	instance * inst = nullptr;
	session * sess = nullptr;
	XrSystemId system_id = XR_NULL_SYSTEM_ID;

	PFN_xrEnumerateSpatialCapabilitiesEXT xrEnumerateSpatialCapabilitiesEXT{};
	PFN_xrEnumerateSpatialCapabilityComponentTypesEXT xrEnumerateSpatialCapabilityComponentTypesEXT{};
	PFN_xrPollFutureEXT xrPollFutureEXT{};
	PFN_xrCreateSpatialContextAsyncEXT xrCreateSpatialContextAsyncEXT{};
	PFN_xrCreateSpatialContextCompleteEXT xrCreateSpatialContextCompleteEXT{};
	PFN_xrCreateSpatialDiscoverySnapshotAsyncEXT xrCreateSpatialDiscoverySnapshotAsyncEXT{};
	PFN_xrCreateSpatialDiscoverySnapshotCompleteEXT xrCreateSpatialDiscoverySnapshotCompleteEXT{};
	PFN_xrCreateSpatialUpdateSnapshotEXT xrCreateSpatialUpdateSnapshotEXT{};
	PFN_xrCreateSpatialEntityFromIdEXT xrCreateSpatialEntityFromIdEXT{};
	PFN_xrDestroySpatialEntityEXT xrDestroySpatialEntityEXT{};
	PFN_xrQuerySpatialComponentDataEXT xrQuerySpatialComponentDataEXT{};
	PFN_xrGetSpatialBufferStringEXT xrGetSpatialBufferStringEXT{};
	PFN_xrGetSpatialBufferUint8EXT xrGetSpatialBufferUint8EXT{};

	using spatial_context_handle = utils::handle<XrSpatialContextEXT>;
	using spatial_snapshot_handle = utils::handle<XrSpatialSnapshotEXT>;
	using spatial_entity_handle = utils::handle<XrSpatialEntityEXT>;

	// Deleters are loaded in the constructor (extension procs)
	PFN_xrDestroySpatialContextEXT xrDestroySpatialContextEXT{};
	PFN_xrDestroySpatialSnapshotEXT xrDestroySpatialSnapshotEXT{};

	enum class state
	{
		idle,
		creating_context,
		ready,
		failed,
	};
	state current_state = state::idle;

	// Display-only label from the server map (never used for matching)
	std::string marker_tag;
	float marker_size_m = 0;
	std::string marker_payload; // exact QR payload to match (required)
	bool marker_static = true;
	// Unconfigured payloads already reported (capped: diagnostic only)
	std::set<std::string> unknown_payloads_logged;
	// Snapshot content diagnostics: count transitions log at info (silence
	// otherwise means zero markers), unreadable-payload state likewise.
	// No gating: the first matching payload sights immediately.
	uint32_t last_entity_count = UINT32_MAX;
	bool last_unreadable = false;
	// One-shot per-entry dump (capability/id/buffer/extents/pos) on the
	// first non-empty snapshot per configure: settles husk-vs-real.
	bool entries_dumped = false;
	// First-seen payload buffer type, logged once per configure so the
	// runtime's encoding is a logged fact rather than another blind round.
	bool buffer_type_logged = false;
	// BOUNDED_2D allowed for the QR capability: the pose source. Without it
	// marker entities carry no pose on this runtime.
	bool bounded_pose = false;
	bool bounded_warned = false;
	XrFutureEXT context_future = XR_NULL_FUTURE_EXT;
	XrFutureEXT discovery_future = XR_NULL_FUTURE_EXT;
	// Constructed with the destroy proc in the constructor init list
	spatial_context_handle spatial_context;

	XrTime last_discovery_start = 0;
	XrTime retry_at = 0;
	XrTime last_now = 0;
	bool discovery_failed_once = false;
	bool discovery_running_logged = false;
	bool update_unsupported_logged = false;

	// Live entities latched from discovery (update snapshots need handles,
	// not ids), one per sighted code so unmatched codes refresh at the
	// same frame cadence. Recreated on discovery matches; cleared when the
	// runtime reports an id invalid. Discovery re-latches within a beat.
	std::map<XrSpatialEntityIdEXT, spatial_entity_handle> spatial_entities;

	// All codes from the latest snapshot pass (see sightings()). Rebuilt
	// every discovery/update; render thread only.
	std::vector<code_sighting> all_sightings;

	// Update-snapshot duplicate accounting: the detector is slower than
	// the frame rate, so most snapshots echo the previous pose. Novel
	// samples feed the continuous filter; repeats are counted and logged
	// as a percentage. Counted per entity queried.
	uint64_t update_queries_total = 0;
	uint64_t update_queries_novel = 0;
	int last_dup_pct = -1;
	XrTime last_dup_log = 0;
	// Last reported pose per entity for echo detection (exact float
	// compare: the runtime repeats values bit-identically on echo).
	std::map<XrSpatialEntityIdEXT, XrPosef> last_poses;

	sighting current;
	std::string status_text = "idle";

	// Query scratch storage (grown on demand, reused across snapshots).
	// bounds[i] parallels marker_data[i]; both parallel entity_ids[i].
	// Entity outputs are the source of truth: per-list counts are
	// capacities the runtime may leave untouched, so marker slots are
	// sentinel-filled (0xFFFFFFFF) to prove what was actually written.
	std::vector<XrSpatialEntityIdEXT> entity_ids;
	std::vector<XrSpatialEntityTrackingStateEXT> entity_states;
	std::vector<XrSpatialMarkerDataEXT> marker_data;
	std::vector<XrSpatialBounded2DDataEXT> bound_boxes;

	static constexpr XrDuration discovery_period_ns = 500'000'000; // 2 Hz
	static constexpr XrDuration retry_delay_ns = 2'000'000'000;

	void fail(const std::string & reason);
	bool poll_ready(XrFutureEXT future, bool & ready);
	std::optional<std::string> read_payload(XrSpatialSnapshotEXT snapshot, XrSpatialBufferEXT buffer);
	void complete_context();
	void start_discovery();
	void complete_discovery(XrSpace world_space, XrTime predicted_time);
	// Synchronous live pose refresh for all latched entities, every frame.
	// No re-enumeration, no timers: novelty is reported per entity via
	// code_sighting::novel so consumers overlay exactly on new data.
	void update_snapshot(XrSpace world_space, XrTime now, XrTime predicted_time);
	void ensure_entity(XrSpatialEntityIdEXT id);
	// Echo check for one entity pose; records current pose on novel.
	bool note_pose(XrSpatialEntityIdEXT id, const XrPosef & pose);
	void log_dup_stats(XrTime now);
};
} // namespace xr
