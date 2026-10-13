// System + view configuration queries. v1: exactly one HMD system,
// stereo-only, OPAQUE-only. View extents are build-time defaults until
// Phase 3 feeds the headset info packet (see views.cpp).

#include "runtime.h"

#include <cstring>

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
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetSystem(XrInstance instance, const XrSystemGetInfo * getInfo, XrSystemId * systemId)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || getInfo == nullptr || systemId == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (getInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
			return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
		*systemId = self->system.id;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_GetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties * properties)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || properties == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (systemId != self->system.id)
			return XR_ERROR_SYSTEM_INVALID;
		if (properties->type != XR_TYPE_SYSTEM_PROPERTIES)
			return XR_ERROR_VALIDATION_FAILURE;
		if (properties->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		std::strncpy(properties->systemName, "WiVRn macOS HMD (v1 MVP)", XR_MAX_SYSTEM_NAME_SIZE - 1);
		properties->systemName[XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
		properties->vendorId = 0;
		properties->graphicsProperties.maxSwapchainImageWidth = wivrn_macos::SystemInfo::maxWidth;
		properties->graphicsProperties.maxSwapchainImageHeight = wivrn_macos::SystemInfo::maxHeight;
		properties->graphicsProperties.maxLayerCount = 4;
		properties->trackingProperties.orientationTracking = XR_TRUE;
		properties->trackingProperties.positionTracking = XR_TRUE;
		return XR_SUCCESS;
	}

} // extern "C"
