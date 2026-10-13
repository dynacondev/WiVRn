// Streamer: owns the headset link, per-eye encoders/snapshots, and
// Bonjour; driven from EndFrame on the app thread. v1 model: the runtime
// dylib IS the server core in the application process (no service/IPC
// split) — the headset connects directly to the app. Headless apps
// (no headset) run the frame loop untouched.

#pragma once

#include "bonjour.h"
#include "encode.h"
#include "net.h"
#include "snapshot.h"

#include <openxr/openxr.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace wivrn_macos
{

struct StreamerConfig
{
	uint16_t port = 9757;
	std::string service_name = "WiVRn";
	uint32_t bitrate_bps = 20'000'000;
	uint32_t fps = 90;
};

class Streamer
{
public:
	static Streamer & singleton();

	Streamer() = default;
	~Streamer();

	Streamer(const Streamer &) = delete;
	Streamer & operator=(const Streamer &) = delete;

	// Idempotent start (first session creation triggers it).
	void ensure_started(const StreamerConfig & config = {});
	void stop();

	bool has_headset() const;

	// Fresh network tracking sample (nullopt when disconnected or
	// stale — caller falls back to synthetic).
	std::optional<TrackingSample> sample() const;

	// Release per-eye GPU resources bound to a dying session (apps
	// destroy sessions before tearing down their device; static-dtor
	// cleanup would touch a dead device).
	void session_destroyed(Session * session);

	// Encode one presented frame: snapshot each referenced swapchain
	// image, encode, shard, and send. Skips eyes with no released image
	// yet (release-after-EndFrame apps cost one frame).
	void end_frame(
	        Session * session,
	        const XrFrameEndInfo * frameEndInfo,
	        XrTime predictedDisplayTime);

private:
	void on_connected(const wivrn::from_headset::headset_info_packet & info);
	void on_disconnected();

	mutable std::mutex mutex_;
	bool started_ = false;
	StreamerConfig config_{};
	HeadsetLink link_;
	BonjourPublisher bonjour_;

	struct Eye
	{
		bool initialized = false;
		Session * session = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		EyeSnapshot snapshot;
		X264Encoder encoder;
		uint64_t frame_index = 0;
		bool force_idr = true;
	};
	Eye eyes_[2]{};

	bool lastConnected_ = false;
	uint64_t timesyncCounter_ = 0;
	uint64_t cookie_ = 0;
};

// Called by tracking.cpp: network sample when live, nullopt otherwise.
std::optional<TrackingSample> streamer_headset_sample();

// Called by DestroySession: release per-eye GPU resources bound to it.
void streamer_session_destroyed(Session * session);

} // namespace wivrn_macos
