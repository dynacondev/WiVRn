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

#include "marker_tracker.h"
#include "session.h"
#include "xr/instance.h"
#include "xr/system.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace
{
// Discovery/query component sets. Shared by start_discovery() and
// complete_discovery() so the snapshot can never ask for a component the
// context doesn't enable (the runtime fails the start for that).
const XrSpatialComponentTypeEXT marker_only[] = {
        XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT,
};
const XrSpatialComponentTypeEXT marker_and_bounds[] = {
        XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT,
        XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT,
};
} // namespace

bool xr::marker_tracker::supported(instance & inst)
{
	return inst.has_extension(XR_EXT_FUTURE_EXTENSION_NAME) and
	       inst.has_extension(XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME) and
	       inst.has_extension(XR_EXT_SPATIAL_ANCHOR_EXTENSION_NAME) and
	       inst.has_extension(XR_EXT_SPATIAL_MARKER_TRACKING_EXTENSION_NAME);
}

xr::marker_tracker::marker_tracker(instance & inst_, session & sess_, system & sys) :
        inst(&inst_),
        sess(&sess_),
        system_id(sys),
        xrEnumerateSpatialCapabilitiesEXT(inst_.get_proc<PFN_xrEnumerateSpatialCapabilitiesEXT>("xrEnumerateSpatialCapabilitiesEXT")),
        xrEnumerateSpatialCapabilityComponentTypesEXT(inst_.get_proc<PFN_xrEnumerateSpatialCapabilityComponentTypesEXT>("xrEnumerateSpatialCapabilityComponentTypesEXT")),
        xrPollFutureEXT(inst_.get_proc<PFN_xrPollFutureEXT>("xrPollFutureEXT")),
        xrCreateSpatialContextAsyncEXT(inst_.get_proc<PFN_xrCreateSpatialContextAsyncEXT>("xrCreateSpatialContextAsyncEXT")),
        xrCreateSpatialContextCompleteEXT(inst_.get_proc<PFN_xrCreateSpatialContextCompleteEXT>("xrCreateSpatialContextCompleteEXT")),
        xrCreateSpatialDiscoverySnapshotAsyncEXT(inst_.get_proc<PFN_xrCreateSpatialDiscoverySnapshotAsyncEXT>("xrCreateSpatialDiscoverySnapshotAsyncEXT")),
        xrCreateSpatialDiscoverySnapshotCompleteEXT(inst_.get_proc<PFN_xrCreateSpatialDiscoverySnapshotCompleteEXT>("xrCreateSpatialDiscoverySnapshotCompleteEXT")),
        xrCreateSpatialUpdateSnapshotEXT(inst_.get_proc<PFN_xrCreateSpatialUpdateSnapshotEXT>("xrCreateSpatialUpdateSnapshotEXT")),
        xrCreateSpatialEntityFromIdEXT(inst_.get_proc<PFN_xrCreateSpatialEntityFromIdEXT>("xrCreateSpatialEntityFromIdEXT")),
        xrDestroySpatialEntityEXT(inst_.get_proc<PFN_xrDestroySpatialEntityEXT>("xrDestroySpatialEntityEXT")),
        xrQuerySpatialComponentDataEXT(inst_.get_proc<PFN_xrQuerySpatialComponentDataEXT>("xrQuerySpatialComponentDataEXT")),
        xrGetSpatialBufferStringEXT(inst_.get_proc<PFN_xrGetSpatialBufferStringEXT>("xrGetSpatialBufferStringEXT")),
        xrGetSpatialBufferUint8EXT(inst_.get_proc<PFN_xrGetSpatialBufferUint8EXT>("xrGetSpatialBufferUint8EXT")),
        xrDestroySpatialContextEXT(inst_.get_proc<PFN_xrDestroySpatialContextEXT>("xrDestroySpatialContextEXT")),
        xrDestroySpatialSnapshotEXT(inst_.get_proc<PFN_xrDestroySpatialSnapshotEXT>("xrDestroySpatialSnapshotEXT")),
        spatial_context{xrDestroySpatialContextEXT}
{
}

