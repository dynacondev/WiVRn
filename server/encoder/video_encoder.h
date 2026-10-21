/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "driver/clock_offset.h"
#include "idr_handler.h"
#include "wivrn_packets.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vulkan/vulkan_raii.hpp>

namespace wivrn
{

struct encoder_settings;
struct vk_bundle;
class wivrn_session;

inline const char * encoder_nvenc = "nvenc";
inline const char * encoder_vaapi = "vaapi";
inline const char * encoder_x264 = "x264";
inline const char * encoder_vulkan = "vulkan";
inline const char * encoder_raw = "raw";

class video_encoder
{
protected:
	struct data
	{
		video_encoder * encoder;
		std::span<uint8_t> span;
		std::shared_ptr<void> mem;
		// true if data should be sent over reliable (TCP) socket
		bool prefer_control = false;
	};

private:
	class sender
	{
		std::mutex mutex;
		std::condition_variable cv;
		std::deque<data> pending;
		std::jthread thread;
		void run();
		sender();

	public:
		void push(data &&);
		static std::shared_ptr<sender> get();
		void wait_idle(video_encoder *);
	};

public:
	const uint8_t stream_idx;
	const uint32_t target_queue;
	const bool need_transfer;
	// Layout the encoder needs y_cbcr to arrive in. Set during construction
	// or init() and read by the compositor on every layer_commit — must not
	// change after the first frame.
	vk::ImageLayout target_layout = vk::ImageLayout::eGeneral;
	// Outstanding present slots per stream: 1 real + 1 replacement synth
	// per commit, times 2 commits in flight. The compositor pool is sized
	// to match; acquire_image fails gracefully past it.
	static const uint8_t num_slots = 4;
	const double bitrate_multiplier;

private:
	using state_t = std::atomic_unsigned_lock_free::value_type;
	static const state_t idle = 0;
	static const state_t busy = 1;
	static const state_t skip = 2;
	std::array<std::atomic_unsigned_lock_free, num_slots> state = {idle, idle, idle, idle};
	// Slot allocator for presents: each present records its slot on the
	// queued frame and encode consumes exactly that slot, so the pairing
	// cannot diverge (no counters to drift apart across exceptions, drops
	// or skips). Atomic as belt-and-braces (both present paths call
	// present_image; today they are serialized by commit_mutex).
	std::atomic<uint8_t> present_cursor{0};

	std::mutex mutex;

	// temporary data
	wivrn_session * cnx;

	// shard to send
	to_headset::video_stream_data_shard shard;

	to_headset::video_stream_data_shard::timing_info_t timing_info;
	clock_offset clock;

	std::ofstream video_dump;

	std::shared_ptr<sender> shared_sender;

protected:
	std::atomic_uint32_t pending_bitrate;
	std::atomic<float> pending_framerate;
	std::unique_ptr<idr_handler> idr;
	const vk::Extent2D extent;

public:
	static std::unique_ptr<video_encoder> create(
	        wivrn::vk_bundle &,
	        const encoder_settings & settings,
	        uint8_t stream_idx);

	video_encoder(vk_bundle &, uint8_t stream_idx, uint32_t target_queue, const encoder_settings & settings, std::unique_ptr<idr_handler>, bool async_send);
	virtual ~video_encoder();

	// Presents the image on the next free slot and returns that slot,
	// or -1 after a bounded (1 s) wait if no slot frees (the caller
	// drops the tick). The caller records the slot on the queued frame;
	// encode() consumes exactly the recorded slot, so present/encode
	// stay paired across exceptions, drops and skips.
	int present_image(vk::Image y_cbcr,
	                  vk::SemaphoreSubmitInfo sem_info,
	                  uint64_t frame_index);

	void on_feedback(const from_headset::feedback &);
	void reset();

	// Release a slot whose present completed but whose frame will never
	// be queued (e.g. a later encoder's present threw and aborted the
	// commit). Prevents a leaked busy slot from wedging the compositor.
	// Safe: no encode can be in flight for a slot whose frame was never
	// queued.
	void release_slot(uint8_t slot)
	{
		state[slot] = idle;
		state[slot].notify_all();
	}

	// bitrate_bps is the bitrate for the whole stream
	// the encoder bitrate will be scaled accordingly
	void set_bitrate(uint32_t bitrate_bps);
	void set_framerate(float framerate);

	void encode(wivrn_session & cnx,
	            const to_headset::video_stream_data_shard::view_info_t & view_info,
	            uint64_t frame_index,
	            uint8_t slot);

protected:
	// called on present to submit command buffers for the image.
	virtual void present_image(vk::Image y_cbcr,
	                           vk::SemaphoreSubmitInfo sem_info,
	                           uint8_t slot,
	                           uint64_t frame_index) = 0;

	// called when command buffer finished executing
	virtual std::optional<data> encode(uint8_t slot, uint64_t frame_index) = 0;

	void SendData(std::span<uint8_t> data, bool end_of_frame, bool control = false);
};

} // namespace wivrn
