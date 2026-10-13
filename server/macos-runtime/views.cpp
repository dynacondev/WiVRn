// View configuration + environment blend modes. v1: STEREO with fixed
// per-eye extents (2064x2208 defaults); Phase 3 replaces these with the
// headset info packet values. OPAQUE only (no passthrough in v1).

#include "runtime.h"

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

XrResult check_system(wivrn_macos::Instance * self, XrSystemId systemId)
{
	if (self == nullptr)
		return XR_ERROR_HANDLE_INVALID;
	if (systemId != self->system.id)
		return XR_ERROR_SYSTEM_INVALID;
	return XR_SUCCESS;
}

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateViewConfigurations(
	        XrInstance instance,
	        XrSystemId systemId,
	        uint32_t viewConfigurationTypeCapacityInput,
	        uint32_t * viewConfigurationTypeCountOutput,
	        XrViewConfigurationType * viewConfigurationTypes)
	{
		wivrn_macos::Instance * self = lookup(instance);
		XrResult res = check_system(self, systemId);
		if (res != XR_SUCCESS)
			return res;
		if (viewConfigurationTypeCountOutput == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (viewConfigurationTypeCapacityInput == 0)
		{
			*viewConfigurationTypeCountOutput = 1;
			return XR_SUCCESS;
		}
		if (viewConfigurationTypes == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (viewConfigurationTypeCapacityInput < 1)
			return XR_ERROR_SIZE_INSUFFICIENT;
		viewConfigurationTypes[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		*viewConfigurationTypeCountOutput = 1;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateViewConfigurationViews(
	        XrInstance instance,
	        XrSystemId systemId,
	        XrViewConfigurationType viewConfigurationType,
	        uint32_t viewCapacityInput,
	        uint32_t * viewCountOutput,
	        XrViewConfigurationView * views)
	{
		wivrn_macos::Instance * self = lookup(instance);
		XrResult res = check_system(self, systemId);
		if (res != XR_SUCCESS)
			return res;
		if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		if (viewCountOutput == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (viewCapacityInput == 0)
		{
			*viewCountOutput = wivrn_macos::SystemInfo::viewCount;
			return XR_SUCCESS;
		}
		if (views == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (viewCapacityInput < wivrn_macos::SystemInfo::viewCount)
			return XR_ERROR_SIZE_INSUFFICIENT;
		for (uint32_t i = 0; i < wivrn_macos::SystemInfo::viewCount; ++i)
		{
			views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
			views[i].next = nullptr;
			views[i].recommendedImageRectWidth = wivrn_macos::SystemInfo::recommendedWidth;
			views[i].recommendedImageRectHeight = wivrn_macos::SystemInfo::recommendedHeight;
			views[i].maxImageRectWidth = wivrn_macos::SystemInfo::maxWidth;
			views[i].maxImageRectHeight = wivrn_macos::SystemInfo::maxHeight;
			views[i].recommendedSwapchainSampleCount = 1;
			views[i].maxSwapchainSampleCount = 1;
		}
		*viewCountOutput = wivrn_macos::SystemInfo::viewCount;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateEnvironmentBlendModes(
	        XrInstance instance,
	        XrSystemId systemId,
	        XrViewConfigurationType viewConfigurationType,
	        uint32_t environmentBlendModeCapacityInput,
	        uint32_t * environmentBlendModeCountOutput,
	        XrEnvironmentBlendMode * environmentBlendModes)
	{
		wivrn_macos::Instance * self = lookup(instance);
		XrResult res = check_system(self, systemId);
		if (res != XR_SUCCESS)
			return res;
		if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		if (environmentBlendModeCountOutput == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (environmentBlendModeCapacityInput == 0)
		{
			*environmentBlendModeCountOutput = 1;
			return XR_SUCCESS;
		}
		if (environmentBlendModes == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (environmentBlendModeCapacityInput < 1)
			return XR_ERROR_SIZE_INSUFFICIENT;
		environmentBlendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		*environmentBlendModeCountOutput = 1;
		return XR_SUCCESS;
	}

} // extern "C"
