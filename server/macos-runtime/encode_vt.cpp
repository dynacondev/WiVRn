// VideoToolbox H.264 backend. See encode_vt.h for the contract.
// Apple-only TU (CMake adds it to Apple targets only).

#if defined(__APPLE__)

#include "encode_vt.h"

#include "runtime.h" // log()

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace wivrn_macos
{
namespace
{

constexpr CMTimeScale kTimescale = 90000;
constexpr uint32_t kDefaultIdrEvery = 120; // ~1.3 s at 90 fps
constexpr uint32_t kDefaultFeedTimeoutMs = 500;
constexpr int32_t kMaxKeyFrameInterval = 3600; // VT backstop; the periodic IDR fires first

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

uint32_t env_u32(const char * name, uint32_t fallback, uint32_t maxv)
{
	if (const char * value = std::getenv(name))
	{
		char * end = nullptr;
		unsigned long parsed = std::strtoul(value, &end, 10);
		if (end != value && parsed <= maxv)
			return (uint32_t)parsed;
	}
	return fallback;
}

bool env_bool(const char * name, bool fallback)
{
	if (const char * value = std::getenv(name))
	{
		if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "off") == 0 ||
		    std::strcmp(value, "no") == 0)
			return false;
		return true;
	}
	return fallback;
}

uint64_t now_ns()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}

} // namespace

VTEncoder::~VTEncoder()
{
	close();
}

OSStatus VTEncoder::try_property(CFStringRef key, CFTypeRef value)
{
	return VTSessionSetProperty(session_, key, value);
}

bool VTEncoder::set_property(CFStringRef key, CFTypeRef value, bool required)
{
	OSStatus st = try_property(key, value);
	if (st != noErr && required)
	{
		log("vt: required property set failed (%d), aborting open", (int)st);
		return false;
	}
	return true;
}

// Read back ExpectedFrameRate after each set: the hint drives VT's
// rate-control/energy planning, so a coerced value is logged rather
// than silently trusted.
void VTEncoder::verify_fps(uint32_t requested)
{
	CFTypeRef value = nullptr;
	int32_t actual = -1;
	if (VTSessionCopyProperty(session_, kVTCompressionPropertyKey_ExpectedFrameRate, kCFAllocatorDefault, &value) == noErr &&
	    value != nullptr)
	{
		if (CFGetTypeID(value) == CFNumberGetTypeID())
			CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &actual);
		CFRelease(value);
	}
	if (actual != (int32_t)requested)
		log("vt: fps hint mismatch (requested=%u effective=%d)", requested, (int)actual);
}

