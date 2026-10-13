// Trimmed MoltenVK-safe Vulkan helpers. See vk_context.h for the
// deliberate NOT-ported list (FD/video/DMA paths stay Linux-only).

#include "vk_context.h"

#include "runtime.h"

#include <chrono>

namespace wivrn_macos
{

namespace
{

constexpr uint32_t swapchainImageCount = 3;

bool format_ok(VkPhysicalDevice phys, VkFormat format)
{
	VkFormatProperties props{};
	vkGetPhysicalDeviceFormatProperties(phys, format, &props);
	constexpr VkFormatFeatureFlags needed =
	        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	return (props.optimalTilingFeatures & needed) == needed;
}

uint32_t memory_type(VkPhysicalDevice phys, uint32_t bits, bool deviceLocal)
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(phys, &mem);
	// Prefer device-local; fall back to any matching type.
	uint32_t fallback = UINT32_MAX;
	for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
	{
		if ((bits & (1u << i)) == 0)
			continue;
		if ((mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
			return i;
		if (fallback == UINT32_MAX)
			fallback = i;
	}
	return fallback;
}

} // namespace

XrResult validate_swapchain_config(
        VkPhysicalDevice phys,
        int64_t format,
        uint32_t width,
        uint32_t height,
        uint32_t arraySize,
        XrSwapchainUsageFlags usage,
        uint32_t sampleCount,
        uint32_t mipCount,
        uint32_t faceCount)
{
	if (width == 0 || height == 0 || width > SystemInfo::maxWidth * 2 || height > SystemInfo::maxHeight * 2)
		return XR_ERROR_VALIDATION_FAILURE;
	if (arraySize < 1 || arraySize > 2)
		return XR_ERROR_VALIDATION_FAILURE;
	if (sampleCount != 1)
	{
		log("swapchain: multisample (%u) unsupported in v1", sampleCount);
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}
	if (mipCount != 1 || faceCount != 1)
		return XR_ERROR_VALIDATION_FAILURE;
	if ((usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
	{
		log("swapchain: depth/stencil unsupported in v1");
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}
	if (!format_ok(phys, static_cast<VkFormat>(format)))
	{
		log("swapchain: format %d lacks color/transfer support on this device", (int)format);
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}
	return XR_SUCCESS;
}

XrResult create_swapchain_images(Swapchain & swap)
{
	VkPhysicalDevice phys = swap.session->vkPhysicalDevice;
	VkFormat format = static_cast<VkFormat>(swap.format);

	VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkImageCreateFlags imageFlags = 0;
	if ((swap.usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0)
		imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((swap.usage & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0)
		imageFlags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

	for (uint32_t i = 0; i < swapchainImageCount; ++i)
	{
		VkImageCreateInfo imageInfo{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = imageFlags,
		        .imageType = VK_IMAGE_TYPE_2D,
		        .format = format,
		        .extent = {swap.width, swap.height, 1},
		        .mipLevels = 1,
		        .arrayLayers = swap.arraySize,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		        .tiling = VK_IMAGE_TILING_OPTIMAL,
		        .usage = imageUsage,
		        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		        .queueFamilyIndexCount = 0,
		        .pQueueFamilyIndices = nullptr,
		        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VkImage image = VK_NULL_HANDLE;
		if (vkCreateImage(swap.device, &imageInfo, nullptr, &image) != VK_SUCCESS)
		{
			log("swapchain: vkCreateImage failed");
			destroy_swapchain_images(swap);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(swap.device, image, &req);
		uint32_t type = memory_type(phys, req.memoryTypeBits, true);
		if (type == UINT32_MAX)
		{
			log("swapchain: no suitable memory type");
			vkDestroyImage(swap.device, image, nullptr);
			destroy_swapchain_images(swap);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		VkMemoryAllocateInfo allocInfo{
		        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		        .pNext = nullptr,
		        .allocationSize = req.size,
		        .memoryTypeIndex = type,
		};
		VkDeviceMemory memory = VK_NULL_HANDLE;
		if (vkAllocateMemory(swap.device, &allocInfo, nullptr, &memory) != VK_SUCCESS ||
		    vkBindImageMemory(swap.device, image, memory, 0) != VK_SUCCESS)
		{
			log("swapchain: image memory bind failed");
			if (memory != VK_NULL_HANDLE)
				vkFreeMemory(swap.device, memory, nullptr);
			vkDestroyImage(swap.device, image, nullptr);
			destroy_swapchain_images(swap);
			return XR_ERROR_OUT_OF_MEMORY;
		}
		VkFenceCreateInfo fenceInfo{
		        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		        .pNext = nullptr,
		        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};
		VkFence fence = VK_NULL_HANDLE;
		if (vkCreateFence(swap.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
		{
			log("swapchain: fence create failed");
			vkFreeMemory(swap.device, memory, nullptr);
			vkDestroyImage(swap.device, image, nullptr);
			destroy_swapchain_images(swap);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		swap.images.push_back(image);
		swap.memories.push_back(memory);
		swap.fences.push_back(fence);
		swap.busy.push_back(false);
		swap.waited.push_back(false);
		swap.virgin.push_back(true);
	}
	return XR_SUCCESS;
}

void destroy_swapchain_images(Swapchain & swap)
{
	if (swap.device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(swap.device);
	for (VkFence fence: swap.fences)
		vkDestroyFence(swap.device, fence, nullptr);
	for (size_t i = 0; i < swap.images.size(); ++i)
	{
		vkDestroyImage(swap.device, swap.images[i], nullptr);
		if (i < swap.memories.size())
			vkFreeMemory(swap.device, swap.memories[i], nullptr);
	}
	swap.images.clear();
	swap.memories.clear();
	swap.fences.clear();
	swap.busy.clear();
	swap.waited.clear();
	swap.virgin.clear();
	swap.outstanding.clear();
	swap.nextIndex = 0;
	swap.lastAcquired = -1;
	swap.lastReleased = -1;
}

int64_t now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	               std::chrono::steady_clock::now().time_since_epoch())
	        .count();
}

} // namespace wivrn_macos
