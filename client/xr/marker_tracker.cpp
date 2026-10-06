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
        xrPollFutureEXT(inst_.get_proc<PFN_xrPollFutureEXT>("xrPollFutureEXT")),
        xrCreateSpatialContextAsyncEXT(inst_.get_proc<PFN_xrCreateSpatialContextAsyncEXT>("xrCreateSpatialContextAsyncEXT")),
        xrCreateSpatialContextCompleteEXT(inst_.get_proc<PFN_xrCreateSpatialContextCompleteEXT>("xrCreateSpatialContextCompleteEXT")),
        xrCreateSpatialDiscoverySnapshotAsyncEXT(inst_.get_proc<PFN_xrCreateSpatialDiscoverySnapshotAsyncEXT>("xrCreateSpatialDiscoverySnapshotAsyncEXT")),
        xrCreateSpatialDiscoverySnapshotCompleteEXT(inst_.get_proc<PFN_xrCreateSpatialDiscoverySnapshotCompleteEXT>("xrCreateSpatialDiscoverySnapshotCompleteEXT")),
        xrQuerySpatialComponentDataEXT(inst_.get_proc<PFN_xrQuerySpatialComponentDataEXT>("xrQuerySpatialComponentDataEXT")),
        xrGetSpatialBufferStringEXT(inst_.get_proc<PFN_xrGetSpatialBufferStringEXT>("xrGetSpatialBufferStringEXT")),
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

void xr::marker_tracker::configure(int32_t id, float size_m, std::string payload)
{
	if (id == marker_id and size_m == marker_size_m and payload == marker_payload and
	    current_state != state::failed and current_state != state::idle)
		return;

	// Failure backoff: configure() is called every frame, attempts only on
	// a fresh tracker or once the retry timer elapsed (no per-frame spam).
	if (current_state == state::failed and last_now < retry_at)
		return;

	bool qr = not payload.empty();
	spdlog::info("marker_tracker: tracking {} marker {} ({:.0f}mm){}",
	             qr ? "QR code" : "AprilTag 36h11", id, (double)(size_m * 1000),
	             qr ? " payload \"" + payload.substr(0, 64) + "\"" : "");
	marker_id = id;
	marker_size_m = size_m;
	marker_payload = std::move(payload);
	unknown_payloads_logged.clear();
	current = sighting{};
	context_future = XR_NULL_FUTURE_EXT;
	discovery_future = XR_NULL_FUTURE_EXT;
	spatial_context = spatial_context_handle{xrDestroySpatialContextEXT};
	current_state = state::idle;
	status_text = "checking capabilities";

	if (marker_size_m <= 0)
	{
		fail("invalid marker size");
		return;
	}

	// Require the marker capability before creating the context. QR codes
	// carry identity in the payload (markerId is 0); an empty payload keeps
	// the legacy numeric-ID AprilTag path for runtimes advertising it.
	bool want_qr = not marker_payload.empty();
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
	XrSpatialCapabilityEXT want_cap = want_qr ? XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT
	                                          : XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT;
	if (not std::ranges::contains(caps, want_cap))
	{
		fail(want_qr ? "QR code tracking not advertised by runtime"
		             : "AprilTag tracking not advertised by runtime");
		return;
	}

	static const XrSpatialComponentTypeEXT components[] = {
	        XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT,
	        XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT,
	};
	XrSpatialMarkerSizeEXT size{
	        .type = XR_TYPE_SPATIAL_MARKER_SIZE_EXT,
	        .markerSideLength = marker_size_m,
	};
	XrSpatialCapabilityConfigurationAprilTagEXT april_tag{
	        .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_APRIL_TAG_EXT,
	        .next = &size,
	        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT,
	        .enabledComponentCount = 2,
	        .enabledComponents = components,
	        .aprilDict = XR_SPATIAL_MARKER_APRIL_TAG_DICT_36H11_EXT,
	};
	XrSpatialCapabilityConfigurationQrCodeEXT qr_code{
	        .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_QR_CODE_EXT,
	        .next = &size,
	        .capability = XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT,
	        .enabledComponentCount = 2,
	        .enabledComponents = components,
	};
	static const XrSpatialComponentTypeEXT anchor_components[] = {
	        XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT,
	};
	XrSpatialCapabilityConfigurationAnchorEXT anchor{
	        .type = XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_ANCHOR_EXT,
	        .capability = XR_SPATIAL_CAPABILITY_ANCHOR_EXT,
	        .enabledComponentCount = 1,
	        .enabledComponents = anchor_components,
	};
	const XrSpatialCapabilityConfigurationBaseHeaderEXT * configs[] = {
	        want_qr ? reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(&qr_code)
	                : reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(&april_tag),
	        reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(&anchor),
	};
	XrSpatialContextCreateInfoEXT create_info{
	        .type = XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT,
	        .capabilityConfigCount = 2,
	        .capabilityConfigs = configs,
	};
	if (XrResult res = xrCreateSpatialContextAsyncEXT(*sess, &create_info, &context_future); res != XR_SUCCESS)
	{
		fail("cannot create spatial context");
		return;
	}

	current_state = state::creating_context;
	status_text = "creating spatial context";
	spdlog::info("marker_tracker: creating spatial context for {} marker {}",
	             want_qr ? "QR code" : "AprilTag 36h11", marker_id);
}

