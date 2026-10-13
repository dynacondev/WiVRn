// Phase 5b proof: full handshake + steady-state exchange against a
// loopback fake headset (no real hardware). Exercises the exact wire
// sequence a Quest will drive in headset testing. Run directly:
//   ./wivrn-macos-net-test

#include "net.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace wivrn_macos;

static int failures = 0;

#define CHECK(cond)                                                                          \
	do                                                                                   \
	{                                                                                    \
		if (!(cond))                                                                 \
		{                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                            \
	} while (false)

#define CHECK_OP(a, op, b)                                                                               \
	do                                                                                               \
	{                                                                                                \
		if (!((a)op(b)))                                                                         \
		{                                                                                        \
			std::fprintf(stderr, "FAIL %s:%d: %s %s %s\n", __FILE__, __LINE__, #a, #op, #b); \
			++failures;                                                                      \
		}                                                                                        \
	} while (false)

namespace
{

constexpr uint16_t kPort = 19757;

sockaddr_in loopback4(uint16_t port)
{
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	return sa;
}

sockaddr_in6 loopback6(uint16_t port)
{
	sockaddr_in6 sa{};
	sa.sin6_family = AF_INET6;
	sa.sin6_port = htons(port);
	sa.sin6_addr = in6addr_loopback;
	return sa;
}

// Fake-headset sockets (mirrored template args: receives to_headset).
using FakeControl = wivrn::typed_socket<wivrn::TCP, wivrn::to_headset::packets, wivrn::from_headset::packets>;
using FakeStream = wivrn::typed_socket<wivrn::UDP, wivrn::to_headset::packets, wivrn::from_headset::packets>;

// Expect one packet of type Want within timeout, validating it inline
// (packets borrow socket buffers: never retain the pointer, only copy
// out scalars inside check()).
template <typename Socket, typename Want, typename Check>
bool expect(Socket & socket, int timeout_ms, const char * what, Check && check)
{
	int waited = 0;
	while (waited < timeout_ms)
	{
		auto drain = [&](auto & packet) -> bool {
			auto * got = std::get_if<Want>(&packet);
			if (got == nullptr)
			{
				std::fprintf(stderr, "FAIL: %s: unexpected packet (wrong type)\n", what);
				++failures;
				return true; // stop waiting; the peer is out of script
			}
			if (!check(*got))
			{
				std::fprintf(stderr, "FAIL: %s: content mismatch\n", what);
				++failures;
			}
			return true;
		};
		if (auto pending = socket.receive_pending())
		{
			if (drain(*pending))
				return failures == 0;
			continue;
		}
		pollfd pfd{.fd = socket.get_fd(), .events = POLLIN, .revents = 0};
		int r = ::poll(&pfd, 1, 50);
		waited += 50;
		if (r < 0)
		{
			std::fprintf(stderr, "FAIL: %s: poll error\n", what);
			++failures;
			return false;
		}
		if (pfd.revents & POLLIN)
		{
			try
			{
				if (auto packet = socket.receive())
				{
					if (drain(*packet))
						return failures == 0;
					continue;
				}
			}
			catch (const std::system_error & e)
			{
				if (e.code().value() != EAGAIN)
					throw;
			}
		}
	}
	std::fprintf(stderr, "FAIL: %s: timeout\n", what);
	++failures;
	return false;
}

template <typename Socket, typename Want>
bool expect(Socket & socket, int timeout_ms, const char * what)
{
	return expect<Socket, Want>(socket, timeout_ms, what, [](const Want &) { return true; });
}

} // namespace

int main()
{
	std::atomic<bool> got_connected{false}, got_disconnected{false}, got_idr{false};
	std::atomic<uint32_t> got_bitrate{0};
	HeadsetLink link;
	HeadsetLink::Events events;
	events.connected = [&](const wivrn::from_headset::headset_info_packet &) { got_connected = true; };
	events.disconnected = [&] { got_disconnected = true; };
	events.need_idr = [&] { got_idr = true; };
	events.settings_changed = [&](uint32_t bitrate, uint32_t) { got_bitrate = bitrate; };
	CHECK(link.start(LinkConfig{.port = kPort, .tcp_only = false}, std::move(events)));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// ---- Fake headset: TCP hello ----
	FakeControl control{loopback4(kPort)};
	control.send(wivrn::from_headset::crypto_handshake{
	        .protocol_version = wivrn::protocol_version,
	        .public_key = {},
	        .name = "net-test-headset",
	});
	{
		using Reply = wivrn::to_headset::crypto_handshake;
		bool crypto_ok = expect<FakeControl, Reply>(control, 5000, "crypto reply", [](const Reply & reply) {
			return reply.state == Reply::crypto_state::encryption_disabled;
		});
		CHECK(crypto_ok);
	}
	control.send(wivrn::from_headset::crypto_handshake{});
	{
		using Offer = wivrn::to_headset::handshake;
		bool offer_ok = expect<FakeControl, Offer>(control, 5000, "stream offer", [](const Offer & hs) {
			return hs.stream_port == kPort;
		});
		CHECK(offer_ok);
	}

	// ---- UDP stream hello (retransmitted like the real client) ----
	FakeStream stream;
	stream.bind(loopback6(0));
	stream.connect(loopback6(kPort));
	for (int i = 0; i < 20; ++i)
	{
		stream.send(wivrn::from_headset::handshake{});
		if (auto pending = control.receive_pending())
		{
			if (std::get_if<wivrn::to_headset::handshake>(&*pending) != nullptr)
				break;
		}
		pollfd pfd{.fd = control.get_fd(), .events = POLLIN, .revents = 0};
		::poll(&pfd, 1, 100);
		if (pfd.revents & POLLIN)
		{
			try
			{
				if (auto packet = control.receive())
				{
					if (std::get_if<wivrn::to_headset::handshake>(&*packet) != nullptr)
						break;
				}
			}
			catch (...)
			{
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	// ---- Headset info ----
	{
		wivrn::from_headset::headset_info_packet info{};
		info.render_eye_width = 512;
		info.render_eye_height = 512;
		info.stream_eye_width = 512;
		info.stream_eye_height = 512;
		info.available_refresh_rates = {90.0f};
		info.settings.preferred_refresh_rate = 90.0f;
		info.settings.minimum_refresh_rate = 60.0f;
		info.settings.fps_divider = 1;
		info.settings.bitrate_bps = 20'000'000;
		info.settings.mirror_gamepad = false;
		info.settings.enabled_body_parts = 0;
		info.hand_tracking = false;
		info.eye_gaze = false;
		info.palm_pose = false;
		info.user_presence = false;
		info.passthrough = false;
		info.face_tracking = wivrn::from_headset::face_type::none;
		info.body_tracking = wivrn::from_headset::body_type::none;
		info.num_generic_trackers = 0;
		info.supported_codecs = {{wivrn::h264, false}};
		info.system_name = "net-test";
		info.language = "en";
		info.country = "US";
		info.variant = "test";
		control.send(std::move(info));
	}
	for (int i = 0; i < 100 && !got_connected; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK(got_connected);
	CHECK(link.connected());
	{
		auto info = link.info();
		CHECK(info.has_value());
		if (info.has_value())
			CHECK(info->render_eye_width == 512);
	}

	// Server-initiated descriptions (normally sent by the streamer).
	link.send_video_stream_description(512, 512, 90.0f);
	link.send_tracking_control();
	{
		using Desc = wivrn::to_headset::video_stream_description;
		bool desc_ok = expect<FakeControl, Desc>(control, 5000, "stream desc", [](const Desc & desc) {
			return desc.width == 512 && desc.height == 512 && desc.codec[0] == wivrn::h264;
		});
		CHECK(desc_ok);
		using TC = wivrn::to_headset::tracking_control;
		bool tc_ok = expect<FakeControl, TC>(control, 5000, "tracking control", [](const TC & tc) {
			return !tc.pattern.empty() && tc.pattern[0].device == wivrn::device_id::HEAD;
		});
		CHECK(tc_ok);
	}

	// ---- Tracking feed -> runtime sample ----
	{
		wivrn::from_headset::tracking tracking{};
		tracking.production_timestamp = 1000;
		tracking.timestamp = 2000;
		tracking.view_flags = 0;
		tracking.state_flags = 0;
		tracking.views[0] = {{{0, 0, 0, 1}, {0, 0, 0}}, {0.8f, 0.8f, 0.8f, 0.8f}};
		tracking.views[1] = {{{0, 0, 0, 1}, {0, 0, 0}}, {0.8f, 0.8f, 0.8f, 0.8f}};
		wivrn::from_headset::tracking::pose head{};
		head.pose = {{0, 0, 0, 1}, {1.0f, 2.0f, 3.0f}};
		head.device = wivrn::device_id::HEAD;
		wivrn::from_headset::tracking::pose grip{};
		grip.pose = {{0, 0, 0, 1}, {0.2f, 1.3f, -0.3f}};
		grip.device = wivrn::device_id::RIGHT_GRIP;
		tracking.device_poses = {head, grip};
		stream.send(std::move(tracking));

		wivrn::from_headset::inputs inputs{};
		inputs.values.push_back({wivrn::device_id::RIGHT_TRIGGER_CLICK, 1.0f, 3000});
		inputs.values.push_back({wivrn::device_id::RIGHT_TRIGGER_VALUE, 0.75f, 3000});
		stream.send(std::move(inputs));
	}
	{
		TrackingSample sample{};
		bool settled = false;
		for (int i = 0; i < 100; ++i)
		{
			sample = link.source()->latest();
			if (sample.hmd.position.x > 0.9f)
			{
				settled = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		CHECK(settled);
		std::printf(
		        "headset hmd: (%.2f, %.2f, %.2f) right select=%d trigger=%.2f\n",
		        sample.hmd.position.x,
		        sample.hmd.position.y,
		        sample.hmd.position.z,
		        (int)sample.right.selectClick,
		        sample.right.triggerValue);
		CHECK_OP(sample.hmd.position.x, >, 0.9f);
		CHECK_OP(sample.right.selectClick, ==, true);
		CHECK_OP(sample.right.triggerValue, >, 0.7f);
		auto echo = link.source()->views_echo();
		CHECK(echo.valid);
	}

	// ---- Timesync roundtrip (known +80 ms headset lead) ----
	link.send_timesync_query();
	{
		using Query = wivrn::to_headset::timesync_query;
		bool query_ok = expect<FakeStream, Query>(stream, 5000, "timesync query", [&](const Query & query) {
			const int64_t kLead = 80'000'000;
			stream.send(wivrn::from_headset::timesync_response{.query = query.query, .response = query.query + kLead});
			return true;
		});
		CHECK(query_ok);
	}
	{
		bool converged = false;
		for (int i = 0; i < 100; ++i)
		{
			int64_t offset = link.clock().offset_ns();
			if (offset > 70'000'000 && offset < 90'000'000)
			{
				converged = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		CHECK(converged);
		std::printf("clock offset: %lld ns\n", (long long)link.clock().offset_ns());
	}

	// ---- Feedback NACK -> IDR, settings -> reconfig event ----
	{
		wivrn::from_headset::feedback nack{};
		nack.frame_index = 7;
		nack.stream_index = 0;
		nack.times_displayed = 0;
		control.send(std::move(nack));
		wivrn::from_headset::settings_changed settings{};
		settings.preferred_refresh_rate = 90.0f;
		settings.minimum_refresh_rate = 60.0f;
		settings.fps_divider = 1;
		settings.bitrate_bps = 10'000'000;
		settings.mirror_gamepad = false;
		settings.enabled_body_parts = 0;
		control.send(std::move(settings));
		bool idr = false, bitrate = false;
		for (int i = 0; i < 100; ++i)
		{
			idr = idr || got_idr;
			bitrate = bitrate || got_bitrate == 10'000'000;
			if (idr && bitrate)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		CHECK(idr);
		CHECK(bitrate);
	}

	link.stop();
	for (int i = 0; i < 40 && !got_disconnected; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	// Disconnect event fires on teardown (stop or peer close); accept it
	// either way but require the link to report disconnected.
	CHECK(!link.connected());

	if (failures == 0)
		std::puts("net test PASS");
	else
		std::printf("net test FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
