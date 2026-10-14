// Phase 5a proof: clear a BGRA image on MoltenVK -> EyeSnapshot ->
// x264 BGRA -> Annex-B NALs -> ≤1400 B shards. No window, no loader,
// no headset: pure pixels-to-bits. Run directly:
//   ./wivrn-macos-encode-test

#include "encode.h"
#include "encode_vt.h"
#include "snapshot.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// P3 fps-measurement bench (additive; shipped paths untouched). Heavy legs
// run only with WIVRN_VT_BENCH=1 in the environment; the default suite
// keeps a lightweight timing-split sanity block (see main). Content is
// synthetic but stereo-like: moving bars + gradient + noise, variant 1
// shifted (disparity) and reseeded like a right-eye view. Generation is
// always complete before the encode clock starts.
namespace
{

uint32_t bench_frames()
{
	if (const char * value = std::getenv("WIVRN_VT_BENCH_FRAMES"))
	{
		unsigned long parsed = std::strtoul(value, nullptr, 10);
		if (parsed >= 1 && parsed <= 2000)
			return (uint32_t)parsed;
	}
	return 200;
}

void gen_bench_frame(
        uint8_t * bgra,
        uint32_t w,
        uint32_t h,
        int variant,
        uint64_t frame,
        const uint8_t * grad_col,
        const uint8_t * diag_col)
{
	uint32_t shift = variant == 0 ? 0u : (variant == 1 ? 24u : 48u); // v2 keeps MB alignment (3x16)
	// Variants 3/4 (E5 ramp-content test): 3 = alternating solid colors
	// (trivial temporal delta), 4 = smooth gradient only (no noise).
	if (variant == 3 || variant == 4)
	{
		uint8_t r = (frame & 1) == 0 ? 40 : 200;
		for (uint32_t y = 0; y < h; ++y)
		{
			uint8_t * row = bgra + (size_t)y * w * 4;
			for (uint32_t x = 0; x < w; ++x)
			{
				row[x * 4 + 0] = variant == 3 ? 90 : grad_col[x];
				row[x * 4 + 1] = variant == 3 ? 120 : diag_col[x];
				row[x * 4 + 2] = variant == 3 ? r : grad_col[x];
				row[x * 4 + 3] = 255;
			}
		}
		return;
	}
	uint32_t period = 96u;
	uint32_t motion = (uint32_t)(frame * 3u);
	std::vector<uint8_t> bar_col(w);
	for (uint32_t x = 0; x < w; ++x)
		bar_col[x] = (((x + shift + motion) % period) < period / 2) ? 200 : 60;
	uint32_t nseed = (uint32_t)frame * 9176u + (uint32_t)variant * 1013904223u;
	for (uint32_t y = 0; y < h; ++y)
	{
		uint8_t * row = bgra + (size_t)y * w * 4;
		uint32_t ynoise = (y * 40503u) ^ nseed;
		for (uint32_t x = 0; x < w; ++x)
		{
			uint32_t n = (x * 2654435761u) ^ ynoise;
			n ^= n >> 13;
			n *= 0x5bd1e995u;
			n ^= n >> 15;
			uint32_t noise = (n >> 8) & 63u;
			uint32_t bar = bar_col[x];
			row[x * 4 + 0] = (uint8_t)((bar + noise) >> 1);
			row[x * 4 + 1] = grad_col[x];
			row[x * 4 + 2] = (uint8_t)((diag_col[x] + bar) >> 1);
			row[x * 4 + 3] = 255;
		}
	}
}

struct BenchLeg
{
	std::vector<double> ms;
	uint64_t bytes = 0;
	uint64_t drops = 0;
};

void bench_leg_report(const char * name, const BenchLeg & leg)
{
	if (leg.ms.empty())
	{
		std::printf("bench %s: n=0 (all dropped)\n", name);
		return;
	}
	std::vector<double> sorted = leg.ms;
	std::sort(sorted.begin(), sorted.end());
	double mean = 0;
	for (double v: sorted)
		mean += v;
	mean /= (double)sorted.size();
	double med = sorted[sorted.size() / 2];
	double p95 = sorted[(sorted.size() * 95) / 100];
	std::printf("bench %s: n=%zu med=%.2f p95=%.2f mean=%.2f ms/frame bytes=%llu/frame drops=%llu\n",
	            name,
	            sorted.size(),
	            med,
	            p95,
	            mean,
	            (unsigned long long)(leg.bytes / sorted.size()),
	            (unsigned long long)leg.drops);
}

// Mean enqueue/wait/total deltas over the feeds a leg added (the
// VTEncoder counters are cumulative, so snapshot-compare per leg).
void bench_split_report(const char * name, wivrn_macos::VTEncoder & enc, uint64_t t0, uint64_t e0, uint64_t w0, uint64_t f0)
{
	uint64_t dt = enc.feeds_timed() - t0;
	if (dt == 0)
	{
		std::printf("bench split %s: n=0\n", name);
		return;
	}
	std::printf("bench split %s: enqueue_us=%llu wait_us=%llu total_us=%llu (n=%llu)\n",
	            name,
	            (unsigned long long)((enc.enqueue_ns_total() - e0) / dt / 1000),
	            (unsigned long long)((enc.wait_ns_total() - w0) / dt / 1000),
	            (unsigned long long)((enc.feed_ns_total() - f0) / dt / 1000),
	            (unsigned long long)dt);
}

// One timed feed into a leg (generation done by the caller, clock wraps
// feed() only). Drops are counted, never timed.
void bench_feed_one(wivrn_macos::VTEncoder & enc, const uint8_t * buf, bool force_idr, int f, int warmup, BenchLeg & leg)
{
	auto t0 = std::chrono::steady_clock::now();
	std::vector<wivrn_macos::EncodedNal> nals = enc.feed(buf, false, force_idr);
	auto t1 = std::chrono::steady_clock::now();
	if (nals.empty() || !nals.back().is_last)
	{
		if (f >= warmup)
			leg.drops += 1;
		return;
	}
	if (f == 0)
	{
		bool frameIdr = false;
		for (const wivrn_macos::EncodedNal & nal: nals)
			frameIdr = frameIdr || nal.is_idr;
		CHECK(frameIdr);
	}
	if (f < warmup)
		return;
	leg.ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
	for (const wivrn_macos::EncodedNal & nal: nals)
		leg.bytes += nal.bytes.size();
}

void bench_solo(
        wivrn_macos::VTEncoder & enc,
        uint8_t * buf,
        uint32_t w,
        uint32_t h,
        int variant,
        int n,
        int warmup,
        const uint8_t * grad_col,
        const uint8_t * diag_col,
        BenchLeg & leg,
        const char * name)
{
	uint64_t t0 = enc.feeds_timed(), e0 = enc.enqueue_ns_total(), w0 = enc.wait_ns_total(), f0 = enc.feed_ns_total();
	for (int f = 0; f < n + warmup; ++f)
	{
		gen_bench_frame(buf, w, h, variant, (uint64_t)f, grad_col, diag_col);
		bench_feed_one(enc, buf, f == 0, f, warmup, leg);
	}
	bench_leg_report(name, leg);
	bench_split_report(name, enc, t0, e0, w0, f0);
	CHECK(enc.frames_submitted() == enc.feeds_timed() + enc.frames_dropped());
}

// Production-like serial pair: first.feed then second.feed per heartbeat,
// timed by position (caller picks session/content per position).
void bench_pair(
        wivrn_macos::VTEncoder & first,
        wivrn_macos::VTEncoder & second,
        uint8_t * buf_first,
        uint8_t * buf_second,
        uint32_t w,
        uint32_t h,
        int variant_first,
        int variant_second,
        int n,
        int warmup,
        const uint8_t * grad_col,
        const uint8_t * diag_col,
        BenchLeg & leg_first,
        BenchLeg & leg_second,
        const char * name_first,
        const char * name_second)
{
	uint64_t t0 = first.feeds_timed(), e0 = first.enqueue_ns_total(), w0 = first.wait_ns_total(), f0 = first.feed_ns_total();
	uint64_t u0 = second.feeds_timed(), ue0 = second.enqueue_ns_total(), uw0 = second.wait_ns_total(), uf0 = second.feed_ns_total();
	for (int f = 0; f < n + warmup; ++f)
	{
		// Both frames are generated before either clock starts (tighter
		// than production, where a snapshot sits between the feeds —
		// strictly more queueing pressure, so a null result here is a
		// conservative bound).
		gen_bench_frame(buf_first, w, h, variant_first, (uint64_t)f, grad_col, diag_col);
		gen_bench_frame(buf_second, w, h, variant_second, (uint64_t)f, grad_col, diag_col);
		bench_feed_one(first, buf_first, f == 0, f, warmup, leg_first);
		bench_feed_one(second, buf_second, f == 0, f, warmup, leg_second);
	}
	bench_leg_report(name_first, leg_first);
	bench_split_report(name_first, first, t0, e0, w0, f0);
	bench_leg_report(name_second, leg_second);
	bench_split_report(name_second, second, u0, ue0, uw0, uf0);
	CHECK(first.frames_submitted() == first.feeds_timed() + first.frames_dropped());
	CHECK(second.frames_submitted() == second.feeds_timed() + second.frames_dropped());
}

void run_vt_bench()
{
	using namespace wivrn_macos;
	uint32_t bw = env_dim("WIVRN_VT_BENCH_W", 2064);
	uint32_t bh = env_dim("WIVRN_VT_BENCH_H", 2208);
	int n = (int)bench_frames();
	int warmup = 10;
	std::printf("bench config: %ux%u @90 25000000bps frames=%d warmup=%d\n", bw, bh, n, warmup);
	std::vector<uint8_t> bufA((size_t)bw * bh * 4), bufB((size_t)bw * bh * 4);
	std::vector<uint8_t> grad_col(bw), diag_col(bw);
	for (uint32_t x = 0; x < bw; ++x)
	{
		grad_col[x] = (uint8_t)((x * 255u) / bw);
		diag_col[x] = (uint8_t)((x * 255u) / (bw + bh * 2u));
	}
	EncoderParams bp{.width = bw, .height = bh, .fps = 90, .bitrate_bps = 25'000'000};

	// L1/L2 solo: same session back-to-back, pure content delta.
	{
		VTEncoder sa;
		if (!sa.open(bp))
		{
			std::puts("bench solo skipped (no HW)");
			return;
		}
		BenchLeg legA, legB;
		bench_solo(sa, bufA.data(), bw, bh, 0, n, warmup, grad_col.data(), diag_col.data(), legA, "solo-A");
		bench_solo(sa, bufB.data(), bw, bh, 1, n, warmup, grad_col.data(), diag_col.data(), legB, "solo-B-same-session");
		sa.close();
	}
	// L3/L4 paired: two sessions, production order then swapped.
	{
		VTEncoder p0, p1;
		if (!p0.open(bp) || !p1.open(bp))
		{
			std::puts("bench pair skipped (no HW)");
			return;
		}
		BenchLeg pos0, pos1, spos0, spos1;
		bench_pair(
		        p0,
		        p1,
		        bufA.data(),
		        bufB.data(),
		        bw,
		        bh,
		        0,
		        1,
		        n,
		        warmup,
		        grad_col.data(),
		        diag_col.data(),
		        pos0,
		        pos1,
		        "pair-pos0-P0A",
		        "pair-pos1-P1B");
		bench_pair(
		        p1,
		        p0,
		        bufB.data(),
		        bufA.data(),
		        bw,
		        bh,
		        1,
		        0,
		        n,
		        warmup,
		        grad_col.data(),
		        diag_col.data(),
		        spos0,
		        spos1,
		        "swap-pos0-P1B",
		        "swap-pos1-P0A");
		p0.close();
		p1.close();
	}
	// L5 paired same-content: identical pixels isolate session/instance.
	{
		VTEncoder q0, q1;
		if (!q0.open(bp) || !q1.open(bp))
		{
			std::puts("bench same-content skipped (no HW)");
			return;
		}
		BenchLeg qp0, qp1;
		bench_pair(
		        q0,
		        q1,
		        bufA.data(),
		        bufB.data(),
		        bw,
		        bh,
		        0,
		        0,
		        n,
		        warmup,
		        grad_col.data(),
		        diag_col.data(),
		        qp0,
		        qp1,
		        "sameA-pos0-Q0",
		        "sameA-pos1-Q1");
		q0.close();
		q1.close();
	}
	// L6 ExpectedFrameRate honesty: same pixels/cadence, hint varies.
	{
		static const uint32_t hints[] = {30, 45, 72, 90, 135};
		for (uint32_t hint: hints)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "%u", hint);
			setenv("WIVRN_MACOS_VT_FPS", text, 1);
			VTEncoder fh;
			if (!fh.open(bp))
			{
				std::puts("bench fps sweep skipped (no HW)");
				unsetenv("WIVRN_MACOS_VT_FPS");
				return;
			}
			BenchLeg leg;
			char name[32];
			std::snprintf(name, sizeof(name), "fps-hint-%u", hint);
			bench_solo(fh, bufA.data(), bw, bh, 0, 30, warmup, grad_col.data(), diag_col.data(), leg, name);
			fh.close();
			unsetenv("WIVRN_MACOS_VT_FPS");
		}
	}
}

