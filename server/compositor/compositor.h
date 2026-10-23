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

	// Warp debug output (timing heartbeat + commit warp GPU timestamps): off in
	// prod. Enabled by WIVRN_WARP_DEBUG=1 (or REPROJEX_DEBUG=1, so one env
	// lights the whole depth pipeline). Read once at construction.
	const bool warp_debug;

	// Warp debug counters: touched on the EndFrame path, hence relaxed
	// atomics. layer_commit emits a 5 s delta heartbeat when warp_debug
	// is on; steady-state cost is a few atomic increments, zero logs.
	struct warp_counters
	{
		std::atomic<uint64_t> c_total{0};
		std::atomic<uint64_t> c_straight{0};
		std::atomic<uint64_t> c_warped{0};
		std::atomic<uint64_t> c_dropped{0};
		std::atomic<uint64_t> c_gate{0};
		std::atomic<uint64_t> c_degenerate{0};
		std::atomic<uint64_t> c_prewait{0};
		std::atomic<uint64_t> c_pool{0};
		std::atomic<uint64_t> c_present{0};
		// Fresh vs repeated client frames: the multi-system thread
		// recommits the last delivered layers every display tick, so
		// c_total runs at display rate while the app may be far slower.
		// A repeat carries the identical client timestamp; only fresh
		// frames advance it. Split here proves the ratio live.
		std::atomic<uint64_t> c_fresh{0};
		std::atomic<uint64_t> c_repeat{0};
	};
	warp_counters wcounts;
	// Last collected commit warp GPU time (ns): the heartbeat reports
	// it alongside the counters.
	std::atomic<uint64_t> last_commit_warp_ns{0};
	// Predicted tick time minus commit wall time (ns, signed): shows
	// whether ticks run ahead of the wall clock. A large positive lead
	// means the destination poses are extrapolated far into the future.
	std::atomic<int64_t> last_tick_lead_ns{0};

	std::array<encoder_settings, 3> settings;

	// Compositor output images awaiting encode: 1 real + 1 replacement
	// synth per commit, times 2 commits in flight. Encoder slots
	// (num_slots per stream) are the outer bound; acquire_image fails
	// gracefully past it.
	std::array<image, 4> images;
	vk::raii::CommandBuffer cmd;
	vk::raii::Semaphore sem;
	// Monotonic timeline value, incremented per submit. Atomic:
	// submit_cmd_buffer is self-synchronizing via the queue mutex
	// (submit order is signal order).
	std::atomic<uint64_t> sem_value{0};

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
	// commits; the commit path warps repeats from it without a layer
	// in hand.
	std::array<xrt_pose, 2> retained_src_pose{};
	std::array<xrt_fov, 2> retained_src_fov{};
	std::array<float, 2> retained_src_near{1, 1};
	std::array<float, 2> retained_src_far{1000, 1000};

	// Last client app-frame timestamp seen on a fast-path commit, for
	// refresh gating: a changed timestamp means the commit carries a new
	// app frame whose color+depth must be retained. -1 until the first
	// fast-path commit. Atomic: set_framerate/resume on the session
	// thread reset it while layer_commit reads it.
	std::atomic<int64_t> last_app_layer_ts{-1};

	// Arrival time of the commit that last refreshed retained sources.
	// Bounds how stale a warp source may be (dead-app guard).
	std::atomic<uint64_t> last_retained_ns{0};
	// Snapshot of the retained clip planes for the warp-debug heartbeat
	// (which runs without the commit lock). Written alongside the
	// retained refresh, read by the 5 s heartbeat. Shows which near/far
	// the warps actually used — the producer constants are open-loop,
	// so the next live run must prove them against the app's projection.
	std::atomic<float> dbg_retained_near{1};
	std::atomic<float> dbg_retained_far{1000};

	// TEMPORARY rotation-only-group diagnostic (remove once pinned):
	// per-app-frame group summary of warp pose deltas. The headset
	// video shows every 2nd/3rd repeat group applying rotation only
	// (with holes) then jumping on the next real frame. Per-group
	// first/last src/dst positions decide between frozen-dst-position
	// (dst_d ~ 0 across the group: tracker stall) and healthy pose
	// deltas with nil image translation (flat depth). Updated on every
	// commit while warp_debug is on; dumped when the next fresh frame
	// closes the group. Commit path only (same serialization as the
	// other plain retained_* members).
	struct warp_group_dbg
	{
		bool active = false;
		int64_t app_ts = 0;
		int n_warp = 0;
		int n_straight = 0;
		int n_straight_nodepth = 0;
		int n_roto = 0;
		float dist_min = 1e9f;
		float dist_max = 0.f;
		float ang_min = 1e9f;
		float ang_max = 0.f;
		float dt_max_ms = 0.f;
		bool has_dst = false;
		xrt_pose first_src{};
		xrt_pose last_src{};
		xrt_pose first_dst{};
		xrt_pose last_dst{};
		long long first_tick = 0;
		long long last_tick = 0;
		uint64_t retained_age_ms = 0;
	};
	warp_group_dbg wgroup;

	// Alpha blend mode of the latest commit, published for the warp
	// path (it cannot safely read layer_accum mid-commit). Staleness is
	// harmless (blend mode changes only on app layer switches).
	std::atomic<bool> last_alpha_blend{false};

	// Last submitted timeline value. Host-visible UBO and descriptor
	// writes must not land while the GPU still reads them (recording
	// is serialized by commit_mutex, but execution is async), so the
	// commit path waits — bounded — for this value's work before
	// touching shared upload state. Set in submit_cmd_buffer.
	std::atomic<uint64_t> last_submit_value{0};

	std::array<std::unique_ptr<video_encoder>, 3> encoders;

	// Serializes the record path (reprojector, foveation UBO /
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

	void append_encoder_barriers(
	        beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 6> & barriers,
	        int image_idx,
	        const to_headset::video_stream_data_shard::view_info_t & info);

	// Submits a fully recorded command buffer on the compositor queue,
	// signaling the timeline semaphore and recording the value in
	// sem_value. Self-synchronizing via the queue mutex
	// (submit order is signal order).
	uint64_t submit_cmd_buffer(vk::raii::CommandBuffer & cmd);

	// Host-visible UBO / descriptor writes must not land while the GPU
	// still reads them from an in-flight submit (recording is
	// serialized by commit_mutex, execution is not). Bounded wait for the
	// last submitted work; false on timeout or error, in which case the
	// caller skips its warp (plain repeat / dropped tick) instead of
	// recording against in-flight state.
	bool wait_for_last_submit(uint64_t timeout_ns);

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