void xr::marker_tracker::fail(const std::string & reason)
{
	current_state = state::failed;
	status_text = reason;
	retry_at = last_now + retry_delay_ns;
	spdlog::warn("marker_tracker: {}", reason);
}

bool xr::marker_tracker::poll_ready(XrFutureEXT future, bool & ready)
{
	ready = false;
	XrFuturePollInfoEXT poll_info{
	        .type = XR_TYPE_FUTURE_POLL_INFO_EXT,
	        .future = future,
	};
	XrFuturePollResultEXT poll_result{
	        .type = XR_TYPE_FUTURE_POLL_RESULT_EXT,
	};
	if (XrResult res = xrPollFutureEXT(*inst, &poll_info, &poll_result); res != XR_SUCCESS)
	{
		spdlog::warn("marker_tracker: xrPollFutureEXT failed: {}", (int)res);
		return false;
	}
	ready = poll_result.state == XR_FUTURE_STATE_READY_EXT;
	return true;
}

void xr::marker_tracker::configure(float size_m, std::string payload, std::string tag, bool is_static)
{
	// The tag is display-only: store it before the no-op check so a
	// rename updates the UI label without restarting tracking.
	marker_tag = std::move(tag);
	if (size_m == marker_size_m and payload == marker_payload and is_static == marker_static and
	    current_state != state::failed and current_state != state::idle)
		return;

	// Failure backoff: configure() is called every frame, attempts only on
	// a fresh tracker or once the retry timer elapsed (no per-frame spam).
	if (current_state == state::failed and last_now < retry_at)
		return;

	marker_size_m = size_m;
	marker_payload = std::move(payload);
	marker_static = is_static;
	unknown_payloads_logged.clear();
	bounded_warned = false;
	discovery_running_logged = false;
	update_unsupported_logged = false;
	last_entity_count = UINT32_MAX;
	last_unreadable = false;
	buffer_type_logged = false;
	entries_dumped = false;
	current = sighting{};
	context_future = XR_NULL_FUTURE_EXT;
	discovery_future = XR_NULL_FUTURE_EXT;
	spatial_context = spatial_context_handle{xrDestroySpatialContextEXT};
	// Entity handles die with their context; discovery re-latches within
	// a beat. Rate stats reset per configure.
	spatial_entities.clear();
	all_sightings.clear();
	update_ticks = 0;
	first_tick_at = 0;
	last_rate_log = 0;
	current_state = state::idle;
	status_text = "checking capabilities";

	if (marker_payload.empty())
	{
		fail("fiducial map entry has no QR payload (marker-data)");
		return;
	}
	spdlog::info("marker_tracker: tracking QR code marker \"{}\" ({:.0f}mm) payload \"{}\" ({})",
	             label(), (double)(size_m * 1000), marker_payload.substr(0, 64),
	             marker_static ? "static" : "moving");

	if (marker_size_m <= 0)
	{
		fail("invalid marker size");
		return;
	}

	// Require the QR capability before creating the context
	uint32_t count = 0;
	if (XrResult res = xrEnumerateSpatialCapabilitiesEXT(*inst, system_id, 0, &count, nullptr); res != XR_SUCCESS)
	{
		fail("cannot enumerate spatial capabilities");
		return;
	}
	std::vector<XrSpatialCapabilityEXT> caps(count);
	if (XrResult res = xrEnumerateSpatialCapabilitiesEXT(*inst, system_id, count, &count, caps.data()); res != XR_SUCCESS)
	{
		fail("cannot enumerate spatial capabilities");
		return;
	}
	for (auto c: caps)
		spdlog::info("marker_tracker: spatial capability {}", (int)c);
	if (not std::ranges::contains(caps, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT))
	{
		fail("QR code tracking not advertised by runtime");
		return;
	}

	// Component allowlist for the QR capability: the runtime rejects
	// components it doesn't support per capability (notably ANCHOR is
	// rejected for marker entities), so the context enables exactly MARKER
	// plus whatever pose component is allowed.
	XrSpatialCapabilityComponentTypesEXT comp_types{
	        .type = XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT,
	};
	if (XrResult res = xrEnumerateSpatialCapabilityComponentTypesEXT(*inst, system_id, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, &comp_types);
	    res != XR_SUCCESS)
	{
		fail("cannot enumerate QR capability components");
		return;
	}
	std::vector<XrSpatialComponentTypeEXT> comp_list(comp_types.componentTypeCountOutput);
	comp_types.componentTypeCapacityInput = (uint32_t)comp_list.size();
	comp_types.componentTypes = comp_list.data();
	if (XrResult res = xrEnumerateSpatialCapabilityComponentTypesEXT(*inst, system_id, XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT, &comp_types);
	    res != XR_SUCCESS)
	{
		fail("cannot enumerate QR capability components");
		return;
	}
	for (auto t: comp_list)
		spdlog::info("marker_tracker: QR capability component {}", (int)t);
	bounded_pose = std::ranges::contains(comp_list, XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT);

	// Marker-only context: ANCHOR is rejected for marker entities, so the
	// pose comes from BOUNDED_2D (center is a full 3D pose) when allowlisted.
	std::vector<XrSpatialComponentTypeEXT> components = {XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT};
	if (bounded_pose)
		components.push_back(XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT);
	XrSpatialMarkerSizeEXT size{
	        .type = XR_TYPE_SPATIAL_MARKER_SIZE_EXT,
	        .markerSideLength = marker_size_m,
	};
	// Our fiducial is stationary or moving per config: tell the runtime
	// whether to integrate stationary markers. Toggling "static" recreates
	// the context here (brief tracking hitch, by design).
	XrSpatialMarkerStaticOptimizationEXT static_opt{
	        .type = XR_TYPE_SPATIAL_MARKER_STATIC_OPTIMIZATION_EXT,
	        .next = &size,
	        .optimizeForStaticMarker = static_cast<XrBool32>(marker_static ? XR_TRUE : XR_FALSE),
	};
	XrSpatialCapabilityConfigurationQrCodeEXT qr_code{
	        .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT,
	        .next = &static_opt,
	        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT,
	        .enabledComponentCount = (uint32_t)components.size(),
	        .enabledComponents = components.data(),
	};
	const XrSpatialCapabilityConfigurationBaseHeaderEXT * configs[] = {
	        reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(&qr_code),
	};
	XrSpatialContextCreateInfoEXT create_info{
	        .type = XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT,
	        .capabilityConfigCount = 1,
	        .capabilityConfigs = configs,
	};
	if (XrResult res = xrCreateSpatialContextAsyncEXT(*sess, &create_info, &context_future); res != XR_SUCCESS)
	{
		fail("cannot create spatial context");
		return;
	}

	current_state = state::creating_context;
	status_text = "creating spatial context";
	spdlog::info("marker_tracker: spatial context creating (pose component: {})",
	             bounded_pose ? "BOUNDED_2D" : "none: markers carry no pose");
}