// Follow-up disambiguation (WIVRN_VT_BENCH2=1): order-swapped solos (B
// first), pure-age rerun (A then A), MB-aligned variant, interleaved
// hint sweep, and the generation-cost context for duty-cycle math.
void run_vt_bench2()
{
	using namespace wivrn_macos;
	uint32_t bw = env_dim("WIVRN_VT_BENCH_W", 2064);
	uint32_t bh = env_dim("WIVRN_VT_BENCH_H", 2208);
	int n = (int)bench_frames();
	int warmup = 10;
	std::printf("bench2 config: %ux%u @90 25000000bps frames=%d warmup=%d\n", bw, bh, n, warmup);
	std::vector<uint8_t> bufA((size_t)bw * bh * 4), bufB((size_t)bw * bh * 4);
	std::vector<uint8_t> grad_col(bw), diag_col(bw);
	for (uint32_t x = 0; x < bw; ++x)
	{
		grad_col[x] = (uint8_t)((x * 255u) / bw);
		diag_col[x] = (uint8_t)((x * 255u) / (bw + bh * 2u));
	}
	{
		auto t0 = std::chrono::steady_clock::now();
		for (int f = 0; f < 20; ++f)
			gen_bench_frame(bufA.data(), bw, bh, 0, (uint64_t)f, grad_col.data(), diag_col.data());
		auto t1 = std::chrono::steady_clock::now();
		std::printf("bench2 gen: %.2f ms/frame\n", std::chrono::duration<double, std::milli>(t1 - t0).count() / 20.0);
	}
	EncoderParams bp{.width = bw, .height = bh, .fps = 90, .bitrate_bps = 25'000'000};

	// E1a order swap: B young on a fresh session, then A old.
	{
		VTEncoder r0;
		if (!r0.open(bp))
		{
			std::puts("bench2 E1a skipped (no HW)");
			return;
		}
		BenchLeg legB, legA;
		bench_solo(r0, bufB.data(), bw, bh, 1, n, warmup, grad_col.data(), diag_col.data(), legB, "E1a-solo-B-young");
		bench_solo(r0, bufA.data(), bw, bh, 0, n, warmup, grad_col.data(), diag_col.data(), legA, "E1a-solo-A-old");
		r0.close();
	}
	// E1b pure age: A young then A old, same session and content.
	{
		VTEncoder r1;
		if (!r1.open(bp))
		{
			std::puts("bench2 E1b skipped (no HW)");
			return;
		}
		BenchLeg legA1, legA2;
		bench_solo(r1, bufA.data(), bw, bh, 0, n, warmup, grad_col.data(), diag_col.data(), legA1, "E1b-solo-A-young");
		bench_solo(r1, bufA.data(), bw, bh, 0, n, warmup, grad_col.data(), diag_col.data(), legA2, "E1b-solo-A-old");
		r1.close();
	}
	// E1c alignment: MB-aligned variant 2 young on a fresh session.
	{
		VTEncoder r2;
		if (!r2.open(bp))
		{
			std::puts("bench2 E1c skipped (no HW)");
			return;
		}
		BenchLeg legV2;
		bench_solo(r2, bufB.data(), bw, bh, 2, n, warmup, grad_col.data(), diag_col.data(), legV2, "E1c-solo-V2-young");
		r2.close();
	}
	// E2 interleaved hint sweep on fresh sessions (lottery control).
	{
		static const uint32_t hints[] = {90, 45, 90, 72, 90, 30, 135, 60, 90, 80};
		for (uint32_t hint: hints)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "%u", hint);
			setenv("WIVRN_MACOS_VT_FPS", text, 1);
			VTEncoder fh;
			if (!fh.open(bp))
			{
				std::puts("bench2 E2 skipped (no HW)");
				unsetenv("WIVRN_MACOS_VT_FPS");
				return;
			}
			BenchLeg leg;
			char name[32];
			std::snprintf(name, sizeof(name), "E2-hint-%u", hint);
			bench_solo(fh, bufA.data(), bw, bh, 0, 30, warmup, grad_col.data(), diag_col.data(), leg, name);
			fh.close();
			unsetenv("WIVRN_MACOS_VT_FPS");
		}
	}
}

