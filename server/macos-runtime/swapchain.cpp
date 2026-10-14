// Swapchains: runtime-owned triple-buffered images on the application
// device, real per-image fences (signaled at creation, reset on
// acquire, GPU-signaled on release via an empty queue submit). Array
// size 1-2, no mips/cubemap/multisample/depth in v1 (loud refusals).

#include "runtime.h"
#include "snapshot.h" // queue-submit mutex + worker release handoff (A3)
#include "streamer.h" // release_swapchain barrier (worker may hold images)
#include "vk_context.h"

namespace
{

wivrn_macos::Swapchain * lookup_swapchain(wivrn_macos::Instance * self, XrSwapchain swapchain)
{
	if (self == nullptr || swapchain == nullptr)
		return nullptr;
	for (const auto & owned: self->ownedSwapchains)
	{
		if (owned.get() == swapchain)
			return &owned->data;
	}
	return nullptr;
}

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateSwapchainFormats(
	        XrSession session,
	        uint32_t formatCapacityInput,
	        uint32_t * formatCountOutput,
	        int64_t * formats)
	{
		if (session == nullptr || formatCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		// BGRA first: the v1 x264 path consumes BGRA with zero swizzle
		// (X264_CSP_BGRA); RGBA apps take a CPU swizzle in Phase 5.
		// D32_SFLOAT last: depth for apps that need it (hello_xr takes
		// it over its fallback textures, which never clear on Metal).
		static constexpr int64_t supported[] = {
		        VK_FORMAT_B8G8R8A8_SRGB,
		        VK_FORMAT_R8G8B8A8_SRGB,
		        VK_FORMAT_D32_SFLOAT,
		};
		static constexpr uint32_t count = sizeof(supported) / sizeof(supported[0]);
		if (formatCapacityInput == 0)
		{
			*formatCountOutput = count;
			return XR_SUCCESS;
		}
		if (formats == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (formatCapacityInput < count)
			return XR_ERROR_SIZE_INSUFFICIENT;
		for (uint32_t i = 0; i < count; ++i)
			formats[i] = supported[i];
		*formatCountOutput = count;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo * createInfo, XrSwapchain * swapchain)
	{
		if (session == nullptr || createInfo == nullptr || swapchain == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (!session->data.hasGraphicsBinding)
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		bool knownFormat = createInfo->format == VK_FORMAT_R8G8B8A8_SRGB || createInfo->format == VK_FORMAT_B8G8R8A8_SRGB ||
		                   createInfo->format == VK_FORMAT_D32_SFLOAT;
		if (!knownFormat)
			return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;

		XrResult res = wivrn_macos::validate_swapchain_config(
		        session->data.vkPhysicalDevice,
		        createInfo->format,
		        createInfo->width,
		        createInfo->height,
		        createInfo->arraySize,
		        createInfo->usageFlags,
		        createInfo->sampleCount,
		        createInfo->mipCount,
		        createInfo->faceCount);
		if (res != XR_SUCCESS)
			return res;

		auto handle = std::make_unique<XrSwapchain_T>();
		handle->data.owner = self;
		handle->data.session = &session->data;
		handle->data.format = createInfo->format;
		handle->data.width = createInfo->width;
		handle->data.height = createInfo->height;
		handle->data.arraySize = createInfo->arraySize;
		handle->data.usage = createInfo->usageFlags;
		handle->data.device = session->data.vkDevice;
		handle->data.queue = session->data.vkQueue;
		res = wivrn_macos::create_swapchain_images(handle->data);
		if (res != XR_SUCCESS)
			return res;
		*swapchain = handle.get();
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		self->ownedSwapchains.push_back(std::move(handle));
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroySwapchain(XrSwapchain swapchain)
	{
		if (swapchain == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = swapchain->data.owner;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		for (auto it = self->ownedSwapchains.begin(); it != self->ownedSwapchains.end(); ++it)
		{
			if (it->get() == swapchain)
			{
				// Barrier first: the worker may hold/stage this record's
				// images (async snapshot); free only after it is out.
				wivrn_macos::streamer_release_swapchain(&(*it)->data);
				wivrn_macos::destroy_swapchain_images((*it)->data);
				self->ownedSwapchains.erase(it);
				return XR_SUCCESS;
			}
		}
		return XR_ERROR_HANDLE_INVALID;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateSwapchainImages(
	        XrSwapchain swapchain,
	        uint32_t imageCapacityInput,
	        uint32_t * imageCountOutput,
	        XrSwapchainImageBaseHeader * images)
	{
		if (swapchain == nullptr || imageCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Swapchain * found = nullptr;
		if (swapchain != nullptr)
			found = lookup_swapchain(swapchain->data.owner, swapchain);
		if (found == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Swapchain & swap = *found;
		if (imageCapacityInput == 0)
		{
			*imageCountOutput = (uint32_t)swap.images.size();
			return XR_SUCCESS;
		}
		if (images == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (imageCapacityInput < swap.images.size())
			return XR_ERROR_SIZE_INSUFFICIENT;
		if (images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
		auto * vulkan = reinterpret_cast<XrSwapchainImageVulkan2KHR *>(images);
		for (size_t i = 0; i < swap.images.size(); ++i)
		{
			vulkan[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
			vulkan[i].next = nullptr;
			vulkan[i].image = swap.images[i];
		}
		*imageCountOutput = (uint32_t)swap.images.size();
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_AcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo * acquireInfo, uint32_t * index)
	{
		if (swapchain == nullptr || acquireInfo == nullptr || index == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (acquireInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::Swapchain * found = nullptr;
		if (swapchain != nullptr)
			found = lookup_swapchain(swapchain->data.owner, swapchain);
		if (found == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Swapchain & swap = *found;
		int32_t staged = swap.staged_idx.load(std::memory_order_acquire);
		int32_t active = swap.active_idx.load(std::memory_order_acquire);
		for (size_t step = 0; step < swap.images.size(); ++step)
		{
			uint32_t candidate = (swap.nextIndex + (uint32_t)step) % (uint32_t)swap.images.size();
			if (swap.busy[candidate])
				continue;
			// Never hand out an image the worker staged or is
			// snapshotting (Phase H holds); skipping is the backpressure
			// signal only when every image is thus held (pool of 4
			// covers app + staged + active + free for single-flight apps).
			if ((int32_t)candidate == staged || (int32_t)candidate == active)
				continue;
			swap.busy[candidate] = true;
			swap.waited[candidate] = false;
			swap.outstanding.push_back(candidate);
			swap.lastAcquired = (int32_t)candidate;
			swap.nextIndex = (candidate + 1) % (uint32_t)swap.images.size();
			*index = candidate;
			return XR_SUCCESS;
		}
		wivrn_macos::log("swapchain: all images busy or worker-held on acquire");
		return XR_ERROR_RUNTIME_FAILURE;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_WaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo * waitInfo)
	{
		if (swapchain == nullptr || waitInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (waitInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::Swapchain * found = nullptr;
		if (swapchain != nullptr)
			found = lookup_swapchain(swapchain->data.owner, swapchain);
		if (found == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Swapchain & swap = *found;
		// XR_INFINITE_DURATION waits forever; anything else is a real timeout.
		XrDuration timeout = waitInfo->timeout;
		if (timeout < 0 && timeout != XR_INFINITE_DURATION)
			return XR_ERROR_VALIDATION_FAILURE;
		// The wait API carries no index: it applies to the oldest
		// outstanding acquire (FIFO; single flight in v1). An empty queue is
		// trivially ready. A waited image stays checked out until release.
		if (swap.outstanding.empty())
			return XR_SUCCESS;
		uint32_t target = swap.outstanding.front();
		// No short-circuit on already-waited images: the fence was reset by
		// the first wait, so only a new GPU signal (release's submit) makes
		// a repeated wait succeed. This keeps TIMEOUT honest.
		uint64_t vkTimeout = timeout == XR_INFINITE_DURATION ? UINT64_MAX : static_cast<uint64_t>(timeout);
		VkResult fence = vkWaitForFences(swap.device, 1, &swap.fences[target], VK_TRUE, vkTimeout);
		if (fence == VK_SUCCESS)
		{
			vkResetFences(swap.device, 1, &swap.fences[target]);
			swap.waited[target] = true;
			return XR_SUCCESS;
		}
		if (fence == VK_TIMEOUT)
			return XR_TIMEOUT_EXPIRED;
		wivrn_macos::log("swapchain: fence wait failed (%d)", (int)fence);
		return XR_ERROR_RUNTIME_FAILURE;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_ReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo * releaseInfo)
	{
		if (swapchain == nullptr || releaseInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (releaseInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::Swapchain * found = nullptr;
		if (swapchain != nullptr)
			found = lookup_swapchain(swapchain->data.owner, swapchain);
		if (found == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Swapchain & swap = *found;
		// Release applies to the oldest outstanding acquire (FIFO, mirroring
		// Wait; single flight in v1).
		if (swap.outstanding.empty())
			return XR_ERROR_VALIDATION_FAILURE;
		uint32_t target = swap.outstanding.front();
		swap.outstanding.erase(swap.outstanding.begin());
		// Signal GPU completion of all prior app work with an empty submit:
		// from here the runtime (Phase 5 snapshot) may read the image. Only
		// legal against an unsignaled fence, i.e. one previously waited.
		// The submit holds the shared queue mutex (serialized with the
		// worker's copy-submit; leaf critical section, no GPU wait
		// inside, so the app thread never blocks on the worker).
		bool gpuEdge = false;
		if (swap.waited[target])
		{
			VkSubmitInfo submit{
			        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			        .pNext = nullptr,
			        .waitSemaphoreCount = 0,
			        .pWaitSemaphores = nullptr,
			        .pWaitDstStageMask = nullptr,
			        .commandBufferCount = 0,
			        .pCommandBuffers = nullptr,
			        .signalSemaphoreCount = 0,
			        .pSignalSemaphores = nullptr,
			};
			VkResult submitted = VK_SUCCESS;
			{
				std::lock_guard queueLock(wivrn_macos::snapshot_queue_mutex());
				submitted = vkQueueSubmit(swap.queue, 1, &submit, swap.fences[target]);
			}
			if (submitted != VK_SUCCESS)
			{
				wivrn_macos::log("swapchain: release submit failed");
				return XR_ERROR_RUNTIME_FAILURE;
			}
			gpuEdge = true;
			swap.waited[target] = false;
		}
		// Handoff for the worker-side snapshot (A3): the copy consumes
		// this note and waits the fence (bounded) before reading, so it
		// can never read a mid-render image when the app waited first.
		// Without a prior Wait there is no GPU edge (flagged in the
		// note); the copy then relies on queue order alone.
		wivrn_macos::SnapshotReleaseNote note{};
		note.device = swap.device;
		note.fence = swap.fences[target];
		note.gpu_edge = gpuEdge;
		note.width = swap.width;
		note.height = swap.height;
		note.arraySize = swap.arraySize;
		wivrn_macos::note_snapshot_release(swap.images[target], note);
		swap.busy[target] = false;
		if (swap.lastAcquired == (int32_t)target && swap.outstanding.empty())
			swap.lastAcquired = -1;
		swap.lastReleased = (int32_t)target;
		return XR_SUCCESS;
	}

} // extern "C"
