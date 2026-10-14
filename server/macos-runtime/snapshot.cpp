// Host-visible snapshot: COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC,
// copyImageToBuffer (tight rows), back to COLOR_ATTACHMENT_OPTIMAL,
// fence-waited on the app queue. Plain HOST_VISIBLE|HOST_COHERENT memory
// (no VMA in the runtime by design) persistently mapped.
//
// A3 synchronization (see snapshot.h contracts):
// - Worker-side GPU-completion wait: capture() consumes the release
//   note for the image (recorded by xrReleaseSwapchainImage) and waits
//   its fence with a 100 ms bound before recording the copy. Timeout
//   logs and proceeds (liveness over purity).
// - Queue-submit mutex: the copy-submit holds snapshot_queue_mutex()
//   (same mutex the release-empty-submit holds). Same-queue FIFO then
//   orders copy after render; app render submits stay the app's
//   business.
// - Barriers cover attachment + compute + transfer producers: apps may
//   post-process with compute/transfer before release, and the
//   attachment-only wait the old code used did not cover those.
// - Sub-image copies honor layer + rect (array swapchains); out-of-range
//   regions clamp with a loud once-only log.

#include "snapshot.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace wivrn_macos
{

namespace
{

// Bounded GPU-completion wait for a staged image (worker-side only).
constexpr uint64_t completion_timeout_ns = 100'000'000ull; // 100 ms

// Self-contained stderr logger in the runtime log format. snapshot.cpp
// links into targets without runtime.cpp (demo), so wivrn_macos::log
// is unavailable here.
void snap_log(const char * fmt, ...)
{
	int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                     std::chrono::steady_clock::now().time_since_epoch())
	                     .count();
	std::fprintf(stderr, "[%8lld] WIVRN_MACOS snapshot: ", (long long)ms);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	va_end(args);
	std::fprintf(stderr, "\n");
}

// Release-note registry: latest release per image wins, consumed once
// by capture(). Guarded by its own mutex; only map ops happen under
// it (never Vulkan calls, never the queue mutex).
std::mutex & note_mutex()
{
	static std::mutex mutex;
	return mutex;
}
std::unordered_map<VkImage, SnapshotReleaseNote> & note_map()
{
	static std::unordered_map<VkImage, SnapshotReleaseNote> map;
	return map;
}

} // namespace

std::mutex & snapshot_queue_mutex()
{
	static std::mutex mutex;
	return mutex;
}

void note_snapshot_release(VkImage image, const SnapshotReleaseNote & note)
{
	if (image == VK_NULL_HANDLE)
		return;
	std::lock_guard lock(note_mutex());
	note_map()[image] = note;
}

bool take_snapshot_release(VkImage image, SnapshotReleaseNote & out)
{
	std::lock_guard lock(note_mutex());
	auto & map = note_map();
	auto it = map.find(image);
	if (it == map.end())
		return false;
	out = it->second;
	map.erase(it);
	return true;
}

void drop_snapshot_release(VkImage image)
{
	std::lock_guard lock(note_mutex());
	note_map().erase(image);
}

EyeSnapshot::~EyeSnapshot()
{
	shutdown();
}

