// Phase 5a proof: clear a BGRA image on MoltenVK -> EyeSnapshot ->
// x264 BGRA -> Annex-B NALs -> ≤1400 B shards. No window, no loader,
// no headset: pure pixels-to-bits. Run directly:
//   ./wivrn-macos-encode-test

#include "encode.h"
#include "snapshot.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

#define CHECK_VK(call)                                                                                \
	do                                                                                            \
	{                                                                                             \
		VkResult res__ = (call);                                                              \
		if (res__ != VK_SUCCESS)                                                              \
		{                                                                                     \
			std::printf("FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, #call, (int)res__); \
			++failures;                                                                   \
		}                                                                                     \
	} while (false)

namespace
{

uint32_t env_dim(const char * name, uint32_t fallback)
{
	if (const char * value = std::getenv(name))
	{
		unsigned long parsed = std::strtoul(value, nullptr, 10);
		if (parsed >= 16 && parsed <= 8192 && (parsed & 1) == 0)
			return (uint32_t)parsed;
	}
	return fallback;
}

// Set WIVRN_ENCODE_TEST_DIMS=WxH (e.g. 2064x2208) to measure full-res
// cost; also prints per-stage milliseconds.
uint32_t kWidth = env_dim("WIVRN_ENCODE_TEST_W", 320);
uint32_t kHeight = env_dim("WIVRN_ENCODE_TEST_H", 240);

struct Device
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
};

bool bring_up(Device & dev)
{
	static const char * extensions[] = {
	        "VK_KHR_surface",
	        "VK_EXT_metal_surface",
	        "VK_KHR_portability_enumeration",
	};
	VkApplicationInfo appInfo{
	        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	        .pApplicationName = "encode-test",
	        .apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instanceInfo{
	        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
	        .pApplicationInfo = &appInfo,
	        .enabledLayerCount = 0,
	        .ppEnabledLayerNames = nullptr,
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
	        .enabledLayerCount = 0,
	        .ppEnabledLayerNames = nullptr,
	        .enabledExtensionCount = 1,
	        .ppEnabledExtensionNames = deviceExtensions,
	        .pEnabledFeatures = nullptr,
	};
	if (vkCreateDevice(dev.phys, &deviceInfo, nullptr, &dev.device) != VK_SUCCESS)
		return false;
	vkGetDeviceQueue(dev.device, dev.family, 0, &dev.queue);
	return dev.queue != VK_NULL_HANDLE;
}

// One-shot clear of image to a solid BGRA color, leaving it in
// COLOR_ATTACHMENT_OPTIMAL (the app-rendered contract snapshot expects).
bool clear_image(Device & dev, VkImage image, VkCommandPool pool, float r, float g, float b)
{
	VkCommandBufferAllocateInfo allocInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	        .pNext = nullptr,
	        .commandPool = pool,
	        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	        .commandBufferCount = 1,
	};
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(dev.device, &allocInfo, &cmd) != VK_SUCCESS)
		return false;
	VkCommandBufferBeginInfo beginInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	        .pNext = nullptr,
	        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	        .pInheritanceInfo = nullptr,
	};
	vkBeginCommandBuffer(cmd, &beginInfo);
	VkImageMemoryBarrier toDst{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .pNext = nullptr,
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
	VkClearColorValue color{{r, g, b, 1.0f}};
	VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	VkImageMemoryBarrier toAttach{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .pNext = nullptr,
	        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toAttach);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo submit{
	        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	        .pNext = nullptr,
	        .waitSemaphoreCount = 0,
	        .pWaitSemaphores = nullptr,
	        .pWaitDstStageMask = nullptr,
	        .commandBufferCount = 1,
	        .pCommandBuffers = &cmd,
	        .signalSemaphoreCount = 0,
	        .pSignalSemaphores = nullptr,
	};
	VkFence fence = VK_NULL_HANDLE;
	VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
	vkCreateFence(dev.device, &fenceInfo, nullptr, &fence);
	vkQueueSubmit(dev.queue, 1, &submit, fence);
	VkResult res = vkWaitForFences(dev.device, 1, &fence, VK_TRUE, 5'000'000'000ull);
	vkDestroyFence(dev.device, fence, nullptr);
	vkFreeCommandBuffers(dev.device, pool, 1, &cmd);
	return res == VK_SUCCESS;
}

} // namespace

int main()
{
	using namespace wivrn_macos;

	// Converter lane check (pure CPU): memory {B,G,R,A} = {0,0,255,255}
	// is pure red -> Y~54; swapped lanes read it as blue -> Y~18.
	{
		uint8_t bgra[4 * 4 * 4] = {};
		for (int i = 0; i < 16; ++i)
		{
			bgra[i * 4 + 0] = 0;
			bgra[i * 4 + 1] = 0;
			bgra[i * 4 + 2] = 255;
			bgra[i * 4 + 3] = 255;
		}
		uint8_t yuv[4 * 4 * 3 / 2] = {};
		convert_to_nv12(bgra, 4, 4, false, yuv);
		CHECK(yuv[0] >= 50 && yuv[0] <= 58);
		convert_to_nv12(bgra, 4, 4, true, yuv);
		CHECK(yuv[0] >= 14 && yuv[0] <= 22);
	}

	Device dev{};
	CHECK(bring_up(dev));

	// Test image: BGRA-optimal, app-rendered layout contract.
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	{
		VkImageCreateInfo imageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = 0,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = VK_FORMAT_B8G8R8A8_SRGB,
		        .extent = {kWidth, kHeight, 1},
		        .mipLevels = 1,
		        .arrayLayers = 1,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		CHECK_VK(vkCreateImage(dev.device, &imageInfo, nullptr, &image));
		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(dev.device, image, &req);
		VkPhysicalDeviceMemoryProperties mem{};
		vkGetPhysicalDeviceMemoryProperties(dev.phys, &mem);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
		{
			if ((req.memoryTypeBits & (1u << i)) != 0 &&
			    (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
			{
				type = i;
				break;
			}
		}
		CHECK(type != UINT32_MAX);
		VkMemoryAllocateInfo allocInfo{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = nullptr, .allocationSize = req.size, .memoryTypeIndex = type};
		CHECK_VK(vkAllocateMemory(dev.device, &allocInfo, nullptr, &memory));
		CHECK_VK(vkBindImageMemory(dev.device, image, memory, 0));
	}
	VkCommandPool pool = VK_NULL_HANDLE;
	{
		VkCommandPoolCreateInfo poolInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = dev.family,
		};
		CHECK_VK(vkCreateCommandPool(dev.device, &poolInfo, nullptr, &pool));
	}

	EyeSnapshot snapshot;
	CHECK(snapshot.init(dev.phys, dev.device, dev.queue, dev.family, kWidth, kHeight));

	X264Encoder encoder;
	EncoderParams params{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
	CHECK(encoder.open(params));

	size_t totalBytes = 0;
	bool sawConfig = false, sawIdr = false;
	int64_t snap_ns = 0, feed_ns = 0;
	auto clock_ns = [] {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	};
	for (int frame = 0; frame < 10; ++frame)
	{
		// Alternate solid colors so P-frames carry real deltas.
		CHECK(clear_image(dev, image, pool, frame % 2 == 0 ? 0.2f : 0.8f, 0.3f, 0.9f));
		int64_t t0 = clock_ns();
		const uint8_t * bgra = snapshot.capture(image);
		int64_t t1 = clock_ns();
		CHECK(bgra != nullptr);
		// Spot-check the snapshot saw the clear color: B=0.9 stays high;
		// R alternates 0.2/0.8 per frame (threshold straddles both the
		// raw-linear and sRGB-encoded interpretations).
		CHECK(bgra[0] > 200);
		if (frame % 2 == 0)
			CHECK(bgra[2] < 150);
		else
			CHECK(bgra[2] > 150);

		bool forceIdr = frame == 0 || frame == 5;
		std::vector<EncodedNal> nals = encoder.feed(bgra, false, forceIdr);
		int64_t t2 = clock_ns();
		snap_ns += t1 - t0;
		feed_ns += t2 - t1;
		CHECK(!nals.empty());
		CHECK(nals.back().is_last);
		for (const EncodedNal & nal: nals)
		{
			CHECK(!nal.bytes.empty());
			totalBytes += nal.bytes.size();
			sawConfig = sawConfig || nal.is_config;
			sawIdr = sawIdr || nal.is_idr;
			// Annex-B start code present.
			CHECK(nal.bytes.size() >= 4 && nal.bytes[0] == 0 && nal.bytes[1] == 0);
		}

		// Shard the frame like the streamer will.
		FrameMeta meta{.frame_idx = (uint64_t)frame};
		size_t shards = 0, shardBytes = 0, nalBytes = 0;
		for (const EncodedNal & nal: nals)
			nalBytes += nal.bytes.size();
		bool seenFirst = false, seenLast = false, reliable = false;
		shard_frame(
		        0,
		        nals,
		        meta,
		        [&](uint8_t stream, const std::vector<uint8_t> & bytes, bool first, bool last, bool rel) {
			        CHECK(stream == 0);
			        CHECK(bytes.size() <= 1400);
			        CHECK(!bytes.empty());
			        seenFirst = seenFirst || first;
			        seenLast = seenLast || last;
			        reliable = reliable || rel;
			        shards += 1;
			        shardBytes += bytes.size();
		        });
		CHECK(seenFirst && seenLast && shards >= 1);
		CHECK(shardBytes == nalBytes);
		CHECK(reliable == (forceIdr || frame == 0));
	}
	std::printf("encoded 10 frames, %zu total bytes\n", totalBytes);
	std::printf("snapshot %.1f ms/frame, feed %.1f ms/frame\n", snap_ns / 10.0 / 1e6, feed_ns / 10.0 / 1e6);
	CHECK(totalBytes > 1000);
	CHECK(sawConfig && sawIdr);

	// Reconfig path (settings_changed) stays alive.
	encoder.reconfig(10'000'000, 72);
	{
		CHECK(clear_image(dev, image, pool, 0.5f, 0.5f, 0.5f));
		const uint8_t * bgra = snapshot.capture(image);
		CHECK(bgra != nullptr);
		std::vector<EncodedNal> nals = encoder.feed(bgra, false, true);
		CHECK(!nals.empty());
	}

	// VideoToolbox backend (Phase H): same wire assertions as x264
	// (Annex-B, SPS/PPS + IDR present, shardable). Skips cleanly where
	// no hardware encoder exists (VMs); the facade fallback is covered
	// below regardless.
	{
		VideoEncoder vt;
		EncoderParams vparams{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
		if (!vt.open(vparams, VideoBackend::VT))
		{
			std::puts("vt backend unavailable, skipping VT cases");
		}
		else
		{
			CHECK(vt.using_vt());
			size_t vbytes = 0;
			bool vsawConfig = false, vsawIdr = false;
			for (int frame = 0; frame < 10; ++frame)
			{
				CHECK(clear_image(dev, image, pool, frame % 2 == 0 ? 0.2f : 0.8f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				bool forceIdr = frame == 0 || frame == 5;
				std::vector<EncodedNal> nals = vt.feed(bgra, false, forceIdr);
				CHECK(!nals.empty());
				CHECK(nals.back().is_last);
				bool frameIdr = false;
				for (const EncodedNal & nal: nals)
				{
					CHECK(!nal.bytes.empty());
					CHECK(nal.bytes.size() >= 4 && nal.bytes[0] == 0 && nal.bytes[1] == 0 && nal.bytes[2] == 0 &&
					      nal.bytes[3] == 1);
					vbytes += nal.bytes.size();
					vsawConfig = vsawConfig || nal.is_config;
					vsawIdr = vsawIdr || nal.is_idr;
					frameIdr = frameIdr || nal.is_idr;
				}
				CHECK(!forceIdr || frameIdr);
			}
			std::printf("vt encoded 10 frames, %zu total bytes\n", vbytes);
			// Flat test content compresses to ~100 B/frame on a HW
			// CABAC encoder (roundtrip-verified faithful); the floor
			// only guards against total collapse, not bitrate.
			CHECK(vbytes > 400);
			CHECK(vsawConfig && vsawIdr);
			vt.reconfig(10'000'000, 72);
			{
				CHECK(clear_image(dev, image, pool, 0.5f, 0.5f, 0.5f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				std::vector<EncodedNal> nals = vt.feed(bgra, false, true);
				CHECK(!nals.empty());
			}
			vt.close();
		}
	}

	// Facade selection: explicit x264 always serves x264; auto serves
	// VT where available else x264 (either satisfies the contract).
	{
		VideoEncoder forced;
		CHECK(forced.open(params, VideoBackend::X264));
		CHECK(!forced.using_vt());
		forced.close();
		VideoEncoder automatic;
		CHECK(automatic.open(params, VideoBackend::Auto));
		automatic.close();
	}

	snapshot.shutdown();
	vkDestroyCommandPool(dev.device, pool, nullptr);
	vkDestroyImage(dev.device, image, nullptr);
	vkFreeMemory(dev.device, memory, nullptr);
	vkDestroyDevice(dev.device, nullptr);
	vkDestroyInstance(dev.instance, nullptr);

	if (failures == 0)
		std::puts("encode test PASS");
	else
		std::printf("encode test FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
