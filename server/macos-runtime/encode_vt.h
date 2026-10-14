// VideoToolbox H.264 backend (Phase H). Same wire contract as the x264
// path (Annex-B, SPS/PPS prepended per IDR, no B-frames, ABR): the
// client cannot tell which backend encoded a frame. Differences from
// x264: 32BGRA pixels in (no CPU NV12 conversion — VT converts
// internally), hardware encode, synchronous feed() via a per-frame
// semaphore wait on the encode callback. Non-Apple builds get a stub
// whose open() always fails (x264 remains the only backend there).

#pragma once

#include "encode.h" // EncoderParams, EncodedNal

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <semaphore>
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
	// order, empty on failure/timeout (caller drops the frame).
	std::vector<EncodedNal> feed(const uint8_t * rgba, bool swap_rb, bool force_idr);

	// Live bitrate/fps update (applies at the next frame; caller forces
	// the IDR like the x264 path).
	void reconfig(uint32_t bitrate_bps, uint32_t fps);

	uint64_t frames_encoded() const
	{
		return frame_index_;
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
	void on_output(OSStatus status, CMSampleBufferRef sample);
	// AVCC -> Annex-B into pending_; prepends cached SPS/PPS on IDRs
	// (re-extracted per IDR so mid-stream set changes survive).
	bool parse_sample(CMSampleBufferRef sample);
	bool extract_params(CMSampleBufferRef sample);
	bool set_property(CFStringRef key, CFTypeRef value, bool required);

	VTCompressionSessionRef session_ = nullptr;
	CVPixelBufferRef pool_px_ = nullptr; // reusable BGRA input (pool copy)
	EncoderParams params_{};
	uint64_t frame_index_ = 0;
	std::mutex mutex_;
	std::binary_semaphore done_{0};
	std::vector<EncodedNal> pending_;
	std::vector<uint8_t> sps_;
	std::vector<uint8_t> pps_;
	bool failed_ = false;
	bool need_sets_ = false; // IDR shipped without param sets: force IDR until they do
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
	bool is_open() const
	{
		return false;
	}
};

#endif

} // namespace wivrn_macos