// Median of leg.ms over [begin, end) for time-course windows.
double bench_window_med(const BenchLeg & leg, size_t begin, size_t end)
{
	if (begin >= end || end > leg.ms.size())
		return -1;
	std::vector<double> w(leg.ms.begin() + begin, leg.ms.begin() + end);
	std::sort(w.begin(), w.end());
	return w[w.size() / 2];
}

void bench_windows_report(const char * name, const BenchLeg & leg, size_t window)
{
	std::printf("bench3 %s windows med:", name);
	for (size_t b = 0; b + window <= leg.ms.size(); b += window)
		std::printf(" %.2f", bench_window_med(leg, b, b + window));
	std::printf(" (n=%zu drops=%llu)\n", leg.ms.size(), (unsigned long long)leg.drops);
}

// E3 feed-one on a pre-generated cycle (no generation gap: cadence ==
// encode cost, matching the fps hint instead of trailing it by ~23 ms).
void bench_cycle_one(wivrn_macos::VTEncoder & enc, const std::vector<std::vector<uint8_t>> & cycle, int f, BenchLeg & leg)
{
	const std::vector<uint8_t> & buf = cycle[(size_t)f % cycle.size()];
	auto t0 = std::chrono::steady_clock::now();
	std::vector<wivrn_macos::EncodedNal> nals = enc.feed(buf.data(), false, f == 0);
	auto t1 = std::chrono::steady_clock::now();
	if (nals.empty() || !nals.back().is_last)
	{
		leg.drops += 1;
		return;
	}
	leg.ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
	for (const wivrn_macos::EncodedNal & nal: nals)
		leg.bytes += nal.bytes.size();
}