std::optional<std::string> xr::marker_tracker::read_payload(XrSpatialSnapshotEXT snapshot, XrSpatialBufferEXT buffer)
{
	// QR payloads are decoded strings or raw bytes; anything else has no
	// usable identity. The runtime's choice is logged once per configure.
	if (buffer.bufferId == XR_NULL_SPATIAL_BUFFER_ID_EXT)
	{
		spdlog::debug("marker_tracker: marker has null payload buffer");
		return std::nullopt;
	}
	if (not buffer_type_logged)
	{
		buffer_type_logged = true;
		spdlog::info("marker_tracker: marker payload buffer type {}", (int)buffer.bufferType);
	}
	if (buffer.bufferType != XR_SPATIAL_BUFFER_TYPE_STRING_EXT and
	    buffer.bufferType != XR_SPATIAL_BUFFER_TYPE_UINT8_EXT)
	{
		spdlog::debug("marker_tracker: marker payload buffer type {} is not string/uint8", (int)buffer.bufferType);
		return std::nullopt;
	}

	XrSpatialBufferGetInfoEXT info{
	        .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT,
	        .bufferId = buffer.bufferId,
	};
	if (buffer.bufferType == XR_SPATIAL_BUFFER_TYPE_UINT8_EXT)
	{
		uint32_t count = 0;
		if (XrResult res = xrGetSpatialBufferUint8EXT(snapshot, &info, 0, &count, nullptr); res != XR_SUCCESS or count == 0)
		{
			spdlog::debug("marker_tracker: payload size query failed");
			return std::nullopt;
		}
		std::vector<uint8_t> bytes(count + 1, 0);
		if (XrResult res = xrGetSpatialBufferUint8EXT(snapshot, &info, count, &count, bytes.data()); res != XR_SUCCESS)
		{
			spdlog::debug("marker_tracker: payload fetch failed");
			return std::nullopt;
		}
		while (count > 0 and bytes[count - 1] == 0)
			--count;
		return std::string(bytes.begin(), bytes.begin() + count);
	}

	uint32_t count = 0;
	if (XrResult res = xrGetSpatialBufferStringEXT(snapshot, &info, 0, &count, nullptr); res != XR_SUCCESS or count == 0)
	{
		spdlog::debug("marker_tracker: payload size query failed");
		return std::nullopt;
	}

	// +1 and explicit trim: runtimes differ on whether count includes NUL
	std::vector<char> text(count + 1, 0);
	if (XrResult res = xrGetSpatialBufferStringEXT(snapshot, &info, count, &count, text.data()); res != XR_SUCCESS)
	{
		spdlog::debug("marker_tracker: payload fetch failed");
		return std::nullopt;
	}
	while (count > 0 and text[count - 1] == 0)
		--count;
	return std::string(text.data(), count);
}

