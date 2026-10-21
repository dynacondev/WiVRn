/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#pragma once

#include "util/comp_base.h"
#include "util/u_logging.h"

#include "depth_reprojector.h"
#include "encoder/encoder_settings.h"
#include "foveation.h"
#include "inplace_vector.hpp"
#include "layer_squasher.h"
#include "pacer.h"
#include "utils/wivrn_vk_bundle.h"

#include "main/comp_compositor.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace wivrn
{

class wivrn_session;
class video_encoder;

class compositor : public comp_base
{
public:
	struct image
	{
		std::atomic<bool> busy = false;
		image_allocation image;
		vk::raii::ImageView view_y;
		vk::raii::ImageView view_cbcr;
		to_headset::video_stream_data_shard::view_info_t view_info{};
		uint64_t frame_index;
	};

private:
	struct timings
	{
		std::array<float, 50> values{};
		int index = 0;
		u_var_timing var{
		        .values = {
		                .data = values.data(),
		                .index_ptr = &index,
		                .length = int(values.size()),
		        },
		        .range = 1000,
		        .dynamic_rescale = true,
		        .unit = "µs",
		};

		void add(float us);
	};

	const u_logging_level log_level;
	timings squasher_times;
	timings warp_times;
	timings foveation_times;
	wivrn_session & session;
	vk_bundle vk;
	vk::raii::CommandPool cmd_pool;
	vk::raii::QueryPool query_pool;

	// Synth path command resources: the ticker records independently of
	// layer_commit (serialized by commit_mutex, never concurrent), so it
	// needs its own pool — resetting the shared pool would invalidate
	// the in-flight record. No query pool: synth warps skip timestamp
	// reads (warp cost is estimated, corrected by the lateness signal).
	vk::raii::CommandPool synth_pool;
	vk::raii::CommandBuffer synth_cmd;

	std::array<encoder_settings, 3> settings;

	// Compositor output images awaiting encode: 1 real + 1 replacement
	// synth per commit, times 2 commits in flight. Encoder slots
	// (num_slots per stream) are the outer bound; acquire_image fails
	// gracefully past it.
	std::array<image, 4> images;
	vk::raii::CommandBuffer cmd;
	vk::raii::Semaphore sem;
	uint64_t sem_value = 0;

	std::atomic<float> requested_refresh_rate;
	std::atomic<float> frame_rate;
	wivrn::pacer pacer;

	layer_squasher squasher;
	wivrn::foveation foveation;
	depth_reprojector reprojector;

	// Cached at construction: server config is startup-read.
	const bool depth_reprojection_enabled;

	// Render-time source metadata for the retained frame (pose/FOV/clip
	// per eye). Written alongside refresh_retained on fresh depth
	// commits, read by the synth ticker under the same commit lock.
	// Lets the ticker warp without a layer in hand.
	std::array<xrt_pose, 2> retained_src_pose{};
	std::array<xrt_fov, 2> retained_src_fov{};
	std::array<float, 2> retained_src_near{1, 1};
	std::array<float, 2> retained_src_far{1000, 1000};

	// Last client app-frame timestamp seen on a fast-path commit, for
	// refresh gating: a changed timestamp means the commit carries a new
	// app frame whose color+depth must be retained. Empty until the
	// first fast-path commit.
	std::optional<int64_t> last_app_layer_ts;

	// Arrival time of the previous layer_commit (monotonic ns). The warp
	// gate compares inter-commit spacing against the display period:
	// layer_commit runs once per app EndFrame, not per display tick, so
	// staleness cannot come from timestamps (consecutive commits always
	// carry fresh ones). Zero until the first commit. Atomic: the synth
	// ticker reads it while layer_commit writes it.
	std::atomic<uint64_t> last_commit_ns{0};

	// Last queued output (monotonic ns, commit or synth path). The ticker
	// synthesizes a tick only when no output covers it.
	std::atomic<uint64_t> last_output_ns{0};
	// Whether the latest commit carried depth. The ticker synthesizes
	// only from depth frames (warping from stale retained content while
	// the app shows non-depth layers would show the wrong frame).
	std::atomic<bool> last_had_depth{false};
	// Arrival time of the commit that last refreshed retained sources.
	// Bounds how stale a synth source may be (dead-app guard).
	std::atomic<uint64_t> last_retained_ns{0};

	// Alpha blend mode of the latest commit. The synth ticker cannot
	// safely read layer_accum (owned by the commit path), so layer_commit
	// publishes it here. Staleness is harmless (blend mode changes only
	// on app layer changes).
	std::atomic<bool> last_alpha_blend{false};

	std::array<std::unique_ptr<video_encoder>, 3> encoders;

	// Serializes the record→submit→present→queue region across the
	// EndFrame path and the synth ticker (reprojector, foveation UBO /
	// descriptor writes, cmd buffer, resolve set, sem_value). Leaf lock:
	// only ever taken alone, never nested inside another lock
	// (queue_encode takes encode_mutex under it: commit → encode order
	// everywhere, no reverse path). Uncontended fast path.
	std::mutex commit_mutex;

	std::mutex encode_mutex;
	std::condition_variable_any encode_cv;
	struct queued_frame
	{
		int image = -1;
		// Per-encoder slot recorded at present time (-1 = stream
		// skipped). Encode consumes exactly the recorded slot, so
		// present/encode cannot diverge across exceptions, drops or
		// skips.
		std::array<int8_t, 3> slots{-1, -1, -1};
	};
	// Bounded in practice by images: at most 2 frames queued per
	// commit while the pool holds 4 (acquire_image drops past it).
	std::deque<queued_frame> encode_queue;
	std::jthread encoder_thread;

	// Last timeline value submitted by the synth ticker. The ticker waits
	// for it (bounded) before resetting its command pool, mirroring the
	// commit path's end-of-tick wait: never reset a pool whose work is
	// still on the GPU. Ticker-thread only.
	uint64_t last_synth_submit_value = 0;

	// Display-rate synth ticker: emits warped frames on ticks with no
	// fresh EndFrame, so slow apps still feed the client at display
	// cadence. Purely additive — the EndFrame path is untouched. Stopped
	// + joined before the encoder thread (it produces; the worker drains).
	std::jthread synth_thread;
	std::mutex synth_mutex;
	std::condition_variable_any synth_cv;

	struct
	{
		comp_frame waited{.id = -1};
		comp_frame rendering{.id = -1};
	} frame;

	xrt_result_t begin_session(const xrt_begin_session_info * info)
	{
		return XRT_SUCCESS;
	};
	xrt_result_t end_session()
	{
		return XRT_SUCCESS;
	};

	xrt_result_t predict_frame(int64_t * out_frame_id,
	                           int64_t * out_wake_time_ns,
	                           int64_t * out_predicted_gpu_time_ns,
	                           int64_t * out_predicted_display_time_ns,
	                           int64_t * out_predicted_display_period_ns);

	xrt_result_t mark_frame(int64_t frame_id,
	                        xrt_compositor_frame_point point,
	                        int64_t when_ns);

	xrt_result_t begin_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	}
	xrt_result_t discard_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	};

	xrt_result_t layer_commit(xrt_graphics_sync_handle_t sync_handle);

	xrt_result_t get_display_refresh_rate(float * out_display_refresh_rate_hz);

