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
		// v1 advertises no extensions. A named layer query finds nothing either.
		(void)layerName;
		(void)properties;
		(void)propertyCapacityInput;
		*propertyCountOutput = 0;
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
		if (createInfo->next != nullptr)
		{
			// v1 supports no instance extensions: any chained struct is either
			// an extension we cannot honor or garbage. Refuse loudly.
			wivrn_macos::log("CreateInstance: chained next structs unsupported in v1");
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (createInfo->enabledExtensionCount > 0)
			return XR_ERROR_EXTENSION_NOT_PRESENT;
		if (createInfo->enabledApiLayerCount > 0)
			return XR_ERROR_VALIDATION_FAILURE;
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

} // extern "C"