void xr::marker_tracker::complete_context()
{
	XrCreateSpatialContextCompletionEXT completion{
	        .type = XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT,
	};
	if (XrResult res = xrCreateSpatialContextCompleteEXT(*sess, context_future, &completion); res != XR_SUCCESS or completion.futureResult != XR_SUCCESS)
	{
		context_future = XR_NULL_FUTURE_EXT;
		XrResult cause = res != XR_SUCCESS ? res : completion.futureResult;
		if (cause == XR_ERROR_PERMISSION_INSUFFICIENT)
			fail("spatial permission denied: grant Scene access, then restart the app");
		else
			fail("spatial context creation failed");
		return;
	}
	context_future = XR_NULL_FUTURE_EXT;
	spatial_context = spatial_context_handle(completion.spatialContext, xrDestroySpatialContextEXT);
	current_state = state::ready;
	status_text = "seeking marker";
	last_discovery_start = 0;
	spdlog::info("marker_tracker: spatial context ready");
}

void xr::marker_tracker::start_discovery()
{
	XrSpatialFilterTrackingStateEXT tracking_filter{
	        .type = XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT,
	        .trackingState = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT,
	};
	XrSpatialDiscoverySnapshotCreateInfoEXT create_info{
	        .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT,
	        .next = &tracking_filter,
	        .componentTypeCount = bounded_pose ? 2u : 1u,
	        .componentTypes = bounded_pose ? marker_and_bounds : marker_only,
	};
	if (XrResult res = xrCreateSpatialDiscoverySnapshotAsyncEXT(spatial_context, &create_info, &discovery_future); res != XR_SUCCESS)
	{
		discovery_future = XR_NULL_FUTURE_EXT;
		if (not discovery_failed_once)
		{
			discovery_failed_once = true;
			spdlog::warn("marker_tracker: cannot start discovery snapshot");
		}
		return;
	}
	discovery_failed_once = false;
}

