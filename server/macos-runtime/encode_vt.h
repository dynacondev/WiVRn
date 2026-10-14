// VideoToolbox H.264 backend (Phase H). Same wire contract as the x264
// path (Annex-B, SPS/PPS prepended per IDR, no B-frames, ABR): the
// client cannot tell which backend encoded a frame. Differences from
// x264: 32BGRA pixels in (no CPU NV12 conversion — VT converts
// internally), hardware encode, synchronous feed() via a per-frame
// condition wait on the encode callback. Non-Apple builds get a stub
// whose open() always fails (x264 remains the only backend there).
//
// Encode-resilience knobs (all read from the environment at open(),
// all new behavior ON by default, each revertible; unset == default):
//   WIVRN_MACOS_VT_IDR_EVERY=N    periodic IDR every N feeds (default
//                                 120, ~1.3 s at 90 fps; 0 = off, forced
//                                 IDRs/NACK recovery unaffected)
//   WIVRN_MACOS_VT_IDR_ON_DROP=0  disable recovery IDR after a locally
//                                 dropped frame (default: drop forces the
//                                 next frame to IDR — a dropped frame
//                                 desyncs the decoder like packet loss)
//   WIVRN_MACOS_VT_FAST=0         disable PrioritizeEncodingSpeed hint
//                                 (default: speed prioritized)
//   WIVRN_MACOS_VT_COLOR=0        disable x264-matching VUI signaling
//                                 (default: 709 primaries/matrix, sRGB
//                                 transfer, full-range source tag)
//   WIVRN_MACOS_VT_FPS=N          override the ExpectedFrameRate hint
//                                 (default 0 = params fps from the headset
//                                 refresh; the PTS timescale always stays
//                                 params fps)
//   WIVRN_MACOS_VT_SLICE_BYTES=N  best-effort MaxH264SliceBytes (default
//                                 0 = off; transport has no per-slice
//                                 recovery, so this stays opt-in)
//   WIVRN_MACOS_VT_FEED_TIMEOUT_MS  callback wait bound (default 500;
//                                 diagnostic lever, 0 forces timeouts)
//   WIVRN_MACOS_VT_BENCH=1        per-feed timing log (enqueue/wait/total
//                                 key=value; default off — measurement only,
//                                 no behavior change)

#pragma once

#include "encode.h" // EncoderParams, EncodedNal

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#endif

namespace wivrn_macos
{

#if defined(__APPLE__)

class VTEncoder
{
public:
	VTEncoder() = default;
	~VTEncoder();

	VTEncoder(const VTEncoder &) = delete;
	VTEncoder & operator=(const VTEncoder &) = delete;

	// Opens a hardware H.264 session. Returns false (caller falls back
	// to x264) when HW encode is unavailable, dims are odd, or setup
	// fails. Even dimensions required (4:2:0).
	bool open(const EncoderParams & params);
	void close();

	// Feed one packed 32-bit frame (stride = width*4, channel order per
	// swap_rb: false = memory B,G,R,A, true = R,G,B,A). Blocks (bounded)
	// for the encode callback; returns the frame's NALs in decode
	// order, empty on failure/timeout (caller drops the frame). Every
	// accepted submit consumes exactly one PTS value, so timestamps
	// never repeat — even across timeouts and callback failures.
	std::vector<EncodedNal> feed(const uint8_t * rgba, bool swap_rb, bool force_idr);

	// Live bitrate/fps update (applies at the next frame; caller forces
	// the IDR like the x264 path).
	void reconfig(uint32_t bitrate_bps, uint32_t fps);

	uint64_t frames_encoded() const
	{
		return encoded_.load(std::memory_order_relaxed);
	}

	// Resilience accounting (monotonic per open; submitted ==
	// encoded + dropped always holds).
	uint64_t frames_submitted() const
	{
		return submitted_.load(std::memory_order_relaxed);
	}
	uint64_t frames_dropped() const
	{
		return dropped_.load(std::memory_order_relaxed);
	}
	// Callbacks that arrived after their waiter left (stale, safely
	// discarded by sequence match — never attributed to a later frame).
	uint64_t late_callbacks() const
	{
		return late_.load(std::memory_order_relaxed);
	}
	// Effective periodic-IDR interval (0 = off) and whether the
	// x264-matching color triple was accepted by the HW encoder.
	uint32_t idr_every() const
	{
		return idr_every_;
	}
	bool color_signaled() const
	{
		return color_ok_.load(std::memory_order_relaxed);
	}
	// Per-feed timing split (feed thread only): EncodeFrame call cost
	// (enqueue) vs callback wait (HW queue + service) vs whole feed
	// (pool copy + enqueue + wait). Frames that drop never produce a
	// callback, so they are excluded: submitted == timed + dropped
	// always holds. Whether the per-feed bench log is on.
	uint64_t feeds_timed() const
	{
		return feeds_timed_;
	}
	uint64_t enqueue_ns_total() const
	{
		return enqueue_ns_total_;
	}
	uint64_t wait_ns_total() const
	{
		return wait_ns_total_;
	}
	uint64_t feed_ns_total() const
	{
		return feed_ns_total_;
	}
	uint64_t feed_ns_min() const
	{
		return feed_ns_min_;
	}
	uint64_t feed_ns_max() const
	{
		return feed_ns_max_;
	}
	uint64_t last_enqueue_us() const
	{
		return last_enqueue_us_;
	}
	uint64_t last_wait_us() const
	{
		return last_wait_us_;
	}
	uint64_t last_total_us() const
	{
		return last_total_us_;
	}
	bool bench_enabled() const
	{
		return bench_;
	}

