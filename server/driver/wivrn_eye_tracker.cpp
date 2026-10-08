/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
 * Copyright (C) 2024  galister <galister@librevr.org>
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

#include "wivrn_eye_tracker.h"

#include "wivrn_packets.h"
#include "wivrn_session.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "util/u_device_id.h"
#include "util/u_logging.h"
#include "utils/method.h"

#include "os/os_time.h"

#include <cmath>
#include <cstdint>
#include <openxr/openxr.h>

namespace wivrn
{

wivrn_eye_tracker::wivrn_eye_tracker(wivrn_session & cnx) :
        xrt_device{
                .id = u_device_id_generate(),
                .name = XRT_DEVICE_EYE_GAZE_INTERACTION,
                .device_type = XRT_DEVICE_TYPE_EYE_TRACKER,
                .str = "WiVRn Eye Tracker",
                .serial = "WiVRn Eye Tracker",
                .tracking_origin = &origin,
                .input_count = 1,
                .inputs = &gaze_input,
                .supported = {
                        .eye_gaze = true,
                },
                .update_inputs = method_pointer<&wivrn_eye_tracker::update_inputs>,
                .get_tracked_pose = method_pointer<&wivrn_eye_tracker::get_tracked_pose>,
                .destroy = [](xrt_device *) {},
        },
        origin{
                .type = XRT_TRACKING_TYPE_ATTACHABLE,
                .initial_offset = XRT_POSE_IDENTITY,
        },
        gaze_input{
                .active = true,
                .name = XRT_INPUT_GENERIC_EYE_GAZE_POSE,
        },
        gaze(device_id::EYE_GAZE),
        cnx(cnx)
{
}

xrt_result_t wivrn_eye_tracker::update_inputs()
{
	return XRT_SUCCESS;
}

xrt_result_t wivrn_eye_tracker::get_tracked_pose(xrt_input_name name, int64_t at_timestamp_ns, xrt_space_relation * out_relation)
{
	if (name == XRT_INPUT_GENERIC_EYE_GAZE_POSE)
	{
		// EXPERIMENT HACK: Quest 3 has no eye hardware, so synthesize a gaze
		// panning left/right along screen center: triangle wave, 5s each
		// direction (10s period), +/-30 deg yaw, zero pitch. Identity pose =
		// looking straight ahead (-Z); yaw rotates around Y.
		// Deliberately NOT calling cnx.add_tracking_request() and NOT reading
		// pose_list `gaze`, so the headset is never polled for EYE_GAZE and
		// WiVRn's own foveated encoding stays fixed (isolates X-Plane VRS).
		(void)at_timestamp_ns;
		int64_t now_ns = os_monotonic_get_ns();
		double sec = fmod(double(now_ns) / 1e9, 10.0);
		float tri = sec < 5.0 ? (-1.0f + float(sec) * (2.0f / 5.0f)) : (1.0f - float(sec - 5.0) * (2.0f / 5.0f));
		constexpr float kAmplitudeRad = 30.0f * 3.14159265358979323846f / 180.0f;
		float yaw = tri * kAmplitudeRad;
		float half = yaw * 0.5f;
		out_relation->relation_flags = (xrt_space_relation_flags)(
		        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		        XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		        XRT_SPACE_RELATION_POSITION_VALID_BIT |
		        XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
		out_relation->pose.orientation = {0.0f, sinf(half), 0.0f, cosf(half)};
		out_relation->pose.position = {0.0f, 0.0f, 0.0f};
		out_relation->linear_velocity = {0.0f, 0.0f, 0.0f};
		out_relation->angular_velocity = {0.0f, 0.0f, 0.0f};
		return XRT_SUCCESS;
	}

	U_LOG_XDEV_UNSUPPORTED_INPUT(this, u_log_get_global_level(), name);
	return XRT_ERROR_INPUT_UNSUPPORTED;
}

void wivrn_eye_tracker::update_tracking(const from_headset::tracking & tracking, const clock_offset & offset)
{
	gaze.update_tracking(tracking, offset);
}
} // namespace wivrn
