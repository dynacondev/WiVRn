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
#include <openxr/openxr.h>
#include <optional>
#include <set>
#include <string>
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
// creation, then throttled discovery snapshots whose MARKER + BOUNDED_2D
// components yield the marker pose in the given base space.
class marker_tracker
{
public:
	struct sighting
	{
		bool tracked = false;
		XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
		XrTime time = 0;
	};

	// All four extensions must be enabled; check supported() first.
	marker_tracker(instance &, session &, system &);
	static bool supported(instance &);

	// (Re)configure for a marker; kicks off async context creation.
	// No-op when already configured for the same marker/size/payload, and
	// throttled by the failure backoff (no per-frame re-attempt spam).
	// Payload is required: QR runtimes report markerId 0.
	void configure(int32_t marker_id, float marker_size_m, std::string marker_payload);

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
	int32_t configured_marker() const
	{
		return marker_id;
	}
	float configured_size() const
	{
		return marker_size_m;
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
	PFN_xrQuerySpatialComponentDataEXT xrQuerySpatialComponentDataEXT{};
	PFN_xrGetSpatialBufferStringEXT xrGetSpatialBufferStringEXT{};
	PFN_xrGetSpatialBufferUint8EXT xrGetSpatialBufferUint8EXT{};

	using spatial_context_handle = utils::handle<XrSpatialContextEXT>;
	using spatial_snapshot_handle = utils::handle<XrSpatialSnapshotEXT>;

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

	int32_t marker_id = -1;
	float marker_size_m = 0;
	std::string marker_payload; // exact QR payload to match (required)
	// Unconfigured payloads already reported (capped: diagnostic only)
	std::set<std::string> unknown_payloads_logged;
	// Snapshot content diagnostics: count transitions log at info (silence
	// otherwise means zero markers), unreadable-payload state likewise.
	// No gating: the first matching payload sights immediately.
	uint32_t last_marker_count = UINT32_MAX;
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

	sighting current;
	std::string status_text = "idle";

	// Query scratch storage (grown on demand, reused across snapshots).
	// bounds[i] parallels marker_data[i] when BOUNDED_2D is queried.
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
};
} // namespace xr