std::optional<std::string> xr::marker_tracker::read_payload(XrSpatialSnapshotEXT snapshot, XrSpatialBufferEXT buffer)
{
	// QR payloads are decoded strings; anything else has no usable identity
	if (buffer.bufferId == XR_NULL_SPATIAL_BUFFER_ID_EXT or
	    buffer.bufferType != XR_SPATIAL_BUFFER_TYPE_STRING_EXT)
		return std::nullopt;

	XrSpatialBufferGetInfoEXT info{
	        .type = XR_TYPE_SPATIAL_BUFFER_GET_INFO_EXT,
	        .bufferId = buffer.bufferId,
	};
	uint32_t count = 0;
	if (XrResult res = xrGetSpatialBufferStringEXT(snapshot, &info, 0, &count, nullptr); res != XR_SUCCESS or count == 0)
		return std::nullopt;

	// +1 and explicit trim: runtimes differ on whether count includes NUL
	std::vector<char> text(count + 1, 0);
	if (XrResult res = xrGetSpatialBufferStringEXT(snapshot, &info, count, &count, text.data()); res != XR_SUCCESS)
		return std::nullopt;
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
	static const XrSpatialComponentTypeEXT components[] = {
	        XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT,
	        XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT,
	};
	XrSpatialFilterTrackingStateEXT tracking_filter{
	        .type = XR_TYPE_SPATIAL_FILTER_TRACKING_STATE_EXT,
	        .trackingState = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT,
	};
	XrSpatialDiscoverySnapshotCreateInfoEXT create_info{
	        .type = XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT,
	        .next = &tracking_filter,
	        .componentTypeCount = 2,
	        .componentTypes = components,
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

	static const XrSpatialComponentTypeEXT components[] = {
	        XR_SPATIAL_COMPONENT_TYPE_MARKER_EXT,
	        XR_SPATIAL_COMPONENT_TYPE_ANCHOR_EXT,
	};
	XrSpatialComponentDataQueryConditionEXT condition{
	        .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT,
	        .componentTypeCount = 2,
	        .componentTypes = components,
	};

	// Query twice at most: grow scratch storage when the snapshot holds more
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (marker_data.empty())
		{
			marker_data.resize(8);
			anchor_poses.resize(8);
		}
		XrSpatialComponentMarkerListEXT marker_list{
		        .type = XR_TYPE_SPATIAL_COMPONENT_MARKER_LIST_EXT,
		        .markerCount = (uint32_t)marker_data.size(),
		        .markers = marker_data.data(),
		};
		XrSpatialComponentAnchorListEXT anchor_list{
		        .type = XR_TYPE_SPATIAL_COMPONENT_ANCHOR_LIST_EXT,
		        .next = &marker_list,
		        .locationCount = (uint32_t)anchor_poses.size(),
		        .locations = anchor_poses.data(),
		};
		XrSpatialComponentDataQueryResultEXT result{
		        .type = XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT,
		        .next = &anchor_list,
		};
		if (XrResult res = xrQuerySpatialComponentDataEXT(snapshot, &condition, &result); res != XR_SUCCESS)
		{
			spdlog::warn("marker_tracker: component query failed");
			return;
		}
		if (marker_list.markerCount > marker_data.size() or anchor_list.locationCount > anchor_poses.size())
		{
			marker_data.resize(std::max(marker_list.markerCount, anchor_list.locationCount));
			anchor_poses.resize(marker_data.size());
			continue;
		}

		uint32_t count = std::min(marker_list.markerCount, anchor_list.locationCount);
		bool found = false;
		for (uint32_t i = 0; i < count; ++i)
		{
			bool match = false;
			if (marker_payload.empty())
			{
				// Legacy numeric matching (AprilTag runtimes)
				match = marker_data[i].capability == XR_SPATIAL_CAPABILITY_MARKER_TRACKING_APRIL_TAG_EXT and
				        marker_data[i].markerId == (uint32_t)marker_id;
			}
			else if (marker_data[i].capability == XR_SPATIAL_CAPABILITY_MARKER_TRACKING_QR_CODE_EXT)
			{
				// QR runtimes report markerId 0: identity is the payload
				auto payload = read_payload(snapshot, marker_data[i].data);
				if (not payload)
					continue;
				if (*payload != marker_payload)
				{
					// Diagnostic: capped, so a room full of foreign QR
					// codes shows what strings exist without spamming
					if (unknown_payloads_logged.size() < 8 and
					    unknown_payloads_logged.insert(*payload).second)
						spdlog::info("marker_tracker: ignoring unconfigured QR payload \"{}\"",
						             payload->substr(0, 64));
					continue;
				}
				match = true;
			}
			if (match)
			{
				if (not current.tracked)
				{
					const auto & p = anchor_poses[i];
					spdlog::info("marker_tracker: marker {} sighted at ({:.2f}, {:.2f}, {:.2f})",
					             marker_id, p.position.x, p.position.y, p.position.z);
				}
				current.tracked = true;
				current.pose = anchor_poses[i];
				current.time = predicted_time;
				status_text = "tracking marker";
				found = true;
				break;
			}
		}
		if (not found)
		{
			if (current.tracked)
				spdlog::info("marker_tracker: lost sight of marker {}", marker_id);
			current.tracked = false;
			status_text = "marker not in view";
		}
		return;
	}
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
			if (marker_id >= 0 and now >= retry_at)
			{
				retry_at = now + retry_delay_ns;
				current_state = state::idle;
				configure(marker_id, marker_size_m, marker_payload);
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
			if (discovery_future != XR_NULL_FUTURE_EXT)
			{
				bool ready = false;
				if (poll_ready(discovery_future, ready) and ready)
					complete_discovery(world_space, predicted_time);
				return;
			}
			if (now - last_discovery_start >= discovery_period_ns)
			{
				last_discovery_start = now;
				start_discovery();
			}
			return;
		}
	}
}
