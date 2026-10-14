// Phase 5d proof: full pixels-to-wire-to-poses loopback. A fake headset
// (TCP+UDP handshake, tracking at 10 Hz, shard reassembly, NACK) drives
// the REAL runtime dylib through the loader while this process acts as
// the VR app (Vulkan session, swapchains, frame loop). Asserts shards
// arrive with view_info, IDR lands on control after NACK, and headset
// poses flow back into LocateViews. Run with:
//   XR_RUNTIME_JSON=<build>/server/macos-runtime/wivrn-macos.json \
//     ./wivrn-macos-stream-test
// (The test sets WIVRN_PORT/WIVRN_HOSTNAME itself; never set
// WIVRN_MACOS_HEADLESS here — streaming is the point.)
//
// WIVRN_MACOS_STREAM_PHASES selects harness phases (default "loop", the
// proof above; "all" runs everything; comma-separated subset also works):
//   tear — headless generation-tag snapshot race detector (no headset).
//     Paints the frame sequence into a corner block of both eyes and
//     captures through the real EyeSnapshot path on a worker thread
//     while the app pipelines renders. Every capture must carry exactly
//     one generation. FAILS on unfenced snapshots, PASSES once worker
//     captures are ordered after the staged image's render completion.
//   skew — eye-fairness soak: overload burst, then paced recovery, then
//     assert the inter-eye encode completion lag re-converges (mean and
//     p50 of eye1-minus-eye0 arrival times over display-time-matched
//     pairs). FAILS on fixed-order sequential encodes, PASSES once both
//     eyes complete within the bound (parallel or alternating service).

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// White-box harness headers: the runtime dylib keeps its globals, but
// the Swapchain record layout (staged_idx/active_idx handoff) is needed
// to mimic the streamer's stage/worker protocol faithfully, and the real
// EyeSnapshot implementation is compiled in so snapshot fixes are
// tracked automatically (see run_tear).
#include "runtime.h"
#include "snapshot.cpp"

#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include "protocol_version.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

