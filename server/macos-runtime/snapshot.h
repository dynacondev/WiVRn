// Host-visible snapshot of a swapchain image (Phase 5a). Copies one
// eye image to a persistently-mapped BGRA buffer for x264. Contract
// (also exercised by test_encode): the image arrives in
// COLOR_ATTACHMENT_OPTIMAL (standard post-render layout) and is
// returned to it; validation layers confirm with real apps in Phase 6.
//
// Layout contract (A3): the caller must deliver the image in
// VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. The runtime cannot observe
// app-internal transitions without intercepting app command buffers
// (the app's business), so there is nothing to query: a GENERAL-based
// first barrier would still need the true oldLayout, and UNDEFINED
// would discard the pixels. Violation is Vulkan UB (stale tiles on
// MoltenVK). All in-tree producers (demo, stream test, encode test)
// leave COLOR_ATTACHMENT_OPTIMAL, matching the OpenXR Vulkan convention.
//
// GPU-completion contract (A3): the worker-side copy must not read a
// mid-render image. Two mechanisms, in order of strength:
//   1. Release handoff: xrReleaseSwapchainImage records its per-image
//      fence (note_snapshot_release, called from swapchain.cpp, which
//      A3 owns). capture() consumes that note and vkWaitForFences it
//      with a 100 ms bound. When the app followed the acquire/wait/
//      release protocol the release did an empty queue submit, so the
//      fence signals only after all prior app work completes: the wait
//      is then an exact render-completion wait, and in the common case
//      it is already signaled (zero added latency).
//   2. Queue order: the release-empty-submit and the copy-submit share
//      one VkQueue and one mutex (snapshot_queue_mutex). Same-queue
//      submissions execute in submission order, so once both submits
//      are externally synchronized the copy cannot overtake the render.
// Timeout policy: on fence timeout capture LOGS and PROCEEDS (a torn
// frame is recoverable; a wedged worker stalls the stream forever).
// The only wait the app thread ever performs is its own
// vkQueueSubmit under the mutex (microseconds, never a GPU wait), so
// the app loop can never block on the worker.
// Deadlock audit: the queue mutex is a leaf (held only across a single
// vkQueueSubmit, no other lock held inside, no wait inside). The
// release-note map mutex is held only for map insert/take/erase (no
// Vulkan calls inside). No lock is ever held across a fence wait, and
// every wait is bounded (100 ms completion fence, 2 s own fence).

#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace wivrn_macos
{

// Sub-image copy region. layer/offset/extent follow the OpenXR
// XrSwapchainSubImage convention (imageArrayIndex + imageRect); the
// legacy capture() copies full() (layer 0, whole image). Partial
// regions pack tightly at buffer offset 0.
struct SnapshotRegion
{
	uint32_t layer = 0;
	int32_t offsetX = 0;
	int32_t offsetY = 0;
	uint32_t width = 0;  // 0 = full image width
	uint32_t height = 0; // 0 = full image height

	static SnapshotRegion full()
	{
		return SnapshotRegion{};
	}
	bool is_full() const
	{
		return layer == 0 && offsetX == 0 && offsetY == 0 && width == 0 && height == 0;
	}
};

// Process-wide serialization for every vkQueueSubmit the runtime
// itself performs on the shared app queue (release-empty-submit in
// swapchain.cpp, copy-submit here). vkQueueSubmit requires external
// synchronization; app render submits stay the app's business.
std::mutex & snapshot_queue_mutex();

// Release handoff (A3, worker-side only): records the per-image fence
// the release-empty-submit signaled (or the lack of one). Keyed by
// VkImage; latest release wins; consumed once by capture().
struct SnapshotReleaseNote
{
	VkDevice device = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	// True when the release performed the GPU-signaling empty submit
	// (i.e. the app had waited first). False means the app skipped
	// Wait: there is no GPU edge to wait on (fence is stale).
	bool gpu_edge = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t arraySize = 1;
};
void note_snapshot_release(VkImage image, const SnapshotReleaseNote & note);
bool take_snapshot_release(VkImage image, SnapshotReleaseNote & out); // consume-once
void drop_snapshot_release(VkImage image);                            // teardown purge

class EyeSnapshot
{
public:
	EyeSnapshot() = default;
	~EyeSnapshot();

	EyeSnapshot(const EyeSnapshot &) = delete;
	EyeSnapshot & operator=(const EyeSnapshot &) = delete;

	// Allocates the staging buffer + command resources. Device/queue are
	// the application's (the release-submit shares this queue, hence
	// snapshot_queue_mutex; capture itself runs on the worker thread).
	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue, uint32_t queueFamily, uint32_t width, uint32_t height);
	void shutdown();

	// Copy image -> host buffer, return pointer to w*h*4 BGRA bytes.
	// The image must arrive in COLOR_ATTACHMENT_OPTIMAL (see contract
	// above) and is restored to it. Consumes the release note for this
	// image when present (bounded GPU-completion wait); without a note
	// (legacy/tests) queue-ordering alone applies. Returns nullptr on
	// Vulkan failure. Never blocks the app thread (worker-side only).
	const uint8_t * capture(VkImage image);
	// Same, but for an explicit sub-image region (array layer + rect).
	const uint8_t * capture(VkImage image, const SnapshotRegion & region);
	// Same, with an explicit GPU-completion fence (for tests and future
	// staged plumbing): waited with a 100 ms bound before the copy.
	// VK_NULL_HANDLE skips the wait.
	const uint8_t * capture(VkImage image, VkFence completion, VkDevice completionDevice);
	const uint8_t * capture(VkImage image, VkFence completion, VkDevice completionDevice, const SnapshotRegion & region);

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
	const uint8_t * capture_impl(VkImage image, VkFence completion, VkDevice completionDevice, const SnapshotRegion & region, const SnapshotReleaseNote * note);

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
