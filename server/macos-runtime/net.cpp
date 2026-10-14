// Headset link implementation. Wire sequence mirrors
// server/driver/wivrn_connection.cpp:166-286 with encryption_disabled
// (no SMP/secrets/AES in v1).

#include "net.h"

#include "runtime.h"    // log()
#include "vk_context.h" // now_ns()

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

namespace wivrn_macos
{
namespace
{

using FromPackets = wivrn::from_headset::packets;

// Dual-stack peer formatting (the UDP socket is v6 with v4-mapped).
std::string format_peer(const sockaddr_in6 & addr)
{
	char host[INET6_ADDRSTRLEN] = {};
	uint16_t port = ntohs(addr.sin6_port);
	if (IN6_IS_ADDR_V4MAPPED(&addr.sin6_addr))
	{
		::inet_ntop(AF_INET, addr.sin6_addr.s6_addr + 12, host, sizeof(host));
	}
	else
	{
		::inet_ntop(AF_INET6, &addr.sin6_addr, host, sizeof(host));
	}
	return std::string(host) + ":" + std::to_string(port);
}

// Poll-then-receive with a total timeout, sliced for stop checks.
// Returns nullopt on timeout; throws on HUP/ERR or stop.
template <typename Socket>
std::optional<FromPackets> receive_one(Socket & socket, int timeout_ms, std::stop_token stop)
{
	int waited = 0;
	// Consecutive HUP-without-data observations (resets on any received
	// packet; bounds teardown latency on real half-open death).
	thread_local int empty_hups = 0;
	while (waited < timeout_ms)
	{
		if (stop.stop_requested())
			throw std::runtime_error("link stopping");
		if (auto pending = socket.receive_pending())
		{
			empty_hups = 0;
			return pending;
		}
		pollfd pfd{.fd = socket.get_fd(), .events = POLLIN, .revents = 0};
		int step = std::min(100, timeout_ms - waited);
		int r = ::poll(&pfd, 1, step);
		waited += step;
		if (r < 0)
			throw std::system_error(errno, std::generic_category());
		if (pfd.revents & (POLLHUP | POLLERR))
		{
			// HUP is advisory: verify with a peek before tearing down.
			// EOF (0) means a real close; data means drain first; EAGAIN
			// with HUP set is a transient edge (observed on Darwin with
			// IPv4-mapped peers) — keep polling, bounded, so a real
			// half-open death still surfaces within seconds.
			char probe = 0;
			ssize_t n = ::recv(socket.get_fd(), &probe, 1, MSG_PEEK);
			if (n == 0)
				throw std::runtime_error("socket hup/err (eof)");
			if (n < 0 && errno != EAGAIN)
				throw std::system_error(errno, std::generic_category());
			if (n < 0)
			{
				if (++empty_hups > 50)
				{
					empty_hups = 0;
					throw std::runtime_error("socket hup/err (persistent)");
				}
				continue;
			}
			// Data pending alongside HUP: fall through to POLLIN below.
		}
		if (pfd.revents & POLLIN)
		{
			try
			{
				if (auto packet = socket.receive())
				{
					empty_hups = 0;
					return packet;
				}
			}
			catch (const std::system_error & e)
			{
				if (e.code().value() != EAGAIN)
					throw;
				// Raced with the poll: keep waiting.
			}
		}
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

// ---- Transport tuning knobs (env, defaults active) ----
//
// WIVRN_MACOS_PACED_SEND=0/1  shard pacing over the UDP stream socket
//                             (default 1).
// WIVRN_MACOS_SHARD_BURST=N   max shards sent back-to-back before a
//                             pacing slice (default 8, clamped 1..256).
// WIVRN_MACOS_TCP_ONLY=0/1    force the TCP-only video path (stream
//                             shards over control; default 0). Read here so
//                             USB setups can force it without touching the
//                             streamer (LinkConfig lives in net.h).
//
// AF41 (DSCP 34) traffic class for the UDP stream socket: low-delay
// Assured-Forwarding video class, best-effort (logged, never fatal).
constexpr int kStreamTos = 0x88; // DSCP 34 << 2

bool env_is_true(const char * name, bool def)
{
	const char * v = std::getenv(name);
	if (v == nullptr || v[0] == '\0')
		return def;
	if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y')
		return true;
	if ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N'))
		return true; // "on"
	return false;
}

long env_long(const char * name, long def)
{
	const char * v = std::getenv(name);
	if (v == nullptr || v[0] == '\0')
		return def;
	char * end = nullptr;
	long parsed = std::strtol(v, &end, 10);
	if (end == v)
		return def;
	return parsed;
}

int64_t steady_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Best-effort stream-socket tuning at pin time: DSCP AF41 (v4-mapped leg
// via IP_TOS, native v6 leg via IPV6_TCLASS) + a 1 MB receive buffer so
// tracking/feedback bursts never drop on ingress. TX stays 5 MB (set by
// the caller). Failures are logged, never thrown.
template <typename Socket>
void tune_stream_socket(Socket & udp)
{
	bool tos_ok = false;
	int tos_err = 0;
	try
	{
		udp.set_tos(kStreamTos);
		tos_ok = true;
	}
	catch (const std::system_error & e)
	{
		tos_err = e.code().value();
	}
	int tclass = kStreamTos;
	bool tclass_ok = ::setsockopt(udp.get_fd(), IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass)) == 0;
	int tclass_err = tclass_ok ? 0 : errno;
	if (tos_ok || tclass_ok)
		log("link: stream DSCP AF41 (tos=%s tclass=%s)", tos_ok ? "ok" : "fail", tclass_ok ? "ok" : "fail");
	else
		log("link: stream DSCP set failed (tos=%s tclass=%s)", std::strerror(tos_err), std::strerror(tclass_err));
	int rcvbuf = 1 * 1024 * 1024;
	if (::setsockopt(udp.get_fd(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) != 0)
		log("link: stream SO_RCVBUF set failed: %s", std::strerror(errno));
}

} // namespace

void HeadsetClock::observe(int64_t query_sent_ns, int64_t response_headset_ns, int64_t received_ns)
{
	int64_t rtt = received_ns - query_sent_ns;
	int64_t sample = response_headset_ns - received_ns + rtt / 2;
	bool first = false;
	{
		std::lock_guard lock(mutex_);
		first = !have_sample_;
		offset_ = have_sample_ ? offset_ * 0.875 + sample * 0.125 : (double)sample;
		have_sample_ = true;
	}
	// The first sample snaps the offset: until it lands every frame's
	// display_time is in the wrong clock domain (log it — a missing
	// line means timesync responses never arrive).
	if (first)
		log("link: timesync converged, offset=%lldms", (long long)(sample / 1'000'000));
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
	// Sides go active only while their devices are present in the
	// packet (controllers asleep/absent must read inactive, not stale).
	bool saw_left = false, saw_right = false, saw_grip = false;
	for (const auto & pose: tracking.device_poses)
	{
		if (pose.device == wivrn::device_id::HEAD)
			sample_.hmd = pose.pose;
		else if (pose.device == wivrn::device_id::LEFT_GRIP || pose.device == wivrn::device_id::LEFT_AIM)
		{
			sample_.left.pose = pose.pose;
			saw_left = true;
			saw_grip = true;
		}
		else if (pose.device == wivrn::device_id::RIGHT_GRIP || pose.device == wivrn::device_id::RIGHT_AIM)
		{
			sample_.right.pose = pose.pose;
			saw_right = true;
			saw_grip = true;
		}
	}
	sample_.left.active = saw_left;
	sample_.right.active = saw_right;
	sample_.timestampNs = now_ns();
	for (size_t i = 0; i < 2 && i < tracking.views.size(); ++i)
	{
		echo_.pose[i] = tracking.views[i].pose;
		echo_.fov[i] = tracking.views[i].fov;
		// Live optics for LocateViews (accept only a usable frustum;
		// anything else keeps the signed fallback: has_fov stays false).
		if (sane_fov(tracking.views[i].fov))
		{
			sample_.fov[i] = tracking.views[i].fov;
			sample_.has_fov = true;
		}
	}
	echo_.valid = true;
	// First live sample: view_info poses + the runtime's HEAD tracking
	// start flowing from here (a missing line means client->server UDP
	// never arrives).
	if (!live_logged_)
	{
		live_logged_ = true;
		log("link: tracking live");
	}
	// First grip/aim receipt: controller poses flow end to end from here
	// (a missing line with awake controllers means the client ignores the
	// grip pattern entries).
	if (saw_grip && !grips_logged_)
	{
		grips_logged_ = true;
		log("link: grips live (left=%d right=%d)", (int)saw_left, (int)saw_right);
	}
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
	// USB fallback without touching the streamer: force the TCP-only
	// video path (stream_port=-1, shards over control) from the env.
	if (env_is_true("WIVRN_MACOS_TCP_ONLY", false))
	{
		config_.tcp_only = true;
		log("link: TCP-only video forced by WIVRN_MACOS_TCP_ONLY");
	}
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
						tune_stream_socket(*udp);
						log("link: stream pinned to %s", format_peer(addr).c_str());
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
				// Genuine NACK = frame never completed to the decoder
				// (mirrors Linux default_idr_handler: !sent_to_decoder).
				// Completion receipts (sent_to_decoder set, displayed 0
				// times yet) are healthy traffic: every frame produced one
				// and the old times_displayed==0 gate forced ~100% IDRs.
				// last_ack tracks the newest decoded index per stream so
				// stale/duplicate skips don't force pointless IDRs.
				uint8_t stream = feedback->stream_index;
				if (stream < 2)
				{
					if (feedback->received_from_decoder != 0)
					{
						uint64_t ack = last_ack_[stream].load();
						while (ack < feedback->frame_index &&
						       !last_ack_[stream].compare_exchange_weak(ack, feedback->frame_index))
						{
						}
						// The forced IDR landed: the decoder reached past
						// the force point, so re-arm the gate.
						if (idr_waiting_[stream].load() && feedback->frame_index > idr_force_point_[stream].load())
							idr_waiting_[stream].store(false);
					}
					// Journey log on completions/NACKs only: per-display
					// updates (disp>=1) flood at 240 lines/s otherwise.
					if (feedback->times_displayed == 0)
						log("link: fb stream=%u frame=%llu recv=%lld/%lld todec=%lld fromdec=%lld blit=%lld displ=%lld",
						    stream,
						    (unsigned long long)feedback->frame_index,
						    (long long)feedback->received_first_packet,
						    (long long)feedback->received_last_packet,
						    (long long)feedback->sent_to_decoder,
						    (long long)feedback->received_from_decoder,
						    (long long)feedback->blitted,
						    (long long)feedback->displayed);
					if (!feedback->sent_to_decoder && events_.need_idr && nack_should_force(stream, feedback->frame_index))
					{
						log("link: NACK stream=%u frame=%llu, forcing IDR", stream, (unsigned long long)feedback->frame_index);
						try
						{
							events_.need_idr();
						}
						catch (...)
						{
						}
					}
				}
			}
			else if (auto * settings = get_as<wivrn::from_headset::settings_changed>(*packet))
			{
				if (events_.settings_changed && settings->bitrate_bps > 0)
				{
					log("link: settings bitrate=%u refresh=%.0f",
					    settings->bitrate_bps,
					    (double)settings->preferred_refresh_rate);
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
		// HEAD drives views/reprojection; grips + aims drive the app's
		// controller poses (hello_xr's hand cubes). The client only sends
		// devices the pattern asks for — HEAD-only meant no cubes, ever.
		control_->send(wivrn::to_headset::tracking_control{
		        .pattern = {{wivrn::device_id::HEAD, 11'000'000},
			            {wivrn::device_id::LEFT_GRIP, 11'000'000},
			            {wivrn::device_id::RIGHT_GRIP, 11'000'000},
			            {wivrn::device_id::LEFT_AIM, 11'000'000},
			            {wivrn::device_id::RIGHT_AIM, 11'000'000}},
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
		// UDP loss is normal (dropped query, next one follows); only a
		// control-path failure means the headset is gone.
		if (!stream_)
			connected_ = false;
	}
}

void HeadsetLink::send_timesync_burst()
{
	std::lock_guard lock(mutex_);
	if (!connected_ || !control_)
		return;
	try
	{
		for (int i = 0; i < 4; ++i)
			control_->send(wivrn::to_headset::timesync_query{.query = now_ns()});
	}
	catch (const std::exception & e)
	{
		log("link: timesync burst failed: %s", e.what());
		connected_ = false;
	}
}

void HeadsetLink::note_frame_sent(uint8_t stream, uint64_t frame_idx, bool reliable)
{
	if (stream >= 2)
		return;
	uint64_t prev = last_sent_idx_[stream].load();
	while (prev < frame_idx && !last_sent_idx_[stream].compare_exchange_weak(prev, frame_idx))
	{
	}
	if (!reliable)
		return;
	uint64_t prev_rel = last_reliable_idx_[stream].load();
	while (prev_rel < frame_idx && !last_reliable_idx_[stream].compare_exchange_weak(prev_rel, frame_idx))
	{
	}
	reliable_seen_[stream].store(true);
}

// NACK->IDR damping (Linux default_idr_handler ideas, wire semantics
// unchanged): stale NACKs, NACKs covered by an IDR already on the wire,
// and duplicates while a forced IDR is still in flight all coalesce — a
// loss storm costs one force per decoder round-trip, not one per NACK.
// The both-eyes co-force is preserved: a true return fires need_idr,
// whose handler forces L+R together (the client needs the pair).
bool HeadsetLink::nack_should_force(uint8_t stream, uint64_t frame_idx)
{
	int64_t now = steady_ns();
	// Stale: the decoder already got past this frame (mirrors the Linux
	// running.last_ack gate: only frame_index >= last_ack forces).
	if (frame_idx < last_ack_[stream].load())
	{
		uint64_t n = ++idr_suppressed_;
		if (n == 1 || n % 60 == 0)
			log("link: NACK stream=%u frame=%llu suppressed (stale, ack=%llu)",
			    stream,
			    (unsigned long long)frame_idx,
			    (unsigned long long)last_ack_[stream].load());
		return false;
	}
	// Covered: an IDR newer than the NACK is already encoded and on the
	// wire (reliable/TCP, so it will arrive). No re-force.
	if (reliable_seen_[stream].load() && frame_idx <= last_reliable_idx_[stream].load())
	{
		uint64_t n = ++idr_suppressed_;
		if (n == 1 || n % 60 == 0)
			log("link: NACK stream=%u frame=%llu suppressed (covered, rel=%llu)",
			    stream,
			    (unsigned long long)frame_idx,
			    (unsigned long long)last_reliable_idx_[stream].load());
		return false;
	}
	int64_t last_force = last_force_ns_[stream].load();
	// In flight: a force is outstanding and the decoder hasn't confirmed
	// past it (Linux wait_idr_feedback). Coalesce unless the wait clearly
	// timed out (>1 s or >100 frames sent since the force, mirroring the
	// Linux 100-frame IDR timeout) — then re-arm and evaluate fresh.
	if (idr_waiting_[stream].load())
	{
		bool timed_out = (now - last_force > 1'000'000'000) ||
		                 (last_sent_idx_[stream].load() > idr_force_point_[stream].load() + 100);
		if (!timed_out)
		{
			uint64_t n = ++idr_suppressed_;
			if (n == 1 || n % 60 == 0)
				log("link: NACK stream=%u frame=%llu suppressed (idr in flight)", stream, (unsigned long long)frame_idx);
			return false;
		}
		idr_waiting_[stream].store(false);
	}
	// Cooldown: at most one force per 100 ms per stream. A genuinely new
	// loss right after a landed IDR waits out the remainder; everything
	// older is already covered by the stale/covered rules above.
	if (now - last_force < 100'000'000)
	{
		uint64_t n = ++idr_suppressed_;
		if (n == 1 || n % 60 == 0)
			log("link: NACK stream=%u frame=%llu suppressed (cooldown)", stream, (unsigned long long)frame_idx);
		return false;
	}
	idr_force_point_[stream].store(last_sent_idx_[stream].load());
	last_force_ns_[stream].store(now);
	idr_waiting_[stream].store(true);
	++idr_forced_;
	return true;
}

void HeadsetLink::send_frame(OutgoingFrame frame)
{
	std::lock_guard lock(mutex_);
	if (!connected_ || !control_)
		return;
	uint8_t stream = frame.stream_idx;
	bool use_stream = stream_ != nullptr && !frame.reliable;
	// The encoder advanced past this index even if the send below drops
	// the frame: record it for the NACK gate up front.
	note_frame_sent(stream, frame.frame_idx, false);
	size_t nshards = frame.shards.size();

	// Pacing budget: spread this frame's shards over min(1 ms, measured
	// inter-send interval / 4) per eye, so both eyes together stay well
	// under half a frame interval (worst case ~2.7 ms incl. sleep
	// overshoot vs 4.16 ms at 120 Hz). Only the UDP path is paced
	// (reliable shards ride TCP, where the kernel paces already).
	bool paced = use_stream && env_is_true("WIVRN_MACOS_PACED_SEND", true);
	long burst = env_long("WIVRN_MACOS_SHARD_BURST", 8);
	if (burst < 1)
		burst = 1;
	if (burst > 256)
		burst = 256;
	int64_t budget_ns = 1'000'000;
	if (paced && stream < 2)
	{
		int64_t send_now = steady_ns();
		int64_t prev_send = last_send_ns_[stream].load();
		if (prev_send != 0)
		{
			int64_t interval = send_now - prev_send;
			if (interval < 2'000'000)
				interval = 2'000'000;
			if (interval > 50'000'000)
				interval = 50'000'000;
			if (interval / 4 < budget_ns)
				budget_ns = interval / 4;
		}
		last_send_ns_[stream].store(send_now);
	}
	int64_t slice_ns = (paced && nshards > 0) ? budget_ns * burst / (int64_t)nshards : 0;

	// Transient send-buffer exhaustion: wait writable up to 1 ms per
	// frame total, then drop the frame on the existing path. UDP sockets
	// are non-blocking, so a full buffer raises EAGAIN (or ENOBUFS);
	// anything else is fatal and propagates as before.
	int64_t eagain_deadline = 0;
	bool stalled = false;
	size_t sent_shards = 0;
	auto send_udp = [&](const wivrn::to_headset::video_stream_data_shard & s) {
		for (;;)
		{
			try
			{
				stream_->send(wivrn::to_headset::video_stream_data_shard{s});
				return;
			}
			catch (const std::system_error & e)
			{
				int err = e.code().value();
				if (err != EAGAIN && err != ENOBUFS)
					throw;
				int64_t now = steady_ns();
				if (eagain_deadline == 0)
				{
					eagain_deadline = now + 1'000'000;
					stalled = true;
				}
				if (now >= eagain_deadline)
					throw;
				pollfd pfd{.fd = stream_->get_fd(), .events = POLLOUT, .revents = 0};
				::poll(&pfd, 1, 1);
			}
		}
	};

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
				send_udp(shard);
			else
				control_->send(wivrn::to_headset::video_stream_data_shard{shard});
			++shard.shard_idx;
			++sent_shards;
			// Pace after each burst group — never after the tail shard,
			// so no sleep is added past the frame's last byte.
			if (slice_ns > 0 && (i + 1) % (size_t)burst == 0 && i + 1 < frame.shards.size())
				std::this_thread::sleep_for(std::chrono::nanoseconds(slice_ns));
		}
		if (frame.reliable)
			note_frame_sent(stream, frame.frame_idx, true);
		if (stalled)
			++eagain_saved_;
		uint64_t sent = ++sent_frames_;
		// Periodic transport accounting (key=value like the streamer
		// heartbeat; the `link: fb` journey lines above stay byte-stable
		// for existing parsers).
		if (sent % 300 == 0)
			log("link: net sent=%llu drop=%llu dshard=%llu estall=%llu esaved=%llu idr_force=%llu idr_supp=%llu",
			    (unsigned long long)sent,
			    (unsigned long long)dropped_frames_.load(),
			    (unsigned long long)dropped_shards_.load(),
			    (unsigned long long)eagain_stalls_.load(),
			    (unsigned long long)eagain_saved_.load(),
			    (unsigned long long)idr_forced_.load(),
			    (unsigned long long)idr_suppressed_.load());
	}
	catch (const std::exception & e)
	{
		// The control path is fate-sharing with the connection, but UDP
		// loss is normal (burst EAGAIN on a full send buffer): drop the
		// frame and keep streaming. Latching disconnect here once froze
		// a live session with the worker still "encoding" behind it.
		if (frame.reliable || !use_stream)
		{
			log("link: frame send failed: %s", e.what());
			connected_ = false;
		}
		else
		{
			if (stalled)
				++eagain_stalls_;
			dropped_shards_ += nshards - sent_shards;
			uint64_t dropped = ++dropped_frames_;
			if (dropped == 1 || dropped % 60 == 0)
				log("link: stream send dropped frame %llu (%s)", (unsigned long long)dropped, e.what());
		}
	}
}

} // namespace wivrn_macos
