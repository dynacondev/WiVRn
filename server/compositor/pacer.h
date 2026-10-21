/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_packets.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "main/comp_target.h"
#include "util/u_var.h"

namespace wivrn
{

struct clock_offset;

class pacer
{
public:
	struct frame_info
	{
		int64_t frame_id;
		int64_t present_ns;
		int64_t predicted_display_time;
	};

private:
	mutable std::mutex mutex;
	int64_t frame_duration_ns;
	int64_t last_ns = 0;
	int64_t frame_id = 0;

	int64_t mean_wake_up_to_present_ns = 1'000'000;
	int64_t safe_present_to_decoded_ns = 0;
	int64_t mean_render_to_display_ns = 0;

	int64_t last_wake_up_ns = 0;

	u_var_draggable_f32 client_margin_ms;

	struct frame_time
	{
		int64_t frame_id = -1;
		XrTime present = 0;
		XrTime decoded = 0;
	};
	std::vector<frame_time> frame_times;

	std::mutex compute_mutex;
	std::condition_variable compute_cv;
	std::vector<frame_time> frame_times_compute;
	std::jthread worker;

	std::array<frame_info, 64> in_flight_frames;

public:
	pacer(uint64_t frame_duration);
	~pacer();

	uint64_t get_frame_duration() const;
	void set_frame_duration(uint64_t frame_duration);

	void predict(
	        int64_t & out_frame_id,
	        int64_t & out_wake_up_time_ns,
	        int64_t & out_desired_present_time_ns,
	        int64_t & out_present_slop_ns,
	        int64_t & out_predicted_display_time_ns);

	// Registers a server-synthesized tick (no app waitFrame behind it)
	// and returns its frame id. Participates in in_flight_frames,
	// feedback matching and phase tracking exactly like real
	// predictions, so the existing learners keep working uniformly.
	int64_t predict_synth(int64_t present_ns, int64_t predicted_display_time_ns);

	// Next display tick strictly after `after_ns`, on the display grid.
	// Grid phase comes from the feedback-locked last_ns, so no separate
	// phase tracking is needed (residual constant offset = constant
	// latency, not judder; the client's nearest-pick tolerates jitter).
	int64_t next_display_tick_ns(int64_t after_ns);

	// Pipeline lead for synth scheduling: present→decoded (learned p99)
	// + render→display (learned mean). Learned from client feedback like
	// everything else the pacer predicts.
	int64_t present_lead_ns() const;

	// Learned render→display latency (monotonic ns). The synth ticker
	// compares its grid ticks against queued outputs' display times;
	// both derive from the pacer's last_ns grid, offset by exactly this
	// mean — subtracting it re-centers the comparison so a learned
	// latency can't systematically cover or uncover ticks.
	int64_t learned_render_latency_ns() const;

	void on_feedback(const wivrn::from_headset::feedback &, const clock_offset &);

	void mark_timing_point(
	        comp_target_timing_point point,
	        int64_t frame_id,
	        int64_t when_ns);

	frame_info present_to_info(int64_t present);

	void reset();
};
} // namespace wivrn
