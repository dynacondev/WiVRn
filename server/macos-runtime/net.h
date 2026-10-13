// Headset link: TCP accept + 9-step handshake (encryption_disabled) +
// steady-state RX/TX over control (TCP) and stream (UDP) sockets.
// Mirrors server/driver/wivrn_connection.cpp's wire sequence without the
// Monado/IPC coupling. All sends happen on the caller's (EndFrame) thread;
// RX runs on background threads. No Monado here, ever.

#pragma once

#include "tracking.h"

#include "protocol_version.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wivrn_macos
{

struct LinkConfig
{
	uint16_t port = 9757;
	bool tcp_only = false;
};

// Clock offset (headset time = server steady time + offset), refined by
// timesync query/response pairs (EWMA like the Linux clock_offset).
class HeadsetClock
{
public:
	void observe(int64_t query_sent_ns, int64_t response_headset_ns, int64_t received_ns);
	// Convert a server-steady timestamp to headset time.
	int64_t to_headset(int64_t server_ns) const;
	int64_t offset_ns() const;

private:
	mutable std::mutex mutex_;
	bool have_sample_ = false;
	double offset_ = 0;
};

// Live tracking feed: latest headset tracking + inputs packets converted
// to the runtime's TrackingSample (replaces the synthetic source once a
// headset connects; receipt-stamped, see below).
class NetworkSource : public TrackingSource
{
public:
	TrackingSample latest() override;

	void update_tracking(const wivrn::from_headset::tracking & tracking);
	void update_inputs(const wivrn::from_headset::inputs & inputs);

	// Last views echo, for view_info on outgoing video shards.
	struct ViewsEcho
	{
		std::array<XrPosef, 2> pose{};
		std::array<XrFovf, 2> fov{};
		bool valid = false;
	};
	ViewsEcho views_echo() const;

private:
	mutable std::mutex mutex_;
	TrackingSample sample_{};
	ViewsEcho echo_{};
	bool live_logged_ = false;
};

// Outgoing video shard batch for one eye frame (built by the streamer,
// sent by the link on the EndFrame thread).
struct OutgoingFrame
{
	uint8_t stream_idx = 0; // 0 left, 1 right
	uint64_t frame_idx = 0;
	std::vector<std::vector<uint8_t>> shards; // each ≤1400 B
	bool reliable = false;                    // IDR/config -> control socket
	wivrn::to_headset::video_stream_data_shard::view_info_t view_info{};
	wivrn::to_headset::video_stream_data_shard::timing_info_t timing_info{};
};

class HeadsetLink
{
public:
	struct Events
	{
		std::function<void(const wivrn::from_headset::headset_info_packet &)> connected;
		std::function<void()> disconnected;
		std::function<void()> need_idr; // feedback NACK / loss
		std::function<void(uint32_t bitrate_bps, uint32_t fps)> settings_changed;
	};

	HeadsetLink() = default;
	~HeadsetLink();

	HeadsetLink(const HeadsetLink &) = delete;
	HeadsetLink & operator=(const HeadsetLink &) = delete;

	// Start listening + background accept thread. Safe to call once.
	bool start(const LinkConfig & config, Events events);
	void stop();

	bool connected() const;
	// Latest info packet (valid only when connected()).
	std::optional<wivrn::from_headset::headset_info_packet> info() const;
	std::shared_ptr<NetworkSource> source() const
	{
		return source_;
	}
	HeadsetClock & clock()
	{
		return clock_;
	}

	// --- TX side (EndFrame thread only) ---
	void send_video_stream_description(uint16_t width, uint16_t height, float fps);
	void send_tracking_control();
	void send_timesync_query();
	// First-seconds burst over the reliable control socket: the first
	// sample snaps the clock offset, and every frame's display_time is
	// unusable until it lands.
	void send_timesync_burst();
	void send_frame(OutgoingFrame frame);
	// Lifetime counters for the streaming log line (atomic: the worker
	// bumps them under no lock discipline of its own).
	uint64_t sent_frames() const
	{
		return sent_frames_.load();
	}
	uint64_t dropped_frames() const
	{
		return dropped_frames_.load();
	}

private:
	using ControlSocket = wivrn::typed_socket<wivrn::TCP, wivrn::from_headset::packets, wivrn::to_headset::packets>;
	using StreamSocket = wivrn::typed_socket<wivrn::UDP, wivrn::from_headset::packets, wivrn::to_headset::packets>;

	bool accept_and_handshake(std::stop_token stop);
	void control_rx_loop(std::stop_token stop);
	void stream_rx_loop(std::stop_token stop);
	void teardown();

	LinkConfig config_{};
	Events events_;
	std::shared_ptr<NetworkSource> source_ = std::make_shared<NetworkSource>();
	HeadsetClock clock_;

	mutable std::mutex mutex_;
	bool listening_ = false;
	bool connected_ = false;
	std::optional<wivrn::from_headset::headset_info_packet> info_;
	std::unique_ptr<wivrn::TCPListener> listener_;
	std::unique_ptr<ControlSocket> control_;
	std::unique_ptr<StreamSocket> stream_;

	std::jthread accept_thread_;
	std::jthread control_thread_;
	std::jthread stream_thread_;
	std::atomic<bool> running_ = false;
	std::atomic<uint64_t> sent_frames_{0};
	std::atomic<uint64_t> dropped_frames_{0};
};

} // namespace wivrn_macos
