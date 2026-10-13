// Host-visible snapshot of a swapchain image (Phase 5a). Copies one
// eye image to a persistently-mapped BGRA buffer for x264. Contract
// (also exercised by test_encode): the image arrives in
// COLOR_ATTACHMENT_OPTIMAL (standard post-render layout) and is
// returned to it; validation layers confirm with real apps in Phase 6.

#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace wivrn_macos
{

class EyeSnapshot
{
public:
	EyeSnapshot() = default;
	~EyeSnapshot();

	EyeSnapshot(const EyeSnapshot &) = delete;
	EyeSnapshot & operator=(const EyeSnapshot &) = delete;

	// Allocates the staging buffer + command resources. Device/queue are
	// the application's (same queue the release-submit uses; all snapshot
	// work happens on the app thread, so no extra locking in v1).
	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queueFamily, uint32_t width, uint32_t height);
	void shutdown();

	// Copy image -> host buffer, return pointer to w*h*4 BGRA bytes.
	// Returns nullptr on Vulkan failure.
	const uint8_t * capture(VkImage image);

	uint32_t width() const
	{
		return width_;
	}
	uint32_t height() const
	{
		return height_;
	}
	size_t stride() const
	{
		return width_ * 4;
	}

private:
	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	VkBuffer buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory memory_ = VK_NULL_HANDLE;
	uint8_t * mapped_ = nullptr;
	VkCommandPool pool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;
};

} // namespace wivrn_macos