// Cadence-matched round (WIVRN_VT_BENCH3=1): pre-generated 4-frame cycles
// fed back-to-back distinguish rate-control/buffer drift (cadence-driven)
// from true session age (feed-count-driven) and thermal drift
// (wall-time-driven). E3d extends the pair-position question into old age.
void run_vt_bench3()
{
	using namespace wivrn_macos;
	uint32_t bw = env_dim("WIVRN_VT_BENCH_W", 2064);
	uint32_t bh = env_dim("WIVRN_VT_BENCH_H", 2208);
	const int n = 420;
	std::printf("bench3 config: %ux%u cycle=4 back-to-back n=%d\n", bw, bh, n);
	std::vector<uint8_t> grad_col(bw), diag_col(bw);
	for (uint32_t x = 0; x < bw; ++x)
	{
		grad_col[x] = (uint8_t)((x * 255u) / bw);
		diag_col[x] = (uint8_t)((x * 255u) / (bw + bh * 2u));
	}
	std::vector<std::vector<uint8_t>> cycA(4, std::vector<uint8_t>((size_t)bw * bh * 4));
	std::vector<std::vector<uint8_t>> cycB(4, std::vector<uint8_t>((size_t)bw * bh * 4));
	for (int k = 0; k < 4; ++k)
	{
		gen_bench_frame(cycA[(size_t)k].data(), bw, bh, 0, (uint64_t)k, grad_col.data(), diag_col.data());
		gen_bench_frame(cycB[(size_t)k].data(), bw, bh, 1, (uint64_t)k, grad_col.data(), diag_col.data());
	}
	EncoderParams bp{.width = bw, .height = bh, .fps = 90, .bitrate_bps = 25'000'000};

	// E3a solo hint-90 back-to-back: flat (cadence-driven) or climb (age)?
	{
		VTEncoder s;
		if (!s.open(bp))
		{
			std::puts("bench3 E3a skipped (no HW)");
			return;
		}
		BenchLeg leg;
		for (int f = 0; f < n; ++f)
			bench_cycle_one(s, cycA, f, leg);
		bench_windows_report("E3a-solo90", leg, 60);
		CHECK(s.frames_submitted() == s.feeds_timed() + s.frames_dropped());
		s.close();
	}
	// E3b solo hint-45 back-to-back (cadence now ~2x the hint budget).
	{
		setenv("WIVRN_MACOS_VT_FPS", "45", 1);
		VTEncoder s;
		if (!s.open(bp))
		{
			std::puts("bench3 E3b skipped (no HW)");
			unsetenv("WIVRN_MACOS_VT_FPS");
			return;
		}
		BenchLeg leg;
		for (int f = 0; f < n; ++f)
			bench_cycle_one(s, cycA, f, leg);
		bench_windows_report("E3b-solo45", leg, 60);
		CHECK(s.frames_submitted() == s.feeds_timed() + s.frames_dropped());
		s.close();
		unsetenv("WIVRN_MACOS_VT_FPS");
	}
	// E3c solo hint-90 at 8 Mbps back-to-back (lower quality ceiling).
	{
		EncoderParams lp = bp;
		lp.bitrate_bps = 8'000'000;
		VTEncoder s;
		if (!s.open(lp))
		{
			std::puts("bench3 E3c skipped (no HW)");
			return;
		}
		BenchLeg leg;
		for (int f = 0; f < n; ++f)
			bench_cycle_one(s, cycA, f, leg);
		bench_windows_report("E3c-solo90-8mbps", leg, 60);
		CHECK(s.frames_submitted() == s.feeds_timed() + s.frames_dropped());
		s.close();
	}
	// E3d paired back-to-back into old age: does the position gap persist?
	{
		VTEncoder p0, p1;
		if (!p0.open(bp) || !p1.open(bp))
		{
			std::puts("bench3 E3d skipped (no HW)");
			return;
		}
		BenchLeg leg0, leg1;
		for (int f = 0; f < n; ++f)
		{
			bench_cycle_one(p0, cycA, f, leg0);
			bench_cycle_one(p1, cycB, f, leg1);
		}
		bench_windows_report("E3d-pos0-P0A", leg0, 60);
		bench_windows_report("E3d-pos1-P1B", leg1, 60);
		CHECK(p0.frames_submitted() == p0.feeds_timed() + p0.frames_dropped());
		CHECK(p1.frames_submitted() == p1.feeds_timed() + p1.frames_dropped());
		p0.close();
		p1.close();
	}
}