public:
	xrt_result_t request_display_refresh_rate(float display_refresh_rate_hz);

	static xrt_result_t get_view_config(xrt_compositor_native *,
	                                    xrt_view_type view_type,
	                                    xrt_view_config * out_view_config);

private:
	void destroy()
	{
		// do nothing, actually owned by wivrn_session object
	}

	int acquire_image();

	void synth_work(std::stop_token tok);

	void append_encoder_barriers(
	        beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 3> & barriers,
	        int image_idx,
	        const to_headset::video_stream_data_shard::view_info_t & info);

	// Submits a fully recorded command buffer on the compositor queue,
	// signaling the timeline semaphore. Returns the signaled value for
	// the present call. Self-synchronizing via the queue mutex (submit
	// order is signal order); callable with or without commit_mutex.
	uint64_t submit_cmd_buffer(vk::raii::CommandBuffer & cmd);

	// Presents one fully-recorded image to all encoders and records
	// their slots. Returns false when no stream could present within the
	// bounded slot wait (tick dropped; caller releases the image).
	// Partial presents queue normally (the worker skips the -1 streams).
	bool present_to_encoders(
	        int image_idx,
	        const to_headset::video_stream_data_shard::view_info_t & info,
	        int64_t frame_id,
	        const vk::SemaphoreSubmitInfo & sem_info,
	        std::array<int8_t, 3> & slots);

	void queue_encode(int image, const std::array<int8_t, 3> & slots);

	void encoder_work(std::stop_token);

	void send_video_stream_description();

public:
	using base_t = xrt_compositor;
	compositor(wivrn_session &);
	~compositor();

	xrt_system_compositor_info sys_info() const;

	int64_t get_frame_duration() const
	{
		return pacer.get_frame_duration();
	}

	float get_requested_refresh_rate() const
	{
		return requested_refresh_rate;
	}

	float get_framerate() const
	{
		return frame_rate;
	}
	void set_framerate(float hz);

	void set_bitrate(uint32_t);
	void update_tracking(const from_headset::tracking &);
	void update_foveation_center_override(const from_headset::override_foveation_center &);

	void resume();

	void on_feedback(const from_headset::feedback &, const clock_offset &);
};

} // namespace wivrn
