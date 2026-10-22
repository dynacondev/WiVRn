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
	// the in-flight record. Timestamp pool exists only for warp-debug
	// runs (zero GPU overhead otherwise); results are collected
	// non-blocking at the next tick, so timing never adds latency.
	vk::raii::CommandPool synth_pool;
	vk::raii::CommandBuffer synth_cmd;
	vk::raii::QueryPool synth_query_pool;
	timings synth_warp_times;

	// Warp debug output (timing heartbeat + synth GPU timestamps): off in
	// prod. Enabled by WIVRN_WARP_DEBUG=1 (or REPROJEX_DEBUG=1, so one env
	// lights the whole depth pipeline). Read once at construction.
	const bool warp_debug;

	// Warp debug counters: touched on both the EndFrame path and the
	// synth ticker, hence relaxed atomics. The ticker emits a 5 s delta
	// heartbeat when warp_debug is on; steady-state cost is a few atomic
	// increments, zero logs.
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
		std::atomic<uint64_t> s_ticks{0};
		std::atomic<uint64_t> s_warped{0};
		std::atomic<uint64_t> s_covered{0};
		std::atomic<uint64_t> s_inactive{0};
		std::atomic<uint64_t> s_gate{0};
		std::atomic<uint64_t> s_degenerate{0};
		std::atomic<uint64_t> s_prewait{0};
		std::atomic<uint64_t> s_pool{0};
		std::atomic<uint64_t> s_present{0};
		std::atomic<uint64_t> s_lock{0};
		std::atomic<uint64_t> s_expected{0};
	};
	warp_counters wcounts;
	// Last collected warp GPU times (ns, any path): the heartbeat reports
	// them alongside the counters.
	std::atomic<uint64_t> last_commit_warp_ns{0};
	std::atomic<uint64_t> last_synth_warp_ns{0};
	// Last raw coverage delta (tick minus last output, before the
	// render-bias correction) and lock-contention count, for diagnosing
	// a coverage check that never fires.
	std::atomic<int64_t> last_raw_dt_ns{0};

	std::array<encoder_settings, 3> settings;

	// Compositor output images awaiting encode: 1 real + 1 replacement
	// synth per commit, times 2 commits in flight. Encoder slots
	// (num_slots per stream) are the outer bound; acquire_image fails
	// gracefully past it.
	std::array<image, 4> images;
	vk::raii::CommandBuffer cmd;
	vk::raii::Semaphore sem;
	// Monotonic timeline value, incremented per submit on either path.
	// Atomic: both paths submit without holding commit_mutex across the
	// queue lock (submit_cmd_buffer is self-synchronizing); the commit
	// path additionally holds commit_mutex when it submits.
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
	// commits, read by the synth ticker under the same commit lock.
	// Lets the ticker warp without a layer in hand.
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

	// Hysteresis latch for the stale-frame warp (see layer_commit):
	// plain bool would race set_framerate/resume on the session thread.
	std::atomic<bool> warp_armed_stale{false};

	// Arrival time of the previous layer_commit (monotonic ns). The warp
	// gate compares inter-commit spacing against the display period:
	// layer_commit runs once per app EndFrame, not per display tick, so
	// staleness cannot come from timestamps (consecutive commits always
	// carry fresh ones). Zero until the first commit. Atomic: the synth
	// ticker reads it while layer_commit writes it.
	std::atomic<uint64_t> last_commit_ns{0};
	// Smoothed inter-commit spacing (monotonic ns, EMA α=1/4). The synth
	// ticker predicts expected commit times from it (see expected-commit
	// skip): a pipelined app commits ~a period behind the working tick,
	// so "no output yet" does not mean "no output coming". Zero until
	// two commits observed. Written by layer_commit, read by the ticker.
	std::atomic<uint64_t> last_spacing_ns{0};

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
	// Snapshot of the retained clip planes for the warp-debug heartbeat
	// (which runs without the commit lock). Written alongside the
	// retained refresh, read by the 5 s heartbeat. Shows which near/far
	// the warps actually used — the producer constants are open-loop,
	// so the next live run must prove them against the app's projection.
	std::atomic<float> dbg_retained_near{1};
	std::atomic<float> dbg_retained_far{1000};

	// Alpha blend mode of the latest commit. The synth ticker cannot
	// safely read layer_accum (owned by the commit path), so layer_commit
	// publishes it here. Staleness is harmless (blend mode changes only
	// on app layer changes).
	std::atomic<bool> last_alpha_blend{false};

	// Last submitted timeline value (any path). Host-visible UBO and
	// descriptor writes must not land while the GPU still reads them
	// (recording is serialized by commit_mutex, but execution is async),
	// so both paths wait — bounded — for this value's work before
	// touching shared upload state. Set in submit_cmd_buffer.
	std::atomic<uint64_t> last_submit_value{0};

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
	        beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 6> & barriers,
	        int image_idx,
	        const to_headset::video_stream_data_shard::view_info_t & info);

	// Submits a fully recorded command buffer on the compositor queue,
	// signaling the timeline semaphore and recording the value in
	// sem_value (both paths). Self-synchronizing via the queue mutex
	// (submit order is signal order); callable with or without
	// commit_mutex.
	uint64_t submit_cmd_buffer(vk::raii::CommandBuffer & cmd);

	// Host-visible UBO / descriptor writes must not land while the GPU
	// still reads them from the other path's submit (recording is
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
