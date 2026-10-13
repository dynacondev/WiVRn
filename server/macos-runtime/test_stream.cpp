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

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "wivrn_packets.h"
#include "wivrn_sockets.h"

#include "protocol_version.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

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
				control.emplace(loopback4(kPort));
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
		stream.connect(loopback6(kPort));
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
					std::fprintf(stderr, "fake: tracking send failed: %s\n", e.what());
					return;
				}
				++n;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
		};
		std::thread tracker(tracking_loop);

		// Main fake loop: shards in, timesync answers + one NACK out.
		bool nacked = false;
		int stream_frames_seen = 0;
		while (!state.stop)
		{
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
						packet = control->receive();
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
		}
		tracker.join();
	}
	catch (const std::exception & e)
	{
		std::fprintf(stderr, "fake headset ended: %s\n", e.what());
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

int main()
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
		CHECK(imageCount == 3);
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
	for (int frame = 0; frame < 12; ++frame)
	{
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
			projViews[eye].fov = {0.86f, 0.86f, 0.86f, 0.86f};
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
	}

	// ---- Assertions on the loop ----
	{
		// Let stragglers land, then stop the fake.
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		fake.stop = true;
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

	// Poses flow back: HEAD (4,5,6) in stage -> LOCAL (origin y=1.6).
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
		CHECK(loc.pose.position.x > 3.5f && loc.pose.position.x < 4.5f);
		CHECK(loc.pose.position.y > 3.0f && loc.pose.position.y < 4.0f);
		CHECK_XR(xrDestroySpace(view), XR_SUCCESS);
	}

	CHECK_XR(xrDestroySpace(local), XR_SUCCESS);
	for (int eye = 0; eye < 2; ++eye)
		CHECK_XR(xrDestroySwapchain(swapchains[eye]), XR_SUCCESS);
	CHECK_XR(xrEndSession(session), XR_SUCCESS);
	CHECK_XR(xrDestroySession(session), XR_SUCCESS);
	CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);
	headset.join();
	vkDestroyDevice(dev.device, nullptr);
	vkDestroyInstance(dev.instance, nullptr);

	if (failures == 0)
		std::puts("stream test PASS");
	else
		std::printf("stream test FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