// Milliseconds since process start (matches the WIVRN_MACOS clock,
// which uses the same steady epoch).
static long long test_ms()
{
	static const auto start = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

#define TLOG(fmt, ...) std::fprintf(stderr, "[%8lld] fake: " fmt "\n", test_ms() __VA_OPT__(, ) __VA_ARGS__)

#define CHECK(cond)                                                                          \
	do                                                                                   \
	{                                                                                    \
		if (!(cond))                                                                 \
		{                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                            \
	} while (false)

#define CHECK_XR(call, expected)                                                                                                             \
	do                                                                                                                                   \
	{                                                                                                                                    \
		XrResult res__ = (call);                                                                                                     \
		if (res__ != (expected))                                                                                                     \
		{                                                                                                                            \
			std::fprintf(stderr, "FAIL %s:%d: %s -> %d, expected %d\n", __FILE__, __LINE__, #call, (int)res__, (int)(expected)); \
			++failures;                                                                                                          \
		}                                                                                                                            \
	} while (false)

namespace
{

constexpr uint16_t kPort = 19758;

using FakeControl = wivrn::typed_socket<wivrn::TCP, wivrn::to_headset::packets, wivrn::from_headset::packets>;
using FakeStream = wivrn::typed_socket<wivrn::UDP, wivrn::to_headset::packets, wivrn::from_headset::packets>;

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

// Reassembled eye frame (Annex-B bytes + first-shard view info).
struct GotFrame
{
	std::vector<uint8_t> bytes;
	bool has_view_info = false;
	XrTime display_time = 0;
	bool reliable = false; // arrived on control
	// Identity foveation must decode to the stream dims (client sizes
	// its swapchain from these; zeros crash it — see headset run 1).
	uint32_t fov_w = 0;
	uint32_t fov_h = 0;
	long long arrival_ms = 0; // test_ms() at reassembly (skew soak)
};

struct ShardBin
{
	std::vector<std::vector<uint8_t>> parts; // indexed by shard_idx (gaps empty)
	bool has_view_info = false;
	XrTime display_time = 0;
	uint32_t fov_w = 0;
	uint32_t fov_h = 0;
	bool complete = false;
};

void ingest(
        std::map<std::pair<uint8_t, uint64_t>, ShardBin> & bins,
        std::map<std::pair<uint8_t, uint64_t>, GotFrame> & done,
        const wivrn::to_headset::video_stream_data_shard & shard,
        bool reliable)
{
	auto key = std::make_pair(shard.stream_item_idx, shard.frame_idx);
	ShardBin & bin = bins[key];
	if (shard.shard_idx >= bin.parts.size())
		bin.parts.resize(shard.shard_idx + 1);
	bin.parts[shard.shard_idx] = std::vector<uint8_t>(shard.payload.begin(), shard.payload.end());
	if (shard.view_info.has_value())
	{
		bin.has_view_info = true;
		bin.display_time = shard.view_info->display_time;
		// Mirror the client's defoveated_size math (count_pixels):
		// single ratio-1 segment must yield the segment length.
		auto encoded_size = [](const std::vector<uint16_t> & param) -> uint32_t {
			if (param.empty())
				return 0;
			uint32_t res = 0;
			int n_ratio = ((int)param.size() - 1) / 2;
			for (size_t i = 0; i < param.size(); ++i)
				res += (uint32_t)(abs(n_ratio - (int)i) + 1) * param[i];
			return res;
		};
		bin.fov_w = encoded_size(shard.view_info->foveation[0].x);
		bin.fov_h = encoded_size(shard.view_info->foveation[0].y);
	}
	if (shard.timing_info.has_value())
	{
		bin.complete = true;
		GotFrame frame;
		for (const auto & part: bin.parts)
			frame.bytes.insert(frame.bytes.end(), part.begin(), part.end());
		frame.has_view_info = bin.has_view_info;
		frame.display_time = bin.display_time;
		frame.fov_w = bin.fov_w;
		frame.fov_h = bin.fov_h;
		frame.reliable = reliable;
		frame.arrival_ms = test_ms();
		done[key] = std::move(frame);
		bins.erase(key);
	}
}

bool has_annexb_start(const std::vector<uint8_t> & bytes)
{
	if (bytes.size() < 4)
		return false;
	if (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1)
		return true;
	return bytes.size() > 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1;
}

bool has_sps(const std::vector<uint8_t> & bytes)
{
	// NAL header 0x67 (SPS) right after a start code.
	for (size_t i = 0; i + 4 < bytes.size(); ++i)
	{
		if (bytes[i] == 0 && bytes[i + 1] == 0 && (bytes[i + 2] == 1 || (bytes[i + 2] == 0 && bytes[i + 3] == 1)))
		{
			uint8_t header = bytes[i + 2] == 1 ? bytes[i + 3] : bytes[i + 4];
			if ((header & 0x1f) == 7)
				return true;
		}
	}
	return false;
}

struct FakeState
{
	std::atomic<bool> stop{false};
	std::atomic<bool> handshake_done{false};
	std::atomic<bool> nack_sent{false};
	uint16_t port = kPort;
	std::map<std::pair<uint8_t, uint64_t>, ShardBin> stream_bins;
	std::map<std::pair<uint8_t, uint64_t>, GotFrame> stream_done;
	std::map<std::pair<uint8_t, uint64_t>, ShardBin> control_bins;
	std::map<std::pair<uint8_t, uint64_t>, GotFrame> control_done;
	std::mutex mutex;
};

template <typename Socket, typename Want, typename Check>
bool expect(Socket & socket, int timeout_ms, Check && check)
{
	// Wrong-type packets are skipped (strays from concurrent flows).
	int waited = 0;
	while (waited < timeout_ms)
	{
		if (auto pending = socket.receive_pending())
		{
			if (auto * got = std::get_if<Want>(&*pending))
			{
				if (!check(*got))
				{
					std::fprintf(stderr, "FAIL: content mismatch\n");
					++failures;
				}
				return failures == 0;
			}
			continue;
		}
		pollfd pfd{.fd = socket.get_fd(), .events = POLLIN, .revents = 0};
		::poll(&pfd, 1, 50);
		waited += 50;
		if (pfd.revents & POLLIN)
		{
			try
			{
				if (auto packet = socket.receive())
				{
					if (auto * got = std::get_if<Want>(&*packet))
					{
						if (!check(*got))
						{
							std::fprintf(stderr, "FAIL: content mismatch\n");
							++failures;
						}
						return failures == 0;
					}
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
	std::fprintf(stderr, "FAIL: expect timeout\n");
	++failures;
	return false;
}

void fake_headset(FakeState & state)
{
	try
	{
		// The server listens from app session creation; retry the
		// connect like a real headset user would (server may still be
		// starting up when the headset looks for it).
		std::optional<FakeControl> control;
		for (int i = 0; i < 150 && !state.stop; ++i)
		{
			try
			{
				control.emplace(loopback4(state.port));
				break;
			}
			catch (const std::system_error &)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		}
		if (!control.has_value())
		{
			std::fprintf(stderr, "FAIL: fake connect timeout\n");
			++failures;
			return;
		}
		control->send(wivrn::from_headset::crypto_handshake{
		        .protocol_version = wivrn::protocol_version,
		        .public_key = {},
		        .name = "stream-test-headset",
		});
		using CryptoReply = wivrn::to_headset::crypto_handshake;
		if (!expect<FakeControl, CryptoReply>(*control, 8000, [](const CryptoReply & r) {
			    return r.state == CryptoReply::crypto_state::encryption_disabled;
		    }))
			return;
		control->send(wivrn::from_headset::crypto_handshake{});
		using Offer = wivrn::to_headset::handshake;
		if (!expect<FakeControl, Offer>(*control, 8000, [](const Offer &) { return true; }))
			return;

		FakeStream stream;
		stream.bind(loopback6(0));
		stream.connect(loopback6(state.port));
		for (int i = 0; i < 40; ++i)
		{
			stream.send(wivrn::from_headset::handshake{});
			if (auto pending = control->receive_pending())
			{
				if (std::get_if<wivrn::to_headset::handshake>(&*pending) != nullptr)
					break;
			}
			pollfd pfd{.fd = control->get_fd(), .events = POLLIN, .revents = 0};
			::poll(&pfd, 1, 50);
			if (pfd.revents & POLLIN)
			{
				try
				{
					if (auto packet = control->receive())
					{
						if (std::get_if<wivrn::to_headset::handshake>(&*packet) != nullptr)
							break;
					}
				}
				catch (...)
				{
				}
			}
		}

		wivrn::from_headset::headset_info_packet info{};
		info.render_eye_width = 256;
		info.render_eye_height = 256;
		info.stream_eye_width = 256;
		info.stream_eye_height = 256;
		info.available_refresh_rates = {90.0f};
		info.settings.preferred_refresh_rate = 90.0f;
		info.settings.minimum_refresh_rate = 60.0f;
		info.settings.fps_divider = 1;
		info.settings.bitrate_bps = 5'000'000;
		info.settings.mirror_gamepad = false;
		info.settings.enabled_body_parts = 0;
		info.face_tracking = wivrn::from_headset::face_type::none;
		info.body_tracking = wivrn::from_headset::body_type::none;
		info.supported_codecs = {{wivrn::h264, false}};
		info.system_name = "stream-test";
		info.language = "en";
		control->send(std::move(info));

		// Tracking at ~10 Hz with a distinctive HEAD pose; inputs held.
		auto tracking_loop = [&] {
			int n = 0;
			while (!state.stop)
			{
				wivrn::from_headset::tracking tracking{};
				if (++n % 20 == 1)
					TLOG("tracking send #%d", n);
				tracking.timestamp = n * 100'000'000;
				tracking.views[0] = {{{0, 0, 0, 1}, {0, 0, 0}}, {0.8f, 0.8f, 0.8f, 0.8f}};
				tracking.views[1] = {{{0, 0, 0, 1}, {0, 0, 0}}, {0.8f, 0.8f, 0.8f, 0.8f}};
				wivrn::from_headset::tracking::pose head{};
				head.pose = {{0, 0, 0, 1}, {4.0f, 5.0f, 6.0f}};
				head.device = wivrn::device_id::HEAD;
				tracking.device_poses = {head};
				try
				{
					stream.send(std::move(tracking));
				}
				catch (const std::exception & e)
				{
					TLOG("tracking send failed: %s", e.what());
					return;
				}
				++n;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			TLOG("tracking loop exiting (stop=%d n=%d)", (int)state.stop, n);
		};
		std::thread tracker(tracking_loop);
		TLOG("handshake complete, loops running");
		{
			sockaddr_in6 local{}, peer{};
			socklen_t len = sizeof(local);
			::getsockname(control->get_fd(), (sockaddr *)&local, &len);
			len = sizeof(peer);
			::getpeername(control->get_fd(), (sockaddr *)&peer, &len);
			char lh[INET6_ADDRSTRLEN] = {}, ph[INET6_ADDRSTRLEN] = {};
			inet_ntop(AF_INET6, &local.sin6_addr, lh, sizeof(lh));
			inet_ntop(AF_INET6, &peer.sin6_addr, ph, sizeof(ph));
			std::fprintf(
			        stderr,
			        "fake: control fd=%d local=[%s]:%u peer=[%s]:%u\n",
			        control->get_fd(),
			        lh,
			        (unsigned)ntohs(local.sin6_port),
			        ph,
			        (unsigned)ntohs(peer.sin6_port));
		}

		// Main fake loop: shards in, timesync answers + one NACK out.
		bool nacked = false;
		int stream_frames_seen = 0;
		int loop_count = 0;
		while (!state.stop)
		{
			auto iter_begin = std::chrono::steady_clock::now();
			if (++loop_count % 20 == 0)
				TLOG("loop alive (%d)", loop_count);
			// Stream shards.
			for (;;)
			{
				std::optional<wivrn::to_headset::packets> packet;
				try
				{
					packet = stream.receive_pending();
					if (!packet.has_value())
					{
						pollfd pfd{.fd = stream.get_fd(), .events = POLLIN, .revents = 0};
						if (::poll(&pfd, 1, 20) <= 0 || (pfd.revents & POLLIN) == 0)
							break;
						packet = stream.receive();
					}
				}
				catch (...)
				{
					break;
				}
				if (!packet.has_value())
					break;
				if (auto * shard = std::get_if<wivrn::to_headset::video_stream_data_shard>(&*packet))
				{
					std::lock_guard lock(state.mutex);
					ingest(state.stream_bins, state.stream_done, *shard, false);
					stream_frames_seen = (int)state.stream_done.size();
				}
				else if (auto * query = std::get_if<wivrn::to_headset::timesync_query>(&*packet))
				{
					try
					{
						stream.send(wivrn::from_headset::timesync_response{.query = query->query, .response = query->query});
					}
					catch (...)
					{
					}
				}
			}
			// Control shards (reliable IDR path).
			for (;;)
			{
				std::optional<wivrn::to_headset::packets> packet;
				try
				{
					packet = control->receive_pending();
					if (!packet.has_value())
					{
						pollfd pfd{.fd = control->get_fd(), .events = POLLIN, .revents = 0};
						if (::poll(&pfd, 1, 20) <= 0 || (pfd.revents & POLLIN) == 0)
							break;
						if (pfd.revents & (POLLHUP | POLLERR))
						{
							TLOG("control hup/err");
							break;
						}
						packet = control->receive();
					}
				}
				catch (const std::exception & e)
				{
					TLOG("control recv: %s", e.what());
					break;
				}
				if (!packet.has_value())
					break;
				if (auto * shard = std::get_if<wivrn::to_headset::video_stream_data_shard>(&*packet))
				{
					std::lock_guard lock(state.mutex);
					ingest(state.control_bins, state.control_done, *shard, true);
				}
			}
			// One NACK once stream frames flow, to force an IDR.
			if (!nacked && stream_frames_seen >= 2)
			{
				nacked = true;
				state.nack_sent = true;
				try
				{
					wivrn::from_headset::feedback nack{};
					nack.frame_index = 1;
					nack.times_displayed = 0;
					control->send(std::move(nack));
				}
				catch (...)
				{
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			auto iter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - iter_begin).count();
			if (iter_ms > 100)
				TLOG("slow iter %lldms (n=%d)", (long long)iter_ms, loop_count);
		}
		TLOG("main loop exiting (stop=%d n=%d)", (int)state.stop, loop_count);
		tracker.join();
		TLOG("thread returning normally");
	}
	catch (const std::exception & e)
	{
		std::fprintf(stderr, "fake headset ended: %s\n", e.what());
	}
	catch (...)
	{
		std::fprintf(stderr, "fake headset ended: unknown\n");
	}
}

struct VkDeviceSet
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
};

bool bring_up_vulkan(VkDeviceSet & dev)
{
	static const char * extensions[] = {
	        "VK_KHR_surface",
	        "VK_EXT_metal_surface",
	        "VK_KHR_portability_enumeration",
	};
	VkApplicationInfo appInfo{
	        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	        .pApplicationName = "stream-test",
	        .apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instanceInfo{
	        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
	        .pApplicationInfo = &appInfo,
	        .enabledExtensionCount = 3,
	        .ppEnabledExtensionNames = extensions,
	};
	if (vkCreateInstance(&instanceInfo, nullptr, &dev.instance) != VK_SUCCESS)
		return false;
	uint32_t physCount = 0;
	if (vkEnumeratePhysicalDevices(dev.instance, &physCount, nullptr) != VK_SUCCESS || physCount == 0)
		return false;
	std::vector<VkPhysicalDevice> physDevs(physCount);
	vkEnumeratePhysicalDevices(dev.instance, &physCount, physDevs.data());
	dev.phys = physDevs[0];
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(dev.phys, &familyCount, families.data());
	for (uint32_t i = 0; i < familyCount; ++i)
	{
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
		{
			dev.family = i;
			break;
		}
	}
	static const char * deviceExtensions[] = {"VK_KHR_portability_subset"};
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .queueFamilyIndex = dev.family,
	        .queueCount = 1,
	        .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .queueCreateInfoCount = 1,
	        .pQueueCreateInfos = &queueInfo,
	        .enabledExtensionCount = 1,
	        .ppEnabledExtensionNames = deviceExtensions,
	};
	if (vkCreateDevice(dev.phys, &deviceInfo, nullptr, &dev.device) != VK_SUCCESS)
		return false;
	vkGetDeviceQueue(dev.device, dev.family, 0, &dev.queue);
	return dev.queue != VK_NULL_HANDLE;
}

} // namespace

// ---- Tear detector (headless, no headset) ----
// The snapshot path can copy a mid-render image: the app stages each
// presented image and the worker snapshots it later, but nothing orders
// the worker's copy after the app's render on the shared queue — the
// app thread's submits and the worker's capture submit race, so the copy
// lands before the render (stale, previous generation) or between the
// background and corner-block clears (torn: two generations in one
// capture). This phase reproduces that topology exactly: the main thread
// pipelines renders (two submits per eye per frame, no fence wait —
// legal, the release fence-submit is meant to carry the dependency)
// while a capturer thread claims staged images and runs the REAL
// EyeSnapshot capture (compiled in above, so the fence fix is tracked).
// Every capture must read back identical bytes in the background sample
// and the corner-block sample: both submits paint one generation.
//
// Generation encoding: background clear color R=gen lo, G=gen hi,
// B=0x80 marker; the 32x32 corner block repeats the SAME color in a
// second submit. The block is deliberately not visually distinct: clear
// values written into an sRGB attachment undergo the linear-to-sRGB
// transfer on write, so an inverted corner would decode to different
// bytes than the background even in a perfectly clean capture. Same
// color in both submits keeps the compare warp-immune (same float ->
// same byte, deterministically): any intra-capture byte difference
// between the corner and background samples is two generations in one
// capture. (demo.cpp's hook paints a visibly distinct corner instead and
// inverts the sRGB curve back in its self-check.)

namespace
{

constexpr uint32_t kTearDim = 256;
constexpr uint32_t kTearBlock = 32;
constexpr int kTearFrames = 1200;
constexpr int kTearMinCaptures = 200;

float tear_lo(uint64_t gen)
{
	return (float)(gen & 0xFF) / 255.0f;
}

float tear_hi(uint64_t gen)
{
	return (float)((gen >> 8) & 0xFF) / 255.0f;
}

struct TearEye
{
	XrSwapchain swap = XR_NULL_HANDLE;
	std::vector<VkImage> images;
	std::vector<VkImageView> views; // kept alive while framebuffers exist
	std::vector<VkFramebuffer> framebuffers;
	std::vector<VkCommandBuffer> bg_cmd;     // per image: fullscreen gen clear
	std::vector<VkCommandBuffer> corner_cmd; // per image: 32x32 gen corner block
	std::vector<VkFence> fences;             // per image: main-thread pipelining only
	wivrn_macos::EyeSnapshot snap;
	std::atomic<bool> done{false};
	std::atomic<uint64_t> captured{0};
	std::atomic<uint64_t> torn{0};
	std::atomic<uint64_t> backpressure{0};
};

bool tear_record(VkDevice device, VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, uint32_t dim, bool corner, uint64_t gen)
{
	VkCommandBufferBeginInfo begin{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	        .pNext = nullptr,
	        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	        .pInheritanceInfo = nullptr,
	};
	if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
		return false;
	VkClearValue clear{};
	clear.color.float32[0] = tear_lo(gen);
	clear.color.float32[1] = tear_hi(gen);
	clear.color.float32[2] = 0x80 / 255.0f;
	clear.color.float32[3] = 1.0f;
	VkRect2D area{{0, 0}, {dim, dim}};
	if (corner)
		area.extent = {kTearBlock, kTearBlock};
	VkRenderPassBeginInfo rpBegin{
	        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	        .pNext = nullptr,
	        .renderPass = pass,
	        .framebuffer = fb,
	        .renderArea = area,
	        .clearValueCount = 1,
	        .pClearValues = &clear,
	};
	vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cmd);
	return vkEndCommandBuffer(cmd) == VK_SUCCESS;
}

#define TFAIL(fails, fmt, ...) std::fprintf(stderr, "FAIL tear: " fmt "\n" __VA_OPT__(, ) __VA_ARGS__), ++(fails)

int run_tear()
{
	int fails = 0;
	::setenv("WIVRN_PORT", "19761", 1);
	::setenv("WIVRN_HOSTNAME", "WiVRn-Tear", 1);
	const char * manifest = std::getenv("XR_RUNTIME_JSON");
	if (manifest == nullptr)
	{
		std::puts("set XR_RUNTIME_JSON to the generated wivrn-macos.json");
		return 2;
	}

	const char * enabledExts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo createInfo{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .next = nullptr,
	        .createFlags = 0,
	        .applicationInfo = {.applicationName = "tear-test", .applicationVersion = 1, .engineName = "none", .engineVersion = 0, .apiVersion = XR_CURRENT_API_VERSION},
	        .enabledApiLayerCount = 0,
	        .enabledApiLayerNames = nullptr,
	        .enabledExtensionCount = 2,
	        .enabledExtensionNames = enabledExts,
	};
	if (xrCreateInstance(&createInfo, &instance) != XR_SUCCESS)
	{
		TFAIL(fails, "xrCreateInstance");
		return fails;
	}

	VkDeviceSet dev{};
	if (!bring_up_vulkan(dev))
	{
		TFAIL(fails, "bring_up_vulkan");
		return fails;
	}

	XrSystemGetInfo sysInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .next = nullptr, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	if (xrGetSystem(instance, &sysInfo, &systemId) != XR_SUCCESS)
	{
		TFAIL(fails, "xrGetSystem");
		return fails;
	}

	XrGraphicsBindingVulkan2KHR binding{
	        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
	        .next = nullptr,
	        .instance = dev.instance,
	        .physicalDevice = dev.phys,
	        .device = dev.device,
	        .queueFamilyIndex = dev.family,
	        .queueIndex = 0,
	};
	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo sessionInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .createFlags = 0, .systemId = systemId};
	if (xrCreateSession(instance, &sessionInfo, &session) != XR_SUCCESS)
	{
		TFAIL(fails, "xrCreateSession");
		return fails;
	}
	// No xrBeginSession and no EndFrame: without a headset the streamer
	// idles, and staging is mimicked below through the real handoff.

	uint32_t formatCount = 0;
	if (xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr) != XR_SUCCESS || formatCount == 0)
	{
		TFAIL(fails, "xrEnumerateSwapchainFormats");
		return fails;
	}
	std::fprintf(stderr, "tear: session up\n");
	std::vector<int64_t> formats(formatCount);
	if (xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()) != XR_SUCCESS)
	{
		TFAIL(fails, "xrEnumerateSwapchainFormats fetch");
		return fails;
	}
	int64_t format = formats[0];

	// Clear-only render pass (no shaders): fullscreen background clear
	// plus a 32x32 corner-block clear as two separate submits, so an
	// unordered capture can land between them. UNDEFINED initial layout
	// is legal every iteration (contents discarded by the clear).
	VkAttachmentDescription att{
	        .flags = 0,
	        .format = (VkFormat)format,
	        .samples = VK_SAMPLE_COUNT_1_BIT,
	        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub{
	        .flags = 0,
	        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	        .inputAttachmentCount = 0,
	        .pInputAttachments = nullptr,
	        .colorAttachmentCount = 1,
	        .pColorAttachments = &colorRef,
	        .pResolveAttachments = nullptr,
	        .pDepthStencilAttachment = nullptr,
	        .preserveAttachmentCount = 0,
	        .pPreserveAttachments = nullptr,
	};
	VkRenderPassCreateInfo rpInfo{
	        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .attachmentCount = 1,
	        .pAttachments = &att,
	        .subpassCount = 1,
	        .pSubpasses = &sub,
	        .dependencyCount = 0,
	        .pDependencies = nullptr,
	};
	VkRenderPass pass = VK_NULL_HANDLE;
	if (vkCreateRenderPass(dev.device, &rpInfo, nullptr, &pass) != VK_SUCCESS)
	{
		TFAIL(fails, "vkCreateRenderPass");
		return fails;
	}
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandPoolCreateInfo poolInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	        .queueFamilyIndex = dev.family,
	};
	if (vkCreateCommandPool(dev.device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
	{
		TFAIL(fails, "vkCreateCommandPool");
		return fails;
	}

	TearEye eyes[2];
	for (int eye = 0; eye < 2; ++eye)
	{
		TearEye & e = eyes[eye];
		XrSwapchainCreateInfo swapInfo{
		        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		        .next = nullptr,
		        .createFlags = 0,
		        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		        .format = format,
		        .sampleCount = 1,
		        .width = kTearDim,
		        .height = kTearDim,
		        .faceCount = 1,
		        .arraySize = 1,
		        .mipCount = 1,
		};
		if (xrCreateSwapchain(session, &swapInfo, &e.swap) != XR_SUCCESS)
		{
			TFAIL(fails, "eye %d xrCreateSwapchain", eye);
			return fails;
		}
		uint32_t imageCount = 0;
		if (xrEnumerateSwapchainImages(e.swap, 0, &imageCount, nullptr) != XR_SUCCESS || imageCount != 4)
		{
			TFAIL(fails, "eye %d imageCount != 4", eye);
			return fails;
		}
		std::vector<XrSwapchainImageVulkan2KHR> imgs(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		uint32_t fetched = imageCount;
		if (xrEnumerateSwapchainImages(
		            e.swap, imageCount, &fetched, reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data())) != XR_SUCCESS)
		{
			TFAIL(fails, "eye %d enumerate images", eye);
			return fails;
		}
		for (const auto & im: imgs)
			e.images.push_back(im.image);
		for (VkImage image: e.images)
		{
			(void)image;
			VkImageViewCreateInfo viewInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			        .pNext = nullptr,
			        .flags = 0,
			        .image = image,
			        .viewType = VK_IMAGE_VIEW_TYPE_2D,
			        .format = (VkFormat)format,
			        .components = {},
			        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			VkImageView view = VK_NULL_HANDLE;
			if (vkCreateImageView(dev.device, &viewInfo, nullptr, &view) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d vkCreateImageView", eye);
				return fails;
			}
			e.views.push_back(view);
			VkFramebufferCreateInfo fbInfo{
			        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			        .pNext = nullptr,
			        .flags = 0,
			        .renderPass = pass,
			        .attachmentCount = 1,
			        .pAttachments = &view,
			        .width = kTearDim,
			        .height = kTearDim,
			        .layers = 1,
			};
			VkFramebuffer fb = VK_NULL_HANDLE;
			if (vkCreateFramebuffer(dev.device, &fbInfo, nullptr, &fb) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d vkCreateFramebuffer", eye);
				return fails;
			}
			e.framebuffers.push_back(fb);
			// Per-image command buffers + fence: the fence waits out the
			// main thread's own previous render of the same image (safe
			// reuse) without serializing against the capturer.
			VkCommandBufferAllocateInfo cmdInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			        .pNext = nullptr,
			        .commandPool = pool,
			        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			        .commandBufferCount = 1,
			};
			VkCommandBuffer bg = VK_NULL_HANDLE, corner = VK_NULL_HANDLE;
			if (vkAllocateCommandBuffers(dev.device, &cmdInfo, &bg) != VK_SUCCESS ||
			    vkAllocateCommandBuffers(dev.device, &cmdInfo, &corner) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d vkAllocateCommandBuffers", eye);
				return fails;
			}
			e.bg_cmd.push_back(bg);
			e.corner_cmd.push_back(corner);
			VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
			VkFence fence = VK_NULL_HANDLE;
			if (vkCreateFence(dev.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d vkCreateFence", eye);
				return fails;
			}
			e.fences.push_back(fence);
		}
		if (!e.snap.init(dev.phys, dev.device, dev.queue, dev.family, kTearDim, kTearDim))
		{
			TFAIL(fails, "eye %d snapshot init", eye);
			return fails;
		}
		std::fprintf(stderr, "tear: eye %d ready (%zu images)\n", eye, e.images.size());
	}

	// Move every image out of UNDEFINED up front: a capture racing a
	// render must observe ATTACHMENT_OPTIMAL with stale-or-new pixels
	// (the torn/stale race under test), never a layout mismatch — the
	// capture barriers assume ATTACHMENT_OPTIMAL and a wrong-oldLayout
	// copy faults instead of tearing, masking the pixel evidence.
	{
		VkCommandBufferAllocateInfo layoutAlloc{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		        .pNext = nullptr,
		        .commandPool = pool,
		        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		        .commandBufferCount = 1,
		};
		VkCommandBuffer layoutCmd = VK_NULL_HANDLE;
		VkFence layoutFence = VK_NULL_HANDLE;
		VkFenceCreateInfo layoutFenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
		bool layoutOk = vkAllocateCommandBuffers(dev.device, &layoutAlloc, &layoutCmd) == VK_SUCCESS &&
		                vkCreateFence(dev.device, &layoutFenceInfo, nullptr, &layoutFence) == VK_SUCCESS;
		for (int eye = 0; eye < 2 && layoutOk; ++eye)
		{
			for (VkImage image: eyes[eye].images)
			{
				VkCommandBufferBeginInfo layoutBegin{
				        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				        .pNext = nullptr,
				        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				        .pInheritanceInfo = nullptr,
				};
				layoutOk = vkBeginCommandBuffer(layoutCmd, &layoutBegin) == VK_SUCCESS;
				if (!layoutOk)
					break;
				VkImageMemoryBarrier toAttach{
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				        .pNext = nullptr,
				        .srcAccessMask = 0,
				        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image = image,
				        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};
				vkCmdPipelineBarrier(
				        layoutCmd,
				        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				        0,
				        0,
				        nullptr,
				        0,
				        nullptr,
				        1,
				        &toAttach);
				layoutOk = vkEndCommandBuffer(layoutCmd) == VK_SUCCESS;
				if (!layoutOk)
					break;
				VkSubmitInfo layoutSubmit{
				        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				        .pNext = nullptr,
				        .waitSemaphoreCount = 0,
				        .pWaitSemaphores = nullptr,
				        .pWaitDstStageMask = nullptr,
				        .commandBufferCount = 1,
				        .pCommandBuffers = &layoutCmd,
				        .signalSemaphoreCount = 0,
				        .pSignalSemaphores = nullptr,
				};
				vkResetFences(dev.device, 1, &layoutFence);
				layoutOk = vkQueueSubmit(dev.queue, 1, &layoutSubmit, layoutFence) == VK_SUCCESS &&
				           vkWaitForFences(dev.device, 1, &layoutFence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
				if (!layoutOk)
					break;
				vkResetCommandBuffer(layoutCmd, 0);
			}
		}
		if (layoutCmd != VK_NULL_HANDLE)
			vkFreeCommandBuffers(dev.device, pool, 1, &layoutCmd);
		if (layoutFence != VK_NULL_HANDLE)
			vkDestroyFence(dev.device, layoutFence, nullptr);
		if (!layoutOk)
		{
			TFAIL(fails, "initial layout transition");
			return fails;
		}
		std::fprintf(stderr, "tear: images in ATTACHMENT_OPTIMAL\n");
	}

	// Capturer threads: claim staged images exactly like the streamer
	// worker take (staged claim, active pin across the copy) and verify
	// every capture carries one generation.
	std::thread helpers[2];
	for (int eye = 0; eye < 2; ++eye)
	{
		helpers[eye] = std::thread([&, eye] {
			TearEye & e = eyes[eye];
			XrSwapchain_T * handle = e.swap;
			std::fprintf(stderr, "tear: capturer %d started\n", eye);
			for (;;)
			{
				int32_t claimed = handle->data.staged_idx.exchange(-1, std::memory_order_acq_rel);
				if (claimed < 0)
				{
					if (e.done.load(std::memory_order_acquire))
						break;
					std::this_thread::sleep_for(std::chrono::microseconds(100));
					continue;
				}
				if (claimed >= (int32_t)e.images.size())
					continue;
				handle->data.active_idx.store(claimed, std::memory_order_release);
				const uint8_t * px = e.snap.capture(e.images[(uint32_t)claimed]);
				handle->data.active_idx.store(-1, std::memory_order_release);
				if (px == nullptr)
				{
					e.torn.fetch_add(1);
					continue;
				}
				const uint8_t * corner = px + ((size_t)kTearBlock / 2 * kTearDim + kTearBlock / 2) * 4;
				const uint8_t * bg = px + ((size_t)(kTearDim - 8) * kTearDim + (kTearDim - 8)) * 4;
				// Warp-immune compare: same painted float must read back
				// as the same bytes in both samples (BGRA order).
				bool same = corner[0] == bg[0] && corner[1] == bg[1] && corner[2] == bg[2];
				e.captured.fetch_add(1);
				if (!same)
				{
					uint64_t torn = e.torn.fetch_add(1) + 1;
					if (torn <= 5)
						std::fprintf(
						        stderr,
						        "tear: eye %d capture %llu corner(%u,%u,%u) != bg(%u,%u,%u)\n",
						        eye,
						        (unsigned long long)e.captured.load(),
						        corner[2],
						        corner[1],
						        corner[0],
						        bg[2],
						        bg[1],
						        bg[0]);
				}
			}
		});
	}

	// Producer: acquire, pipeline two submits per eye (no fence wait —
	// the release fence-submit carries the dependency, exactly like a
	// display-driven app), release, stage latest-only.
	XrSwapchainImageAcquireInfo acquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
	XrSwapchainImageWaitInfo waitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .next = nullptr, .timeout = XR_INFINITE_DURATION};
	XrSwapchainImageReleaseInfo releaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
	for (uint64_t gen = 1; gen <= (uint64_t)kTearFrames; ++gen)
	{
		if (gen % 50 == 1)
			std::fprintf(stderr, "tear: producing gen %llu\n", (unsigned long long)gen);
		for (int eye = 0; eye < 2; ++eye)
		{
			TearEye & e = eyes[eye];
			uint32_t index = 0;
			bool got = false;
			for (int attempt = 0; attempt < 200 && !got; ++attempt)
			{
				if (xrAcquireSwapchainImage(e.swap, &acquireInfo, &index) == XR_SUCCESS)
					got = true;
				else
				{
					e.backpressure.fetch_add(1);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			if (!got)
				continue; // latest-only drop, like the streaming mailbox
			if (xrWaitSwapchainImage(e.swap, &waitInfo) != XR_SUCCESS)
			{
				TFAIL(fails, "eye %d xrWaitSwapchainImage", eye);
				break;
			}
			// Wait out our own previous render of this image (command
			// buffer + content reuse), then re-record both submits.
			if (vkWaitForFences(dev.device, 1, &e.fences[index], VK_TRUE, UINT64_MAX) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d app fence wait", eye);
				break;
			}
			vkResetCommandBuffer(e.bg_cmd[index], 0);
			vkResetCommandBuffer(e.corner_cmd[index], 0);
			if (!tear_record(dev.device, e.bg_cmd[index], pass, e.framebuffers[index], kTearDim, false, gen) ||
			    !tear_record(dev.device, e.corner_cmd[index], pass, e.framebuffers[index], kTearDim, true, gen))
			{
				TFAIL(fails, "eye %d record gen %llu", eye, (unsigned long long)gen);
				break;
			}
			VkSubmitInfo bgSubmit{
			        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			        .pNext = nullptr,
			        .waitSemaphoreCount = 0,
			        .pWaitSemaphores = nullptr,
			        .pWaitDstStageMask = nullptr,
			        .commandBufferCount = 1,
			        .pCommandBuffers = &e.bg_cmd[index],
			        .signalSemaphoreCount = 0,
			        .pSignalSemaphores = nullptr,
			};
			VkSubmitInfo cornerSubmit = bgSubmit;
			cornerSubmit.pCommandBuffers = &e.corner_cmd[index];
			vkResetFences(dev.device, 1, &e.fences[index]);
			if (vkQueueSubmit(dev.queue, 1, &bgSubmit, VK_NULL_HANDLE) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d bg submit", eye);
				break;
			}
			// Widen the race window: pause between the two submits so an
			// unordered capturer submit can land between them (the
			// misordered copy then carries both generations). An ordered
			// snapshot (post-fix) still reads one generation.
			std::this_thread::sleep_for(std::chrono::microseconds(250));
			if (vkQueueSubmit(dev.queue, 1, &cornerSubmit, e.fences[index]) != VK_SUCCESS)
			{
				TFAIL(fails, "eye %d corner submit", eye);
				break;
			}
			if (xrReleaseSwapchainImage(e.swap, &releaseInfo) != XR_SUCCESS)
			{
				TFAIL(fails, "eye %d xrReleaseSwapchainImage", eye);
				break;
			}
			// Stage latest-only for the capturer (mirrors end_frame).
			XrSwapchain_T * handle = e.swap;
			handle->data.staged_idx.store((int32_t)index, std::memory_order_release);
		}
		// No pacing: burst staging keeps the capturer racing the
		// producer's current renders (the overload regime where the
		// race bites).
	}

	for (int eye = 0; eye < 2; ++eye)
	{
		eyes[eye].done.store(true, std::memory_order_release);
		helpers[eye].join();
	}
	for (int eye = 0; eye < 2; ++eye)
	{
		TearEye & e = eyes[eye];
		std::printf(
		        "tear: eye %d captured %llu torn %llu backpressure %llu\n",
		        eye,
		        (unsigned long long)e.captured.load(),
		        (unsigned long long)e.torn.load(),
		        (unsigned long long)e.backpressure.load());
		if (e.captured.load() < (uint64_t)kTearMinCaptures)
		{
			TFAIL(fails, "eye %d only %llu captures (need %d)", eye, (unsigned long long)e.captured.load(), kTearMinCaptures);
		}
		if (e.torn.load() != 0)
		{
			TFAIL(
			        fails,
			        "eye %d tore %llu/%llu captures (snapshot copied a mid-render image)",
			        eye,
			        (unsigned long long)e.torn.load(),
			        (unsigned long long)e.captured.load());
		}
	}

	for (int eye = 0; eye < 2; ++eye)
	{
		for (VkFramebuffer fb: eyes[eye].framebuffers)
			vkDestroyFramebuffer(dev.device, fb, nullptr);
		for (VkImageView view: eyes[eye].views)
			vkDestroyImageView(dev.device, view, nullptr);
		for (VkFence fence: eyes[eye].fences)
			vkDestroyFence(dev.device, fence, nullptr);
		eyes[eye].snap.shutdown();
		xrDestroySwapchain(eyes[eye].swap);
	}
	xrDestroySession(session);
	xrDestroyInstance(instance);
	vkDestroyCommandPool(dev.device, pool, nullptr);
	vkDestroyRenderPass(dev.device, pass, nullptr);
	vkDestroyDevice(dev.device, nullptr);
	vkDestroyInstance(dev.instance, nullptr);

	if (fails == 0)
		std::puts("tear test PASS");
	else
		std::printf("tear test FAIL (%d)\n", fails);
	return fails;
}

// ---- Eye-skew soak (fake headset + paced app loop) ----
// Overload burst (unthrottled staging, latest-only drops), then paced
// recovery, then match frames across eyes by display_time and measure
// the eye1-minus-eye0 arrival lag. A fixed-order sequential worker
// completes eye1 one encode after eye0 every pass, so the mean signed
// lag sits at ~+one encode; fair service (parallel or alternating eyes)
// re-converges it to ~0. WIVRN_MACOS_SKEW_BOUND_MS overrides the bound
// (default 2.0); WIVRN_MACOS_VIDEO_ENCODER=x264 is forced here so the
// encode cost (and the lag it induces) is deterministic.

#undef TFAIL
#define TFAIL(fails, fmt, ...) std::fprintf(stderr, "FAIL skew: " fmt "\n" __VA_OPT__(, ) __VA_ARGS__), ++(fails)

int run_skew()
{
	int fails = 0;
	::setenv("WIVRN_PORT", "19762", 1);
	::setenv("WIVRN_HOSTNAME", "WiVRn-Skew", 1);
	::setenv("WIVRN_MACOS_VIDEO_ENCODER", "x264", 1);
	double bound_ms = 2.0;
	if (const char * bound = std::getenv("WIVRN_MACOS_SKEW_BOUND_MS"))
	{
		double parsed = std::atof(bound);
		if (parsed > 0.0 && parsed < 1000.0)
			bound_ms = parsed;
	}
	const char * manifest = std::getenv("XR_RUNTIME_JSON");
	if (manifest == nullptr)
	{
		std::puts("set XR_RUNTIME_JSON to the generated wivrn-macos.json");
		return 2;
	}

	FakeState fake;
	fake.port = 19762;
	std::thread headset([&] { fake_headset(fake); });

	const char * enabledExts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo createInfo{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .next = nullptr,
	        .createFlags = 0,
	        .applicationInfo = {.applicationName = "skew-test", .applicationVersion = 1, .engineName = "none", .engineVersion = 0, .apiVersion = XR_CURRENT_API_VERSION},
	        .enabledApiLayerCount = 0,
	        .enabledApiLayerNames = nullptr,
	        .enabledExtensionCount = 2,
	        .enabledExtensionNames = enabledExts,
	};
	if (xrCreateInstance(&createInfo, &instance) != XR_SUCCESS)
	{
		TFAIL(fails, "xrCreateInstance");
		fake.stop = true;
		headset.join();
		return fails;
	}

	VkDeviceSet dev{};
	if (!bring_up_vulkan(dev))
	{
		TFAIL(fails, "bring_up_vulkan");
		fake.stop = true;
		headset.join();
		return fails;
	}

	XrSystemGetInfo sysInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .next = nullptr, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	if (xrGetSystem(instance, &sysInfo, &systemId) != XR_SUCCESS)
	{
		TFAIL(fails, "xrGetSystem");
		fake.stop = true;
		headset.join();
		return fails;
	}

	XrGraphicsBindingVulkan2KHR binding{
	        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
	        .next = nullptr,
	        .instance = dev.instance,
	        .physicalDevice = dev.phys,
	        .device = dev.device,
	        .queueFamilyIndex = dev.family,
	        .queueIndex = 0,
	};
	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo sessionInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .createFlags = 0, .systemId = systemId};
	if (xrCreateSession(instance, &sessionInfo, &session) != XR_SUCCESS)
	{
		TFAIL(fails, "xrCreateSession");
		fake.stop = true;
		headset.join();
		return fails;
	}

	XrViewConfigurationType config = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	XrSessionBeginInfo beginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO, .next = nullptr, .primaryViewConfigurationType = config};
	if (xrBeginSession(session, &beginInfo) != XR_SUCCESS)
	{
		TFAIL(fails, "xrBeginSession");
		fake.stop = true;
		headset.join();
		return fails;
	}

	std::this_thread::sleep_for(std::chrono::seconds(3));

	uint32_t formatCount = 0;
	if (xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr) != XR_SUCCESS || formatCount == 0)
	{
		TFAIL(fails, "xrEnumerateSwapchainFormats");
		fake.stop = true;
		headset.join();
		return fails;
	}
	std::vector<int64_t> formats(formatCount);
	if (xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()) != XR_SUCCESS)
	{
		TFAIL(fails, "xrEnumerateSwapchainFormats fetch");
		fake.stop = true;
		headset.join();
		return fails;
	}
	int64_t format = formats[0];

	XrSwapchain swapchains[2] = {};
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainCreateInfo swapInfo{
		        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		        .next = nullptr,
		        .createFlags = 0,
		        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		        .format = format,
		        .sampleCount = 1,
		        .width = 1024,
		        .height = 1024,
		        .faceCount = 1,
		        .arraySize = 1,
		        .mipCount = 1,
		};
		if (xrCreateSwapchain(session, &swapInfo, &swapchains[eye]) != XR_SUCCESS)
		{
			TFAIL(fails, "eye %d xrCreateSwapchain", eye);
			fake.stop = true;
			headset.join();
			return fails;
		}
	}

	XrReferenceSpaceCreateInfo spaceInfo{
	        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	        .next = nullptr,
	        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	XrSpace local = XR_NULL_HANDLE;
	if (xrCreateReferenceSpace(session, &spaceInfo, &local) != XR_SUCCESS)
	{
		TFAIL(fails, "xrCreateReferenceSpace");
		fake.stop = true;
		headset.join();
		return fails;
	}

	// No rendering: virgin images keep the burst cheap; the metric is
	// encode-completion timing, not pixels.
	XrSwapchainImageAcquireInfo acquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
	XrSwapchainImageWaitInfo waitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .next = nullptr, .timeout = XR_INFINITE_DURATION};
	XrSwapchainImageReleaseInfo releaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
	XrFrameWaitInfo frameWaitInfo{.type = XR_TYPE_FRAME_WAIT_INFO, .next = nullptr};
	XrFrameBeginInfo frameBeginInfo{.type = XR_TYPE_FRAME_BEGIN_INFO, .next = nullptr};
	auto pump = [&](int frames, int pace_ms) {
		for (int frame = 0; frame < frames; ++frame)
		{
			XrFrameState frameState{.type = XR_TYPE_FRAME_STATE, .next = nullptr};
			if (xrWaitFrame(session, &frameWaitInfo, &frameState) != XR_SUCCESS)
			{
				TFAIL(fails, "xrWaitFrame");
				return;
			}
			for (int eye = 0; eye < 2; ++eye)
			{
				uint32_t index = 0;
				if (xrAcquireSwapchainImage(swapchains[eye], &acquireInfo, &index) != XR_SUCCESS)
				{
					// Quad-buffered pool is sized for single-flight apps;
					// an all-held pool under burst is backpressure, not a
					// bug: skip this eye this frame (latest-only).
					continue;
				}
				if (xrWaitSwapchainImage(swapchains[eye], &waitInfo) != XR_SUCCESS)
				{
					TFAIL(fails, "xrWaitSwapchainImage");
					return;
				}
			}
			if (xrBeginFrame(session, &frameBeginInfo) != XR_SUCCESS)
			{
				TFAIL(fails, "xrBeginFrame");
				return;
			}
			XrCompositionLayerProjectionView projViews[2] = {};
			XrCompositionLayerProjection proj{
			        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
			        .next = nullptr,
			        .layerFlags = 0,
			        .space = local,
			        .viewCount = 2,
			        .views = projViews,
			};
			for (int eye = 0; eye < 2; ++eye)
			{
				projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				projViews[eye].pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}};
				projViews[eye].fov = {-0.86f, 0.86f, 0.86f, -0.86f};
				projViews[eye].subImage = {.swapchain = swapchains[eye], .imageRect = {.offset = {0, 0}, .extent = {1024, 1024}}, .imageArrayIndex = 0};
			}
			const XrCompositionLayerBaseHeader * layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj)};
			XrFrameEndInfo endInfo{
			        .type = XR_TYPE_FRAME_END_INFO,
			        .next = nullptr,
			        .displayTime = frameState.predictedDisplayTime,
			        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
			        .layerCount = 1,
			        .layers = layers,
			};
			if (xrEndFrame(session, &endInfo) != XR_SUCCESS)
			{
				TFAIL(fails, "xrEndFrame");
				return;
			}
			for (int eye = 0; eye < 2; ++eye)
				xrReleaseSwapchainImage(swapchains[eye], &releaseInfo);
			if (pace_ms > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
		}
	};
	pump(250, 0);  // overload burst: unthrottled staging, mailbox drops
	pump(150, 25); // recovery: paced like a display-driven app

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	fake.stop = true;
	headset.join();

	// Match frames across eyes by display_time (both eyes share the
	// EndFrame's predicted time) and measure eye1-minus-eye0 arrivals.
	std::map<XrTime, long long> arrive[2];
	{
		std::lock_guard lock(fake.mutex);
		for (int eye = 0; eye < 2; ++eye)
		{
			for (const auto & maps = {&fake.stream_done, &fake.control_done}; auto * done: maps)
			{
				for (const auto & [key, frame]: *done)
				{
					if (key.first != (uint8_t)eye || !frame.has_view_info || frame.display_time == 0)
						continue;
					if (!arrive[eye].contains(frame.display_time))
						arrive[eye][frame.display_time] = frame.arrival_ms;
				}
			}
		}
	}
	std::vector<double> lags;
	for (const auto & [display, ms0]: arrive[0])
	{
		auto it = arrive[1].find(display);
		if (it != arrive[1].end())
			lags.push_back((double)it->second - (double)ms0);
	}
	size_t pairs = lags.size();
	double mean = 0, p50 = 0, maxabs = 0;
	if (!lags.empty())
	{
		for (double lag: lags)
		{
			mean += lag;
			maxabs = std::max(maxabs, std::fabs(lag));
		}
		mean /= (double)lags.size();
		std::nth_element(lags.begin(), lags.begin() + (long long)(lags.size() / 2), lags.end());
		p50 = lags[lags.size() / 2];
	}
	size_t count0 = arrive[0].size(), count1 = arrive[1].size();
	std::printf(
	        "skew: pairs=%zu mean=%+.2fms p50=%+.2fms maxabs=%.2fms counts=%zu/%zu (bound %.1fms)\n",
	        pairs,
	        mean,
	        p50,
	        maxabs,
	        count0,
	        count1,
	        bound_ms);
	if (pairs < 30)
	{
		TFAIL(fails, "only %zu matched pairs (need 30)", pairs);
	}
	else
	{
		if (std::fabs(mean) > bound_ms)
		{
			TFAIL(
			        fails,
			        "mean inter-eye lag %+.2fms exceeds %.1fms (eyes do not re-converge after burst)",
			        mean,
			        bound_ms);
		}
		if (std::fabs(p50) > bound_ms)
		{
			TFAIL(fails, "p50 inter-eye lag %+.2fms exceeds %.1fms", p50, bound_ms);
		}
	}
	// Loose completion guard: both eyes must keep flowing (catches a
	// stalled eye, not scheduling phase).
	size_t total = count0 + count1;
	if (total > 0 && (count0 + count1 > 0) &&
	    (count0 > count1 ? count0 - count1 : count1 - count0) > 10 + total / 20)
	{
		TFAIL(fails, "eye completion counts diverged: %zu vs %zu", count0, count1);
	}

	::unsetenv("WIVRN_MACOS_VIDEO_ENCODER");
	xrDestroySpace(local);
	for (int eye = 0; eye < 2; ++eye)
		xrDestroySwapchain(swapchains[eye]);
	xrEndSession(session);
	xrDestroySession(session);
	xrDestroyInstance(instance);
	vkDestroyDevice(dev.device, nullptr);
	vkDestroyInstance(dev.instance, nullptr);

	if (fails == 0)
		std::puts("skew test PASS");
	else
		std::printf("skew test FAIL (%d)\n", fails);
	return fails;
}

} // namespace

