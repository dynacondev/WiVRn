// Host-visible snapshot: COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC,
// copyImageToBuffer (tight rows), back to COLOR_ATTACHMENT_OPTIMAL,
// fence-waited on the app queue. Plain HOST_VISIBLE|HOST_COHERENT memory
// (no VMA in the runtime by design) persistently mapped.

#include "snapshot.h"

namespace wivrn_macos
{

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

	// App-rendered images arrive COLOR_ATTACHMENT_OPTIMAL (see header).
	VkImageMemoryBarrier toTransfer{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .pNext = nullptr,
	        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

	VkBufferImageCopy region{
	        .bufferOffset = 0,
	        .bufferRowLength = 0, // tight
	        .bufferImageHeight = 0,
	        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	        .imageOffset = {0, 0, 0},
	        .imageExtent = {width_, height_, 1},
	};
	vkCmdCopyImageToBuffer(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &region);

	VkImageMemoryBarrier toAttach{
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .pNext = nullptr,
	        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toAttach);

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
	if (vkQueueSubmit(queue_, 1, &submit, fence_) != VK_SUCCESS)
		return nullptr;
	if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS)
		return nullptr;
	return mapped_;
}

} // namespace wivrn_macos
