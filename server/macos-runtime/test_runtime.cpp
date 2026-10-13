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
	CHECK_XR(xrEndSession(session), XR_SUCCESS);
	CHECK_XR(xrEndSession(session), XR_ERROR_SESSION_NOT_RUNNING);
	CHECK_XR(xrRequestExitSession(session), XR_SUCCESS);
	CHECK_XR(xrDestroySession(session), XR_SUCCESS);

	// Session WITH a graphics binding: refused loudly until Phase 4.
	XrGraphicsBindingVulkanKHR vulkanBinding{.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, .next = nullptr};
	XrSessionCreateInfo boundSessionInfo = sessionInfo;
	boundSessionInfo.next = reinterpret_cast<const void *>(&vulkanBinding);
	XrSession boundSession = XR_NULL_HANDLE;
	CHECK_XR(xrCreateSession(instance, &boundSessionInfo, &boundSession), XR_ERROR_VALIDATION_FAILURE);
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
