// Trimmed MoltenVK-safe Vulkan helpers (Phase 4). Deliberately WITHOUT:
// external-memory/fence/semaphore FD imports, video-encode queues,
// DMA-BUF / DRM modifiers, multi-queue encode selection. Single graphics
// queue, runtime-owned images, device-local memory. No Monado here, ever.

#pragma once

#include <openxr/openxr.h>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace wivrn_macos
{

struct Swapchain;

// Validate a requested swapchain configuration against v1 limits +
// MoltenVK format support. Returns XR_SUCCESS or a refusal code.
XrResult validate_swapchain_config(
        VkPhysicalDevice phys,
        int64_t format,
        uint32_t width,
        uint32_t height,
        uint32_t arraySize,
        XrSwapchainUsageFlags usage,
        uint32_t sampleCount,
        uint32_t mipCount,
        uint32_t faceCount);

// Create runtime-owned images + memories + signaled fences for a
// swapchain record (triple-buffered). Cleans up on failure.
XrResult create_swapchain_images(Swapchain & swap);

// Destroy everything create_swapchain_images made (safe on partial
// state, device waits idle first).
void destroy_swapchain_images(Swapchain & swap);

// Monotonic clock, nanoseconds (frame prediction base).
int64_t now_ns();

} // namespace wivrn_macos