// Sleep-recovery round (WIVRN_VT_BENCH4=1): 120-feed back-to-back blocks
// separated by 10 s idle on the SAME session. Recovery across sleeps =>
// power/thermal integrator; steady climb => feed-count state. The CPU
// generation witness before/after detects global thermal throttling.
void run_vt_bench4()
{
	using namespace wivrn_macos;
	uint32_t bw = env_dim("WIVRN_VT_BENCH_W", 2064);
	uint32_t bh = env_dim("WIVRN_VT_BENCH_H", 2208);
	std::printf("bench4 config: %ux%u cycle=4 blocks=3x120 sleep=10s\n", bw, bh);
	std::vector<uint8_t> grad_col(bw), diag_col(bw);
	for (uint32_t x = 0; x < bw; ++x)
	{
		grad_col[x] = (uint8_t)((x * 255u) / bw);
		diag_col[x] = (uint8_t)((x * 255u) / (bw + bh * 2u));
	}
	std::vector<std::vector<uint8_t>> cycA(4, std::vector<uint8_t>((size_t)bw * bh * 4));
	for (int k = 0; k < 4; ++k)
		gen_bench_frame(cycA[(size_t)k].data(), bw, bh, 0, (uint64_t)k, grad_col.data(), diag_col.data());
	auto gen_witness = [&] {
		auto t0 = std::chrono::steady_clock::now();
		for (int f = 0; f < 10; ++f)
			gen_bench_frame(cycA[0].data(), bw, bh, 0, (uint64_t)f, grad_col.data(), diag_col.data());
		auto t1 = std::chrono::steady_clock::now();
		return std::chrono::duration<double, std::milli>(t1 - t0).count() / 10.0;
	};
	std::printf("bench4 gen-witness-before: %.2f ms/frame\n", gen_witness());
	EncoderParams bp{.width = bw, .height = bh, .fps = 90, .bitrate_bps = 25'000'000};
	VTEncoder s;
	if (!s.open(bp))
	{
		std::puts("bench4 skipped (no HW)");
		return;
	}
	for (int b = 0; b < 3; ++b)
	{
		BenchLeg leg;
		for (int f = 0; f < 120; ++f)
			bench_cycle_one(s, cycA, b * 120 + f, leg);
		char name[32];
		std::snprintf(name, sizeof(name), "E4-block-%d", b);
		bench_windows_report(name, leg, 60);
		if (b < 2)
			std::this_thread::sleep_for(std::chrono::seconds(10));
	}
	std::printf("bench4 gen-witness-after: %.2f ms/frame\n", gen_witness());
	CHECK(s.frames_submitted() == s.feeds_timed() + s.frames_dropped());
	s.close();
}

