// Minimal x264 wrapper. Parameter choices mirror
// server/encoder/video_encoder_x264.cpp (ultrafast/zerolatency, main,
// 32 slices, Annex-B, repeat-headers, infinite keyint, ABR+VBV, BT.709
// VUI); only the pixel path differs (BGRA in, converted by x264).

#include "encode.h"

#include "encode_vt.h"
#include "runtime.h" // log()

#include <x264.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace wivrn_macos
{

// Out-of-line (VTEncoder complete here; header only forward-declares).
VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::open(const EncoderParams & params, VideoBackend backend)
{
	close();
	if (backend == VideoBackend::Auto)
	{
		if (const char * env = std::getenv("WIVRN_MACOS_VIDEO_ENCODER"))
		{
			if (std::strcmp(env, "x264") == 0)
				backend = VideoBackend::X264;
			else if (std::strcmp(env, "vt") == 0)
				backend = VideoBackend::VT;
		}
	}
	if (backend != VideoBackend::X264)
	{
		vt_ = std::make_unique<VTEncoder>();
		if (vt_->open(params))
		{
			use_vt_ = true;
			return true;
		}
		log("video-encoder: VT unavailable, falling back to x264");
		vt_.reset();
	}
	use_vt_ = false;
	return x264_.open(params);
}

void VideoEncoder::close()
{
	if (vt_)
	{
		vt_->close();
		vt_.reset();
	}
	x264_.close();
	use_vt_ = false;
}

std::vector<EncodedNal> VideoEncoder::feed(const uint8_t * rgba, bool swap_rb, bool force_idr)
{
	if (vt_)
		return vt_->feed(rgba, swap_rb, force_idr);
	return x264_.feed(rgba, swap_rb, force_idr);
}

void VideoEncoder::reconfig(uint32_t bitrate_bps, uint32_t fps)
{
	if (vt_)
		vt_->reconfig(bitrate_bps, fps);
	else
		x264_.reconfig(bitrate_bps, fps);
}

uint64_t VideoEncoder::frames_encoded() const
{
	if (vt_)
		return vt_->frames_encoded();
	return x264_.frames_encoded();
}

void convert_to_nv12(const uint8_t * rgba, uint32_t width, uint32_t height, bool swap_rb, uint8_t * yuv)
{
	uint8_t * y = yuv;
	uint8_t * uv = yuv + width * height;
	// Byte lane of R and B in memory order.
	const uint32_t r_lane = swap_rb ? 0 : 2;
	const uint32_t b_lane = swap_rb ? 2 : 0;
	for (uint32_t row = 0; row < height; ++row)
	{
		const uint8_t * src = rgba + row * width * 4;
		uint8_t * dstY = y + row * width;
		for (uint32_t col = 0; col < width; ++col)
		{
			int b = src[col * 4 + b_lane];
			int g = src[col * 4 + 1];
			int r = src[col * 4 + r_lane];
			int yy = (54 * r + 183 * g + 18 * b + 128) >> 8;
			dstY[col] = (uint8_t)(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
		}
		if ((row & 1) != 0)
		{
			uint8_t * dstUV = uv + (row / 2) * width;
			for (uint32_t col = 0; col < width; col += 2)
			{
				int r = 0, g = 0, b = 0;
				for (uint32_t dy = 0; dy < 2; ++dy)
				{
					const uint8_t * s = rgba + (row - 1 + dy) * width * 4 + col * 4;
					b += s[b_lane] + s[4 + b_lane];
					g += s[1] + s[5];
					r += s[r_lane] + s[4 + r_lane];
				}
				// Sums over 4 pixels: coefficients are x256/4 (>> 10).
				int cb = ((-29 * r - 99 * g + 128 * b) >> 10) + 128;
				int cr = ((128 * r - 116 * g - 12 * b) >> 10) + 128;
				dstUV[col + 0] = (uint8_t)(cb < 0 ? 0 : (cb > 255 ? 255 : cb));
				dstUV[col + 1] = (uint8_t)(cr < 0 ? 0 : (cr > 255 ? 255 : cr));
			}
		}
	}
}

X264Encoder::~X264Encoder()
{
	close();
}

bool X264Encoder::open(const EncoderParams & params)
{
	close();
	if (params.width == 0 || params.height == 0 || params.fps == 0)
		return false;
	if ((params.width & 1) != 0 || (params.height & 1) != 0)
		return false; // 4:2:0 needs even dimensions

	x264_param_t param{};
	if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0)
		return false;
	// Encoded bitstream is 4:2:0 (main profile). x264 has no
	// RGB->420 conversion on this path, so BGRA snapshots are converted
	// to NV12 on the CPU in feed() (integer BT.709 full-range); the YUV
	// compute shader is deferred accordingly.
	param.i_csp = X264_CSP_NV12;
	param.i_slice_count = 32;
	// Deterministic single-threaded encode: slice callbacks fire on the
	// calling thread, so pending_ needs no cross-thread sync and frames
	// cannot interleave. Throughput tuning (frame threads + locking)
	// waits for real headset measurements.
	param.i_threads = 1;
	param.i_width = (int)params.width;
	param.i_height = (int)params.height;
	param.i_log_level = X264_LOG_WARNING;
	param.i_fps_num = (int)params.fps;
	param.i_fps_den = 1;
	param.b_repeat_headers = 1;
	param.b_aud = 0;
	param.i_keyint_max = X264_KEYINT_MAX_INFINITE;
	param.vui.b_fullrange = 1;
	param.vui.i_colorprim = 1; // BT.709
	param.vui.i_colmatrix = 1;
	param.vui.i_transfer = 13; // sRGB
	param.vui.i_sar_width = (int)params.width;
	param.vui.i_sar_height = (int)params.height;
	param.rc.i_rc_method = X264_RC_ABR;
	param.rc.i_bitrate = (int)(params.bitrate_bps / 1000);
	param.rc.i_vbv_max_bitrate = (int)(params.bitrate_bps / 1000);
	param.rc.i_vbv_buffer_size = (int)(params.bitrate_bps / 1000 / params.fps * 1.1);
	if (x264_param_apply_profile(&param, "main") < 0)
		return false;
	param.nalu_process = &X264Encoder::nalu_callback;

	encoder_ = x264_encoder_open(&param);
	if (encoder_ == nullptr)
		return false;
	picture_ = new (std::nothrow) x264_picture_t;
	if (picture_ == nullptr)
	{
		close();
		return false;
	}
	std::memset(picture_, 0, sizeof(*picture_));
	// Input is converted BGRA -> NV12 below (x264 has no RGB->420 path
	// that main-profile output accepts); the picture always describes
	// our own NV12 buffers. Strides are width/width (tight).
	picture_->img.i_csp = X264_CSP_NV12;
	picture_->img.i_plane = 2;
	picture_->img.i_stride[0] = (int)params.width;
	picture_->img.i_stride[1] = (int)params.width;
	yuv_.assign(params.width * params.height * 3 / 2, 0);
	picture_->img.plane[0] = yuv_.data();
	picture_->img.plane[1] = yuv_.data() + params.width * params.height;
	params_ = params;
	frame_index_ = 0;
	return true;
}

void X264Encoder::close()
{
	if (encoder_ != nullptr)
	{
		// Drain (zerolatency emits nothing delayed, but be correct).
		x264_nal_t * nals = nullptr;
		int num = 0;
		x264_picture_t out{};
		while (x264_encoder_encode(encoder_, &nals, &num, nullptr, &out) > 0)
		{
		}
		x264_encoder_close(encoder_);
		encoder_ = nullptr;
	}
	delete picture_;
	picture_ = nullptr;
	pending_.clear();
}

void X264Encoder::nalu_callback(x264_t *, x264_nal_t * nal, void * opaque)
{
	static_cast<X264Encoder *>(opaque)->on_nal(nal);
}

void X264Encoder::on_nal(x264_nal_t * nal)
{
	// Keep config + slices only (SEI and friends are dropped like the
	// Linux path); Annex-B encode into a fresh buffer.
	if (nal->i_type != NAL_SLICE && nal->i_type != NAL_SLICE_DPA && nal->i_type != NAL_SLICE_DPB &&
	    nal->i_type != NAL_SLICE_DPC && nal->i_type != NAL_SLICE_IDR && nal->i_type != NAL_SPS && nal->i_type != NAL_PPS)
		return;
	std::vector<uint8_t> bytes(nal->i_payload * 3 / 2 + 64, 0);
	x264_nal_encode(encoder_, bytes.data(), nal);
	bytes.resize(nal->i_payload);
	EncodedNal out;
	out.bytes = std::move(bytes);
	out.is_config = nal->i_type == NAL_SPS || nal->i_type == NAL_PPS;
	out.is_idr = nal->i_type == NAL_SLICE_IDR;
	out.is_last = false; // fixed up in feed()
	std::lock_guard lock(pending_mutex_);
	pending_.push_back(std::move(out));
}

std::vector<EncodedNal> X264Encoder::feed(const uint8_t * rgba, bool swap_rb, bool force_idr)
{
	std::vector<EncodedNal> empty;
	if (encoder_ == nullptr || picture_ == nullptr || rgba == nullptr)
		return empty;
	pending_.clear();
	failed_ = false;
	convert_to_nv12(rgba, params_.width, params_.height, swap_rb, yuv_.data());
	picture_->i_pts = (int64_t)frame_index_;
	picture_->i_type = force_idr ? X264_TYPE_IDR : X264_TYPE_AUTO;
	// The nalu_process opaque is the INPUT picture's opaque (not the
	// param's, which is internal-only): route callbacks back to this.
	picture_->opaque = this;

	x264_nal_t * nals = nullptr;
	int num = 0;
	x264_picture_t pic_out{};
	if (x264_encoder_encode(encoder_, &nals, &num, picture_, &pic_out) < 0 || failed_)
	{
		log("x264: encode failed at frame %llu", (unsigned long long)frame_index_);
		return empty;
	}
	// Config NALs (SPS/PPS) precede slices in callback order already
	// (zerolatency emits in order; no raster reorder needed).
	if (!pending_.empty())
		pending_.back().is_last = true;
	frame_index_ += 1;
	return std::move(pending_);
}

void X264Encoder::reconfig(uint32_t bitrate_bps, uint32_t fps)
{
	if (encoder_ == nullptr || bitrate_bps == 0 || fps == 0)
		return;
	x264_param_t param{};
	x264_encoder_parameters(encoder_, &param);
	param.i_fps_num = (int)fps;
	param.i_fps_den = 1;
	param.rc.i_bitrate = (int)(bitrate_bps / 1000);
	param.rc.i_vbv_max_bitrate = (int)(bitrate_bps / 1000);
	param.rc.i_vbv_buffer_size = (int)(bitrate_bps / 1000 / fps * 1.1);
	x264_encoder_reconfig(encoder_, &param);
	params_.bitrate_bps = bitrate_bps;
	params_.fps = fps;
}

void shard_frame(uint8_t stream_idx, const std::vector<EncodedNal> & nals, const FrameMeta & meta, ShardSink sink)
{
	// Flatten the frame (Annex-B start codes delimit NALs; per-NAL
	// boundaries need not survive sharding) and split into ≤1400 B.
	// Config/IDR frames ride the reliable channel like the Linux path.
	constexpr size_t max_payload = 1400;
	bool reliable = false;
	size_t total = 0;
	for (const EncodedNal & nal: nals)
	{
		reliable = reliable || nal.is_config || nal.is_idr;
		total += nal.bytes.size();
	}
	if (total == 0)
		return;
	std::vector<uint8_t> flat;
	flat.reserve(total);
	for (const EncodedNal & nal: nals)
		flat.insert(flat.end(), nal.bytes.begin(), nal.bytes.end());

	size_t offset = 0;
	bool first = true;
	while (offset < flat.size())
	{
		size_t chunk = std::min(max_payload, flat.size() - offset);
		bool last = offset + chunk == flat.size();
		std::vector<uint8_t> payload(flat.begin() + offset, flat.begin() + offset + chunk);
		sink(stream_idx, payload, first, last, reliable);
		offset += chunk;
		first = false;
	}
	(void)meta;
}

} // namespace wivrn_macos
