// Minimal x264 wrapper (Phase 5a). Mirrors the Linux encoder's wire
// contract (ultrafast/zerolatency/main/ABR/Annex-B/SPS-PPS-per-IDR,
// ≤1400 B shards, IDR+config on the reliable channel) with two v1
// simplifications: BGRA pixels in (x264 converts internally, so the YUV
// compute shader is deferred), and whole-frame NAL assembly instead of
// the slice-raster reorder (zerolatency emits in order). No Monado here.

#pragma once

#include <openxr/openxr.h>

#include <x264.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace wivrn_macos
{

struct EncoderParams
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps = 90;
	uint32_t bitrate_bps = 20'000'000;
};

struct EncodedNal
{
	std::vector<uint8_t> bytes; // Annex-B (start codes included)
	bool is_config = false;     // SPS/PPS
	bool is_idr = false;        // contains an IDR slice
	bool is_last = false;       // last NAL of the frame
};

class X264Encoder
{
public:
	X264Encoder() = default;
	~X264Encoder();

	X264Encoder(const X264Encoder &) = delete;
	X264Encoder & operator=(const X264Encoder &) = delete;

	bool open(const EncoderParams & params);
	void close();

	// Feed one packed 32-bit frame (stride = width*4, channel order per
	// swap_rb: false = memory B,G,R,A, true = R,G,B,A). force_idr emits
	// an IDR (feedback NACK, bitrate change). Returns the frame's NALs
	// in decode order; empty only on encoder failure.
	std::vector<EncodedNal> feed(const uint8_t * rgba, bool swap_rb, bool force_idr);

	// Live bitrate/fps update (applies at the next frame + forces IDR,
	// mirroring the Linux reconfig path).
	void reconfig(uint32_t bitrate_bps, uint32_t fps);

	uint64_t frames_encoded() const
	{
		return frame_index_;
	}

private:
	static void nalu_callback(x264_t * handle, x264_nal_t * nal, void * opaque);
	void on_nal(x264_nal_t * nal);

	x264_t * encoder_ = nullptr;
	x264_picture_t * picture_ = nullptr;
	EncoderParams params_{};
	uint64_t frame_index_ = 0;
	std::mutex pending_mutex_;
	std::vector<EncodedNal> pending_;
	bool failed_ = false;
	// Owned NV12 frame (converted from BGRA in feed()).
	std::vector<uint8_t> yuv_;
};

// Packed 32-bit (memory order B,G,R,A or R,G,B,A per swap_rb) to NV12,
// integer BT.709 full-range with 2x2 chroma box average. v1 deliberately
// CPU-side (matches the Linux contract; a GPU conversion shader is the
// later optimization).
void convert_to_nv12(const uint8_t * rgba, uint32_t width, uint32_t height, bool swap_rb, uint8_t * yuv);

// Split a frame's NALs into ≤1400 B shards. First shard carries view
// info, last carries timing info (mirrors video_encoder::SendData).
// reliable() selects the TCP control socket (IDR/config) vs the stream.
struct FrameMeta
{
	uint64_t frame_idx = 0;
	XrTime display_time = 0;
	std::array<XrPosef, 2> pose{};
	std::array<XrFovf, 2> fov{};
	XrTime encode_begin = 0;
	XrTime encode_end = 0;
};

using ShardSink = std::function<void(uint8_t stream_idx, const std::vector<uint8_t> & bytes, bool first, bool last, bool reliable)>;

void shard_frame(uint8_t stream_idx, const std::vector<EncodedNal> & nals, const FrameMeta & meta, ShardSink sink);

} // namespace wivrn_macos
