// Streamer: owns the headset link, per-eye snapshots/encoders, and
// Bonjour; driven from EndFrame on the app thread. Encode runs on a
// worker thread with a latest-only mailbox per eye: the app snapshots
// each presented frame (fast) and drops it if the worker is still busy,
// so the app loop is never throttled and the wire always carries fresh
// frames. v1 model: the runtime dylib IS the server core in the
// application process (no service/IPC split) — the headset connects
// directly to the app. Headless apps (no headset) run untouched.

#pragma once

#include "bonjour.h"
#include "encode.h"
#include "net.h"
#include "snapshot.h"

#include <openxr/openxr.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

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
		// App thread owns snapshot init + capture; worker owns encoder
		// open/feed. Guarded by mutex, except: pending/feeding/dead are
		// atomic for lock-free fast paths, and the worker releases the
		// lock across feed (see encode_eye) so the app never blocks.
		std::mutex mutex;
		bool initialized = false;
		Session * session = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		EyeSnapshot snapshot;
		X264Encoder encoder;
		uint32_t last_bitrate = 0;
		uint32_t last_fps = 0;
		uint32_t enc_width = 0;
		uint32_t enc_height = 0;
		uint64_t frame_index = 0;
		uint64_t encoded_count = 0;
		std::atomic<bool> force_idr{true};
		// Latest-only mailbox (app -> worker). pending is lock-free so
		// end_frame can drop without blocking; payload under mutex.
		std::atomic<bool> pending{false};
		// True while the worker owns a taken job (set under mutex at
		// take, cleared after feed). App skips staging while set;
		// session_destroyed spins on it before closing the encoder.
		std::atomic<bool> feeding{false};
		// Set under mutex by session_destroyed; worker drops jobs for
		// dead eyes instead of sending.
		bool dead = false;
		std::vector<uint8_t> pixels; // w*h*4 snapshot bytes
		uint32_t job_width = 0;
		uint32_t job_height = 0;
		bool job_swap_rb = false;
		bool job_force_idr = false;
		uint32_t job_bitrate = 0;
		uint32_t job_fps = 0;
		std::array<XrPosef, 2> job_pose{};
		std::array<XrFovf, 2> job_fov{};
		XrTime job_display_time = 0;
		int64_t job_snap_ns = 0;
	};
	Eye eyes_[2]{};

	std::jthread worker_;
	std::condition_variable_any worker_cv_;
	void worker_loop(std::stop_token stop);
	void encode_eye(uint32_t eye_index);

	bool lastConnected_ = false;
	uint64_t cookie_ = 0;
	// Reconfig requests from settings events (applied by the worker).
	uint32_t pending_bitrate_ = 0;
	uint32_t pending_fps_ = 0;
};

// Called by tracking.cpp: network sample when live, nullopt otherwise.
std::optional<TrackingSample> streamer_headset_sample();

// Called by DestroySession: release per-eye GPU resources bound to it.
void streamer_session_destroyed(Session * session);

} // namespace wivrn_macos