bool EyeSnapshot::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queueFamily, uint32_t width, uint32_t height)
{
	shutdown();
	phys_ = phys;
	device_ = device;
	queue_ = queue;
	width_ = width;
	height_ = height;

	VkBufferCreateInfo bufferInfo{
	        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	        .size = (VkDeviceSize)width * height * 4,
	        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer_) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(device, buffer_, &req);
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
	{
		if ((req.memoryTypeBits & (1u << i)) == 0)
			continue;
		if ((mem.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		{
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX)
	{
		shutdown();
		return false;
	}
	VkMemoryAllocateInfo allocInfo{
	        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	        .pNext = nullptr,
	        .allocationSize = req.size,
	        .memoryTypeIndex = type,
	};
	if (vkAllocateMemory(device, &allocInfo, nullptr, &memory_) != VK_SUCCESS ||
	    vkBindBufferMemory(device, buffer_, memory_, 0) != VK_SUCCESS ||
	    vkMapMemory(device, memory_, 0, VK_WHOLE_SIZE, 0, (void **)&mapped_) != VK_SUCCESS)
	{
		shutdown();
		return false;
	}
	VkCommandPoolCreateInfo poolInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	        .queueFamilyIndex = queueFamily,
	};
	if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
	{
		shutdown();
		return false;
	}
	VkCommandBufferAllocateInfo cmdInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	        .pNext = nullptr,
	        .commandPool = pool_,
	        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	        .commandBufferCount = 1,
	};
	if (vkAllocateCommandBuffers(device, &cmdInfo, &cmd_) != VK_SUCCESS)
	{
		shutdown();
		return false;
	}
	VkFenceCreateInfo fenceInfo{
	        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	        .pNext = nullptr,
	        .flags = 0,
	};
	if (vkCreateFence(device, &fenceInfo, nullptr, &fence_) != VK_SUCCESS)
	{
		shutdown();
		return false;
	}
	return true;
}

void EyeSnapshot::shutdown()
{
	if (device_ == VK_NULL_HANDLE)
		return;
	vkDeviceWaitIdle(device_);
	if (fence_ != VK_NULL_HANDLE)
		vkDestroyFence(device_, fence_, nullptr);
	if (cmd_ != VK_NULL_HANDLE)
		vkFreeCommandBuffers(device_, pool_, 1, &cmd_);
	if (pool_ != VK_NULL_HANDLE)
		vkDestroyCommandPool(device_, pool_, nullptr);
	if (mapped_ != nullptr)
		vkUnmapMemory(device_, memory_);
	if (buffer_ != VK_NULL_HANDLE)
		vkDestroyBuffer(device_, buffer_, nullptr);
	if (memory_ != VK_NULL_HANDLE)
		vkFreeMemory(device_, memory_, nullptr);
	phys_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
	queue_ = VK_NULL_HANDLE;
	width_ = 0;
	height_ = 0;
	buffer_ = VK_NULL_HANDLE;
	memory_ = VK_NULL_HANDLE;
	mapped_ = nullptr;
	pool_ = VK_NULL_HANDLE;
	cmd_ = VK_NULL_HANDLE;
	fence_ = VK_NULL_HANDLE;
}

const uint8_t * EyeSnapshot::capture(VkImage image)
{
	if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
		return nullptr;
	// Consume the release note when the image came through the
	// swapchain release path (frozen streamer stages it afterwards).
	SnapshotReleaseNote note{};
	const SnapshotReleaseNote * notePtr = nullptr;
	if (take_snapshot_release(image, note))
		notePtr = &note;
	return capture_impl(image, VK_NULL_HANDLE, VK_NULL_HANDLE, SnapshotRegion::full(), notePtr);
}

const uint8_t * EyeSnapshot::capture(VkImage image, const SnapshotRegion & region)
{
	if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
		return nullptr;
	SnapshotReleaseNote note{};
	const SnapshotReleaseNote * notePtr = nullptr;
	if (take_snapshot_release(image, note))
		notePtr = &note;
	return capture_impl(image, VK_NULL_HANDLE, VK_NULL_HANDLE, region, notePtr);
}

const uint8_t * EyeSnapshot::capture(VkImage image, VkFence completion, VkDevice completionDevice)
{
	if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
		return nullptr;
	return capture_impl(image, completion, completionDevice, SnapshotRegion::full(), nullptr);
}

const uint8_t * EyeSnapshot::capture(VkImage image, VkFence completion, VkDevice completionDevice, const SnapshotRegion & region)
{
	if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
		return nullptr;
	return capture_impl(image, completion, completionDevice, region, nullptr);
}

const uint8_t * EyeSnapshot::capture_impl(
        VkImage image,
        VkFence completion,
        VkDevice completionDevice,
        const SnapshotRegion & region,
        const SnapshotReleaseNote * note)
{
	// Resolve the GPU-completion fence: explicit argument wins, else
	// the consumed release note. A note without a GPU edge means the
	// app skipped Wait before release: there is nothing to wait on
	// (the fence is stale-signaled), so say so loudly once and rely
	// on queue order below.
	VkFence waitFence = completion;
	VkDevice waitDevice = completionDevice;
	if (waitFence == VK_NULL_HANDLE && note != nullptr && note->gpu_edge && note->fence != VK_NULL_HANDLE)
	{
		waitFence = note->fence;
		waitDevice = note->device;
	}
	if (note != nullptr && !note->gpu_edge)
	{
		static std::once_flag noWaitWarned;
		std::call_once(noWaitWarned, [] {
			snap_log("release staged without a prior Wait: no GPU-completion edge, copy relies on queue order alone");
		});
	}
	if (waitFence != VK_NULL_HANDLE && waitDevice != VK_NULL_HANDLE)
	{
		VkResult wait = vkWaitForFences(waitDevice, 1, &waitFence, VK_TRUE, completion_timeout_ns);
		if (wait == VK_TIMEOUT)
		{
			// Liveness over purity: a wedged render must not wedge
			// the worker (and the stream) behind it. The copy below
			// may tear this one frame; the encoder still emits.
			snap_log("GPU-completion wait timed out (100 ms), copying anyway");
		}
		else if (wait != VK_SUCCESS)
		{
			snap_log("GPU-completion wait failed (%d), copying anyway", (int)wait);
		}
	}

	// Resolve the copy region against the staging buffer (width_ x
	// height_) and, when a release note carries them, the image dims.
	uint32_t copyW = region.width != 0 ? region.width : width_;
	uint32_t copyH = region.height != 0 ? region.height : height_;
	uint32_t layer = region.layer;
	int32_t offX = region.offsetX < 0 ? 0 : region.offsetX;
	int32_t offY = region.offsetY < 0 ? 0 : region.offsetY;
	if (note != nullptr && note->width > 0 && note->height > 0)
	{
		if (copyW > note->width || copyH > note->height || layer >= note->arraySize ||
		    (uint32_t)offX + copyW > note->width || (uint32_t)offY + copyH > note->height)
		{
			static std::once_flag regionWarned;
			std::call_once(regionWarned, [] {
				snap_log("sub-image region out of range, clamping (need EndFrame layer/rect plumbing for array images)");
			});
			if (layer >= note->arraySize)
				layer = 0;
			if ((uint32_t)offX >= note->width)
				offX = 0;
			if ((uint32_t)offY >= note->height)
				offY = 0;
			if ((uint32_t)offX + copyW > note->width)
				copyW = note->width - (uint32_t)offX;
			if ((uint32_t)offY + copyH > note->height)
				copyH = note->height - (uint32_t)offY;
		}
	}
	if (copyW > width_ || copyH > height_ || copyW == 0 || copyH == 0)
	{
		snap_log("region %ux%u does not fit staging %ux%u, dropping frame", copyW, copyH, width_, height_);
		return nullptr;
	}

	vkResetFences(device_, 1, &fence_);
	vkResetCommandBuffer(cmd_, 0);

	VkCommandBufferBeginInfo beginInfo{
	        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	        .pNext = nullptr,
	        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	        .pInheritanceInfo = nullptr,
	};
	if (vkBeginCommandBuffer(cmd_, &beginInfo) != VK_SUCCESS)
		return nullptr;

	// App-rendered images arrive COLOR_ATTACHMENT_OPTIMAL (layout
	// contract, see header). The source stage/access cover every
	// producer the app may have used before release: raster
	// attachment writes, compute shader writes, and transfer writes
	// (blit/clear/fill post-passes). Attachment-only was insufficient.
	VkImageMemoryBarrier toTransfer{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .pNext = nullptr,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1},
	};
	vkCmdPipelineBarrier(
	        cmd_,
	        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT,
	        0,
	        0,
	        nullptr,
	        0,
	        nullptr,
	        1,
	        &toTransfer);

	VkBufferImageCopy copyRegion{
	        .bufferOffset = 0,
	        .bufferRowLength = 0, // tight
	        .bufferImageHeight = 0,
	        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
	        .imageOffset = {offX, offY, 0},
	        .imageExtent = {copyW, copyH, 1},
	};
	vkCmdCopyImageToBuffer(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &copyRegion);

	// The image is returned to COLOR_ATTACHMENT_OPTIMAL for the app's
	// next render. Destination scope covers the consumer side for the
	// same producer set (our read needs no write-visibility, but the
	// layout transition and execution ordering do).
	VkImageMemoryBarrier toAttach{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .pNext = nullptr,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1},
	};
	vkCmdPipelineBarrier(
	        cmd_,
	        VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	        0,
	        0,
	        nullptr,
	        0,
	        nullptr,
	        1,
	        &toAttach);

	if (vkEndCommandBuffer(cmd_) != VK_SUCCESS)
		return nullptr;
	VkSubmitInfo submit{
	        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	        .pNext = nullptr,
	        .waitSemaphoreCount = 0,
	        .pWaitSemaphores = nullptr,
	        .pWaitDstStageMask = nullptr,
	        .commandBufferCount = 1,
	        .pCommandBuffers = &cmd_,
	        .signalSemaphoreCount = 0,
	        .pSignalSemaphores = nullptr,
	};
	// Serialized with the release-empty-submit on the shared queue
	// (external sync for vkQueueSubmit; leaf critical section: no
	// other lock held, no wait inside, so the app thread holding it
	// in release can never block on the worker).
	VkResult submitted = VK_SUCCESS;
	{
		std::lock_guard lock(snapshot_queue_mutex());
		submitted = vkQueueSubmit(queue_, 1, &submit, fence_);
	}
	if (submitted != VK_SUCCESS)
		return nullptr;
	if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS)
		return nullptr;
	return mapped_;
}

} // namespace wivrn_macos
