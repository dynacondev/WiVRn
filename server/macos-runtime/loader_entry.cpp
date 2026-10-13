// Loader negotiation + xrGetInstanceProcAddr dispatch table.
// This is the runtime's only dlsym'd symbol: everything else flows
// through the table below (VDXR framework/entry.cpp pattern).

#include "runtime.h"

#include <openxr/openxr_loader_negotiation.h>

#include <cstring>

// Forward declarations of the API entry points (defined per-subsystem).
extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_xrGetInstanceProcAddr(XrInstance, const char *, PFN_xrVoidFunction *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateInstance(const XrInstanceCreateInfo *, XrInstance *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyInstance(XrInstance);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetInstanceProperties(XrInstance, XrInstanceProperties *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetSystem(XrInstance, const XrSystemGetInfo *, XrSystemId *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t, uint32_t *, XrViewConfigurationType *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t *, XrViewConfigurationView *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t *, XrEnvironmentBlendMode *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateSession(XrInstance, const XrSessionCreateInfo *, XrSession *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroySession(XrSession);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_BeginSession(XrSession, const XrSessionBeginInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EndSession(XrSession);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_RequestExitSession(XrSession);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_PollEvent(XrInstance, XrEventDataBuffer *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_ResultToString(XrInstance, XrResult, char[XR_MAX_RESULT_STRING_SIZE]);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateInstanceExtensionProperties(const char *, uint32_t, uint32_t *, XrExtensionProperties *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateApiLayerProperties(uint32_t, uint32_t *, XrApiLayerProperties *);
}

namespace
{

struct FunctionEntry
{
	const char * name;
	PFN_xrVoidFunction function;
	bool global; // callable with instance == XR_NULL_HANDLE
};

#define ENTRY(fn) \
	{         \
	        "xr" #fn, reinterpret_cast<PFN_xrVoidFunction>(wivrn_macos_##fn), false}

const FunctionEntry functions[] = {
        {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(wivrn_macos_xrGetInstanceProcAddr), true},
        {"xrEnumerateInstanceExtensionProperties",
	 reinterpret_cast<PFN_xrVoidFunction>(wivrn_macos_EnumerateInstanceExtensionProperties),
	 true},
        {"xrEnumerateApiLayerProperties",
	 reinterpret_cast<PFN_xrVoidFunction>(wivrn_macos_EnumerateApiLayerProperties),
	 true},
        // xrCreateInstance is the only instance-creating call: per spec it
        // must be retrievable with instance == XR_NULL_HANDLE (the loader
        // has no instance yet). Everything below needs a live instance.
        {"xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction>(wivrn_macos_CreateInstance), true},
        ENTRY(DestroyInstance),
        ENTRY(GetInstanceProperties),
        ENTRY(GetSystem),
        ENTRY(GetSystemProperties),
        ENTRY(EnumerateViewConfigurations),
        ENTRY(EnumerateViewConfigurationViews),
        ENTRY(EnumerateEnvironmentBlendModes),
        ENTRY(CreateSession),
        ENTRY(DestroySession),
        ENTRY(BeginSession),
        ENTRY(EndSession),
        ENTRY(RequestExitSession),
        ENTRY(PollEvent),
        ENTRY(ResultToString),
};

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_xrGetInstanceProcAddr(XrInstance instance, const char * name, PFN_xrVoidFunction * function)
	{
		*function = nullptr;
		if (name == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		for (const FunctionEntry & entry: functions)
		{
			if (std::strcmp(entry.name, name) == 0)
			{
				if (instance == XR_NULL_HANDLE && !entry.global)
					return XR_ERROR_HANDLE_INVALID;
				*function = entry.function;
				return XR_SUCCESS;
			}
		}
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo * loaderInfo, XrNegotiateRuntimeRequest * runtimeRequest)
	{
		if (loaderInfo == nullptr || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
		    loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
		    loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo))
			return XR_ERROR_INITIALIZATION_FAILED;
		if (runtimeRequest == nullptr ||
		    runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
		    runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION ||
		    runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest))
			return XR_ERROR_INITIALIZATION_FAILED;
		if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
		    loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
			return XR_ERROR_INITIALIZATION_FAILED;
		if (XR_VERSION_MAJOR(loaderInfo->minApiVersion) > XR_VERSION_MAJOR(XR_CURRENT_API_VERSION) ||
		    XR_VERSION_MAJOR(loaderInfo->maxApiVersion) < XR_VERSION_MAJOR(XR_CURRENT_API_VERSION))
			return XR_ERROR_INITIALIZATION_FAILED;

		runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
		runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
		runtimeRequest->getInstanceProcAddr = wivrn_macos_xrGetInstanceProcAddr;
		return XR_SUCCESS;
	}

} // extern "C"
