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

// Include first because of incompatibility between Eigen and X includes
#include "driver/wivrn_session.h"

#include "video_encoder.h"

#include "encoder_settings.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/wivrn_trace.h"
#include "wivrn_config.h"

#include <atomic>
#include <string>
#include <thread>

#if WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#if WIVRN_USE_VAAPI
#include "ffmpeg/video_encoder_va.h"
#endif
#if WIVRN_USE_X264
#include "video_encoder_x264.h"
#endif
#if WIVRN_USE_VULKAN_ENCODE
#include "video_encoder_vulkan_h264.h"
#include "video_encoder_vulkan_h265.h"
#endif
#include "video_encoder_raw.h"

namespace wivrn
{

video_encoder::sender::sender() :
        thread([this](std::stop_token t) {
	        while (not t.stop_requested())
	        {
		        data * d = nullptr;
		        {
			        std::unique_lock lock(mutex);
			        if (pending.empty())
				        cv.wait_for(lock, std::chrono::milliseconds(100));
			        else
			        {
				        if (pending.size() > 16)
				        {
					        // Video backing up: normally the queue drains in
					        // milliseconds. A deep queue means the headset
					        // stopped consuming (or the link stalled) — say so,
					        // throttled, while it is still recoverable.
					        uint64_t now_ns = os_monotonic_get_ns();
					        static std::atomic<uint64_t> last_depth_log_ns{0};
					        if (now_ns - last_depth_log_ns.load(std::memory_order_relaxed) > 5'000'000'000ull)
					        {
						        last_depth_log_ns.store(now_ns, std::memory_order_relaxed);
						        U_LOG_W("video sender: %zu packets queued (video backing up?)", pending.size());
					        }
				        }
				        d = &pending.front();
			        }
		        }
		        if (d)
		        {
			        if (not d->span.empty())
				        d->encoder->SendData(d->span, true, d->prefer_control);
			        else
			        {
				        // A zero-byte encode output carries no frame, but it
				        // must still be popped: leaving it at the head of the
				        // queue wedges this thread (and with it encoder_work
				        // in wait_idle and the compositor in present_image)
				        // forever, with no log anywhere.
				        U_LOG_W("video sender: dropping empty packet for stream %u", d->encoder->stream_idx);
			        }
			        {
				        std::unique_lock lock(mutex);
				        pending.pop_front();
				        cv.notify_all();
			        }
		        }
	        }
	        std::unique_lock lock(mutex);
	        pending.clear();
	        cv.notify_all();
        })
{
}

void video_encoder::sender::push(data && d)
{
	std::unique_lock lock(mutex);
	// Drop empty packets at the door: a zero-byte encode output carries
	// no frame, and must never reach the queue (see run()).
	if (d.span.empty())
		return;
	pending.push_back(std::move(d));
	cv.notify_all();
}

void video_encoder::sender::wait_idle(video_encoder * encoder)
{
	std::unique_lock lock(mutex);
	uint64_t stall_start_ns = 0;
	while (std::ranges::any_of(pending, [=](auto & data) { return data.encoder == encoder; }))
	{
		if (not stall_start_ns)
			stall_start_ns = os_monotonic_get_ns();
		cv.wait_for(lock, std::chrono::milliseconds(100));
		uint64_t now_ns = os_monotonic_get_ns();
		static std::atomic<uint64_t> last_idle_log_ns{0};
		if (now_ns - stall_start_ns > 5'000'000'000ull)
		{
			// The sender stopped draining our packets: the headset is
			// not consuming video (or the sender wedged). encoder_work
			// is parked here and the compositor will stall behind it —
			// say so, throttled, instead of dying silently. Past
			// another 5 s, proceed anyway: the queue is bounded by the
			// image pool, and parking forever helps nothing.
			if (now_ns - last_idle_log_ns.load(std::memory_order_relaxed) > 5'000'000'000ull)
			{
				last_idle_log_ns.store(now_ns, std::memory_order_relaxed);
				U_LOG_W("video sender: %zu packets pending for stream %u, encoder stalled %.1f s (headset not consuming video?)",
				        pending.size(), encoder->stream_idx, (now_ns - stall_start_ns) / 1e9);
			}
			if (now_ns - stall_start_ns > 10'000'000'000ull)
				break;
		}
	}
}

std::shared_ptr<video_encoder::sender> video_encoder::sender::get()
{
	static std::weak_ptr<video_encoder::sender> instance;
	static std::mutex m;
	std::unique_lock lock(m);
	auto s = instance.lock();
	if (s)
		return s;
	s.reset(new video_encoder::sender());
	instance = s;
	return s;
}

std::unique_ptr<video_encoder> video_encoder::create(
        wivrn::vk_bundle & wivrn_vk,
        const encoder_settings & settings,
        uint8_t stream_idx)
{
	using namespace std::string_literals;
	std::unique_ptr<video_encoder> res;
	if (settings.encoder_name == encoder_vulkan)
	{
#if WIVRN_USE_VULKAN_ENCODE
		switch (settings.codec)
		{
			case video_codec::h264:
				res = video_encoder_vulkan_h264::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::h265:
				res = video_encoder_vulkan_h265::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::av1:
				throw std::runtime_error("av1 not supported for vulkan video encode");
			case video_codec::raw:
				throw std::runtime_error("raw codec only supported on raw encoder");
		}
#else
		throw std::runtime_error("Vulkan video encode not enabled");
#endif
	}
	if (settings.encoder_name == encoder_x264)
	{
#if WIVRN_USE_X264
		res = std::make_unique<video_encoder_x264>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("x264 encoder not enabled");
#endif
	}
	if (settings.encoder_name == encoder_nvenc)
	{
#if WIVRN_USE_NVENC
		res = std::make_unique<video_encoder_nvenc>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("nvenc support not enabled");
#endif
	}
	if (settings.encoder_name == encoder_vaapi)
	{
#if WIVRN_USE_VAAPI
		res = std::make_unique<video_encoder_va>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("vaapi support not enabled");
#endif
	}

	if (settings.encoder_name == encoder_raw)
	{
		res = std::make_unique<video_encoder_raw>(wivrn_vk, settings, stream_idx);
	}

	if (not res)
		throw std::runtime_error("Failed to create encoder " + settings.encoder_name);

	auto wivrn_dump_video = std::getenv("WIVRN_DUMP_VIDEO");
	if (wivrn_dump_video)
	{
		std::string file(wivrn_dump_video);
		file += "-" + std::to_string(stream_idx);
		switch (settings.codec)
		{
			case h264:
				file += ".h264";
				break;
			case h265:
				file += ".h265";
				break;
			case av1:
				file += ".av1";
				break;
			case raw:
				file += ".yuv";
				break;
		}
		res->video_dump.open(file);
	}
	return res;
}

video_encoder::video_encoder(vk_bundle & vk,
                             uint8_t stream_idx,
                             uint32_t target_queue,
                             const encoder_settings & settings,
                             std::unique_ptr<idr_handler> idr,
                             bool async_send) :
        stream_idx(stream_idx),
        target_queue(target_queue),
        need_transfer(not vk.optimal_transfer(vk.queue.family_index, target_queue)),
        bitrate_multiplier(settings.bitrate_multiplier),
        shared_sender(async_send ? sender::get() : nullptr),
        idr(std::move(idr)),
        extent{
                .width = settings.width,
                .height = settings.height,
        }
{
	assert(this->idr);
}

video_encoder::~video_encoder()
{
	if (shared_sender)
		shared_sender->wait_idle(this);
}

void video_encoder::on_feedback(const from_headset::feedback & feedback)
{
	assert(feedback.stream_index == stream_idx);
	idr->on_feedback(feedback);
}

void video_encoder::reset()
{
	idr->reset();
}

void video_encoder::set_bitrate(uint32_t bitrate_bps)
{
	pending_bitrate = bitrate_bps * bitrate_multiplier;
}

void video_encoder::set_framerate(float framerate)
{
	pending_framerate = framerate;
}

int video_encoder::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint64_t frame_index)
{
	wivrn::trace::scope trace_present(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "present_image");
	// Wait for encoder to be done
	uint8_t slot = present_cursor.fetch_add(1) % num_slots;
	{
		// Poll instead of an unbounded atomic wait, bounded at 1 s: a
		// wedged encoder must drop the tick, never park the commit
		// thread. The slot is still not reused until idle, so the
		// healthy path is unchanged (the wait is normally zero).
		uint64_t stall_start_ns = 0;
		static std::atomic<uint64_t> last_present_drop_ns{0};
		while (state[slot].load() == busy)
		{
			if (not stall_start_ns)
				stall_start_ns = os_monotonic_get_ns();
			uint64_t now_ns = os_monotonic_get_ns();
			if (now_ns - stall_start_ns > 1'000'000'000ull)
			{
				uint64_t last_ns = last_present_drop_ns.load(std::memory_order_relaxed);
				if (now_ns - last_ns > 5'000'000'000ull)
				{
					last_present_drop_ns.store(now_ns, std::memory_order_relaxed);
					U_LOG_W("video encoder %u: no free slot for 1 s, dropping frame %lu (encoder wedged?)",
					        stream_idx, (unsigned long)frame_index);
				}
				return -1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	if (idr->should_skip(frame_index, stream_idx))
	{
		state[slot] = skip;
		return slot;
	}
	state[slot] = busy;
	try
	{
		present_image(y_cbcr, info, slot, frame_index);
	}
	catch (...)
	{
		// The present never reached the GPU, so no encode will ever
		// consume this slot: release it instead of leaking it busy
		// (a leaked slot wedges the compositor with zero logs), then
		// rethrow so the frame still fails loudly.
		state[slot] = idle;
		state[slot].notify_all();
		U_LOG_E("video encoder %u: present threw for frame %lu, slot %u released", stream_idx, (unsigned long)frame_index, slot);
		throw;
	}
	return slot;
}

void video_encoder::encode(wivrn_session & cnx,
                           const to_headset::video_stream_data_shard::view_info_t & view_info,
                           uint64_t frame_index,
                           uint8_t slot)
{
	struct idle_setter
	{
		std::atomic_unsigned_lock_free & state;
		~idle_setter()
		{
			state = idle;
			state.notify_all();
		}
	};
	idle_setter i{state[slot]};

	if (state[slot] == skip)
		return;

	if (shared_sender)
		shared_sender->wait_idle(this);
	this->cnx = &cnx;
	clock = cnx.get_offset();

	wivrn::trace::scope trace_encode(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "encode");
	auto encode_begin = os_monotonic_get_ns();
	timing_info = {
	        .encode_begin = clock.to_headset(encode_begin),
	};

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;
	shard.timing_info.reset();

	auto data = encode(slot, frame_index);
	if (data)
	{
		timing_info.encode_end = clock.to_headset(os_monotonic_get_ns());
		assert(shared_sender);
		shared_sender->push(std::move(*data));
	}
}

void video_encoder::SendData(std::span<uint8_t> data, bool end_of_frame, bool control)
{
	std::lock_guard lock(mutex);
	if (shard.shard_idx == 0)
	{
		// One SendData call per NAL; span the whole frame, not each call.
		wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
	}
	if (end_of_frame)
	{
		timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
		if (not timing_info.encode_end)
			timing_info.encode_end = timing_info.send_end;
	}
	if (video_dump)
		video_dump.write((char *)data.data(), data.size());

	ssize_t max_payload_size = (cnx->has_stream() and not control) ? to_headset::video_stream_data_shard::max_payload_size : std::numeric_limits<uint32_t>::max();

	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		const size_t payload_size = std::max(0z, max_payload_size - ssize_t(serialized_size(shard.view_info)));
		auto next = std::min(end, begin + payload_size);
		if (next == end)
		{
			if (end_of_frame)
			{
				if (serialized_size(timing_info) + std::distance(begin, next) <= max_payload_size)
					shard.timing_info = timing_info;
				else
					--next;
			}
		}
		shard.payload = {begin, next};
		try
		{
			if (control)
				cnx->send_control(to_headset::video_stream_data_shard{shard});
			else
				cnx->send_stream(to_headset::video_stream_data_shard{shard});
		}
		catch (...)
		{
			// Ignore network errors
		}
		++shard.shard_idx;
		shard.view_info.reset();
		begin = next;
	}
	if (end_of_frame)
		wivrn::trace::cpu_end(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
}

} // namespace wivrn