void xr::marker_tracker::complete_discovery(XrSpace world_space, XrTime predicted_time)
{
	XrCreateSpatialDiscoverySnapshotCompletionInfoEXT complete_info{
	        .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT,
	        .baseSpace = world_space,
	        .time = predicted_time,
	        .future = discovery_future,
	};
	XrCreateSpatialDiscoverySnapshotCompletionEXT completion{
	        .type = XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT,
	};
	discovery_future = XR_NULL_FUTURE_EXT;
	if (XrResult res = xrCreateSpatialDiscoverySnapshotCompleteEXT(spatial_context, &complete_info, &completion); res != XR_SUCCESS or completion.futureResult != XR_SUCCESS)
	{
		spdlog::warn("marker_tracker: discovery snapshot failed");
		return;
	}
	spatial_snapshot_handle snapshot(completion.snapshot, xrDestroySpatialSnapshotEXT);

	if (not discovery_running_logged)
	{
		discovery_running_logged = true;
		spdlog::info("marker_tracker: discovery running");
	}

	XrSpatialComponentDataQueryConditionEXT condition{
	        .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT,
	        .componentTypeCount = bounded_pose ? 2u : 1u,
	        .componentTypes = bounded_pose ? marker_and_bounds : marker_only,
	};

	if (not bounded_pose)
	{
		// No pose component was allowlisted: marker entities cannot be
		// located on this runtime. Loud once, quiet status after.
		if (not bounded_warned)
		{
			bounded_warned = true;
			spdlog::warn("marker_tracker: QR capability exposes no pose component, markers cannot be located");
		}
		current.tracked = false;
		status_text = "marker has no pose component";
		return;
	}

	// Query twice at most: grow scratch storage when the snapshot holds more
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (entity_ids.empty())
		{
			entity_ids.assign(8, UINT64_MAX);
			entity_states.resize(8);
			marker_data.resize(8);
			bound_boxes.resize(8);
			for (auto & m: marker_data)
				m.markerId = 0xFFFFFFFFu;
		}
		XrSpatialComponentMarkerListEXT marker_list{
		        .type = XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT,
		        .markerCount = (uint32_t)marker_data.size(),
		        .markers = marker_data.data(),
		};
		XrSpatialComponentBounded2DListEXT bounds_list{
		        .type = XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT,
		        .next = &marker_list,
		        .boundCount = (uint32_t)bound_boxes.size(),
		        .bounds = bound_boxes.data(),
		};
		XrSpatialComponentDataQueryResultEXT result{
		        .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT,
		        .next = &bounds_list,
		        .entityIdCapacityInput = (uint32_t)entity_ids.size(),
		        .entityIds = entity_ids.data(),
		        .entityStateCapacityInput = (uint32_t)entity_states.size(),
		        .entityStates = entity_states.data(),
		};
		if (XrResult res = xrQuerySpatialComponentDataEXT(snapshot, &condition, &result); res != XR_SUCCESS)
		{
			spdlog::warn("marker_tracker: component query failed");
			return;
		}
		if (result.entityIdCountOutput > entity_ids.size() or
		    result.entityStateCountOutput > entity_states.size() or
		    marker_list.markerCount > marker_data.size() or
		    bounds_list.boundCount > bound_boxes.size())
		{
			size_t n = std::max({(size_t)result.entityIdCountOutput, (size_t)result.entityStateCountOutput,
			                     (size_t)marker_list.markerCount, (size_t)bounds_list.boundCount});
			size_t old = entity_ids.size();
			entity_ids.resize(n);
			entity_states.resize(n);
			marker_data.resize(n);
			bound_boxes.resize(n);
			for (size_t i = old; i < n; ++i)
			{
				entity_ids[i] = UINT64_MAX;
				marker_data[i].markerId = 0xFFFFFFFFu;
			}
			continue;
		}

		// Entity outputs are the source of truth; per-list counts are
		// capacities the runtime may echo back untouched (sentinels in the
		// scratch prove what was actually written).
		uint32_t count = std::min({result.entityIdCountOutput, (uint32_t)marker_data.size(), (uint32_t)bound_boxes.size()});
		if (result.entityIdCountOutput != last_entity_count)
		{
			last_entity_count = result.entityIdCountOutput;
			spdlog::info("marker_tracker: discovery: {} entities (markers {}, bounds {})",
			             result.entityIdCountOutput, marker_list.markerCount, bounds_list.boundCount);
		}
		if (result.entityIdCountOutput > 0 and not entries_dumped)
		{
			entries_dumped = true;
			for (uint32_t i = 0; i < count; ++i)
			{
				const auto & md = marker_data[i];
				const auto & b = bound_boxes[i];
				int estate = i < result.entityStateCountOutput ? (int)entity_states[i] : -1;
				spdlog::info("marker_tracker: entry[{}]: estate={} cap={} id={} buf={} buftype={} ext=({:.3f},{:.3f}) pos=({:.2f},{:.2f},{:.2f})",
				             i, estate, (int)md.capability, md.markerId,
				             (unsigned long long)md.data.bufferId, (int)md.data.bufferType,
				             b.extents.width, b.extents.height,
				             b.center.position.x, b.center.position.y, b.center.position.z);
			}
		}
		bool found = false;
		uint32_t unreadable = 0;
		// All readable codes are reported (matched and otherwise): the
		// debug overlays show every code in view, and every entity id is
		// latched so the per-frame update path refreshes them all.
		all_sightings.clear();
		for (uint32_t i = 0; i < count; ++i)
		{
			// QR runtimes report markerId 0: identity is the payload
			auto payload = read_payload(snapshot, marker_data[i].data);
			if (not payload)
			{
				++unreadable;
				continue;
			}
			bool matched = *payload == marker_payload;
			if (not matched)
			{
				// Diagnostic: capped, so a room full of foreign QR
				// codes shows what strings exist without spamming
				if (unknown_payloads_logged.size() < 8 and
				    unknown_payloads_logged.insert(*payload).second)
					spdlog::info("marker_tracker: ignoring unconfigured QR payload \"{}\"",
					             payload->substr(0, 64));
			}
			XrSpatialEntityIdEXT eid = i < result.entityIdCountOutput ? entity_ids[i] : XR_NULL_SPATIAL_ENTITY_ID_EXT;
			ensure_entity(eid);
			all_sightings.push_back(code_sighting{
			        .payload = *payload,
			        .pose = bound_boxes[i].center,
			        .extents = bound_boxes[i].extents,
			        .entity_id = eid,
			        .time = predicted_time,
			        .matched = matched,
			});
			if (not matched)
				continue;
			if (not current.tracked)
			{
				const auto & c = bound_boxes[i].center;
				const auto & e = bound_boxes[i].extents;
				spdlog::info("marker_tracker: marker \"{}\" sighted at ({:.2f}, {:.2f}, {:.2f}) quat ({:.3f}, {:.3f}, {:.3f}, {:.3f}) extents ({:.3f}, {:.3f})",
				             label(), c.position.x, c.position.y, c.position.z,
				             c.orientation.x, c.orientation.y, c.orientation.z, c.orientation.w,
				             e.width, e.height);
			}
			current.tracked = true;
			current.pose = bound_boxes[i].center;
			current.time = predicted_time;
			status_text = "tracking marker";
			found = true;
		}
		if (not found)
		{
			// Markers present but none readable: payload path broken, say
			// so once (reasons at debug). Otherwise the count line above
			// already told the story.
			bool unreadable_now = unreadable > 0;
			if (unreadable_now != last_unreadable)
			{
				last_unreadable = unreadable_now;
				if (unreadable_now)
					spdlog::info("marker_tracker: markers visible but payload unreadable");
			}
			if (current.tracked)
				spdlog::info("marker_tracker: lost sight of marker \"{}\"", label());
			current.tracked = false;
			status_text = "marker not in view";
		}
		return;
	}
}

