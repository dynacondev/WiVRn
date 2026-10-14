// VideoToolbox H.264 backend. See encode_vt.h for the contract.
// Apple-only TU (CMake adds it to Apple targets only).

#if defined(__APPLE__)

#include "encode_vt.h"

#include "runtime.h" // log()

#include <CoreVideo/CoreVideo.h>

#include <chrono>
#include <cstring>
#include <iterator>

namespace wivrn_macos
{
namespace
{

constexpr CMTimeScale kTimescale = 90000;
constexpr int64_t kFeedTimeoutMs = 500;
constexpr int32_t kMaxKeyFrameInterval = 3600;

CFNumberRef make_s32(int32_t v)
{
	return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
}

uint32_t read_be32(const uint8_t * p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void append_annexb_nal(std::vector<EncodedNal> & out, const uint8_t * data, size_t len, bool is_config, bool is_idr)
{
	EncodedNal nal;
	nal.bytes.reserve(len + 4);
	nal.bytes.push_back(0);
	nal.bytes.push_back(0);
	nal.bytes.push_back(0);
	nal.bytes.push_back(1);
	nal.bytes.insert(nal.bytes.end(), data, data + len);
	nal.is_config = is_config;
	nal.is_idr = is_idr;
	nal.is_last = false;
	out.push_back(std::move(nal));
}

} // namespace

VTEncoder::~VTEncoder()
{
	close();
}

bool VTEncoder::set_property(CFStringRef key, CFTypeRef value, bool required)
{
	OSStatus st = VTSessionSetProperty(session_, key, value);
	if (st != noErr && required)
	{
		log("vt: required property set failed (%d), aborting open", (int)st);
		return false;
	}
	return true;
}

bool VTEncoder::open(const EncoderParams & params)
{
	close();
	if (params.width == 0 || params.height == 0 || params.fps == 0)
		return false;
	if ((params.width & 1) != 0 || (params.height & 1) != 0)
		return false; // 4:2:0 needs even dimensions
	if (params.bitrate_bps == 0)
		return false;

	// Require hardware: without this the session can silently fall back
	// to software (defeating the phase). Failure -> x264 fallback.
	CFMutableDictionaryRef spec =
	        CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
	OSStatus st = VTCompressionSessionCreate(
	        kCFAllocatorDefault,
	        (int32_t)params.width,
	        (int32_t)params.height,
	        kCMVideoCodecType_H264,
	        spec,
	        nullptr, // pixel buffer attributes: we supply buffers per feed
	        nullptr, // compressed data allocator
	        &VTEncoder::output_callback,
	        this,
	        &session_);
	CFRelease(spec);
	if (st != noErr || session_ == nullptr)
	{
		log("vt: session create failed (%d), falling back", (int)st);
		session_ = nullptr;
		return false;
	}
	if (!set_property(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, true) ||
	    !set_property(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel, true) ||
	    !set_property(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, true))
	{
		close();
		return false;
	}
	{
		CFNumberRef fps = make_s32((int32_t)params.fps);
		bool ok = set_property(kVTCompressionPropertyKey_ExpectedFrameRate, fps, true);
		CFRelease(fps);
		if (!ok)
		{
			close();
			return false;
		}
	}
	{
		// NOTE: VT takes bits/sec (x264's i_bitrate is kbps — do not
		// divide here; 5000 bps once crushed output to ~100 B/frame).
		CFNumberRef br = make_s32((int32_t)params.bitrate_bps);
		bool ok = set_property(kVTCompressionPropertyKey_AverageBitRate, br, true);
		CFRelease(br);
		if (!ok)
		{
			close();
			return false;
		}
	}
	// No DataRateLimits: VT interprets it as a hard bytes-per-window
	// cap (unlike x264's VBV burst allowance) and crushes quality to
	// fit it. AverageBitRate + RealTime is the v1 rate control; add
	// smoothing back only if NAL spikes appear on real links.
	{
		CFNumberRef ki = make_s32(kMaxKeyFrameInterval);
		set_property(kVTCompressionPropertyKey_MaxKeyFrameInterval, ki, false);
		CFRelease(ki);
	}
	// MaxFrameDelayCount≈0 keeps queue depth minimal (zerolatency
	// equivalent); best-effort: absence must not fail open.
	{
		CFNumberRef md = make_s32(0);
		set_property(CFSTR("MaxFrameDelayCount"), md, false);
		CFRelease(md);
	}
	st = VTCompressionSessionPrepareToEncodeFrames(session_);
	if (st != noErr)
	{
		log("vt: prepare failed (%d), falling back", (int)st);
		close();
		return false;
	}
	// Confirm hardware (Create can succeed while settling for software
	// on some configurations despite the spec).
	CFTypeRef usingHW = nullptr;
	if (VTSessionCopyProperty(session_, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kCFAllocatorDefault, &usingHW) ==
	    noErr)
	{
		bool hw = usingHW == kCFBooleanTrue;
		if (usingHW != nullptr)
			CFRelease(usingHW);
		if (!hw)
		{
			log("vt: session is not hardware-accelerated, falling back");
			close();
			return false;
		}
	}
	// Reusable BGRA input buffer (pool copy: deterministic row stride
	// for the HW encoder; attachments tag the VUI-ish colorspace to
	// match the x264 path's 709/sRGB signaling).
	st = CVPixelBufferCreate(
	        kCFAllocatorDefault, (size_t)params.width, (size_t)params.height, kCVPixelFormatType_32BGRA, nullptr, &pool_px_);
	if (st != kCVReturnSuccess || pool_px_ == nullptr)
	{
		log("vt: pixel buffer create failed (%d), falling back", (int)st);
		close();
		return false;
	}
	params_ = params;
	frame_index_ = 0;
	need_sets_ = false;
	log("vt: hardware H.264 session open (%ux%u @%u, %u bps)", params.width, params.height, params.fps, params.bitrate_bps);
	return true;
}

void VTEncoder::close()
{
	if (session_ != nullptr)
	{
		// Drain (emits trailing callbacks into pending_, cleared below).
		VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
		VTCompressionSessionInvalidate(session_);
		CFRelease(session_);
		session_ = nullptr;
	}
	if (pool_px_ != nullptr)
	{
		CVPixelBufferRelease(pool_px_);
		pool_px_ = nullptr;
	}
	pending_.clear();
	sps_.clear();
	pps_.clear();
	failed_ = false;
	need_sets_ = false;
	// Drain any stale completion signal (drain callbacks post during
	// close with no waiter; the next session must not consume it).
	while (done_.try_acquire())
	{
	}
}

void VTEncoder::output_callback(void * refcon, void *, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
{
	VTEncoder * self = static_cast<VTEncoder *>(refcon);
	std::lock_guard lock(self->mutex_);
	if (self->session_ == nullptr)
		return; // closed mid-drain
	if (status != noErr || sample == nullptr)
		self->failed_ = true;
	else if (!self->parse_sample(sample))
		self->failed_ = true;
	self->done_.release();
}

bool VTEncoder::extract_params(CMSampleBufferRef sample)
{
	CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
	if (fmt == nullptr)
		return false;
	bool ok = true;
	for (int idx = 0; idx < 2; ++idx)
	{
		const uint8_t * ptr = nullptr;
		size_t len = 0;
		size_t count = 0;
		if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, (size_t)idx, &ptr, &len, &count, nullptr) != noErr ||
		    ptr == nullptr || len == 0)
		{
			ok = false;
			break;
		}
		std::vector<uint8_t> & dst = idx == 0 ? sps_ : pps_;
		dst.clear();
		dst.reserve(len + 4);
		dst.push_back(0);
		dst.push_back(0);
		dst.push_back(0);
		dst.push_back(1);
		dst.insert(dst.end(), ptr, ptr + len);
	}
	return ok;
}

bool VTEncoder::parse_sample(CMSampleBufferRef sample)
{
	CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
	if (data == nullptr)
		return false;
	size_t total = 0;
	char * base = nullptr;
	if (CMBlockBufferGetDataPointer(data, 0, nullptr, &total, &base) != noErr || base == nullptr || total < 5)
		return false;
	const uint8_t * ptr = reinterpret_cast<const uint8_t *>(base);
	std::vector<EncodedNal> frame;
	bool sawIdr = false;
	size_t off = 0;
	while (off + 4 <= total)
	{
		uint32_t nlen = read_be32(ptr + off);
		off += 4;
		// AVCC length sanity (4-byte sizes; zero/trailing garbage ends).
		if (nlen == 0 || nlen > total || off + nlen > total)
			return false;
		uint8_t type = ptr[off] & 0x1F;
		bool is_idr = type == 5;
		bool is_config = type == 7 || type == 8;
		sawIdr = sawIdr || is_idr;
		append_annexb_nal(frame, ptr + off, nlen, is_config, is_idr);
		off += nlen;
	}
	if (frame.empty())
		return false;
	if (sawIdr)
	{
		// (Re)extract per IDR so mid-stream set changes survive; missing
		// sets keep forcing IDR until they ship (decoder can't use bare
		// IDRs).
		if (!extract_params(sample) || sps_.empty() || pps_.empty())
		{
			need_sets_ = true;
			return false;
		}
		need_sets_ = false;
		EncodedNal sps, pps;
		sps.bytes = sps_;
		sps.is_config = true;
		pps.bytes = pps_;
		pps.is_config = true;
		pending_.push_back(std::move(sps));
		pending_.push_back(std::move(pps));
	}
	pending_.insert(pending_.end(), std::make_move_iterator(frame.begin()), std::make_move_iterator(frame.end()));
	return true;
}

std::vector<EncodedNal> VTEncoder::feed(const uint8_t * rgba, bool swap_rb, bool force_idr)
{
	std::vector<EncodedNal> empty;
	if (session_ == nullptr || pool_px_ == nullptr || rgba == nullptr)
		return empty;
	{
		std::lock_guard lock(mutex_);
		pending_.clear();
		failed_ = false;
	}
	force_idr = force_idr || need_sets_;
	// Pool copy into the reusable BGRA buffer (tight snapshot rows ->
	// pool rows; R/B lane swap for RGBA sources).
	CVPixelBufferLockBaseAddress(pool_px_, 0);
	uint8_t * dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pool_px_));
	size_t dstStride = CVPixelBufferGetBytesPerRow(pool_px_);
	uint32_t w = params_.width, h = params_.height;
	if (dst == nullptr)
	{
		CVPixelBufferUnlockBaseAddress(pool_px_, 0);
		return empty;
	}
	if (!swap_rb && dstStride == (size_t)w * 4)
	{
		std::memcpy(dst, rgba, (size_t)w * h * 4);
	}
	else
	{
		uint32_t r_lane = swap_rb ? 0 : 2;
		uint32_t b_lane = swap_rb ? 2 : 0;
		for (uint32_t y = 0; y < h; ++y)
		{
			const uint8_t * src = rgba + (size_t)y * w * 4;
			uint8_t * d = dst + (size_t)y * dstStride;
			for (uint32_t x = 0; x < w; ++x)
			{
				d[x * 4 + 0] = src[x * 4 + b_lane];
				d[x * 4 + 1] = src[x * 4 + 1];
				d[x * 4 + 2] = src[x * 4 + r_lane];
				d[x * 4 + 3] = src[x * 4 + 3];
			}
		}
	}
	CVPixelBufferUnlockBaseAddress(pool_px_, 0);

	CFDictionaryRef frameProps = nullptr;
	CFMutableDictionaryRef forceKey = nullptr;
	if (force_idr)
	{
		forceKey = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(forceKey, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
		frameProps = forceKey;
	}
	VTEncodeInfoFlags infoFlags = 0;
	OSStatus st = VTCompressionSessionEncodeFrame(
	        session_, pool_px_, CMTimeMake((int64_t)frame_index_, (int32_t)params_.fps), CMTimeMake(1, (int32_t)params_.fps), frameProps, nullptr, &infoFlags);
	if (forceKey != nullptr)
		CFRelease(forceKey);
	if (st != noErr)
	{
		log("vt: encode frame %llu failed (%d)", (unsigned long long)frame_index_, (int)st);
		return empty;
	}
	if (!done_.try_acquire_for(std::chrono::milliseconds(kFeedTimeoutMs)))
	{
		log("vt: encode frame %llu timed out", (unsigned long long)frame_index_);
		return empty;
	}
	std::lock_guard lock(mutex_);
	if (failed_ || pending_.empty())
	{
		if (failed_)
			log("vt: encode frame %llu callback failed", (unsigned long long)frame_index_);
		return empty;
	}
	pending_.back().is_last = true;
	frame_index_ += 1;
	return std::move(pending_);
}

void VTEncoder::reconfig(uint32_t bitrate_bps, uint32_t fps)
{
	if (session_ == nullptr || bitrate_bps == 0 || fps == 0)
		return;
	CFNumberRef br = make_s32((int32_t)bitrate_bps); // bits/sec (see open())
	set_property(kVTCompressionPropertyKey_AverageBitRate, br, false);
	CFRelease(br);
	CFNumberRef fr = make_s32((int32_t)fps);
	set_property(kVTCompressionPropertyKey_ExpectedFrameRate, fr, false);
	CFRelease(fr);
	params_.bitrate_bps = bitrate_bps;
	params_.fps = fps;
}

} // namespace wivrn_macos

#endif
