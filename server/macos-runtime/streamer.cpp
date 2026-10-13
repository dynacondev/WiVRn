// Streamer: EndFrame pixels-to-wire. Per eye: snapshot the released
// swapchain image, x264-encode, shard, and send; view_info carries the
// echoed headset views + offset-converted display time. Skips cleanly
// with no headset (headless apps run untouched).

#include "streamer.h"

#include "runtime.h"    // log(), Swapchain lookup
#include "vk_context.h" // now_ns()

#include "protocol_version.h"

#include <chrono>
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
	// Mark dead + drop snapshot under lock (snapshot is app-thread-only;
	// DestroySession is not concurrent with EndFrame in v1 apps), then
	// wait out any in-flight worker feed before closing the encoder.
	for (Eye & eye: eyes_)
	{
		std::lock_guard lock(eye.mutex);
		if (eye.initialized && eye.session == session)
		{
			eye.dead = true;
			eye.snapshot.shutdown();
			eye.initialized = false;
			eye.session = nullptr;
			eye.pending = false;
			eye.pixels.clear();
		}
	}
	for (Eye & eye: eyes_)
	{
		while (eye.feeding.load())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::lock_guard lock(eye.mutex);
		if (eye.dead)
		{
			eye.encoder.close();
			eye.dead = false;
			eye.enc_width = 0;
			eye.enc_height = 0;
			eye.last_bitrate = 0;
			eye.last_fps = 0;
		}
	}
}

void streamer_session_destroyed(Session * session)
{
	Streamer::singleton().session_destroyed(session);
}

void Streamer::on_connected(const wivrn::from_headset::headset_info_packet & info)
{
	(void)info;
	std::lock_guard lock(mutex_);
	// First connected frame re-syncs pacing + forces IDR (see end_frame).
	for (Eye & eye: eyes_)
		eye.force_idr = true;
	lastConnected_ = true;
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
			uint32_t image_idx = (uint32_t)swap->lastReleased;
			if (image_idx >= swap->images.size())
				continue;

			Eye & eye = eyes_[eye_index];
			// Never block the app loop: the worker only holds the lock
			// for take/init/destroy (never across feed), so try_lock.
			std::unique_lock eye_lock(eye.mutex, std::try_to_lock);
			if (!eye_lock.owns_lock())
				continue;
			if (eye.feeding.load())
				continue; // worker mid-feed; its job is newer than ours anyway
			if (!eye.initialized || eye.session != session || eye.width != swap->width || eye.height != swap->height)
			{
				eye.snapshot.shutdown();
				eye.encoder.close();
				eye.session = session;
				if (!eye.snapshot.init(
				            session->vkPhysicalDevice, session->vkDevice, session->vkQueue, session->vkQueueFamily, swap->width, swap->height))
				{
					log("streamer: eye %u snapshot init failed", eye_index);
					continue;
				}
				// (Encoder opens lazily on the worker from the first job;
				// see encode_eye. Snapshot init stays here: its submits
				// serialize with release submits on this same thread.)
				eye.width = swap->width;
				eye.height = swap->height;
				eye.initialized = true;
				eye.force_idr = true;
				// Describe the stream now that dims are known.
				float fps = 90.0f;
				{
					auto info = link_.info();
					if (info.has_value() && info->settings.preferred_refresh_rate >= 60.0f)
						fps = info->settings.preferred_refresh_rate;
				}
				link_.send_video_stream_description((uint16_t)swap->width, (uint16_t)swap->height, fps);
				link_.send_tracking_control();
			}

			int64_t snap_begin = now_ns();
			const uint8_t * bgra = eye.snapshot.capture(swap->images[image_idx]);
			if (bgra == nullptr)
			{
				log("streamer: eye %u snapshot failed", eye_index);
				continue;
			}
			eye.pixels.assign(bgra, bgra + (size_t)swap->width * swap->height * 4);
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
			if (views_echo.valid)
			{
				eye.job_pose = views_echo.pose;
				eye.job_fov = views_echo.fov;
			}
			eye.job_display_time = display_headset;
			eye.job_snap_ns = now_ns() - snap_begin;
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
	if (!eye.pending.load() || !eye.initialized || eye.dead)
		return;
	// Take the job (move pixels out, clear pending first so the app can
	// already stage the next frame while this one encodes). feeding stays
	// set across the lock-free feed so session_destroyed waits us out.
	std::vector<uint8_t> pixels = std::move(eye.pixels);
	uint32_t width = eye.job_width, height = eye.job_height;
	bool swap_rb = eye.job_swap_rb, force_idr = eye.job_force_idr;
	uint32_t bitrate = eye.job_bitrate, fps = eye.job_fps;
	auto pose = eye.job_pose;
	auto fov = eye.job_fov;
	XrTime display_time = eye.job_display_time;
	int64_t snap_ns = eye.job_snap_ns;
	eye.pending = false;
	eye.feeding = true;
	if (pixels.size() != (size_t)width * height * 4 || width == 0 || height == 0)
	{
		eye.feeding = false;
		return;
	}
	// From here the feed runs lock-free (feeding guards session_destroyed);
	// the app keeps staging via try_lock meanwhile.
	eye_lock.unlock();

	// Encoder lifecycle lives entirely on the worker: open on first
	// job, reopen on size change, reconfig on settings. (Snapshot dims
	// eye.width/height are app-set and only gate the snapshot side.)
	if (eye.enc_width != width || eye.enc_height != height)
	{
		eye.encoder.close();
		eye.enc_width = 0;
		eye.enc_height = 0;
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
	std::vector<EncodedNal> nals = eye.encoder.feed(pixels.data(), swap_rb, force_idr);
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
	FrameMeta meta{};
	meta.frame_idx = out.frame_idx;
	shard_frame(
	        out.stream_idx,
	        nals,
	        meta,
	        [&](uint8_t, const std::vector<uint8_t> & bytes, bool, bool, bool) {
		        out.shards.push_back(bytes);
	        });
	link_.send_frame(std::move(out));

	if (++eye.encoded_count % 60 == 0)
		log("streamer: eye %u encoded=%llu snap=%.1fms enc=%.1fms",
		    eye_index,
		    (unsigned long long)eye.encoded_count,
		    snap_ns / 1e6,
		    (encode_end - encode_begin) / 1e6);

	// Periodic clock sync (every ~120 encoded frames per eye stream).
	static thread_local uint64_t timesync_counter = 0;
	if (++timesync_counter % 120 == 0)
		link_.send_timesync_query();
}

} // namespace wivrn_macos