// Ramp-content round (WIVRN_VT_BENCH5=1): back-to-back solos on trivially
// compressible cycles. No ramp on easy content => the x2 dynamic is gated
// by content incompressibility (rate-control quality convergence), not an
// unconditional session tax — production hello_xr content decides.
void run_vt_bench5()
{
	using namespace wivrn_macos;
	uint32_t bw = env_dim("WIVRN_VT_BENCH_W", 2064);
	uint32_t bh = env_dim("WIVRN_VT_BENCH_H", 2208);
	const int n = 420;
	std::printf("bench5 config: %ux%u cycle=4 back-to-back n=%d\n", bw, bh, n);
	std::vector<uint8_t> grad_col(bw), diag_col(bw);
	for (uint32_t x = 0; x < bw; ++x)
	{
		grad_col[x] = (uint8_t)((x * 255u) / bw);
		diag_col[x] = (uint8_t)((x * 255u) / (bw + bh * 2u));
	}
	EncoderParams bp{.width = bw, .height = bh, .fps = 90, .bitrate_bps = 25'000'000};
	for (int variant: {3, 4})
	{
		std::vector<std::vector<uint8_t>> cyc(4, std::vector<uint8_t>((size_t)bw * bh * 4));
		for (int k = 0; k < 4; ++k)
			gen_bench_frame(cyc[(size_t)k].data(), bw, bh, variant, (uint64_t)k, grad_col.data(), diag_col.data());
		VTEncoder s;
		if (!s.open(bp))
		{
			std::puts("bench5 skipped (no HW)");
			return;
		}
		BenchLeg leg;
		for (int f = 0; f < n; ++f)
			bench_cycle_one(s, cyc, f, leg);
		char name[32];
		std::snprintf(name, sizeof(name), "E5-variant-%d", variant);
		bench_windows_report(name, leg, 60);
		if (!leg.ms.empty())
		{
			std::vector<double> sorted = leg.ms;
			std::sort(sorted.begin(), sorted.end());
			std::printf("bench5 %s bytes=%llu/frame\n", name, (unsigned long long)(leg.bytes / sorted.size()));
		}
		CHECK(s.frames_submitted() == s.feeds_timed() + s.frames_dropped());
		s.close();
	}
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
		VTEncoder pen;
		EncoderParams pparams{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
		// Periodic IDR: EVERY=5 bounds every gap between IDR frames
		// even with no NACK (forced-IDR path below stays intact).
		setenv("WIVRN_MACOS_VT_IDR_EVERY", "5", 1);
		if (!pen.open(pparams))
		{
			std::puts("vt periodic-IDR cases skipped (no HW)");
		}
		else
		{
			CHECK(pen.idr_every() == 5);
			// x264-matching VUI triple accepted by the HW encoder.
			CHECK(pen.color_signaled());
			std::vector<int> idr_frames;
			std::vector<uint8_t> dump;
			for (int frame = 0; frame < 13; ++frame)
			{
				CHECK(clear_image(dev, image, pool, frame % 2 == 0 ? 0.2f : 0.8f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				std::vector<EncodedNal> nals = pen.feed(bgra, false, false);
				CHECK(!nals.empty());
				CHECK(nals.back().is_last);
				bool frameIdr = false;
				for (const EncodedNal & nal: nals)
				{
					CHECK(nal.bytes.size() >= 4 && nal.bytes[0] == 0 && nal.bytes[1] == 0 && nal.bytes[2] == 0 &&
					      nal.bytes[3] == 1);
					frameIdr = frameIdr || nal.is_idr;
					dump.insert(dump.end(), nal.bytes.begin(), nal.bytes.end());
				}
				if (frameIdr)
					idr_frames.push_back(frame);
			}
			// Forced IDRs fire on the 5-frame period at the latest:
			// gaps stay bounded and the tail of the run is covered.
			CHECK(idr_frames.size() >= 2);
			for (size_t i = 1; i < idr_frames.size(); ++i)
				CHECK(idr_frames[i] - idr_frames[i - 1] <= 5);
			CHECK(!idr_frames.empty() && idr_frames.back() >= 7);
			// NACK-forced IDR still served on demand.
			{
				CHECK(clear_image(dev, image, pool, 0.1f, 0.4f, 0.7f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				std::vector<EncodedNal> nals = pen.feed(bgra, false, true);
				CHECK(!nals.empty());
				bool frameIdr = false;
				for (const EncodedNal & nal: nals)
					frameIdr = frameIdr || nal.is_idr;
				CHECK(frameIdr);
			}
			// No drops on a healthy session; the accounting identity holds.
			CHECK(pen.frames_dropped() == 0);
			CHECK(pen.frames_submitted() == pen.frames_encoded() + pen.frames_dropped());
			std::printf("vt periodic IDR frames:");
			for (int f: idr_frames)
				std::printf(" %d", f);
			std::puts("");
			std::printf("vt accounting (submitted=%llu encoded=%llu dropped=%llu late=%llu)\n",
			            (unsigned long long)pen.frames_submitted(),
			            (unsigned long long)pen.frames_encoded(),
			            (unsigned long long)pen.frames_dropped(),
			            (unsigned long long)pen.late_callbacks());
			// Annex-B dump for the ffmpeg roundtrip (ffprobe VUI +
			// content-faithful decode, run outside the test).
			FILE * out = std::fopen("/tmp/vt_roundtrip.h264", "wb");
			CHECK(out != nullptr);
			if (out != nullptr)
			{
				CHECK(std::fwrite(dump.data(), 1, dump.size(), out) == dump.size());
				std::fclose(out);
				std::printf("vt roundtrip dump %zu bytes -> /tmp/vt_roundtrip.h264\n", dump.size());
			}
			pen.close();
		}
		unsetenv("WIVRN_MACOS_VT_IDR_EVERY");
	}
	{
		// Timeout accounting: with a 0 ms bound every feed drops, each
		// consuming a distinct PTS (never repeats), and the counters
		// add up. The session serves healthy frames again afterwards.
		VTEncoder ten;
		EncoderParams tparams{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
		setenv("WIVRN_MACOS_VT_FEED_TIMEOUT_MS", "0", 1);
		if (!ten.open(tparams))
		{
			std::puts("vt timeout cases skipped (no HW)");
		}
		else
		{
			for (int frame = 0; frame < 3; ++frame)
			{
				CHECK(clear_image(dev, image, pool, 0.2f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				CHECK(ten.feed(bgra, false, false).empty());
			}
			CHECK(ten.frames_submitted() == 3);
			CHECK(ten.frames_dropped() == 3);
			CHECK(ten.frames_encoded() == 0);
			CHECK(ten.frames_submitted() == ten.frames_encoded() + ten.frames_dropped());
			std::printf(
			        "vt timeout accounting (submitted=%llu encoded=%llu dropped=%llu late=%llu)\n",
			        (unsigned long long)ten.frames_submitted(),
			        (unsigned long long)ten.frames_encoded(),
			        (unsigned long long)ten.frames_dropped(),
			        (unsigned long long)ten.late_callbacks());
			ten.close();
		}
		unsetenv("WIVRN_MACOS_VT_FEED_TIMEOUT_MS");
		VTEncoder rec;
		if (rec.open(tparams))
		{
			for (int frame = 0; frame < 2; ++frame)
			{
				CHECK(clear_image(dev, image, pool, 0.2f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				std::vector<EncodedNal> nals = rec.feed(bgra, false, frame == 0);
				CHECK(!nals.empty());
			}
			CHECK(rec.frames_encoded() == 2);
			rec.close();
		}
	}
	{
		// PrioritizeEncodingSpeed A/B: both modes must emit valid
		// frames; timings are reported for the keep-if-neutral call.
		EncoderParams sparams{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
		for (int mode = 0; mode < 2; ++mode)
		{
			if (mode == 0)
				unsetenv("WIVRN_MACOS_VT_FAST"); // default: speed hint on
			else
				setenv("WIVRN_MACOS_VT_FAST", "0", 1); // opt-out: hint off
			VTEncoder sen;
			if (!sen.open(sparams))
			{
				std::puts("vt speed A/B skipped (no HW)");
				break;
			}
			auto clock_ns = [] {
				return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
				        .count();
			};
			int64_t enc_ns = 0;
			for (int frame = 0; frame < 10; ++frame)
			{
				CHECK(clear_image(dev, image, pool, frame % 2 == 0 ? 0.2f : 0.8f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				int64_t t0 = clock_ns();
				std::vector<EncodedNal> nals = sen.feed(bgra, false, frame == 0);
				enc_ns += clock_ns() - t0;
				CHECK(!nals.empty());
				CHECK(nals.back().is_last);
			}
			std::printf("vt speed %s: %.3f ms/frame over 10 frames\n", mode == 0 ? "on " : "off", enc_ns / 10.0 / 1e6);
			sen.close();
		}
		unsetenv("WIVRN_MACOS_VT_FAST");
	}
	{
		// Slice opt-in stays a no-crash lever: output remains valid
		// whether or not this HW gates MaxH264SliceBytes.
		VTEncoder sen;
		EncoderParams lparams{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
		setenv("WIVRN_MACOS_VT_SLICE_BYTES", "1400", 1);
		if (!sen.open(lparams))
		{
			std::puts("vt slice cases skipped (no HW)");
		}
		else
		{
			for (int frame = 0; frame < 3; ++frame)
			{
				CHECK(clear_image(dev, image, pool, 0.2f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				std::vector<EncodedNal> nals = sen.feed(bgra, false, frame == 0);
				CHECK(!nals.empty());
				CHECK(nals.back().is_last);
			}
			sen.close();
		}
		unsetenv("WIVRN_MACOS_VT_SLICE_BYTES");
	}

	// P3 timing-split sanity (always on, small dims): enqueue+wait never
	// exceeds the whole feed, min<=max, counters add up, and the
	// per-feed bench log stays off unless opted in.
	{
		VTEncoder men;
		EncoderParams mparams{.width = kWidth, .height = kHeight, .fps = 90, .bitrate_bps = 5'000'000};
		if (!men.open(mparams))
		{
			std::puts("vt timing cases skipped (no HW)");
		}
		else
		{
			CHECK(men.feeds_timed() == 0);
			CHECK(!men.bench_enabled());
			for (int frame = 0; frame < 3; ++frame)
			{
				CHECK(clear_image(dev, image, pool, 0.2f, 0.3f, 0.9f));
				const uint8_t * bgra = snapshot.capture(image);
				CHECK(bgra != nullptr);
				std::vector<EncodedNal> nals = men.feed(bgra, false, frame == 0);
				CHECK(!nals.empty());
				CHECK(nals.back().is_last);
			}
			CHECK(men.feeds_timed() == 3);
			CHECK(men.frames_submitted() == men.feeds_timed() + men.frames_dropped());
			CHECK(men.enqueue_ns_total() + men.wait_ns_total() <= men.feed_ns_total());
			CHECK(men.feed_ns_min() <= men.feed_ns_max());
			CHECK(men.feed_ns_max() > 0);
			std::printf("vt timing (enqueue=%lluus wait=%lluus total=%lluus min=%lluns max=%lluns)\n",
			            (unsigned long long)men.last_enqueue_us(),
			            (unsigned long long)men.last_wait_us(),
			            (unsigned long long)men.last_total_us(),
			            (unsigned long long)men.feed_ns_min(),
			            (unsigned long long)men.feed_ns_max());
			men.close();
		}
		// Opt-in knob wires up: one session with the bench log on.
		setenv("WIVRN_MACOS_VT_BENCH", "1", 1);
		VTEncoder ben;
		if (ben.open(mparams))
		{
			CHECK(ben.bench_enabled());
			CHECK(clear_image(dev, image, pool, 0.2f, 0.3f, 0.9f));
			const uint8_t * bgra = snapshot.capture(image);
			CHECK(bgra != nullptr);
			CHECK(!ben.feed(bgra, false, true).empty());
			CHECK(ben.feeds_timed() == 1);
			ben.close();
		}
		unsetenv("WIVRN_MACOS_VT_BENCH");
	}

	// P3 heavy benches (env-gated): solo vs paired vs swapped vs fps
	// sweep (WIVRN_VT_BENCH=1), disambiguation round (WIVRN_VT_BENCH2=1).
	// Default suite cost is zero (full-res defaults 2064x2208 x200, see
	// WIVRN_VT_BENCH_W/H/FRAMES).
	if (std::getenv("WIVRN_VT_BENCH") != nullptr)
		run_vt_bench();
	if (std::getenv("WIVRN_VT_BENCH2") != nullptr)
		run_vt_bench2();
	if (std::getenv("WIVRN_VT_BENCH3") != nullptr)
		run_vt_bench3();
	if (std::getenv("WIVRN_VT_BENCH4") != nullptr)
		run_vt_bench4();
	if (std::getenv("WIVRN_VT_BENCH5") != nullptr)
		run_vt_bench5();

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
