// Phase 2 proof: drive the REAL Khronos loader (openxr_loader target)
// against our manifest and exercise negotiate -> CreateInstance ->
// GetSystem -> views -> session stubs. Run with:
//   XR_RUNTIME_JSON=<build>/server/macos-runtime/wivrn-macos.json \
//     ./wivrn-macos-runtime-test
// Note: the loader reads XR_RUNTIME_JSON itself; this file never dlopens
// the runtime directly.

// Test-only: needed for the (rejected) Vulkan binding struct below. The
// runtime itself stays graphics-agnostic until Phase 4.
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                 \
	do                                                                          \
	{                                                                           \
		if (!(cond))                                                        \
		{                                                                   \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                 \
		}                                                                   \
	} while (false)

#define CHECK_XR(call, expected)                                       \
	do                                                             \
	{                                                              \
		XrResult res__ = (call);                               \
		if (res__ != (expected))                               \
		{                                                      \
			std::printf(                                   \
			        "FAIL %s:%d: %s -> %d, expected %d\n", \
			        __FILE__,                              \
			        __LINE__,                              \
			        #call,                                 \
			        (int)res__,                            \
			        (int)(expected));                      \
			++failures;                                    \
		}                                                      \
	} while (false)

int main()
{
	const char * manifest = std::getenv("XR_RUNTIME_JSON");
	std::printf("XR_RUNTIME_JSON=%s\n", manifest ? manifest : "(unset)");
	if (manifest == nullptr)
	{
		std::puts("set XR_RUNTIME_JSON to the generated wivrn-macos.json");
		return 2;
	}

	// Loader resolves our manifest here (negotiate happens inside).
	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo createInfo{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .next = nullptr,
	        .createFlags = 0,
	        .applicationInfo =
	                {
	                        .applicationName = "wivrn-macos-runtime-test",
	                        .applicationVersion = 1,
	                        .engineName = "none",
	                        .engineVersion = 0,
	                        .apiVersion = XR_CURRENT_API_VERSION,
	                },
	        .enabledApiLayerCount = 0,
	        .enabledApiLayerNames = nullptr,
	        .enabledExtensionCount = 0,
	        .enabledExtensionNames = nullptr,
	};
	// Negative first (no live instance yet): unknown extensions are
	// refused loudly. (The loader itself rejects simultaneous
	// instances, so all negative CreateInstance checks run solo.)
	const char * bogus[] = {"XR_BOGUS_nope"};
	XrInstanceCreateInfo badExt = createInfo;
	badExt.enabledExtensionCount = 1;
	badExt.enabledExtensionNames = bogus;
	XrInstance bad = XR_NULL_HANDLE;
	CHECK_XR(xrCreateInstance(&badExt, &bad), XR_ERROR_EXTENSION_NOT_PRESENT);
	CHECK(bad == XR_NULL_HANDLE);

	XrInstanceCreateInfo badVersion = createInfo;
	badVersion.applicationInfo.apiVersion = XR_MAKE_VERSION(2, 0, 0);
	XrInstance badV = XR_NULL_HANDLE;
	CHECK_XR(xrCreateInstance(&badVersion, &badV), XR_ERROR_API_VERSION_UNSUPPORTED);

	CHECK_XR(xrCreateInstance(&createInfo, &instance), XR_SUCCESS);
	CHECK(instance != XR_NULL_HANDLE);

	XrInstanceProperties props{.type = XR_TYPE_INSTANCE_PROPERTIES, .next = nullptr};
	CHECK_XR(xrGetInstanceProperties(instance, &props), XR_SUCCESS);
	std::printf("runtime: %s version %u.%u.%u\n", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
	CHECK(std::strcmp(props.runtimeName, "WiVRn-macOS") == 0);

	XrSystemGetInfo sysInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .next = nullptr, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	CHECK_XR(xrGetSystem(instance, &sysInfo, &systemId), XR_SUCCESS);
	CHECK(systemId != XR_NULL_SYSTEM_ID);

	XrSystemGetInfo badForm = sysInfo;
	badForm.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
	XrSystemId badSystem = XR_NULL_SYSTEM_ID;
	CHECK_XR(xrGetSystem(instance, &badForm, &badSystem), XR_ERROR_FORM_FACTOR_UNSUPPORTED);

	XrSystemProperties sysProps{.type = XR_TYPE_SYSTEM_PROPERTIES, .next = nullptr};
	CHECK_XR(xrGetSystemProperties(instance, systemId, &sysProps), XR_SUCCESS);
	std::printf("system: %s\n", sysProps.systemName);
	CHECK(sysProps.trackingProperties.orientationTracking == XR_TRUE);
	CHECK(sysProps.trackingProperties.positionTracking == XR_TRUE);

	uint32_t configCount = 0;
	CHECK_XR(xrEnumerateViewConfigurations(instance, systemId, 0, &configCount, nullptr), XR_SUCCESS);
	CHECK(configCount == 1);
	XrViewConfigurationType config = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
	CHECK_XR(xrEnumerateViewConfigurations(instance, systemId, 1, &configCount, &config), XR_SUCCESS);
	CHECK(config == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

	uint32_t viewCount = 0;
	CHECK_XR(xrEnumerateViewConfigurationViews(instance, systemId, config, 0, &viewCount, nullptr), XR_SUCCESS);
	CHECK(viewCount == 2);
	XrViewConfigurationView views[2] = {};
	CHECK_XR(xrEnumerateViewConfigurationViews(instance, systemId, config, 2, &viewCount, views), XR_SUCCESS);
	CHECK(viewCount == 2);
	for (uint32_t i = 0; i < viewCount; ++i)
	{
		std::printf(
		        "view %u: recommended %ux%u max %ux%u\n",
		        i,
		        views[i].recommendedImageRectWidth,
		        views[i].recommendedImageRectHeight,
		        views[i].maxImageRectWidth,
		        views[i].maxImageRectHeight);
		CHECK(views[i].recommendedImageRectWidth == 2064);
		CHECK(views[i].recommendedImageRectHeight == 2208);
	}

	uint32_t blendCount = 0;
	CHECK_XR(xrEnumerateEnvironmentBlendModes(instance, systemId, config, 0, &blendCount, nullptr), XR_SUCCESS);
	CHECK(blendCount == 1);
	XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM;
	CHECK_XR(xrEnumerateEnvironmentBlendModes(instance, systemId, config, 1, &blendCount, &blend), XR_SUCCESS);
	CHECK(blend == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);

	// Session without a graphics binding: accepted (headless-style v1
	// stub); the IDLE<->SYNCHRONIZED transitions below are real.
	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo sessionInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = nullptr, .createFlags = 0, .systemId = systemId};
	CHECK_XR(xrCreateSession(instance, &sessionInfo, &session), XR_SUCCESS);
	CHECK(session != XR_NULL_HANDLE);

	XrEventDataBuffer event{.type = XR_TYPE_EVENT_DATA_BUFFER, .next = nullptr};
	CHECK_XR(xrPollEvent(instance, &event), XR_EVENT_UNAVAILABLE);

	XrSessionBeginInfo beginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO, .next = nullptr, .primaryViewConfigurationType = config};
	CHECK_XR(xrBeginSession(session, &beginInfo), XR_SUCCESS);
	CHECK_XR(xrBeginSession(session, &beginInfo), XR_ERROR_SESSION_NOT_READY);

	// ---- Phase 3: paths, actions, spaces (synthetic tracking) ----
	XrPath leftHand = XR_NULL_PATH, rightHand = XR_NULL_PATH;
	CHECK_XR(xrStringToPath(instance, "/user/hand/left", &leftHand), XR_SUCCESS);
	CHECK_XR(xrStringToPath(instance, "/user/hand/right", &rightHand), XR_SUCCESS);
	CHECK(leftHand != rightHand);
	char pathBuf[64];
	uint32_t pathLen = 0;
	CHECK_XR(xrPathToString(instance, leftHand, sizeof(pathBuf), &pathLen, pathBuf), XR_SUCCESS);
	CHECK(std::strcmp(pathBuf, "/user/hand/left") == 0);

	auto make_action = [&](XrActionSet set, const char * name, XrActionType type, XrAction & out) {
		XrPath subs[2] = {leftHand, rightHand};
		XrActionCreateInfo info{
		        .type = XR_TYPE_ACTION_CREATE_INFO,
		        .next = nullptr,
		        .actionName = {},
		        .actionType = type,
		        .countSubactionPaths = 2,
		        .subactionPaths = subs,
		        .localizedActionName = {},
		};
		std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		CHECK_XR(xrCreateAction(set, &info, &out), XR_SUCCESS);
	};

	XrActionSet gameplay = XR_NULL_HANDLE;
	XrActionSetCreateInfo setInfo{
	        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
	        .next = nullptr,
	        .actionSetName = {},
	        .localizedActionSetName = {},
	};
	std::strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
	std::strncpy(setInfo.localizedActionSetName, "gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
	CHECK_XR(xrCreateActionSet(instance, &setInfo, &gameplay), XR_SUCCESS);

	XrAction select = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, stick = XR_NULL_HANDLE, gripPose = XR_NULL_HANDLE;
	make_action(gameplay, "select", XR_ACTION_TYPE_BOOLEAN_INPUT, select);
	make_action(gameplay, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, trigger);
	make_action(gameplay, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT, stick);
	make_action(gameplay, "grip", XR_ACTION_TYPE_POSE_INPUT, gripPose);

	auto suggest = [&](const char * profile, std::initializer_list<std::pair<XrAction, const char *>> entries) {
		XrPath profilePath = XR_NULL_PATH;
		CHECK_XR(xrStringToPath(instance, profile, &profilePath), XR_SUCCESS);
		std::vector<XrActionSuggestedBinding> flat;
		std::vector<std::string> held;
		for (const auto & [action, component]: entries)
		{
			XrPath componentPath = XR_NULL_PATH;
			CHECK_XR(xrStringToPath(instance, component, &componentPath), XR_SUCCESS);
			flat.push_back({action, componentPath});
		}
		XrInteractionProfileSuggestedBinding suggested{
		        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
		        .next = nullptr,
		        .interactionProfile = profilePath,
		        .countSuggestedBindings = (uint32_t)flat.size(),
		        .suggestedBindings = flat.data(),
		};
		CHECK_XR(xrSuggestInteractionProfileBindings(instance, &suggested), XR_SUCCESS);
	};
	// One suggest call per profile (spec: most-recent-wins per profile).
	suggest(
	        "/interaction_profiles/khr/simple_controller",
	        {
	                {select, "/user/hand/right/input/select/click"},
	                {trigger, "/user/hand/right/input/trigger/value"},
	                {stick, "/user/hand/right/input/thumbstick"},
	                {gripPose, "/user/hand/right/input/grip/pose"},
	        });
	// Vive/index aliases resolve through the shared component names.
	suggest("/interaction_profiles/htc/vive_controller", {{select, "/user/hand/right/input/trigger/click"}});

	XrActionSet sets[1] = {gameplay};
	XrSessionActionSetsAttachInfo attachInfo{
	        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
	        .next = nullptr,
	        .countActionSets = 1,
	        .actionSets = sets,
	};
	CHECK_XR(xrAttachSessionActionSets(session, &attachInfo), XR_SUCCESS);
	CHECK_XR(xrAttachSessionActionSets(session, &attachInfo), XR_ERROR_ACTIONSETS_ALREADY_ATTACHED);

	XrActiveActionSet active{gameplay, XR_NULL_PATH};
	XrActionsSyncInfo syncInfo{.type = XR_TYPE_ACTIONS_SYNC_INFO, .next = nullptr, .countActiveActionSets = 1, .activeActionSets = &active};
	CHECK_XR(xrSyncActions(session, &syncInfo), XR_SUCCESS);

	// Right hand carries the pressed demo state (see tracking.cpp).
	XrActionStateGetInfo getRight{.type = XR_TYPE_ACTION_STATE_GET_INFO, .next = nullptr, .action = XR_NULL_HANDLE, .subactionPath = rightHand};
	XrActionStateGetInfo getLeft{.type = XR_TYPE_ACTION_STATE_GET_INFO, .next = nullptr, .action = XR_NULL_HANDLE, .subactionPath = leftHand};

	getRight.action = select;
	XrActionStateBoolean selectState{.type = XR_TYPE_ACTION_STATE_BOOLEAN, .next = nullptr};
	CHECK_XR(xrGetActionStateBoolean(session, &getRight, &selectState), XR_SUCCESS);
	CHECK(selectState.isActive);
	CHECK(selectState.currentState);

	getLeft.action = select;
	XrActionStateBoolean selectLeft{.type = XR_TYPE_ACTION_STATE_BOOLEAN, .next = nullptr};
	CHECK_XR(xrGetActionStateBoolean(session, &getLeft, &selectLeft), XR_SUCCESS);
	CHECK(selectLeft.isActive);
	CHECK(!selectLeft.currentState);

	getRight.action = trigger;
	XrActionStateFloat triggerState{.type = XR_TYPE_ACTION_STATE_FLOAT, .next = nullptr};
	CHECK_XR(xrGetActionStateFloat(session, &getRight, &triggerState), XR_SUCCESS);
	CHECK(triggerState.isActive);
	CHECK(triggerState.currentState > 0.4f && triggerState.currentState < 0.6f);

	getRight.action = stick;
	XrActionStateVector2f stickState{.type = XR_TYPE_ACTION_STATE_VECTOR2F, .next = nullptr};
	CHECK_XR(xrGetActionStateVector2f(session, &getRight, &stickState), XR_SUCCESS);
	CHECK(stickState.isActive);
	CHECK(stickState.currentState.x > 0.2f && stickState.currentState.y < -0.4f);

	getRight.action = gripPose;
	XrActionStatePose poseState{.type = XR_TYPE_ACTION_STATE_POSE, .next = nullptr};
	CHECK_XR(xrGetActionStatePose(session, &getRight, &poseState), XR_SUCCESS);
	CHECK(poseState.isActive);

	uint32_t sourceCount = 0;
	XrBoundSourcesForActionEnumerateInfo boundInfo{
	        .type = XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, .next = nullptr, .action = select};
	CHECK_XR(xrEnumerateBoundSourcesForAction(session, &boundInfo, 0, &sourceCount, nullptr), XR_SUCCESS);
	CHECK(sourceCount >= 1);

	// Spaces: LOCAL self-locate is identity; grip space tracks the
	// synthetic right controller. LOCAL origin sits at head height
	// (0, 1.6, 0), so the stage-space controller at (0.2, 1.3, -0.3)
	// locates at (0.2, -0.3, -0.3) in LOCAL.
	XrReferenceSpaceCreateInfo localInfo{
	        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	        .next = nullptr,
	        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	XrSpace local = XR_NULL_HANDLE;
	CHECK_XR(xrCreateReferenceSpace(session, &localInfo, &local), XR_SUCCESS);

	XrSpaceLocation selfLoc{.type = XR_TYPE_SPACE_LOCATION, .next = nullptr};
	CHECK_XR(xrLocateSpace(local, local, 1, &selfLoc), XR_SUCCESS);
	CHECK((selfLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
	CHECK(selfLoc.pose.position.x > -0.01f && selfLoc.pose.position.x < 0.01f);
	CHECK_XR(xrLocateSpace(local, local, 0, &selfLoc), XR_ERROR_TIME_INVALID);

	XrActionSpaceCreateInfo gripSpaceInfo{
	        .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
	        .next = nullptr,
	        .action = gripPose,
	        .subactionPath = rightHand,
	        .poseInActionSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	XrSpace grip = XR_NULL_HANDLE;
	CHECK_XR(xrCreateActionSpace(session, &gripSpaceInfo, &grip), XR_SUCCESS);

	XrSpaceLocation gripLoc{.type = XR_TYPE_SPACE_LOCATION, .next = nullptr};
	CHECK_XR(xrLocateSpace(grip, local, 1, &gripLoc), XR_SUCCESS);
	CHECK((gripLoc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0);
	std::printf(
	        "grip in local: (%.3f, %.3f, %.3f)\n",
	        gripLoc.pose.position.x,
	        gripLoc.pose.position.y,
	        gripLoc.pose.position.z);
	CHECK(gripLoc.pose.position.x > 0.19f && gripLoc.pose.position.x < 0.21f);
	CHECK(gripLoc.pose.position.y > -0.31f && gripLoc.pose.position.y < -0.29f);
	CHECK(gripLoc.pose.position.z > -0.31f && gripLoc.pose.position.z < -0.29f);

	// Views: stereo pair around the synthetic HMD. In LOCAL coords the
	// eyes sit near y=0 (head height cancels), split ±32 mm in x.

	// Views: stereo pair around the synthetic HMD. In LOCAL coords the
	// eyes sit near y=0 (head height cancels), split ±32 mm in x.
	XrViewLocateInfo viewInfo{
	        .type = XR_TYPE_VIEW_LOCATE_INFO,
	        .next = nullptr,
	        .viewConfigurationType = config,
	        .displayTime = 1,
	        .space = local,
	};
	XrViewState viewState{.type = XR_TYPE_VIEW_STATE, .next = nullptr};
	uint32_t locatedCount = 0;
	XrView located[2] = {};
	CHECK_XR(xrLocateViews(session, &viewInfo, &viewState, 2, &locatedCount, located), XR_SUCCESS);
	CHECK(locatedCount == 2);
	CHECK((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0);
	for (uint32_t i = 0; i < 2; ++i)
	{
		std::printf(
		        "view %u: pos (%.3f, %.3f, %.3f)\n",
		        i,
		        located[i].pose.position.x,
		        located[i].pose.position.y,
		        located[i].pose.position.z);
		CHECK(located[i].pose.position.y > -0.05f && located[i].pose.position.y < 0.05f);
		CHECK(located[i].fov.angleLeft > 0.8f && located[i].fov.angleLeft < 0.9f);
	}
	CHECK(located[0].pose.position.x < -0.01f);
	CHECK(located[1].pose.position.x > 0.01f);

	CHECK_XR(xrDestroySpace(grip), XR_SUCCESS);
	CHECK_XR(xrDestroySpace(local), XR_SUCCESS);
	CHECK_XR(xrDestroyAction(gripPose), XR_SUCCESS);
	CHECK_XR(xrDestroyAction(stick), XR_SUCCESS);
	CHECK_XR(xrDestroyAction(trigger), XR_SUCCESS);
	CHECK_XR(xrDestroyAction(select), XR_SUCCESS);
	CHECK_XR(xrDestroyActionSet(gameplay), XR_SUCCESS);

	CHECK_XR(xrEndSession(session), XR_SUCCESS);
	CHECK_XR(xrEndSession(session), XR_ERROR_SESSION_NOT_RUNNING);
	CHECK_XR(xrRequestExitSession(session), XR_SUCCESS);
	CHECK_XR(xrDestroySession(session), XR_SUCCESS);

	// Session WITH a null Vulkan binding struct: GRAPHICS_DEVICE_INVALID
	// (empty handles; see test_graphics for the real-device path).
	XrGraphicsBindingVulkanKHR vulkanBinding{.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, .next = nullptr};
	XrSessionCreateInfo boundSessionInfo = sessionInfo;
	boundSessionInfo.next = reinterpret_cast<const void *>(&vulkanBinding);
	XrSession boundSession = XR_NULL_HANDLE;
	CHECK_XR(xrCreateSession(instance, &boundSessionInfo, &boundSession), XR_ERROR_GRAPHICS_DEVICE_INVALID);
	CHECK(boundSession == XR_NULL_HANDLE);

	CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);
	instance = XR_NULL_HANDLE;

	// Singleton released: create-destroy-create works.
	CHECK_XR(xrCreateInstance(&createInfo, &instance), XR_SUCCESS);
	CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);
	instance = XR_NULL_HANDLE;

	if (failures == 0)
		std::puts("runtime test PASS");
	else
		std::printf("runtime test FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
