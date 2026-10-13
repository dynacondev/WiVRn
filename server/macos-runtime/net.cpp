// Headset link implementation. Wire sequence mirrors
// server/driver/wivrn_connection.cpp:166-286 with encryption_disabled
// (no SMP/secrets/AES in v1).

#include "net.h"

#include "runtime.h"    // log()
#include "vk_context.h" // now_ns()

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

namespace wivrn_macos
{
namespace
{

using FromPackets = wivrn::from_headset::packets;

// Poll-then-receive with a total timeout, sliced for stop checks.
// Returns nullopt on timeout; throws on HUP/ERR or stop.
template <typename Socket>
std::optional<FromPackets> receive_one(Socket & socket, int timeout_ms, std::stop_token stop)
{
	int waited = 0;
	while (waited < timeout_ms)
	{
		if (stop.stop_requested())
			throw std::runtime_error("link stopping");
		if (auto pending = socket.receive_pending())
			return pending;
		pollfd pfd{.fd = socket.get_fd(), .events = POLLIN, .revents = 0};
		int step = std::min(100, timeout_ms - waited);
		int r = ::poll(&pfd, 1, step);
		waited += step;
		if (r < 0)
			throw std::system_error(errno, std::generic_category());
		if (pfd.revents & (POLLHUP | POLLERR))
			throw std::runtime_error("socket hup/err");
		if (pfd.revents & POLLIN)
		{
			try
			{
				if (auto packet = socket.receive())
					return packet;
			}
			catch (const std::system_error & e)
			{
				if (e.code().value() != EAGAIN)
					throw;
				// Raced with the poll: keep waiting.
			}
		}
	}
	return std::nullopt;
}

template <typename T>
const T * get_as(const FromPackets & packet)
{
	return std::get_if<T>(&packet);
}

} // namespace

void HeadsetClock::observe(int64_t query_sent_ns, int64_t response_headset_ns, int64_t received_ns)
{
	int64_t rtt = received_ns - query_sent_ns;
	int64_t sample = response_headset_ns - received_ns + rtt / 2;
	std::lock_guard lock(mutex_);
	offset_ = have_sample_ ? offset_ * 0.875 + sample * 0.125 : (double)sample;
	have_sample_ = true;
}

int64_t HeadsetClock::to_headset(int64_t server_ns) const
{
	std::lock_guard lock(mutex_);
	return server_ns + (int64_t)offset_;
}

int64_t HeadsetClock::offset_ns() const
{
	std::lock_guard lock(mutex_);
	return (int64_t)offset_;
}

TrackingSample NetworkSource::latest()
{
	std::lock_guard lock(mutex_);
	return sample_;
}

NetworkSource::ViewsEcho NetworkSource::views_echo() const
{
	std::lock_guard lock(mutex_);
	return echo_;
}

void NetworkSource::update_tracking(const wivrn::from_headset::tracking & tracking)
{
	std::lock_guard lock(mutex_);
	// Head pose: prefer the HEAD device entry, hold last otherwise.
	for (const auto & pose: tracking.device_poses)
	{
		if (pose.device == wivrn::device_id::HEAD)
			sample_.hmd = pose.pose;
		else if (pose.device == wivrn::device_id::LEFT_GRIP || pose.device == wivrn::device_id::LEFT_AIM)
			sample_.left.pose = pose.pose;
		else if (pose.device == wivrn::device_id::RIGHT_GRIP || pose.device == wivrn::device_id::RIGHT_AIM)
			sample_.right.pose = pose.pose;
	}
	sample_.left.active = sample_.right.active = true;
	sample_.timestampNs = now_ns();
	for (size_t i = 0; i < 2 && i < tracking.views.size(); ++i)
	{
		echo_.pose[i] = tracking.views[i].pose;
		echo_.fov[i] = tracking.views[i].fov;
	}
	echo_.valid = true;
}

void NetworkSource::update_inputs(const wivrn::from_headset::inputs & inputs)
{
	using id = wivrn::device_id;
	std::lock_guard lock(mutex_);
	for (const auto & value: inputs.values)
	{
		ControllerSample * side = nullptr;
		switch (value.id)
		{
			case id::LEFT_TRIGGER_CLICK:
			case id::LEFT_MENU_CLICK:
			case id::LEFT_SQUEEZE_CLICK:
			case id::LEFT_SQUEEZE_VALUE:
			case id::LEFT_TRIGGER_VALUE:
			case id::LEFT_TRIGGER_TOUCH:
			case id::LEFT_THUMBSTICK_X:
			case id::LEFT_THUMBSTICK_Y:
			case id::LEFT_THUMBSTICK_CLICK:
			case id::LEFT_THUMBSTICK_TOUCH:
				side = &sample_.left;
				break;
			default:
				break;
		}
		switch (value.id)
		{
			case id::RIGHT_TRIGGER_CLICK:
			case id::RIGHT_MENU_CLICK:
			case id::RIGHT_SQUEEZE_CLICK:
			case id::RIGHT_SQUEEZE_VALUE:
			case id::RIGHT_TRIGGER_VALUE:
			case id::RIGHT_TRIGGER_TOUCH:
			case id::RIGHT_THUMBSTICK_X:
			case id::RIGHT_THUMBSTICK_Y:
			case id::RIGHT_THUMBSTICK_CLICK:
			case id::RIGHT_THUMBSTICK_TOUCH:
				side = &sample_.right;
				break;
			default:
				break;
		}
		if (side == nullptr)
			continue;
		switch (value.id)
		{
			case id::LEFT_TRIGGER_CLICK:
			case id::RIGHT_TRIGGER_CLICK:
				side->selectClick = value.value > 0.5f;
				break;
			case id::LEFT_MENU_CLICK:
			case id::RIGHT_MENU_CLICK:
				side->menuClick = value.value > 0.5f;
				break;
			case id::LEFT_SQUEEZE_CLICK:
			case id::RIGHT_SQUEEZE_CLICK:
				side->squeezeClick = value.value > 0.5f;
				break;
			case id::LEFT_SQUEEZE_VALUE:
			case id::RIGHT_SQUEEZE_VALUE:
				break; // v1: click only
			case id::LEFT_TRIGGER_VALUE:
			case id::RIGHT_TRIGGER_VALUE:
				side->triggerValue = value.value;
				break;
			case id::LEFT_THUMBSTICK_X:
			case id::RIGHT_THUMBSTICK_X:
				side->thumbstick.x = value.value;
				break;
			case id::LEFT_THUMBSTICK_Y:
			case id::RIGHT_THUMBSTICK_Y:
				side->thumbstick.y = value.value;
				break;
			case id::LEFT_THUMBSTICK_CLICK:
			case id::RIGHT_THUMBSTICK_CLICK:
				side->thumbstickClick = value.value > 0.5f;
				break;
			default:
				break;
		}
	}
	sample_.timestampNs = now_ns();
}

HeadsetLink::~HeadsetLink()
{
	stop();
}

bool HeadsetLink::start(const LinkConfig & config, Events events)
{
	std::lock_guard lock(mutex_);
	if (listening_)
		return false;
	config_ = config;
	events_ = std::move(events);
	try
	{
		listener_ = std::make_unique<wivrn::TCPListener>(config.port);
	}
	catch (const std::exception & e)
	{
		log("link: listen on %u failed: %s", config.port, e.what());
		return false;
	}
	listening_ = true;
	running_ = true;
	// Shutdown ordering (deadlock-free by construction): only the accept
	// thread joins the RX threads and tears sockets down. stop() merely
	// requests the accept thread to stop and joins it; teardown strictly
	// happens-after every RX thread exit, so no thread ever touches a
	// reset socket.
	accept_thread_ = std::jthread([this](std::stop_token stop) {
		auto cleanup_session = [&] {
			if (control_thread_.joinable())
			{
				control_thread_.request_stop();
				control_thread_.join();
			}
			if (stream_thread_.joinable())
			{
				stream_thread_.request_stop();
				stream_thread_.join();
			}
			teardown();
		};
		while (!stop.stop_requested())
		{
			try
			{
				if (accept_and_handshake(stop))
				{
					control_thread_ = std::jthread([this](std::stop_token s) { control_rx_loop(s); });
					stream_thread_ = std::jthread([this](std::stop_token s) { stream_rx_loop(s); });
					// Park until disconnect or stop.
					while (!stop.stop_requested())
					{
						{
							std::lock_guard lock(mutex_);
							if (!connected_)
								break;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
					}
					cleanup_session();
				}
			}
			catch (const std::exception & e)
			{
				log("link: accept cycle: %s", e.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		}
		cleanup_session();
	});
	return true;
}

void HeadsetLink::stop()
{
	running_ = false;
	{
		std::lock_guard lock(mutex_);
		if (!listening_)
			return;
		listening_ = false;
	}
	if (accept_thread_.joinable())
	{
		accept_thread_.request_stop();
		accept_thread_.join();
	}
}

bool HeadsetLink::connected() const
{
	std::lock_guard lock(mutex_);
	return connected_;
}

std::optional<wivrn::from_headset::headset_info_packet> HeadsetLink::info() const
{
	std::lock_guard lock(mutex_);
	return info_;
}

void HeadsetLink::teardown()
{
	bool was = false;
	{
		std::lock_guard lock(mutex_);
		was = connected_;
		connected_ = false;
		info_.reset();
		control_.reset();
		stream_.reset();
	}
	if (was && events_.disconnected)
	{
		try
		{
			events_.disconnected();
		}
		catch (...)
		{
		}
	}
}

bool HeadsetLink::accept_and_handshake(std::stop_token stop)
{
	// Blocking accept, slice-polled for stop (mirrors accept_connection).
	wivrn::TCP tcp;
	{
		pollfd pfd{.fd = listener_->get_fd(), .events = POLLIN, .revents = 0};
		while (!stop.stop_requested())
		{
			int r = ::poll(&pfd, 1, 100);
			if (r < 0)
				throw std::system_error(errno, std::generic_category());
			if (stop.stop_requested())
				return false;
			if (pfd.revents & POLLIN)
			{
				tcp = listener_->accept().first;
				break;
			}
		}
		if (stop.stop_requested())
			return false;
	}
	log("link: headset TCP accepted, handshake...");

	auto control = std::make_unique<ControlSocket>(std::move(tcp));

	// 1. crypto hello + version gate.
	auto hello = receive_one(*control, 10'000, stop);
	auto * crypto = hello ? get_as<wivrn::from_headset::crypto_handshake>(*hello) : nullptr;
	if (crypto == nullptr)
	{
		log("link: no crypto hello, dropping");
		return false;
	}
	if (crypto->protocol_version != wivrn::protocol_version)
	{
		log("link: protocol mismatch (headset %llu, server %llu)",
		    (unsigned long long)crypto->protocol_version,
		    (unsigned long long)wivrn::protocol_version);
		try
		{
			control->send(wivrn::to_headset::crypto_handshake{
			        .public_key = {},
			        .state = wivrn::to_headset::crypto_handshake::crypto_state::incompatible_version,
			});
		}
		catch (...)
		{
		}
		return false;
	}

	// 2-3. encryption_disabled: no SMP/secrets/AES in v1.
	control->send(wivrn::to_headset::crypto_handshake{
	        .public_key = {},
	        .state = wivrn::to_headset::crypto_handshake::crypto_state::encryption_disabled,
	});
	auto confirm = receive_one(*control, 10'000, stop);
	if (!confirm.has_value() || get_as<wivrn::from_headset::crypto_handshake>(*confirm) == nullptr)
	{
		log("link: no crypto confirm, dropping");
		return false;
	}

	// 4. stream port offer.
	int stream_port = config_.tcp_only ? -1 : (int)config_.port;
	control->send(wivrn::to_headset::handshake{.stream_port = stream_port});

	std::unique_ptr<StreamSocket> stream;
	if (!config_.tcp_only)
	{
		// 5. UDP bind + learn the client ephemeral port from its
		// retransmitted stream handshake (receive_from_raw exposes it).
		auto udp = std::make_unique<StreamSocket>();
		sockaddr_in6 bind_addr{};
		bind_addr.sin6_family = AF_INET6;
		bind_addr.sin6_port = htons(config_.port);
		bind_addr.sin6_addr = in6addr_any;
		udp->bind(bind_addr);

		bool pinned = false;
		int waited = 0;
		while (waited < 10'000 && !stop.stop_requested())
		{
			// Drain either socket (client retransmits on UDP; be liberal).
			if (auto pending = control->receive_pending())
			{
				if (get_as<wivrn::from_headset::handshake>(*pending) != nullptr)
				{
					log("link: stream handshake arrived on control (no UDP)");
					stream.reset();
					pinned = true;
					break;
				}
			}
			pollfd pfds[2] = {};
			pfds[0].fd = control->get_fd();
			pfds[0].events = POLLIN;
			pfds[1].fd = udp->get_fd();
			pfds[1].events = POLLIN;
			int r = ::poll(pfds, 2, 100);
			waited += 100;
			if (r < 0)
				throw std::system_error(errno, std::generic_category());
			if (pfds[1].revents & POLLIN)
			{
				auto [packet, addr] = udp->receive_from_raw();
				if (!packet.empty())
				{
					auto decoded = packet.deserialize<wivrn::from_headset::packets>();
					if (std::holds_alternative<wivrn::from_headset::handshake>(decoded))
					{
						udp->connect(addr);
						int sndbuf = 5 * 1024 * 1024;
						::setsockopt(udp->get_fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
						stream = std::move(udp);
						pinned = true;
						break;
					}
				}
			}
		}
		if (!pinned)
		{
			log("link: no stream handshake, dropping");
			return false;
		}
	}

	// 6. handshake repeat, then 7. headset info on control.
	control->send(wivrn::to_headset::handshake{.stream_port = stream_port});
	auto info_msg = receive_one(*control, 10'000, stop);
	auto * info = info_msg ? get_as<wivrn::from_headset::headset_info_packet>(*info_msg) : nullptr;
	if (info == nullptr)
	{
		log("link: no headset info, dropping");
		return false;
	}
	bool has_h264 = false;
	for (const auto & cap: info->supported_codecs)
	{
		if (cap.codec == wivrn::h264)
			has_h264 = true;
	}
	if (!has_h264 || info->render_eye_width == 0 || info->render_eye_height == 0)
	{
		log("link: headset lacks h264/dimensions, dropping");
		return false;
	}

	{
		std::lock_guard lock(mutex_);
		control_ = std::move(control);
		stream_ = std::move(stream);
		info_ = *info;
		connected_ = true;
	}
	log("link: connected (%ux%u, %zu codecs)", info->render_eye_width, info->render_eye_height, info->supported_codecs.size());
	if (events_.connected)
	{
		try
		{
			events_.connected(*info);
		}
		catch (...)
		{
		}
	}
	return true;
}

void HeadsetLink::control_rx_loop(std::stop_token stop)
{
	while (!stop.stop_requested())
	{
		try
		{
			auto packet = receive_one(*control_, 100, stop);
			if (!packet.has_value())
				continue;
			if (auto * feedback = get_as<wivrn::from_headset::feedback>(*packet))
			{
				// NACK (never displayed) forces an IDR like the Linux path.
				if (feedback->times_displayed == 0 && events_.need_idr)
				{
					try
					{
						events_.need_idr();
					}
					catch (...)
					{
					}
				}
			}
			else if (auto * settings = get_as<wivrn::from_headset::settings_changed>(*packet))
			{
				if (events_.settings_changed && settings->bitrate_bps > 0)
				{
					try
					{
						float fps = settings->preferred_refresh_rate / (float)(settings->fps_divider != 0 ? settings->fps_divider : 1);
						events_.settings_changed(settings->bitrate_bps, (uint32_t)fps);
					}
					catch (...)
					{
					}
				}
			}
		}
		catch (const std::exception & e)
		{
			log("link: control rx ended: %s", e.what());
			break;
		}
	}
	{
		std::lock_guard lock(mutex_);
		connected_ = false;
	}
}

void HeadsetLink::stream_rx_loop(std::stop_token stop)
{
	log("link: stream rx start (tmp-debug)");
	while (!stop.stop_requested())
	{
		try
		{
			std::unique_lock lock(mutex_);
			bool has_stream = stream_ != nullptr;
			lock.unlock();
			if (!has_stream)
				return;
			auto packet = receive_one(*stream_, 100, stop);
			if (!packet.has_value())
				continue;
			if (auto * tracking = get_as<wivrn::from_headset::tracking>(*packet))
				source_->update_tracking(*tracking);
			else if (auto * inputs = get_as<wivrn::from_headset::inputs>(*packet))
				source_->update_inputs(*inputs);
			else if (auto * response = get_as<wivrn::from_headset::timesync_response>(*packet))
				clock_.observe(response->query, response->response, now_ns());
		}
		catch (const std::exception & e)
		{
			log("link: stream rx ended: %s", e.what());
			break;
		}
	}
	{
		std::lock_guard lock(mutex_);
		connected_ = false;
	}
}

void HeadsetLink::send_video_stream_description(uint16_t width, uint16_t height, float fps)
{
	std::lock_guard lock(mutex_);
	if (!connected_ || !control_)
		return;
	try
	{
		control_->send(wivrn::to_headset::video_stream_description{
		        .width = width,
		        .height = height,
		        .codec = {wivrn::h264, wivrn::h264, wivrn::h264},
		        .frame_rate = fps,
		        .refresh_rate = fps,
		});
	}
	catch (const std::exception & e)
	{
		log("link: stream description send failed: %s", e.what());
		connected_ = false;
	}
}

void HeadsetLink::send_tracking_control()
{
	std::lock_guard lock(mutex_);
	if (!connected_ || !control_)
		return;
	try
	{
		// Minimal pattern: the client REQUIRES a HEAD entry.
		control_->send(wivrn::to_headset::tracking_control{
		        .pattern = {{wivrn::device_id::HEAD, 11'000'000}},
		        .motions_to_photons = 22'000'000,
		});
	}
	catch (const std::exception & e)
	{
		log("link: tracking control send failed: %s", e.what());
		connected_ = false;
	}
}

void HeadsetLink::send_timesync_query()
{
	std::lock_guard lock(mutex_);
	if (!connected_)
		return;
	try
	{
		if (stream_)
			stream_->send(wivrn::to_headset::timesync_query{.query = now_ns()});
		else if (control_)
			control_->send(wivrn::to_headset::timesync_query{.query = now_ns()});
	}
	catch (...)
	{
		connected_ = false;
	}
}

void HeadsetLink::send_frame(OutgoingFrame frame)
{
	std::lock_guard lock(mutex_);
	if (!connected_ || !control_)
		return;
	bool use_stream = stream_ != nullptr && !frame.reliable;
	try
	{
		wivrn::to_headset::video_stream_data_shard shard;
		shard.stream_item_idx = frame.stream_idx;
		shard.frame_idx = frame.frame_idx;
		shard.shard_idx = 0;
		bool first = true;
		for (size_t i = 0; i < frame.shards.size(); ++i)
		{
			bool last = i + 1 == frame.shards.size();
			shard.payload = std::span<uint8_t>(frame.shards[i].data(), frame.shards[i].size());
			shard.view_info.reset();
			shard.timing_info.reset();
			if (first)
			{
				shard.view_info = frame.view_info;
				first = false;
			}
			if (last)
				shard.timing_info = frame.timing_info;
			if (frame.reliable)
				control_->send(wivrn::to_headset::video_stream_data_shard{shard});
			else if (use_stream)
				stream_->send(wivrn::to_headset::video_stream_data_shard{shard});
			else
				control_->send(wivrn::to_headset::video_stream_data_shard{shard});
			++shard.shard_idx;
		}
	}
	catch (const std::exception & e)
	{
		log("link: frame send failed: %s", e.what());
		connected_ = false;
	}
}

} // namespace wivrn_macos