	// True while a session is open (tests + backend selection).
	bool is_open() const
	{
		return session_ != nullptr;
	}

private:
	static void output_callback(
	        void * refcon,
	        void * sourceFrameRefCon,
	        OSStatus status,
	        VTEncodeInfoFlags infoFlags,
	        CMSampleBufferRef sample);
	void on_output(uint64_t seq, OSStatus status, CMSampleBufferRef sample);
	// AVCC -> Annex-B into pending_; prepends cached SPS/PPS on IDRs
	// (re-extracted per IDR so mid-stream set changes survive).
	bool parse_sample(CMSampleBufferRef sample);
	bool extract_params(CMSampleBufferRef sample);
	bool set_property(CFStringRef key, CFTypeRef value, bool required);
	OSStatus try_property(CFStringRef key, CFTypeRef value);
	void verify_fps(uint32_t requested);
	void probe_slice_support();

	VTCompressionSessionRef session_ = nullptr;
	CVPixelBufferRef pool_px_ = nullptr; // reusable BGRA input (pool copy)
	EncoderParams params_{};
	// Next PTS value (feed thread only; +1 per accepted submit, never
	// reused, so a retried frame can never collide with its original).
	uint64_t next_pts_ = 0;
	std::atomic<uint64_t> submitted_{0};
	std::atomic<uint64_t> encoded_{0};
	std::atomic<uint64_t> dropped_{0};
	std::atomic<uint64_t> late_{0};
	std::atomic<bool> color_ok_{false};
	std::mutex mutex_;
	std::condition_variable cv_;
	bool has_waiter_ = false; // a feed is parked waiting for wait_seq_
	uint64_t wait_seq_ = 0;
	bool done_ = false;
	std::vector<EncodedNal> pending_;
	std::vector<uint8_t> sps_;
	std::vector<uint8_t> pps_;
	bool failed_ = false;
	bool need_sets_ = false; // IDR shipped without param sets: force IDR until they do
	// Periodic-IDR pacing (feed thread only).
	uint32_t idr_every_ = 0;
	uint64_t frames_since_idr_ = 0;
	bool drop_recover_ = false; // a drop desyncs the decoder: next feed IDRs
	// Env-configured behavior (read at open()).
	uint32_t feed_timeout_ms_ = 500;
	bool idr_on_drop_ = true;
	bool want_speed_ = true;
	bool want_color_ = true;
	uint32_t fps_override_ = 0;
	uint32_t slice_bytes_ = 0;
	bool bench_ = false; // WIVRN_MACOS_VT_BENCH=1: per-feed key=value log
	// Per-feed timing split (feed thread only; see the getters).
	uint64_t feeds_timed_ = 0;
	uint64_t enqueue_ns_total_ = 0;
	uint64_t wait_ns_total_ = 0;
	uint64_t feed_ns_total_ = 0;
	uint64_t feed_ns_min_ = 0;
	uint64_t feed_ns_max_ = 0;
	uint64_t last_enqueue_us_ = 0;
	uint64_t last_wait_us_ = 0;
	uint64_t last_total_us_ = 0;
};

#else

// Non-Apple stub: open() always fails so the facade falls back to x264.
class VTEncoder
{
public:
	bool open(const EncoderParams &)
	{
		return false;
	}
	void close()
	{
	}
	std::vector<EncodedNal> feed(const uint8_t *, bool, bool)
	{
		return {};
	}
	void reconfig(uint32_t, uint32_t)
	{
	}
	uint64_t frames_encoded() const
	{
		return 0;
	}
	uint64_t frames_submitted() const
	{
		return 0;
	}
	uint64_t frames_dropped() const
	{
		return 0;
	}
	uint64_t late_callbacks() const
	{
		return 0;
	}
	uint32_t idr_every() const
	{
		return 0;
	}
	bool color_signaled() const
	{
		return false;
	}
	bool is_open() const
	{
		return false;
	}
};

#endif

} // namespace wivrn_macos