void xr::marker_tracker::ensure_entity(XrSpatialEntityIdEXT id)
{
	if (id == XR_NULL_SPATIAL_ENTITY_ID_EXT)
		return;
	if (spatial_entities.contains(id))
		return;
	XrSpatialEntityFromIdCreateInfoEXT from_id{
	        .type = XR_TYPE_SPATIAL_ENTITY_FROM_ID_CREATE_INFO_EXT,
	        .entityId = id,
	};
	XrSpatialEntityEXT entity{nullptr};
	if (XrResult res = xrCreateSpatialEntityFromIdEXT(spatial_context, &from_id, &entity); res != XR_SUCCESS)
	{
		spdlog::debug("marker_tracker: cannot create entity from id: {}", (int)res);
		return;
	}
	spatial_entities.emplace(id, spatial_entity_handle(entity, xrDestroySpatialEntityEXT));
}

void xr::marker_tracker::log_rate(XrTime now)
{
	if (update_ticks == 0 or first_tick_at == 0)
		return;
	// Skip the first second: the base is too short for a sane rate.
	if (now - first_tick_at < 1'000'000'000LL)
		return;
	if (now - last_rate_log < 5'000'000'000LL)
		return;
	last_rate_log = now;
	double rate_hz = update_ticks * 1e9 / (double)(now - first_tick_at);
	spdlog::info("marker_tracker: update snapshots running ({} ticks, {:.1f}/s)", update_ticks, rate_hz);
}

void xr::marker_tracker::update_snapshot(XrSpace world_space, XrTime now, XrTime predicted_time)
{
	if (spatial_entities.empty())
		return;
	if (not xrCreateSpatialUpdateSnapshotEXT)
	{
		if (not update_unsupported_logged)
		{
			update_unsupported_logged = true;
			spdlog::info("marker_tracker: update snapshots unsupported, discovery-only tracking");
		}
		return;
	}
	const XrSpatialComponentTypeEXT * types = bounded_pose ? marker_and_bounds : marker_only;
	std::vector<XrSpatialEntityEXT> entities;
	entities.reserve(spatial_entities.size());
	for (const auto & [id, handle]: spatial_entities)
		entities.push_back(handle);
	XrSpatialUpdateSnapshotCreateInfoEXT create_info{
	        .type = XR_TYPE_SPATIAL_UPDATE_SNAPSHOT_CREATE_INFO_EXT,
	        .entityCount = (uint32_t)entities.size(),
	        .entities = entities.data(),
	        .componentTypeCount = bounded_pose ? 2u : 1u,
	        .componentTypes = types,
	        .baseSpace = world_space,
	        .time = predicted_time,
	};
	XrSpatialSnapshotEXT raw_snapshot = XR_NULL_HANDLE;
	if (XrResult res = xrCreateSpatialUpdateSnapshotEXT(spatial_context, &create_info, &raw_snapshot); res != XR_SUCCESS)
	{
		if (res == XR_ERROR_SPATIAL_ENTITY_ID_INVALID_EXT)
		{
			// An id expired (runtime recycled ids): drop everything so
			// the next discovery pass re-latches what is still around.
			spatial_entities.clear();
		}
		else
			spdlog::debug("marker_tracker: update snapshot failed: {}", (int)res);
		return;
	}
	spatial_snapshot_handle snapshot(raw_snapshot, xrDestroySpatialSnapshotEXT);

	XrSpatialComponentDataQueryConditionEXT condition{
	        .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT,
	        .componentTypeCount = bounded_pose ? 2u : 1u,
	        .componentTypes = types,
	};
	// The runtime must not return unlisted entities, so sizing scratch to
	// the request guarantees fit; grow once if a previous pass left it
	// smaller.
	size_t need = entities.size();
	if (entity_ids.size() < need)
	{
		entity_ids.assign(need, UINT64_MAX);
		entity_states.resize(need);
		marker_data.resize(need);
		bound_boxes.resize(need);
	}
	for (auto & m: marker_data)
		m.markerId = 0xFFFFFFFFu;
	XrSpatialComponentMarkerListEXT marker_list{
	        .type = XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT,
	        .markerCount = (uint32_t)marker_data.size(),
	        .markers = marker_data.data(),
	};
	XrSpatialComponentBounded2DListEXT bounds_list{
	        .type = XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT,
	        .next = &marker_list,
	        .boundCount = (uint32_t)bound_boxes.size(),
	        .bounds = bound_boxes.data(),
	};
	XrSpatialComponentDataQueryResultEXT result{
	        .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT,
	        .next = &bounds_list,
	        .entityIdCapacityInput = (uint32_t)entity_ids.size(),
	        .entityIds = entity_ids.data(),
	        .entityStateCapacityInput = (uint32_t)entity_states.size(),
	        .entityStates = entity_states.data(),
	};
	if (XrResult res = xrQuerySpatialComponentDataEXT(snapshot, &condition, &result); res != XR_SUCCESS)
	{
		spdlog::debug("marker_tracker: update query failed: {}", (int)res);
		return;
	}
	if (not bounded_pose)
	{
		current.tracked = false;
		status_text = "marker has no pose component";
		return;
	}
	// Rebuild sightings from this tick; ids the runtime no longer returns
	// are stale (dropped, discovery re-latches survivors).
	std::set<XrSpatialEntityIdEXT> seen;
	all_sightings.clear();
	bool matched_tick = false;
	uint32_t out = std::min({result.entityIdCountOutput, (uint32_t)marker_data.size(), (uint32_t)bound_boxes.size()});
	for (uint32_t i = 0; i < out; ++i)
	{
		XrSpatialEntityIdEXT eid = entity_ids[i];
		seen.insert(eid);
		if (i >= result.entityStateCountOutput or entity_states[i] != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT)
			continue;
		auto payload = read_payload(snapshot, marker_data[i].data);
		if (not payload)
			continue;
		bool matched = *payload == marker_payload;
		if (not matched)
		{
			if (unknown_payloads_logged.size() < 8 and
			    unknown_payloads_logged.insert(*payload).second)
				spdlog::info("marker_tracker: ignoring unconfigured QR payload \"{}\"",
				             payload->substr(0, 64));
		}
		all_sightings.push_back(code_sighting{
		        .payload = *payload,
		        .pose = bound_boxes[i].center,
		        .extents = bound_boxes[i].extents,
		        .entity_id = eid,
		        .time = predicted_time,
		        .matched = matched,
		});
		if (not matched)
			continue;
		if (not current.tracked)
			spdlog::info("marker_tracker: marker \"{}\" re-sighted via update path", label());
		current.tracked = true;
		current.pose = bound_boxes[i].center;
		current.time = predicted_time;
		status_text = "tracking marker";
		matched_tick = true;
	}
	for (auto it = spatial_entities.begin(); it != spatial_entities.end();)
	{
		if (seen.contains(it->first))
			++it;
		else
			it = spatial_entities.erase(it);
	}
	if (not matched_tick)
	{
		if (current.tracked)
			spdlog::info("marker_tracker: lost sight of marker \"{}\"", label());
		current.tracked = false;
		status_text = "marker not in view";
	}
	if (first_tick_at == 0)
		first_tick_at = now;
	update_ticks++;
	log_rate(now);
}

void xr::marker_tracker::update(XrSpace world_space, XrTime now, XrTime predicted_time)
{
	last_now = now;
	switch (current_state)
	{
		case state::idle:
			return;

		case state::failed:
			// Auto-retry with backoff: transient runtime hiccups recover.
			// configure() itself honors retry_at, so this only re-arms idle.
			if (not marker_payload.empty() and now >= retry_at)
			{
				retry_at = now + retry_delay_ns;
				current_state = state::idle;
				configure(marker_size_m, marker_payload, marker_tag, marker_static);
			}
			return;

		case state::creating_context: {
			if (now < retry_at)
				return;
			bool ready = false;
			if (not poll_ready(context_future, ready))
			{
				retry_at = now + retry_delay_ns;
				return;
			}
			if (ready)
				complete_context();
			return;
		}

	case state::ready: {
		// Live refresh every frame (no timers). Runs before the discovery
		// poll below so an acquisition completing this frame has the final
		// word on the current sighting.
		if (not spatial_entities.empty())
			update_snapshot(world_space, now, predicted_time);
		if (discovery_future != XR_NULL_FUTURE_EXT)
		{
			bool ready = false;
			if (poll_ready(discovery_future, ready) and ready)
				complete_discovery(world_space, predicted_time);
			// The update path above is independent of the in-flight
			// discovery: latched entities refresh even while a new
			// discovery is outstanding.
		}
		// Discovery stays throttled: acquisition and re-acquire only.
		if (now - last_discovery_start >= discovery_period_ns)
		{
			last_discovery_start = now;
			start_discovery();
		}
		return;
	}
	}
}