// One-shot probe at open(): documents whether this machine's HW
// encoder gates MaxH264SliceBytes, without changing any default.
void VTEncoder::probe_slice_support()
{
	CFDictionaryRef supported = nullptr;
	if (VTSessionCopySupportedPropertyDictionary(session_, &supported) != noErr || supported == nullptr)
	{
		log("vt: slice support probe unavailable");
		return;
	}
	bool present = CFDictionaryContainsKey(supported, kVTCompressionPropertyKey_MaxH264SliceBytes);
	log("vt: slice support (max_slice_bytes_supported=%d)", present ? 1 : 0);
	CFRelease(supported);
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

	idr_every_ = env_u32("WIVRN_MACOS_VT_IDR_EVERY", kDefaultIdrEvery, 3600);
	feed_timeout_ms_ = env_u32("WIVRN_MACOS_VT_FEED_TIMEOUT_MS", kDefaultFeedTimeoutMs, 10000);
	idr_on_drop_ = env_bool("WIVRN_MACOS_VT_IDR_ON_DROP", true);
	want_speed_ = env_bool("WIVRN_MACOS_VT_FAST", true);
	want_color_ = env_bool("WIVRN_MACOS_VT_COLOR", true);
	fps_override_ = env_u32("WIVRN_MACOS_VT_FPS", 0, 1000);
	slice_bytes_ = env_u32("WIVRN_MACOS_VT_SLICE_BYTES", 0, 65535);
	bench_ = env_bool("WIVRN_MACOS_VT_BENCH", false);

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
		// The hint must match the cadence VT actually sees: params fps
		// comes from the headset refresh, with an env override for
		// rigs whose per-eye cadence differs. The PTS timescale below
		// always stays params fps (mid-stream timescale jumps would
		// skew every later timestamp).
		uint32_t hint = fps_override_ > 0 ? fps_override_ : params.fps;
		CFNumberRef fps = make_s32((int32_t)hint);
		bool ok = set_property(kVTCompressionPropertyKey_ExpectedFrameRate, fps, true);
		CFRelease(fps);
		if (!ok)
		{
			close();
			return false;
		}
		verify_fps(hint);
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
	// Latency-first hint: the HW encoder otherwise spends extra passes
	// refining decisions at this bitrate. Best-effort (not all
	// encoders implement it); WIVRN_MACOS_VT_FAST=0 reverts.
	if (want_speed_)
	{
		OSStatus st = try_property(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
		log("vt: speed hint (prioritize_encoding_speed=%d)", st == noErr ? 1 : 0);
	}
	// x264-parity VUI (encode.cpp: primaries/matrix 709, sRGB transfer,
	// full-range): without these VT emits unspecified colorimetry and
	// the two backends decode differently on the client. Best-effort —
	// a rejection is logged, never fatal. The HW encoder may only
	// accept the 709 transfer, so sRGB falls back to 709 (still fully
	// specified, and what the client forces on decode).
	bool color_ok = false;
	if (want_color_)
	{
		OSStatus s_prim = try_property(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
		OSStatus s_tr = try_property(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_sRGB);
		if (s_tr != noErr)
			s_tr = try_property(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
		OSStatus s_mx = try_property(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
		color_ok = s_prim == noErr && s_tr == noErr && s_mx == noErr;
		log("vt: color (primaries=%d transfer=%d matrix=%d signaled=%d)",
		    (int)s_prim,
		    (int)s_tr,
		    (int)s_mx,
		    color_ok ? 1 : 0);
	}
	color_ok_.store(color_ok, std::memory_order_relaxed);
	// Slice ceiling: opt-in only (WIVRN_MACOS_VT_SLICE_BYTES). The v1
	// transport shards flattened frames with no per-slice recovery, so
	// slices alone cannot bound loss spread — periodic IDRs do that.
	// Kept as a lever for links that need bounded NAL sizes.
	if (slice_bytes_ > 0)
	{
		CFNumberRef sb = make_s32((int32_t)slice_bytes_);
		OSStatus st = try_property(kVTCompressionPropertyKey_MaxH264SliceBytes, sb);
		CFRelease(sb);
		log("vt: slices (max_bytes=%u applied=%d)", slice_bytes_, st == noErr ? 1 : 0);
	}
	probe_slice_support();
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
	// for the HW encoder). Source color attachments tag the conversion
	// VT performs internally, matching the x264 path's 709/sRGB input
	// assumptions; full-range because BGRA pixels are full-range.
	st = CVPixelBufferCreate(
	        kCFAllocatorDefault, (size_t)params.width, (size_t)params.height, kCVPixelFormatType_32BGRA, nullptr, &pool_px_);
	if (st != kCVReturnSuccess || pool_px_ == nullptr)
	{
		log("vt: pixel buffer create failed (%d), falling back", (int)st);
		close();
		return false;
	}
	if (want_color_)
	{
		CVBufferSetAttachment(
		        pool_px_, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
		CVBufferSetAttachment(
		        pool_px_, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB, kCVAttachmentMode_ShouldPropagate);
		CVBufferSetAttachment(
		        pool_px_, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
		CVBufferSetAttachment(
		        pool_px_, kCMFormatDescriptionExtension_FullRangeVideo, kCFBooleanTrue, kCVAttachmentMode_ShouldPropagate);
	}
	params_ = params;
	next_pts_ = 0;
	submitted_.store(0, std::memory_order_relaxed);
	encoded_.store(0, std::memory_order_relaxed);
	dropped_.store(0, std::memory_order_relaxed);
	late_.store(0, std::memory_order_relaxed);
	feeds_timed_ = 0;
	enqueue_ns_total_ = 0;
	wait_ns_total_ = 0;
	feed_ns_total_ = 0;
	feed_ns_min_ = 0;
	feed_ns_max_ = 0;
	last_enqueue_us_ = 0;
	last_wait_us_ = 0;
	last_total_us_ = 0;
	frames_since_idr_ = 0;
	drop_recover_ = false;
	need_sets_ = false;
	log("vt: hardware H.264 session open (%ux%u @%u, %u bps, idr_every=%u, timeout_ms=%u)",
	    params.width,
	    params.height,
	    params.fps,
	    params.bitrate_bps,
	    idr_every_,
	    feed_timeout_ms_);
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
	{
		std::lock_guard lock(mutex_);
		has_waiter_ = false;
		done_ = false;
		pending_.clear();
		sps_.clear();
		pps_.clear();
		failed_ = false;
		need_sets_ = false;
		drop_recover_ = false;
	}
	cv_.notify_all();
}

void VTEncoder::output_callback(void * refcon, void * sourceFrameRefCon, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
{
	VTEncoder * self = static_cast<VTEncoder *>(refcon);
	self->on_output((uint64_t)(uintptr_t)sourceFrameRefCon, status, sample);
}

void VTEncoder::on_output(uint64_t seq, OSStatus status, CMSampleBufferRef sample)
{
	std::unique_lock lock(mutex_);
	if (session_ == nullptr)
		return; // closed mid-drain
	// Sequence match: a callback arriving after its feed timed out (or
	// from an earlier session generation) must never release, or be
	// attributed to, a later frame's wait. Count and discard it.
	if (!has_waiter_ || seq != wait_seq_)
	{
		late_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (status != noErr || sample == nullptr)
		failed_ = true;
	else if (!parse_sample(sample))
		failed_ = true;
	done_ = true;
	lock.unlock();
	cv_.notify_one();
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
	// Periodic IDR bounds post-loss smearing to idr_every_ frames even
	// when no NACK arrives; a locally dropped frame forces the next
	// feed to IDR (the decoder never saw it, exactly like packet loss).
	// The +1 keeps the cadence an exact period: with N=5, four P-frames
	// then an IDR (frames_since_idr_ counts trailing non-IDRs).
	bool periodic = idr_every_ > 0 && frames_since_idr_ + 1 >= idr_every_;
	force_idr = force_idr || need_sets_ || drop_recover_ || periodic;
	// Pool copy into the reusable BGRA buffer (tight snapshot rows ->
	// pool rows; R/B lane swap for RGBA sources).
	uint64_t copy_start_ns = now_ns();
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

	// The sequence doubles as the PTS: one value per accepted submit,
	// advanced below on every post-submit exit, so timestamps can never
	// repeat (a retry never reuses its original PTS).
	uint64_t seq = next_pts_;
	{
		std::unique_lock lock(mutex_);
		has_waiter_ = true;
		wait_seq_ = seq;
		done_ = false;
	}
	CFDictionaryRef frameProps = nullptr;
	CFMutableDictionaryRef forceKey = nullptr;
	if (force_idr)
	{
		forceKey = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(forceKey, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
		frameProps = forceKey;
	}
	VTEncodeInfoFlags infoFlags = 0;
	uint64_t enc_call_ns = now_ns();
	OSStatus st = VTCompressionSessionEncodeFrame(
	        session_,
	        pool_px_,
	        CMTimeMake((int64_t)seq, (int32_t)params_.fps),
	        CMTimeMake(1, (int32_t)params_.fps),
	        frameProps,
	        (void *)(uintptr_t)seq,
	        &infoFlags);
	uint64_t wait_start_ns = now_ns();
	if (forceKey != nullptr)
		CFRelease(forceKey);
	if (st != noErr)
	{
		// Never submitted: no PTS consumed, safe to retry with the
		// same value next feed.
		std::lock_guard lock(mutex_);
		has_waiter_ = false;
		log("vt: encode frame %llu failed (%d)", (unsigned long long)seq, (int)st);
		return empty;
	}
	submitted_.fetch_add(1, std::memory_order_relaxed);
	bool arrived = false;
	{
		std::unique_lock lock(mutex_);
		arrived = cv_.wait_for(lock, std::chrono::milliseconds(feed_timeout_ms_), [this] { return done_; });
		has_waiter_ = false;
	}
	uint64_t wait_end_ns = now_ns();
	// The PTS is consumed whatever happened: the frame entered the
	// session, so the next submit must advance past it.
	next_pts_ = seq + 1;
	if (!arrived)
	{
		// The late callback (if any) is discarded by sequence match in
		// on_output; the decoder gap is closed by a recovery IDR.
		dropped_.fetch_add(1, std::memory_order_relaxed);
		drop_recover_ = idr_on_drop_;
		frames_since_idr_ += 1;
		log("vt: frame dropped (reason=timeout pts=%llu submitted=%llu dropped=%llu)",
		    (unsigned long long)seq,
		    (unsigned long long)submitted_.load(std::memory_order_relaxed),
		    (unsigned long long)dropped_.load(std::memory_order_relaxed));
		return empty;
	}
	std::unique_lock lock(mutex_);
	if (failed_ || pending_.empty())
	{
		lock.unlock();
		dropped_.fetch_add(1, std::memory_order_relaxed);
		drop_recover_ = idr_on_drop_;
		frames_since_idr_ += 1;
		log("vt: frame dropped (reason=callback pts=%llu submitted=%llu dropped=%llu)",
		    (unsigned long long)seq,
		    (unsigned long long)submitted_.load(std::memory_order_relaxed),
		    (unsigned long long)dropped_.load(std::memory_order_relaxed));
		return empty;
	}
	pending_.back().is_last = true;
	bool saw_idr = std::any_of(pending_.begin(), pending_.end(), [](const EncodedNal & nal) { return nal.is_idr; });
	// Timing split: the EncodeFrame call only enqueues (returns once the
	// frame is queued), the condition wait covers the HW queue + service.
	// VT exposes no finer split — enqueue vs wait is the separable pair.
	uint64_t enqueue_ns = wait_start_ns >= enc_call_ns ? wait_start_ns - enc_call_ns : 0;
	uint64_t wait_ns = wait_end_ns >= wait_start_ns ? wait_end_ns - wait_start_ns : 0;
	uint64_t total_ns = wait_end_ns >= copy_start_ns ? wait_end_ns - copy_start_ns : 0;
	feeds_timed_ += 1;
	enqueue_ns_total_ += enqueue_ns;
	wait_ns_total_ += wait_ns;
	feed_ns_total_ += total_ns;
	if (feeds_timed_ == 1 || total_ns < feed_ns_min_)
		feed_ns_min_ = total_ns;
	if (total_ns > feed_ns_max_)
		feed_ns_max_ = total_ns;
	last_enqueue_us_ = enqueue_ns / 1000;
	last_wait_us_ = wait_ns / 1000;
	last_total_us_ = total_ns / 1000;
	if (bench_)
	{
		size_t nbytes = 0;
		for (const EncodedNal & nal: pending_)
			nbytes += nal.bytes.size();
		log("vt bench: feed=%llu enqueue_us=%llu wait_us=%llu total_us=%llu bytes=%zu idr=%d",
		    (unsigned long long)seq,
		    (unsigned long long)(enqueue_ns / 1000),
		    (unsigned long long)(wait_ns / 1000),
		    (unsigned long long)(total_ns / 1000),
		    nbytes,
		    saw_idr ? 1 : 0);
	}
	if (saw_idr)
	{
		frames_since_idr_ = 0;
		drop_recover_ = false;
	}
	else
	{
		frames_since_idr_ += 1;
	}
	encoded_.fetch_add(1, std::memory_order_relaxed);
	return std::move(pending_);
}

void VTEncoder::reconfig(uint32_t bitrate_bps, uint32_t fps)
{
	if (session_ == nullptr || bitrate_bps == 0 || fps == 0)
		return;
	CFNumberRef br = make_s32((int32_t)bitrate_bps); // bits/sec (see open())
	set_property(kVTCompressionPropertyKey_AverageBitRate, br, false);
	CFRelease(br);
	uint32_t hint = fps_override_ > 0 ? fps_override_ : fps;
	CFNumberRef fr = make_s32((int32_t)hint);
	set_property(kVTCompressionPropertyKey_ExpectedFrameRate, fr, false);
	CFRelease(fr);
	verify_fps(hint);
	params_.bitrate_bps = bitrate_bps;
	params_.fps = fps;
}

} // namespace wivrn_macos

#endif
