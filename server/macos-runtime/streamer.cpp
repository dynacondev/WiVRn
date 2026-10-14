// Streamer: EndFrame pixels-to-wire. Per eye: snapshot the released
// swapchain image, x264-encode, shard, and send; view_info carries the
// echoed headset views + offset-converted display time. Skips cleanly
// with no headset (headless apps run untouched).

#include "streamer.h"

#include "runtime.h"    // log(), Swapchain lookup
#include "vk_context.h" // now_ns()

#include "protocol_version.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

namespace wivrn_macos
{

Streamer & Streamer::singleton()
{
	static Streamer streamer;
	return streamer;
}

Streamer::~Streamer()
{
	stop();
}

void Streamer::ensure_started(const StreamerConfig & config)
{
	std::lock_guard lock(mutex_);
	if (started_)
		return;
	config_ = config;
	// Launcher overrides (wivrn-server-macos --port).
	if (const char * port = std::getenv("WIVRN_PORT"))
	{
		unsigned long value = std::strtoul(port, nullptr, 10);
		if (value > 0 && value < 65536)
			config_.port = (uint16_t)value;
	}
	if (const char * name = std::getenv("WIVRN_HOSTNAME"))
	{
		if (name[0] != '\0')
			config_.service_name = name;
	}
	cookie_ = std::random_device{}() | ((uint64_t)std::random_device{}() << 32);
	HeadsetLink::Events events;
	events.connected = [this](const wivrn::from_headset::headset_info_packet & info) { on_connected(info); };
	events.disconnected = [this] { on_disconnected(); };
	events.need_idr = [this] { eyes_[0].force_idr = eyes_[1].force_idr = true; };
	events.settings_changed = [this](uint32_t bitrate_bps, uint32_t fps) {
		std::lock_guard lock(mutex_);
		// Applied by the worker before its next feed (the encoder is
		// worker-owned; touching it here would race).
		if (bitrate_bps > 0 && fps > 0)
		{
			pending_bitrate_ = bitrate_bps;
			pending_fps_ = fps;
		}
		for (Eye & eye: eyes_)
			eye.force_idr = true;
	};
	if (!link_.start(LinkConfig{.port = config_.port, .tcp_only = false}, std::move(events)))
	{
		log("streamer: link start failed (port %u)", config_.port);
		return;
	}
	BonjourConfig bonjour;
	bonjour.name = config_.service_name;
	bonjour.port = config_.port;
	bonjour.protocol_version = wivrn::protocol_version;
	bonjour.version = "0.1.0";
	bonjour.cookie = cookie_;
	if (!bonjour_.start(bonjour))
		log("streamer: bonjour start failed (continuing headless-capable)");
	worker_ = std::jthread([this](std::stop_token stop) { worker_loop(stop); });
	started_ = true;
}

void Streamer::stop()
{
	{
		std::lock_guard lock(mutex_);
		if (!started_)
			return;
		started_ = false;
	}
	// NOTE: per-eye GPU resources are released by session_destroyed
	// (apps destroy sessions before their device); stopping here only
	// drops networking + the worker so a dead device is never touched.
	if (worker_.joinable())
	{
		worker_.request_stop();
		worker_cv_.notify_all();
		worker_.join();
	}
	link_.stop();
	bonjour_.stop();
}

bool Streamer::has_headset() const
{
	return link_.connected();
}

void Streamer::session_destroyed(Session * session)
{
	// Wait out any in-flight worker snapshot+feed first (feeding spans
	// take through send-end), then tear down under lock. DestroySession
	// is not concurrent with EndFrame in v1 apps, so no new jobs stage
	// mid-teardown; the worker sees dead at take and drops.
	for (Eye & eye: eyes_)
	{
		while (eye.feeding.load())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::lock_guard lock(eye.mutex);
		if (eye.initialized && eye.session == session)
		{
			eye.dead = true;
			eye.snapshot.shutdown();
			eye.encoder.close();
			eye.initialized = false;
			eye.session = nullptr;
			eye.pending = false;
			eye.job_swap = nullptr;
			eye.enc_width = 0;
			eye.enc_height = 0;
			eye.last_bitrate = 0;
			eye.last_fps = 0;
		}
	}
}

void Streamer::release_swapchain(Swapchain * swap)
{
	if (swap == nullptr)
		return;
	// Drop staged jobs on this record under eye locks (a worker take
	// serializes on the same lock: taken jobs set feeding and are
	// waited out below, untaken ones die here).
	for (Eye & eye: eyes_)
	{
		std::lock_guard lock(eye.mutex);
		if (eye.job_swap == swap)
		{
			eye.job_swap = nullptr;
			eye.pending = false;
		}
	}
	swap->staged_idx.store(-1, std::memory_order_release);
	// Wait out any in-flight snapshot/feed that claimed an image of
	// this record before the drop above (bounded: snapshot fence and
	// encoder waits time out; destroy is rare).
	for (Eye & eye: eyes_)
	{
		while (eye.feeding.load())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void streamer_session_destroyed(Session * session)
{
	Streamer::singleton().session_destroyed(session);
}

void streamer_release_swapchain(Swapchain * swap)
{
	Streamer::singleton().release_swapchain(swap);
}

void Streamer::on_connected(const wivrn::from_headset::headset_info_packet & info)
{
	(void)info;
	std::lock_guard lock(mutex_);
	// First connected frame re-syncs pacing + forces IDR (see end_frame).
	for (Eye & eye: eyes_)
		eye.force_idr = true;
	lastConnected_ = true;
	// A new headset means a new room frame: re-latch LOCAL from its first
	// fresh HMD sample (see end_frame).
	localLatched_ = false;
	// Converge the headset clock NOW, not after ~120 encodes: the first
	// sample snaps the offset, and every frame's display_time is in the
	// wrong clock domain until it lands (client holds future-dated
	// frames). Reliable socket: handshake proved TCP works.
	link_.send_timesync_burst();
}

void Streamer::on_disconnected()
{
	std::lock_guard lock(mutex_);
	lastConnected_ = false;
}

std::optional<TrackingSample> Streamer::sample() const
{
	if (!link_.connected())
		return std::nullopt;
	TrackingSample sample = link_.source()->latest();
	// Stale samples (headset stalled) fall back to synthetic.
	if (sample.timestampNs == 0 || now_ns() - sample.timestampNs > 500'000'000)
		return std::nullopt;
	return sample;
}

std::optional<TrackingSample> streamer_headset_sample()
{
	return Streamer::singleton().sample();
}

XrPosef Streamer::local_origin() const
{
	std::lock_guard lock(mutex_);
	return localOrigin_;
}

namespace
{
// Rotate vector by unit quaternion (matches spaces.cpp).
XrVector3f qrot_vec(const XrQuaternionf & q, const XrVector3f & v)
{
	XrVector3f qv{q.x, q.y, q.z};
	XrVector3f t{
	        2 * (qv.y * v.z - qv.z * v.y),
	        2 * (qv.z * v.x - qv.x * v.z),
	        2 * (qv.x * v.y - qv.y * v.x),
	};
	XrVector3f u{
	        t.x + 2 * q.w * v.x,
	        t.y + 2 * q.w * v.y,
	        t.z + 2 * q.w * v.z,
	};
	return {
	        v.x + (qv.y * u.z - qv.z * u.y),
	        v.y + (qv.z * u.x - qv.x * u.z),
	        v.z + (qv.x * u.y - qv.y * u.x),
	};
}
} // namespace

XrPosef streamer_local_origin()
{
	return Streamer::singleton().local_origin();
}

void Streamer::end_frame(Session * session, const XrFrameEndInfo * frameEndInfo, XrTime predictedDisplayTime)
{
	if (session == nullptr || frameEndInfo == nullptr)
		return;
	ensure_started();
	if (!link_.connected())
		return;

	// Session pacing follows the headset's preferred rate once known.
	{
		auto info = link_.info();
		if (info.has_value())
		{
			float rate = info->settings.preferred_refresh_rate;
			if (rate >= 60.0f && rate <= 240.0f)
				session->periodNs = (int64_t)(1'000'000'000.0 / rate);
		}
	}

	// Fresh connection: (re)describe the stream with the known dims so
	// a reconnecting headset gets its setup again. First-ever connect
	// has no dims yet; eye init below sends then instead.
	{
		float fps = 90.0f;
		{
			auto info = link_.info();
			if (info.has_value() && info->settings.preferred_refresh_rate >= 60.0f)
				fps = info->settings.preferred_refresh_rate;
		}
		std::lock_guard lock(mutex_);
		if (lastConnected_ && eyes_[0].initialized)
		{
			link_.send_video_stream_description((uint16_t)eyes_[0].width, (uint16_t)eyes_[0].height, fps);
			link_.send_tracking_control();
		}
		lastConnected_ = false;
	}

	auto views_echo = link_.source()->views_echo();
	int64_t display_headset = link_.clock().to_headset(predictedDisplayTime);

	// Latch LOCAL to the client's room frame (floor under the HMD + HMD
	// yaw at connect, Quest-style). Until latched, spaces fall back to
	// (0,1.6,0) — content sits wrong but the link still comes up.
	{
		std::lock_guard lock(mutex_);
		if (!localLatched_)
		{
			TrackingSample sample = link_.source()->latest();
			if (sample.timestampNs != 0 && now_ns() - sample.timestampNs < 500'000'000)
			{
				XrVector3f fwd = qrot_vec(sample.hmd.orientation, {0, 0, -1});
				float yaw = std::atan2(-fwd.x, -fwd.z);
				float half = yaw * 0.5f;
				localOrigin_ = {{0, std::sin(half), 0, std::cos(half)},
				                {sample.hmd.position.x, 0, sample.hmd.position.z}};
				localLatched_ = true;
				log("streamer: LOCAL latched at (%.2f,0,%.2f) yaw=%.1fdeg",
				    (double)sample.hmd.position.x,
				    (double)sample.hmd.position.z,
				    (double)(yaw * 180 / 3.14159265));
			}
		}
	}

	uint32_t eye_index = 0;
	for (uint32_t li = 0; li < frameEndInfo->layerCount && eye_index < 2; ++li)
	{
		const XrCompositionLayerBaseHeader * header = frameEndInfo->layers[li];
		if (header == nullptr || header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
			continue;
		auto * proj = reinterpret_cast<const XrCompositionLayerProjection *>(header);
		for (uint32_t vi = 0; vi < proj->viewCount && eye_index < 2; ++vi, ++eye_index)
		{
			const XrCompositionLayerProjectionView & view = proj->views[vi];
			XrSwapchain swapchain = view.subImage.swapchain;
			if (swapchain == nullptr)
				continue;
			// Find the swapchain record + most recently released image.
			Swapchain * swap = nullptr;
			{
				Instance * self = session->instance;
				if (self == nullptr)
					continue;
				for (const auto & owned: self->ownedSwapchains)
				{
					if (owned.get() == swapchain)
					{
						swap = &owned->data;
						break;
					}
				}
			}
			if (swap == nullptr || swap->lastReleased < 0)
				continue;
			int32_t released = swap->lastReleased;
			if (released < 0 || (uint32_t)released >= swap->images.size())
				continue;

			Eye & eye = eyes_[eye_index];
			// Never block the app loop: the worker only holds the lock
			// for take (never across snapshot/feed), so try_lock.
			std::unique_lock eye_lock(eye.mutex, std::try_to_lock);
			if (!eye_lock.owns_lock())
				continue;
			if (eye.feeding.load())
				continue; // worker mid-feed; its job is newer than ours anyway
			if (eye.session != session)
				eye.dead = false; // new session on this eye: revive (worker drops while dead)
				                  // Stage the released image for the worker (latest-only: an
				                  // unconsumed previous stage is silently dropped). The pixels
				                  // stay on the GPU until the worker snapshots them; acquire
				                  // skips staged/active images so the app can never reclaim one
				                  // mid-copy. No pixel work happens on this thread anymore.
			swap->lastReleased = -1;  // consume-once
			swap->staged_idx.store(released, std::memory_order_release);
			eye.session = session;
			eye.job_swap = swap;
			eye.job_image_idx = (uint32_t)released;
			eye.job_width = swap->width;
			eye.job_height = swap->height;
			// Snapshot bytes follow the swapchain format; RGBA needs an
			// R/B lane swap for the NV12 conversion.
			eye.job_swap_rb = swap->format == (int64_t)VK_FORMAT_R8G8B8A8_SRGB;
			eye.job_force_idr = eye.force_idr.exchange(false);
			eye.job_bitrate = config_.bitrate_bps;
			eye.job_fps = 90;
			{
				auto info = link_.info();
				if (info.has_value())
				{
					if (info->settings.bitrate_bps > 0)
						eye.job_bitrate = info->settings.bitrate_bps;
					if (info->settings.preferred_refresh_rate >= 60.0f)
						eye.job_fps = (uint32_t)info->settings.preferred_refresh_rate;
				}
			}
			// Forward the app's own layer poses for both views (Linux-style):
			// they are already in the app space the client renders. (Never
			// echo poses: the client's views[] live in its VIEW frame, which
			// silently offsets every located object by the room displacement.
			// FOV angles are frame-free, so the echo stays for those.)
			for (uint32_t v = 0; v < 2 && v < proj->viewCount; ++v)
				eye.job_pose[v] = proj->views[v].pose;
			if (views_echo.valid)
				eye.job_fov = views_echo.fov;
			else
			{
				// Pre-connect: forward the app's FOVs, sanitizing any
				// degenerate frustum to the signed fallback (equal edges
				// divide by zero in the client's timewarp math too).
				for (uint32_t v = 0; v < 2 && v < proj->viewCount; ++v)
					eye.job_fov[v] = sane_fov(proj->views[v].fov) ? proj->views[v].fov : fallback_fov;
			}
			eye.job_display_time = display_headset;
			eye.pending = true;
			eye_lock.unlock();
			worker_cv_.notify_one();
		}
	}
}
void Streamer::worker_loop(std::stop_token stop)
{
	while (!stop.stop_requested())
	{
		{
			std::unique_lock lock(mutex_);
			worker_cv_.wait_for(lock, std::chrono::milliseconds(100), [this, &stop] {
				return stop.stop_requested() || eyes_[0].pending.load() || eyes_[1].pending.load();
			});
		}
		if (stop.stop_requested())
			return;
		try
		{
			encode_eye(0);
			encode_eye(1);
		}
		catch (const std::exception & e)
		{
			// A dead worker is a dead stream with zero logs: never let
			// one bad frame kill the thread (drop it and continue).
			log("streamer: worker dropped frame: %s", e.what());
		}
		catch (...)
		{
			log("streamer: worker dropped frame (unknown exception)");
		}
	}
}

void Streamer::encode_eye(uint32_t eye_index)
{
	if (eye_index >= 2)
		return;
	Eye & eye = eyes_[eye_index];
	std::unique_lock eye_lock(eye.mutex);
	if (!eye.pending.load() || eye.dead)
		return;
	// Take the job (meta copies + staged image claim). feeding is set
	// here under lock so session_destroyed waits out the snapshot below
	// as well as the feed; the lock is released across both so the app
	// never blocks.
	Swapchain * swap = eye.job_swap;
	uint32_t width = eye.job_width, height = eye.job_height;
	bool swap_rb = eye.job_swap_rb;
	bool force_idr = eye.job_force_idr;
	uint32_t bitrate = eye.job_bitrate, fps = eye.job_fps;
	auto pose = eye.job_pose;
	auto fov = eye.job_fov;
	XrTime display_time = eye.job_display_time;
	// NACKs landing between stage and take are swallowed by mailbox
	// overwrites (the staged flag dies with the dropped job): pick the
	// live flag up directly so a real loss always gets its IDR.
	force_idr = eye.force_idr.exchange(false) || force_idr;
	eye.pending = false;
	eye.feeding = true;
	if (swap == nullptr || width == 0 || height == 0)
	{
		eye.feeding = false;
		return;
	}
	int32_t claimed = swap->staged_idx.exchange(-1, std::memory_order_acq_rel);
	if (claimed < 0 || (uint32_t)claimed >= swap->images.size())
	{
		eye.feeding = false;
		return;
	}
	uint32_t image_idx = (uint32_t)claimed;
	// Pin across the snapshot so acquire can't hand the image out
	// mid-copy (freed the moment the copy lands).
	swap->active_idx.store((int32_t)image_idx, std::memory_order_release);
	Session * session = eye.session;
	eye_lock.unlock();

	// Snapshot + encoder lifecycle live entirely on the worker: open on
	// first job, reopen on session/size change, reconfig on settings.
	// (Snapshot submits serialize with release submits on the shared
	// queue by submission order.)
	if (!eye.initialized || session == nullptr || eye.width != width || eye.height != height)
	{
		// Unlocked init region: concurrent end_frame touches only job_*
		// fields, staged_idx and atomics (disjoint); session_destroyed
		// waits out feeding before tearing down, so no teardown races.
		eye.snapshot.shutdown();
		eye.encoder.close();
		eye.session = session;
		if (session == nullptr ||
		    !eye.snapshot.init(
		            session->vkPhysicalDevice, session->vkDevice, session->vkQueue, session->vkQueueFamily, width, height))
		{
			log("streamer: eye %u snapshot init failed", eye_index);
			swap->active_idx.store(-1, std::memory_order_release);
			eye.feeding = false;
			return;
		}
		eye.width = width;
		eye.height = height;
		eye.initialized = true;
		eye.enc_width = 0; // force encoder (re)open below
		eye.enc_height = 0;
		eye.force_idr = true;
		force_idr = true;
		// Describe the stream now that dims are known.
		float desc_fps = 90.0f;
		{
			auto info = link_.info();
			if (info.has_value() && info->settings.preferred_refresh_rate >= 60.0f)
				desc_fps = info->settings.preferred_refresh_rate;
		}
		link_.send_video_stream_description((uint16_t)width, (uint16_t)height, desc_fps);
		link_.send_tracking_control();
	}

	int64_t snap_begin = now_ns();
	const uint8_t * bgra = eye.snapshot.capture(swap->images[image_idx]);
	int64_t snap_end = now_ns();
	swap->active_idx.store(-1, std::memory_order_release);
	int64_t snap_ns = snap_end - snap_begin;
	if (bgra == nullptr)
	{
		log("streamer: eye %u snapshot failed", eye_index);
		eye.feeding = false;
		return;
	}
	{
		std::lock_guard lock(mutex_);
		if (pending_bitrate_ > 0 && pending_fps_ > 0)
		{
			bitrate = pending_bitrate_;
			fps = pending_fps_;
		}
	}
	if (eye.enc_width == 0 || bitrate != eye.last_bitrate || fps != eye.last_fps)
	{
		if (eye.enc_width != 0)
			eye.encoder.reconfig(bitrate, fps);
		else
		{
			EncoderParams params{};
			params.width = width;
			params.height = height;
			params.fps = fps;
			params.bitrate_bps = bitrate;
			if (!eye.encoder.open(params))
			{
				log("streamer: eye %u encoder open failed", eye_index);
				eye.feeding = false;
				return;
			}
			eye.enc_width = width;
			eye.enc_height = height;
		}
		eye.last_bitrate = bitrate;
		eye.last_fps = fps;
		force_idr = true;
	}
	int64_t encode_begin = now_ns();
	std::vector<EncodedNal> nals = eye.encoder.feed(bgra, swap_rb, force_idr);
	int64_t encode_end = now_ns();
	eye.feeding = false;
	{
		// Drop instead of sending into a dead session (destroyed mid-feed).
		std::lock_guard dead_check(eye.mutex);
		if (eye.dead)
			return;
	}
	if (nals.empty())
		return;

	OutgoingFrame out;
	out.stream_idx = (uint8_t)eye_index;
	{
		std::lock_guard lock(mutex_);
		out.frame_idx = eye.frame_index++;
	}
	out.reliable = force_idr;
	for (const EncodedNal & nal: nals)
		out.reliable = out.reliable || nal.is_config || nal.is_idr;
	out.view_info.display_time = display_time;
	out.view_info.pose = pose;
	out.view_info.fov = fov;
	// Identity (1:1, no-foveation) params: a single segment of ratio 1
	// decodes to the full stream size on the client. Empty vectors
	// decode to 0x0 and crash its swapchain creation — never send those.
	// (GPU foveation is fast-follow; the client 1:1-blits these.)
	out.view_info.foveation[0].x = {(uint16_t)width};
	out.view_info.foveation[0].y = {(uint16_t)height};
	out.view_info.foveation[1].x = {(uint16_t)width};
	out.view_info.foveation[1].y = {(uint16_t)height};
	out.view_info.alpha = false;
	out.timing_info.encode_begin = encode_begin;
	out.timing_info.encode_end = encode_end;
	out.timing_info.send_begin = now_ns();
	// One-shot: the exact app-space placements the client renders (frame
	// bugs show up here, not in the transport counters below).
	if (eye.encoded_count == 0)
	{
		const XrPosef & p0 = out.view_info.pose[0];
		const XrPosef & p1 = out.view_info.pose[1];
		log("streamer: eye %u first view poses L=(%.2f,%.2f,%.2f) R=(%.2f,%.2f,%.2f) display=%lld",
		    eye_index,
		    (double)p0.position.x,
		    (double)p0.position.y,
		    (double)p0.position.z,
		    (double)p1.position.x,
		    (double)p1.position.y,
		    (double)p1.position.z,
		    (long long)out.view_info.display_time);
	}
	FrameMeta meta{};
	meta.frame_idx = out.frame_idx;
	shard_frame(
	        out.stream_idx,
	        nals,
	        meta,
	        [&](uint8_t, const std::vector<uint8_t> & bytes, bool, bool, bool) {
		        out.shards.push_back(bytes);
	        });
	eye.sent_index = out.frame_idx;
	if (out.reliable)
		++eye.reliable_sent;
	link_.send_frame(std::move(out));

	if (++eye.encoded_count % 60 == 0)
		log("streamer: eye %u encoded=%llu idx=%llu rel=%llu sent=%llu drop=%llu snap=%.1fms enc=%.1fms",
		    eye_index,
		    (unsigned long long)eye.encoded_count,
		    (unsigned long long)eye.sent_index.load(),
		    (unsigned long long)eye.reliable_sent.load(),
		    (unsigned long long)link_.sent_frames(),
		    (unsigned long long)link_.dropped_frames(),
		    snap_ns / 1e6,
		    (encode_end - encode_begin) / 1e6);

	// Periodic clock sync (every ~120 encoded frames per eye stream).
	static thread_local uint64_t timesync_counter = 0;
	if (++timesync_counter % 120 == 0)
		link_.send_timesync_query();
}

} // namespace wivrn_macos
