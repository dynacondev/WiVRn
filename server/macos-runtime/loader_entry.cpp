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
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_StringToPath(XrInstance, const char *, XrPath *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_PathToString(XrInstance, XrPath, uint32_t, uint32_t *, char *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateActionSet(XrInstance, const XrActionSetCreateInfo *, XrActionSet *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyActionSet(XrActionSet);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateAction(XrActionSet, const XrActionCreateInfo *, XrAction *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyAction(XrAction);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_AttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SyncActions(XrSession, const XrActionsSyncInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateBoundSourcesForAction(XrSession, const XrBoundSourcesForActionEnumerateInfo *, uint32_t, uint32_t *, XrPath *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetActionStateBoolean(XrSession, const XrActionStateGetInfo *, XrActionStateBoolean *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetActionStateFloat(XrSession, const XrActionStateGetInfo *, XrActionStateFloat *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetActionStateVector2f(XrSession, const XrActionStateGetInfo *, XrActionStateVector2f *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetActionStatePose(XrSession, const XrActionStateGetInfo *, XrActionStatePose *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_ApplyHapticFeedback(XrSession, const XrHapticActionInfo *, const XrHapticBaseHeader *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_StopHapticFeedback(XrSession, const XrHapticActionInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateReferenceSpaces(XrSession, uint32_t, uint32_t *, XrReferenceSpaceType *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *, XrSpace *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateActionSpace(XrSession, const XrActionSpaceCreateInfo *, XrSpace *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroySpace(XrSpace);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_LocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_LocateViews(XrSession, const XrViewLocateInfo *, XrViewState *, uint32_t, uint32_t *, XrView *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateSwapchainFormats(XrSession, uint32_t, uint32_t *, int64_t *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateSwapchain(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroySwapchain(XrSwapchain);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateSwapchainImages(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_AcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_WaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_ReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_WaitFrame(XrSession, const XrFrameWaitInfo *, XrFrameState *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_BeginFrame(XrSession, const XrFrameBeginInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EndFrame(XrSession, const XrFrameEndInfo *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanInstanceExtensionsKHR(XrInstance, XrSystemId, uint32_t, uint32_t *, char *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanDeviceExtensionsKHR(XrInstance, XrSystemId, uint32_t, uint32_t *, char *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanGraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanGraphicsRequirements2KHR(XrInstance, XrSystemId, XrGraphicsRequirementsVulkan2KHR *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateVulkanInstanceKHR(XrInstance, const XrVulkanInstanceCreateInfoKHR *, VkInstance *, VkResult *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateVulkanDeviceKHR(XrInstance, const XrVulkanDeviceCreateInfoKHR *, VkDevice *, VkResult *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetVulkanGraphicsDevice2KHR(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR *, VkPhysicalDevice *);
	XRAPI_ATTR const char * XRAPI_CALL wivrn_macos_StructureTypeToString(XrInstance, XrStructureType);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_CreateDebugUtilsMessengerEXT(XrInstance, const XrDebugUtilsMessengerCreateInfoEXT *, XrDebugUtilsMessengerEXT *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyDebugUtilsMessengerEXT(XrDebugUtilsMessengerEXT);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SubmitDebugUtilsMessageEXT(XrInstance, XrDebugUtilsMessageSeverityFlagsEXT, XrDebugUtilsMessageTypeFlagsEXT, const XrDebugUtilsMessengerCallbackDataEXT *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SetDebugUtilsObjectNameEXT(XrInstance, const XrDebugUtilsObjectNameInfoEXT *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SessionBeginDebugUtilsLabelRegionEXT(XrSession, const XrDebugUtilsLabelEXT *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SessionEndDebugUtilsLabelRegionEXT(XrSession);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SessionInsertDebugUtilsLabelEXT(XrSession, const XrDebugUtilsLabelEXT *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType, XrViewConfigurationProperties *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_LocateSpaces(XrSession, const XrSpacesLocateInfo *, XrSpaceLocations *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType, XrExtent2Df *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetCurrentInteractionProfile(XrSession, XrPath, XrInteractionProfileState *);
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetInputSourceLocalizedName(XrSession, const XrInputSourceLocalizedNameGetInfo *, uint32_t, uint32_t *, char *);
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
        ENTRY(StructureTypeToString),
        ENTRY(CreateDebugUtilsMessengerEXT),
        ENTRY(DestroyDebugUtilsMessengerEXT),
        ENTRY(SubmitDebugUtilsMessageEXT),
        ENTRY(SetDebugUtilsObjectNameEXT),
        ENTRY(SessionBeginDebugUtilsLabelRegionEXT),
        ENTRY(SessionEndDebugUtilsLabelRegionEXT),
        ENTRY(SessionInsertDebugUtilsLabelEXT),
        ENTRY(StringToPath),
        ENTRY(PathToString),
        ENTRY(CreateActionSet),
        ENTRY(DestroyActionSet),
        ENTRY(CreateAction),
        ENTRY(DestroyAction),
        ENTRY(SuggestInteractionProfileBindings),
        ENTRY(AttachSessionActionSets),
        ENTRY(SyncActions),
        ENTRY(EnumerateBoundSourcesForAction),
        ENTRY(GetActionStateBoolean),
        ENTRY(GetActionStateFloat),
        ENTRY(GetActionStateVector2f),
        ENTRY(GetActionStatePose),
        ENTRY(ApplyHapticFeedback),
        ENTRY(StopHapticFeedback),
        ENTRY(EnumerateReferenceSpaces),
        ENTRY(CreateReferenceSpace),
        ENTRY(CreateActionSpace),
        ENTRY(DestroySpace),
        ENTRY(LocateSpace),
        ENTRY(LocateViews),
        ENTRY(EnumerateSwapchainFormats),
        ENTRY(CreateSwapchain),
        ENTRY(DestroySwapchain),
        ENTRY(EnumerateSwapchainImages),
        ENTRY(AcquireSwapchainImage),
        ENTRY(WaitSwapchainImage),
        ENTRY(ReleaseSwapchainImage),
        ENTRY(WaitFrame),
        ENTRY(BeginFrame),
        ENTRY(EndFrame),
        ENTRY(GetVulkanInstanceExtensionsKHR),
        ENTRY(GetVulkanDeviceExtensionsKHR),
        ENTRY(GetVulkanGraphicsRequirementsKHR),
        ENTRY(GetVulkanGraphicsRequirements2KHR),
        ENTRY(CreateVulkanInstanceKHR),
        ENTRY(CreateVulkanDeviceKHR),
        ENTRY(GetVulkanGraphicsDevice2KHR),
        ENTRY(GetViewConfigurationProperties),
        ENTRY(LocateSpaces),
        ENTRY(GetReferenceSpaceBoundsRect),
        ENTRY(GetCurrentInteractionProfile),
        ENTRY(GetInputSourceLocalizedName),
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
		// Loud about gaps (apps crash on null pointers, not on errors).
		wivrn_macos::log("GetInstanceProcAddr: unsupported %s", name);
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