int run_loopback()
{
	::setenv("WIVRN_PORT", "19758", 1);
	::setenv("WIVRN_HOSTNAME", "WiVRn-Test", 1);
	const char * manifest = std::getenv("XR_RUNTIME_JSON");
	if (manifest == nullptr)
	{
		std::puts("set XR_RUNTIME_JSON to the generated wivrn-macos.json");
		return 2;
	}

	FakeState fake;
	std::thread headset([&] { fake_headset(fake); });

	// ---- App side through the real loader ----
	const char * enabledExts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo createInfo{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .next = nullptr,
	        .createFlags = 0,
	        .applicationInfo = {.applicationName = "stream-test", .applicationVersion = 1, .engineName = "none", .engineVersion = 0, .apiVersion = XR_CURRENT_API_VERSION},
	        .enabledApiLayerCount = 0,
	        .enabledApiLayerNames = nullptr,
	        .enabledExtensionCount = 2,
	        .enabledExtensionNames = enabledExts,
	};
	CHECK_XR(xrCreateInstance(&createInfo, &instance), XR_SUCCESS);

	VkDeviceSet dev{};
	CHECK(bring_up_vulkan(dev));

	XrSystemGetInfo sysInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .next = nullptr, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	CHECK_XR(xrGetSystem(instance, &sysInfo, &systemId), XR_SUCCESS);

	XrGraphicsBindingVulkan2KHR binding{
	        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
	        .next = nullptr,
	        .instance = dev.instance,
	        .physicalDevice = dev.phys,
	        .device = dev.device,
	        .queueFamilyIndex = dev.family,
	        .queueIndex = 0,
	};
	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo sessionInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .createFlags = 0, .systemId = systemId};
	CHECK_XR(xrCreateSession(instance, &sessionInfo, &session), XR_SUCCESS);

	XrViewConfigurationType config = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	XrSessionBeginInfo beginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO, .next = nullptr, .primaryViewConfigurationType = config};
	CHECK_XR(xrBeginSession(session, &beginInfo), XR_SUCCESS);

	// Let the fake headset finish its handshake + start tracking: app
	// frames before connect encode nothing (headless skip), so the proof
	// needs the link up first. Loopback handshake completes in ~1 s.
	std::this_thread::sleep_for(std::chrono::seconds(3));

	uint32_t formatCount = 0;
	CHECK_XR(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), XR_SUCCESS);
	std::vector<int64_t> formats(formatCount);
	CHECK_XR(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), XR_SUCCESS);
	int64_t format = formats[0];

	XrSwapchain swapchains[2] = {};
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainCreateInfo swapInfo{
		        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		        .next = nullptr,
		        .createFlags = 0,
		        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		        .format = format,
		        .sampleCount = 1,
		        .width = 256,
		        .height = 256,
		        .faceCount = 1,
		        .arraySize = 1,
		        .mipCount = 1,
		};
		CHECK_XR(xrCreateSwapchain(session, &swapInfo, &swapchains[eye]), XR_SUCCESS);
		uint32_t imageCount = 0;
		CHECK_XR(xrEnumerateSwapchainImages(swapchains[eye], 0, &imageCount, nullptr), XR_SUCCESS);
		CHECK(imageCount == 4); // quad-buffered: app + staged + active + free
	}

	XrReferenceSpaceCreateInfo spaceInfo{
	        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	        .next = nullptr,
	        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	XrSpace local = XR_NULL_HANDLE;
	CHECK_XR(xrCreateReferenceSpace(session, &spaceInfo, &local), XR_SUCCESS);

	// No rendering: virgin UNDEFINED images exercise the snapshot path.
	XrSwapchainImageAcquireInfo acquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
	XrSwapchainImageWaitInfo waitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .next = nullptr, .timeout = XR_INFINITE_DURATION};
	XrSwapchainImageReleaseInfo releaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
	XrFrameWaitInfo frameWaitInfo{.type = XR_TYPE_FRAME_WAIT_INFO, .next = nullptr};
	XrFrameBeginInfo frameBeginInfo{.type = XR_TYPE_FRAME_BEGIN_INFO, .next = nullptr};
	// Closed-loop framing: the streamer mailbox is latest-only, so a fixed
	// burst is overwritten faster than the worker drains it (the two first
	// x264 opens alone take ~100ms+). Frame until the fake reassembles
	// enough; the cap still fails loudly on a real stall.
	for (int frame = 0; frame < 400; ++frame)
	{
		{
			std::lock_guard lock(fake.mutex);
			if (fake.stream_done.size() >= 4)
				break;
		}
		XrFrameState frameState{.type = XR_TYPE_FRAME_STATE, .next = nullptr};
		CHECK_XR(xrWaitFrame(session, &frameWaitInfo, &frameState), XR_SUCCESS);
		for (int eye = 0; eye < 2; ++eye)
		{
			uint32_t index = 0;
			CHECK_XR(xrAcquireSwapchainImage(swapchains[eye], &acquireInfo, &index), XR_SUCCESS);
			CHECK_XR(xrWaitSwapchainImage(swapchains[eye], &waitInfo), XR_SUCCESS);
		}
		CHECK_XR(xrBeginFrame(session, &frameBeginInfo), XR_SUCCESS);
		XrCompositionLayerProjectionView projViews[2] = {};
		XrCompositionLayerProjection proj{
		        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		        .next = nullptr,
		        .layerFlags = 0,
		        .space = local,
		        .viewCount = 2,
		        .views = projViews,
		};
		for (int eye = 0; eye < 2; ++eye)
		{
			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}};
			projViews[eye].fov = {-0.86f, 0.86f, 0.86f, -0.86f}; // OpenXR signs: left/down negative
			projViews[eye].subImage = {.swapchain = swapchains[eye], .imageRect = {.offset = {0, 0}, .extent = {256, 256}}, .imageArrayIndex = 0};
		}
		const XrCompositionLayerBaseHeader * layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj)};
		XrFrameEndInfo endInfo{
		        .type = XR_TYPE_FRAME_END_INFO,
		        .next = nullptr,
		        .displayTime = frameState.predictedDisplayTime,
		        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		        .layerCount = 1,
		        .layers = layers,
		};
		CHECK_XR(xrEndFrame(session, &endInfo), XR_SUCCESS);
		for (int eye = 0; eye < 2; ++eye)
			CHECK_XR(xrReleaseSwapchainImage(swapchains[eye], &releaseInfo), XR_SUCCESS);
		// Paced like a display-driven app (~40 Hz): an unthrottled loop
		// finishes all 400 frames in milliseconds (our WaitFrame never
		// blocks), outrunning the worker's cold start (snapshot init +
		// encoder open ≈ 100-300 ms) so only the first burst's tail ever
		// encodes. Pacing lets the worker drain steadily like production.
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	// ---- Assertions on the loop ----
	// (The fake keeps running throughout: it must stay alive to receive
	// shards and send tracking/NACKs. fake.stop is set only after the
	// assertions below, just before teardown.)
	{
		// Let stragglers land.
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		// NACK should have produced a reliable IDR on control by now.
		bool streamFrames = false, idrControl = false, viewInfoOk = false, fovOk = true;
		for (int i = 0; i < 100; ++i)
		{
			{
				std::lock_guard lock(fake.mutex);
				streamFrames = fake.stream_done.size() >= 4; // 2+ frames per eye
				for (const auto & [key, frame]: fake.stream_done)
				{
					(void)key;
					if (frame.has_view_info && frame.display_time > 0 && has_annexb_start(frame.bytes))
						viewInfoOk = true;
					// Foveation must decode to the stream dims (256x256
					// here); zeros crashed the real client (run 1).
					if (frame.has_view_info && (frame.fov_w != 256 || frame.fov_h != 256))
						fovOk = false;
				}
				for (const auto & [key, frame]: fake.control_done)
				{
					(void)key;
					if (has_sps(frame.bytes))
						idrControl = true;
				}
			}
			if (streamFrames && idrControl && viewInfoOk && fovOk)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		{
			std::lock_guard lock(fake.mutex);
			std::printf(
			        "stream frames: %zu, control frames: %zu, nack sent: %d\n",
			        fake.stream_done.size(),
			        fake.control_done.size(),
			        (int)fake.nack_sent);
		}
		CHECK(streamFrames);
		CHECK(viewInfoOk);
		CHECK(fovOk);
		CHECK(idrControl);
	}

	// Poses flow back: HEAD (4,5,6) in stage -> LOCAL latches under the
	// HMD at connect (floor (4,0,6) + yaw 0), so VIEW-in-LOCAL ~= (0,5,0).
	// (A fixed (0,1.6,0) LOCAL would read (4,3.4,6) here — and strand all
	// located content by the room displacement on a real headset.)
	{
		XrSpaceLocation loc{.type = XR_TYPE_SPACE_LOCATION, .next = nullptr};
		// Locate the VIEW space (live HMD) in LOCAL.
		XrReferenceSpaceCreateInfo viewInfo{
		        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
		        .next = nullptr,
		        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
		        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
		};
		XrSpace view = XR_NULL_HANDLE;
		CHECK_XR(xrCreateReferenceSpace(session, &viewInfo, &view), XR_SUCCESS);
		CHECK_XR(xrLocateSpace(view, local, 1, &loc), XR_SUCCESS);
		std::printf("view in local: (%.2f, %.2f, %.2f)\n", loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
		CHECK(loc.pose.position.x > -0.5f && loc.pose.position.x < 0.5f);
		CHECK(loc.pose.position.y > 4.5f && loc.pose.position.y < 5.5f);
		CHECK(loc.pose.position.z > -0.5f && loc.pose.position.z < 0.5f);
		CHECK_XR(xrDestroySpace(view), XR_SUCCESS);
	}

	CHECK_XR(xrDestroySpace(local), XR_SUCCESS);
	for (int eye = 0; eye < 2; ++eye)
		CHECK_XR(xrDestroySwapchain(swapchains[eye]), XR_SUCCESS);
	CHECK_XR(xrEndSession(session), XR_SUCCESS);
	CHECK_XR(xrDestroySession(session), XR_SUCCESS);
	CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);
	fake.stop = true;
	headset.join();
	vkDestroyDevice(dev.device, nullptr);
	vkDestroyInstance(dev.instance, nullptr);

	if (failures == 0)
		std::puts("stream test PASS");
	else
		std::printf("stream test FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}

int main()
{
	std::string phases = "loop";
	if (const char * env = std::getenv("WIVRN_MACOS_STREAM_PHASES"))
		phases = env;
	bool all = phases == "all";
	auto want = [&](const char * name) { return all || phases.find(name) != std::string::npos; };
	int rc = 0;
	if (want("tear"))
		rc += run_tear();
	if (want("skew"))
		rc += run_skew();
	if (want("loop"))
		rc += run_loopback();
	if (want("tear") || want("skew"))
		std::printf("harness phases done: rc=%d\n", rc);
	return rc == 0 ? 0 : 1;
}
