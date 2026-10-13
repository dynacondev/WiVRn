// Instance lifecycle + instance-level queries. v1 supports no instance
// extensions and no API layers; exactly one live instance per process.

#include "runtime.h"

#include <cstring>
#include <new>

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateInstanceExtensionProperties(
	        const char * layerName,
	        uint32_t propertyCapacityInput,
	        uint32_t * propertyCountOutput,
	        XrExtensionProperties * properties)
	{
		if (propertyCountOutput == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		// v1 advertises exactly the two Vulkan extensions (DIY device via
		// enable, runtime-created device via enable2). A named layer query
		// finds nothing.
		(void)layerName;
		struct Advertised
		{
			const char * name;
			uint32_t version;
		};
		static constexpr Advertised advertised[] = {
		        {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION},
		        {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION},
		};
		static constexpr uint32_t count = sizeof(advertised) / sizeof(advertised[0]);
		if (propertyCapacityInput == 0)
		{
			*propertyCountOutput = count;
			return XR_SUCCESS;
		}
		if (properties == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (propertyCapacityInput < count)
			return XR_ERROR_SIZE_INSUFFICIENT;
		for (uint32_t i = 0; i < count; ++i)
		{
			properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
			properties[i].next = nullptr;
			std::strncpy(properties[i].extensionName, advertised[i].name, XR_MAX_EXTENSION_NAME_SIZE - 1);
			properties[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
			properties[i].extensionVersion = advertised[i].version;
		}
		*propertyCountOutput = count;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateApiLayerProperties(
	        uint32_t propertyCapacityInput,
	        uint32_t * propertyCountOutput,
	        XrApiLayerProperties * properties)
	{
		if (propertyCountOutput == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		(void)properties;
		(void)propertyCapacityInput;
		*propertyCountOutput = 0;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateInstance(const XrInstanceCreateInfo * createInfo, XrInstance * instance)
	{
		if (createInfo == nullptr || instance == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::log(
		        "CreateInstance: enter next=%p exts=%u layers=%u api=%u.%u.%u",
		        createInfo->next,
		        createInfo->enabledExtensionCount,
		        createInfo->enabledApiLayerCount,
		        XR_VERSION_MAJOR(createInfo->applicationInfo.apiVersion),
		        XR_VERSION_MINOR(createInfo->applicationInfo.apiVersion),
		        XR_VERSION_PATCH(createInfo->applicationInfo.apiVersion));
		if (createInfo->enabledExtensionCount > 0)
		{
			// v1 honors exactly the advertised Vulkan extensions.
			for (uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i)
			{
				const char * name = createInfo->enabledExtensionNames[i];
				if (std::strcmp(name, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) != 0 &&
				    std::strcmp(name, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) != 0)
				{
					wivrn_macos::log("CreateInstance: extension not present: %s", name != nullptr ? name : "(null)");
					return XR_ERROR_EXTENSION_NOT_PRESENT;
				}
			}
		}
		// API layers are the loader's business (validated before chaining here).
		XrVersion requested = createInfo->applicationInfo.apiVersion;
		if (requested != 0 && (XR_VERSION_MAJOR(requested) != XR_VERSION_MAJOR(XR_CURRENT_API_VERSION) ||
		                       XR_VERSION_MINOR(requested) > XR_VERSION_MINOR(XR_CURRENT_API_VERSION)))
			return XR_ERROR_API_VERSION_UNSUPPORTED;

		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		if (runtime.instance != nullptr)
			return XR_ERROR_LIMIT_REACHED;

		XrInstance_T * handle = new (std::nothrow) XrInstance_T;
		if (handle == nullptr)
			return XR_ERROR_OUT_OF_MEMORY;
		handle->data.appInfo = createInfo->applicationInfo;
		handle->data.apiVersion = requested != 0 ? requested : XR_CURRENT_API_VERSION;
		runtime.instance = &handle->data;
		*instance = handle;
		wivrn_macos::log(
		        "CreateInstance: app='%s' engine='%s' api=%u.%u.%u",
		        createInfo->applicationInfo.applicationName,
		        createInfo->applicationInfo.engineName,
		        XR_VERSION_MAJOR(handle->data.apiVersion),
		        XR_VERSION_MINOR(handle->data.apiVersion),
		        XR_VERSION_PATCH(handle->data.apiVersion));
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyInstance(XrInstance instance)
	{
		if (instance == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		if (runtime.instance == &instance->data)
			runtime.instance = nullptr;
		// v1 shortcut: live sessions die with the instance (apps must not do
		// this; the spec leaves it undefined).
		delete instance;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetInstanceProperties(XrInstance instance, XrInstanceProperties * instanceProperties)
	{
		if (instance == nullptr || instanceProperties == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (instanceProperties->type != XR_TYPE_INSTANCE_PROPERTIES)
			return XR_ERROR_VALIDATION_FAILURE;
		std::strncpy(
		        instanceProperties->runtimeName,
		        "WiVRn-macOS",
		        XR_MAX_RUNTIME_NAME_SIZE - 1);
		instanceProperties->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
		instanceProperties->runtimeVersion = XR_MAKE_VERSION(
		        wivrn_macos::runtime_version_major,
		        wivrn_macos::runtime_version_minor,
		        wivrn_macos::runtime_version_patch);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_ResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
	{
		if (instance == nullptr || buffer == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		const char * text = nullptr;
		switch (value)
		{
			case XR_SUCCESS:
				text = "XR_SUCCESS";
				break;
			case XR_TIMEOUT_EXPIRED:
				text = "XR_TIMEOUT_EXPIRED";
				break;
			case XR_SESSION_LOSS_PENDING:
				text = "XR_SESSION_LOSS_PENDING";
				break;
			case XR_EVENT_UNAVAILABLE:
				text = "XR_EVENT_UNAVAILABLE";
				break;
			case XR_SPACE_BOUNDS_UNAVAILABLE:
				text = "XR_SPACE_BOUNDS_UNAVAILABLE";
				break;
			case XR_SESSION_NOT_FOCUSED:
				text = "XR_SESSION_NOT_FOCUSED";
				break;
			case XR_FRAME_DISCARDED:
				text = "XR_FRAME_DISCARDED";
				break;
			case XR_ERROR_VALIDATION_FAILURE:
				text = "XR_ERROR_VALIDATION_FAILURE";
				break;
			case XR_ERROR_RUNTIME_FAILURE:
				text = "XR_ERROR_RUNTIME_FAILURE";
				break;
			case XR_ERROR_OUT_OF_MEMORY:
				text = "XR_ERROR_OUT_OF_MEMORY";
				break;
			case XR_ERROR_API_VERSION_UNSUPPORTED:
				text = "XR_ERROR_API_VERSION_UNSUPPORTED";
				break;
			case XR_ERROR_INITIALIZATION_FAILED:
				text = "XR_ERROR_INITIALIZATION_FAILED";
				break;
			case XR_ERROR_FUNCTION_UNSUPPORTED:
				text = "XR_ERROR_FUNCTION_UNSUPPORTED";
				break;
			case XR_ERROR_FEATURE_UNSUPPORTED:
				text = "XR_ERROR_FEATURE_UNSUPPORTED";
				break;
			case XR_ERROR_EXTENSION_NOT_PRESENT:
				text = "XR_ERROR_EXTENSION_NOT_PRESENT";
				break;
			case XR_ERROR_LIMIT_REACHED:
				text = "XR_ERROR_LIMIT_REACHED";
				break;
			case XR_ERROR_SIZE_INSUFFICIENT:
				text = "XR_ERROR_SIZE_INSUFFICIENT";
				break;
			case XR_ERROR_HANDLE_INVALID:
				text = "XR_ERROR_HANDLE_INVALID";
				break;
			case XR_ERROR_INSTANCE_LOST:
				text = "XR_ERROR_INSTANCE_LOST";
				break;
			case XR_ERROR_FORM_FACTOR_UNSUPPORTED:
				text = "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
				break;
			case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
				text = "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
				break;
			case XR_ERROR_API_LAYER_NOT_PRESENT:
				text = "XR_ERROR_API_LAYER_NOT_PRESENT";
				break;
			case XR_ERROR_CALL_ORDER_INVALID:
				text = "XR_ERROR_CALL_ORDER_INVALID";
				break;
			case XR_ERROR_GRAPHICS_DEVICE_INVALID:
				text = "XR_ERROR_GRAPHICS_DEVICE_INVALID";
				break;
			case XR_ERROR_POSE_INVALID:
				text = "XR_ERROR_POSE_INVALID";
				break;
			case XR_ERROR_INDEX_OUT_OF_RANGE:
				text = "XR_ERROR_INDEX_OUT_OF_RANGE";
				break;
			case XR_ERROR_PATH_UNSUPPORTED:
				text = "XR_ERROR_PATH_UNSUPPORTED";
				break;
			case XR_ERROR_PATH_INVALID:
				text = "XR_ERROR_PATH_INVALID";
				break;
			case XR_ERROR_SYSTEM_INVALID:
				text = "XR_ERROR_SYSTEM_INVALID";
				break;
			case XR_ERROR_SESSION_RUNNING:
				text = "XR_ERROR_SESSION_RUNNING";
				break;
			case XR_ERROR_SESSION_NOT_RUNNING:
				text = "XR_ERROR_SESSION_NOT_RUNNING";
				break;
			case XR_ERROR_SESSION_LOST:
				text = "XR_ERROR_SESSION_LOST";
				break;
			case XR_ERROR_TIME_INVALID:
				text = "XR_ERROR_TIME_INVALID";
				break;
			case XR_ERROR_SESSION_NOT_READY:
				text = "XR_ERROR_SESSION_NOT_READY";
				break;
			default:
				break;
		}
		if (text == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		std::strncpy(buffer, text, XR_MAX_RESULT_STRING_SIZE - 1);
		buffer[XR_MAX_RESULT_STRING_SIZE - 1] = '\0';
		return XR_SUCCESS;
	}

	XRAPI_ATTR const char * XRAPI_CALL wivrn_macos_StructureTypeToString(XrInstance instance, XrStructureType type)
	{
		(void)instance;
		// Never returns null (loader contract); unknown types get a stable
		// placeholder rather than a crash.
		switch (type)
		{
			case XR_TYPE_UNKNOWN:
				return "XR_TYPE_UNKNOWN";
			case XR_TYPE_API_LAYER_PROPERTIES:
				return "XR_TYPE_API_LAYER_PROPERTIES";
			case XR_TYPE_EXTENSION_PROPERTIES:
				return "XR_TYPE_EXTENSION_PROPERTIES";
			case XR_TYPE_INSTANCE_CREATE_INFO:
				return "XR_TYPE_INSTANCE_CREATE_INFO";
			case XR_TYPE_SYSTEM_GET_INFO:
				return "XR_TYPE_SYSTEM_GET_INFO";
			case XR_TYPE_SYSTEM_PROPERTIES:
				return "XR_TYPE_SYSTEM_PROPERTIES";
			case XR_TYPE_VIEW_LOCATE_INFO:
				return "XR_TYPE_VIEW_LOCATE_INFO";
			case XR_TYPE_VIEW:
				return "XR_TYPE_VIEW";
			case XR_TYPE_SESSION_CREATE_INFO:
				return "XR_TYPE_SESSION_CREATE_INFO";
			case XR_TYPE_SWAPCHAIN_CREATE_INFO:
				return "XR_TYPE_SWAPCHAIN_CREATE_INFO";
			case XR_TYPE_SESSION_BEGIN_INFO:
				return "XR_TYPE_SESSION_BEGIN_INFO";
			case XR_TYPE_VIEW_STATE:
				return "XR_TYPE_VIEW_STATE";
			case XR_TYPE_FRAME_END_INFO:
				return "XR_TYPE_FRAME_END_INFO";
			case XR_TYPE_FRAME_WAIT_INFO:
				return "XR_TYPE_FRAME_WAIT_INFO";
			case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
				return "XR_TYPE_COMPOSITION_LAYER_PROJECTION";
			case XR_TYPE_COMPOSITION_LAYER_QUAD:
				return "XR_TYPE_COMPOSITION_LAYER_QUAD";
			case XR_TYPE_REFERENCE_SPACE_CREATE_INFO:
				return "XR_TYPE_REFERENCE_SPACE_CREATE_INFO";
			case XR_TYPE_ACTION_SPACE_CREATE_INFO:
				return "XR_TYPE_ACTION_SPACE_CREATE_INFO";
			case XR_TYPE_ACTION_STATE_GET_INFO:
				return "XR_TYPE_ACTION_STATE_GET_INFO";
			case XR_TYPE_ACTION_SET_CREATE_INFO:
				return "XR_TYPE_ACTION_SET_CREATE_INFO";
			case XR_TYPE_ACTION_CREATE_INFO:
				return "XR_TYPE_ACTION_CREATE_INFO";
			case XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO:
				return "XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO";
			case XR_TYPE_ACTIONS_SYNC_INFO:
				return "XR_TYPE_ACTIONS_SYNC_INFO";
			case XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO:
				return "XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO";
			case XR_TYPE_SPACES_LOCATE_INFO:
				return "XR_TYPE_SPACES_LOCATE_INFO";
			case XR_TYPE_SPACE_LOCATIONS:
				return "XR_TYPE_SPACE_LOCATIONS";
			case XR_TYPE_VIEW_CONFIGURATION_VIEW:
				return "XR_TYPE_VIEW_CONFIGURATION_VIEW";
			case XR_TYPE_VIEW_CONFIGURATION_PROPERTIES:
				return "XR_TYPE_VIEW_CONFIGURATION_PROPERTIES";
			case XR_TYPE_INSTANCE_PROPERTIES:
				return "XR_TYPE_INSTANCE_PROPERTIES";
			case XR_TYPE_FRAME_BEGIN_INFO:
				return "XR_TYPE_FRAME_BEGIN_INFO";
			case XR_TYPE_FRAME_STATE:
				return "XR_TYPE_FRAME_STATE";
			case XR_TYPE_EVENT_DATA_BUFFER:
				return "XR_TYPE_EVENT_DATA_BUFFER";
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				return "XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED";
			case XR_TYPE_ACTION_STATE_BOOLEAN:
				return "XR_TYPE_ACTION_STATE_BOOLEAN";
			case XR_TYPE_ACTION_STATE_FLOAT:
				return "XR_TYPE_ACTION_STATE_FLOAT";
			case XR_TYPE_ACTION_STATE_VECTOR2F:
				return "XR_TYPE_ACTION_STATE_VECTOR2F";
			case XR_TYPE_ACTION_STATE_POSE:
				return "XR_TYPE_ACTION_STATE_POSE";
			case XR_TYPE_HAPTIC_ACTION_INFO:
				return "XR_TYPE_HAPTIC_ACTION_INFO";
			case XR_TYPE_SPACE_LOCATION:
				return "XR_TYPE_SPACE_LOCATION";
			case XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO:
				return "XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO";
			case XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO:
				return "XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO";
			case XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO:
				return "XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO";
			case XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING:
				return "XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING";
			case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR:
				return "XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR";
			case XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR:
				return "XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR";
			case XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR:
				return "XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR";
			case XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR:
				return "XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR";
			case XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR:
				return "XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR";
			default:
				break;
		}
		return "UnknownStructureType";
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateDebugUtilsMessengerEXT(
	        XrInstance instance,
	        const XrDebugUtilsMessengerCreateInfoEXT * createInfo,
	        XrDebugUtilsMessengerEXT * messenger)
	{
		(void)instance;
		(void)createInfo;
		(void)messenger;
		// v1 ships no debug messenger (validation runs in-process under the
		// test binaries instead). A loud error, never a null handle.
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT messenger)
	{
		(void)messenger;
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SubmitDebugUtilsMessageEXT(
	        XrInstance instance,
	        XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
	        XrDebugUtilsMessageTypeFlagsEXT messageTypes,
	        const XrDebugUtilsMessengerCallbackDataEXT * callbackData)
	{
		(void)instance;
		(void)messageSeverity;
		(void)messageTypes;
		(void)callbackData;
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SetDebugUtilsObjectNameEXT(XrInstance instance, const XrDebugUtilsObjectNameInfoEXT * nameInfo)
	{
		(void)instance;
		(void)nameInfo;
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SessionBeginDebugUtilsLabelRegionEXT(XrSession session, const XrDebugUtilsLabelEXT * labelInfo)
	{
		(void)session;
		(void)labelInfo;
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SessionEndDebugUtilsLabelRegionEXT(XrSession session)
	{
		(void)session;
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SessionInsertDebugUtilsLabelEXT(XrSession session, const XrDebugUtilsLabelEXT * labelInfo)
	{
		(void)session;
		(void)labelInfo;
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}

} // extern "C"
