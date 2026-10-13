// Streamer: EndFrame pixels-to-wire. Per eye: snapshot the released
// swapchain image, x264-encode, shard, and send; view_info carries the
// echoed headset views + offset-converted display time. Skips cleanly
// with no headset (headless apps run untouched).

#include "streamer.h"

#include "runtime.h"    // log(), Swapchain lookup
#include "vk_context.h" // now_ns()

#include "protocol_version.h"

#include <random>

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
	events.need_idr = [this] {
		std::lock_guard lock(mutex_);
		eyes_[0].force_idr = eyes_[1].force_idr = true;
	};
	events.settings_changed = [this](uint32_t bitrate_bps, uint32_t fps) {
		std::lock_guard lock(mutex_);
		for (Eye & eye: eyes_)
		{
			if (eye.initialized && bitrate_bps > 0 && fps > 0)
			{
				eye.encoder.reconfig(bitrate_bps, fps);
				eye.force_idr = true;
			}
		}
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
	started_ = true;
}

void Streamer::stop()
{
	std::lock_guard lock(mutex_);
	if (!started_)
		return;
	started_ = false;
	// NOTE: per-eye GPU resources are released by session_destroyed
	// (apps destroy sessions before their device); stopping here only
	// drops networking so a dead device is never touched.
	link_.stop();
	bonjour_.stop();
}

bool Streamer::has_headset() const
{
	return link_.connected();
}

void Streamer::session_destroyed(Session * session)
{
	std::lock_guard lock(mutex_);
	for (Eye & eye: eyes_)
	{
		if (eye.initialized && eye.session == session)
		{
			eye.snapshot.shutdown();
			eye.encoder.close();
			eye.initialized = false;
			eye.session = nullptr;
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
	timesyncCounter_ = 0;
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
				EncoderParams params{};
				params.width = swap->width;
				params.height = swap->height;
				params.fps = 90;
				{
					auto info = link_.info();
					if (info.has_value() && info->settings.preferred_refresh_rate >= 60.0f)
						params.fps = (uint32_t)info->settings.preferred_refresh_rate;
				}
				params.bitrate_bps = config_.bitrate_bps;
				{
					auto info = link_.info();
					if (info.has_value() && info->settings.bitrate_bps > 0)
						params.bitrate_bps = info->settings.bitrate_bps;
				}
				if (!eye.encoder.open(params))
				{
					log("streamer: eye %u encoder open failed", eye_index);
					continue;
				}
				eye.width = swap->width;
				eye.height = swap->height;
				eye.initialized = true;
				eye.force_idr = true;
				// Describe the stream now that dims are known.
				link_.send_video_stream_description((uint16_t)swap->width, (uint16_t)swap->height, (float)params.fps);
				link_.send_tracking_control();
			}

			int64_t encode_begin = now_ns();
			bool virgin = image_idx < swap->virgin.size() && swap->virgin[image_idx];
			const uint8_t * bgra = eye.snapshot.capture(swap->images[image_idx], virgin);
			if (bgra == nullptr)
			{
				log("streamer: eye %u snapshot failed", eye_index);
				continue;
			}
			if (image_idx < swap->virgin.size())
				swap->virgin[image_idx] = false;
			bool force_idr = eye.force_idr;
			eye.force_idr = false;
			std::vector<EncodedNal> nals = eye.encoder.feed(bgra, force_idr);
			int64_t encode_end = now_ns();
			if (nals.empty())
				continue;

			OutgoingFrame out;
			out.stream_idx = (uint8_t)eye_index;
			out.frame_idx = eye.frame_index++;
			out.reliable = force_idr;
			for (const EncodedNal & nal: nals)
				out.reliable = out.reliable || nal.is_config || nal.is_idr;
			out.view_info.display_time = display_headset;
			if (views_echo.valid)
			{
				out.view_info.pose = views_echo.pose;
				out.view_info.fov = views_echo.fov;
			}
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

			// Periodic clock sync (every ~120 frames per eye stream).
			if (++timesyncCounter_ % 120 == 0)
				link_.send_timesync_query();
		}
	}
}

} // namespace wivrn_macos
