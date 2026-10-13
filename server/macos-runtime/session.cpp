// Session lifecycle stubs. v1 accepts NO graphics binding yet (Phase 4):
// any chained graphics struct is refused loudly. The IDLE<->SYNCHRONIZED
// transitions below are real; frame-driven states arrive with Phase 5/6.

#include "runtime.h"
#include "streamer.h"

#include <cstdlib>
#include <new>

namespace
{

wivrn_macos::Instance * lookup(XrInstance instance)
{
	if (instance == nullptr)
		return nullptr;
	wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
	std::lock_guard lock(runtime.mutex);
	if (runtime.instance != &instance->data)
		return nullptr;
	return &instance->data;
}

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateSession(XrInstance instance, const XrSessionCreateInfo * createInfo, XrSession * session)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || createInfo == nullptr || session == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->systemId != self->system.id)
			return XR_ERROR_SYSTEM_INVALID;

		XrSession_T * handle = new (std::nothrow) XrSession_T;
		if (handle == nullptr)
			return XR_ERROR_OUT_OF_MEMORY;
		// Graphics bindings (Vulkan only; v1/v2 structs are aliases). A bare
		// session stays valid for headless lifecycle tests.
		if (createInfo->next != nullptr)
		{
			const XrBaseInStructure * chain = reinterpret_cast<const XrBaseInStructure *>(createInfo->next);
			if (chain->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR ||
			    chain->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR)
			{
				auto * binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR *>(chain);
				if (binding->instance == VK_NULL_HANDLE || binding->physicalDevice == VK_NULL_HANDLE ||
				    binding->device == VK_NULL_HANDLE)
				{
					delete handle;
					return XR_ERROR_GRAPHICS_DEVICE_INVALID;
				}
				// The struct carries family + index but no queue handle;
				// resolve it here (single-family MoltenVK in practice).
				VkQueue queue = VK_NULL_HANDLE;
				vkGetDeviceQueue(binding->device, binding->queueFamilyIndex, binding->queueIndex, &queue);
				if (queue == VK_NULL_HANDLE)
				{
					delete handle;
					return XR_ERROR_GRAPHICS_DEVICE_INVALID;
				}
				handle->data.hasGraphicsBinding = true;
				handle->data.vkInstance = binding->instance;
				handle->data.vkPhysicalDevice = binding->physicalDevice;
				handle->data.vkDevice = binding->device;
				handle->data.vkQueue = queue;
				handle->data.vkQueueFamily = binding->queueFamilyIndex;
			}
			else
			{
				// D3D/Metal bindings: refuse loudly rather than creating a
				// session that can never submit.
				wivrn_macos::log("CreateSession: binding type %d unsupported in v1 (Vulkan only)", (int)chain->type);
				delete handle;
				return XR_ERROR_VALIDATION_FAILURE;
			}
		}
		handle->data.instance = self;
		handle->data.state = XR_SESSION_STATE_IDLE;
		{
			wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
			std::lock_guard lock(runtime.mutex);
			self->liveSessions += 1;
		}
		*session = handle;
		// Listen + advertise from session creation (not first frame), so a
		// headset can connect while the app is still starting up. Headless
		// runs opt out via WIVRN_MACOS_HEADLESS (see frame.cpp).
		static bool headless = std::getenv("WIVRN_MACOS_HEADLESS") != nullptr;
		if (!headless)
			wivrn_macos::Streamer::singleton().ensure_started();
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroySession(XrSession session)
	{
		if (session == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = session->data.instance;
		if (self != nullptr)
		{
			wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
			std::lock_guard lock(runtime.mutex);
			if (self->liveSessions > 0)
				self->liveSessions -= 1;
		}
		wivrn_macos::streamer_session_destroyed(&session->data);
		delete session;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_BeginSession(XrSession session, const XrSessionBeginInfo * beginInfo)
	{
		if (session == nullptr || beginInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (session->data.state != XR_SESSION_STATE_IDLE)
			return XR_ERROR_SESSION_NOT_READY;
		session->data.state = XR_SESSION_STATE_SYNCHRONIZED;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EndSession(XrSession session)
	{
		if (session == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (session->data.state != XR_SESSION_STATE_SYNCHRONIZED)
			return XR_ERROR_SESSION_NOT_RUNNING;
		session->data.state = XR_SESSION_STATE_IDLE;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_RequestExitSession(XrSession session)
	{
		if (session == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		session->data.state = XR_SESSION_STATE_EXITING;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_PollEvent(XrInstance instance, XrEventDataBuffer * eventData)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || eventData == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		// v1 posts no events (session-state changes arrive with Phase 5/6).
		(void)self;
		return XR_EVENT_UNAVAILABLE;
	}

} // extern "C"
