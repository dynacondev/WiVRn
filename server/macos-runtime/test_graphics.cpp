// Phase 4 proof: headless MoltenVK frame-loop drive through the real
// loader — instance extensions, device bring-up, swapchain acquire /
// wait / release cycles, Wait/Begin/EndFrame — with Khronos validation
// layers enabled when present (zero validation errors required). Run with:
//   XR_RUNTIME_JSON=<build>/server/macos-runtime/wivrn-macos.json \
//     ./wivrn-macos-graphics-test
// No window, no headset: all poses come from the synthetic source.

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static int validationErrors = 0;
static bool validationOn = false;

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

#define CHECK_VK(call)                                                                                \
	do                                                                                            \
	{                                                                                             \
		VkResult res__ = (call);                                                              \
		if (res__ != VK_SUCCESS)                                                              \
		{                                                                                     \
			std::printf("FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, #call, (int)res__); \
			++failures;                                                                   \
		}                                                                                     \
	} while (false)

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT * data,
        void *)
{
	if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
	{
		std::printf("VALIDATION ERROR: %s\n", data->pMessage);
		++validationErrors;
	}
	return VK_FALSE;
}

static bool has_layer(const char * name)
{
	uint32_t count = 0;
	vkEnumerateInstanceLayerProperties(&count, nullptr);
	std::vector<VkLayerProperties> layers(count);
	vkEnumerateInstanceLayerProperties(&count, layers.data());
	for (const auto & layer: layers)
	{
		if (std::strcmp(layer.layerName, name) == 0)
			return true;
	}
	return false;
}

int main()
{
	const char * manifest = std::getenv("XR_RUNTIME_JSON");
	std::printf("XR_RUNTIME_JSON=%s\n", manifest ? manifest : "(unset)");
	if (manifest == nullptr)
		return 2;

	// ---- OpenXR instance with the Vulkan extension ----
	uint32_t xrExtCount = 0;
	CHECK_XR(xrEnumerateInstanceExtensionProperties(nullptr, 0, &xrExtCount, nullptr), XR_SUCCESS);
	CHECK(xrExtCount >= 1);
	std::vector<XrExtensionProperties> xrExts(xrExtCount, {XR_TYPE_EXTENSION_PROPERTIES});
	CHECK_XR(xrEnumerateInstanceExtensionProperties(nullptr, xrExtCount, &xrExtCount, xrExts.data()), XR_SUCCESS);
	bool haveVulkanEnable = false, haveVulkanEnable2 = false;
	for (uint32_t i = 0; i < xrExtCount; ++i)
	{
		if (std::strcmp(xrExts[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
			haveVulkanEnable = true;
		if (std::strcmp(xrExts[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
			haveVulkanEnable2 = true;
	}
	CHECK(haveVulkanEnable);
	CHECK(haveVulkanEnable2);

	const char * enabledExts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstance instance = XR_NULL_HANDLE;
	XrInstanceCreateInfo createInfo{
	        .type = XR_TYPE_INSTANCE_CREATE_INFO,
	        .next = nullptr,
	        .createFlags = 0,
	        .applicationInfo = {.applicationName = "graphics-test", .applicationVersion = 1, .engineName = "none", .engineVersion = 0, .apiVersion = XR_CURRENT_API_VERSION},
	        .enabledApiLayerCount = 0,
	        .enabledApiLayerNames = nullptr,
	        .enabledExtensionCount = 2,
	        .enabledExtensionNames = enabledExts,
	};
	CHECK_XR(xrCreateInstance(&createInfo, &instance), XR_SUCCESS);

	XrSystemGetInfo sysInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .next = nullptr, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	CHECK_XR(xrGetSystem(instance, &sysInfo, &systemId), XR_SUCCESS);

	// Extension entry points resolve through the instance (the loader
	// only exports core directly). Exercise both the v1 string queries
	// and the enable2 requirements spelling.
	PFN_xrGetVulkanGraphicsRequirements2KHR pfnRequirements = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR pfnInstanceExtensions = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR pfnDeviceExtensions = nullptr;
	PFN_xrCreateVulkanInstanceKHR pfnCreateInstance = nullptr;
	PFN_xrCreateVulkanDeviceKHR pfnCreateDevice = nullptr;
	{
		PFN_xrVoidFunction fn = nullptr;
		CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", &fn), XR_SUCCESS);
		pfnRequirements = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(fn);
		CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR", &fn), XR_SUCCESS);
		pfnInstanceExtensions = reinterpret_cast<PFN_xrGetVulkanInstanceExtensionsKHR>(fn);
		CHECK_XR(xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR", &fn), XR_SUCCESS);
		pfnDeviceExtensions = reinterpret_cast<PFN_xrGetVulkanDeviceExtensionsKHR>(fn);
		CHECK_XR(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR", &fn), XR_SUCCESS);
		pfnCreateInstance = reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(fn);
		CHECK_XR(xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR", &fn), XR_SUCCESS);
		pfnCreateDevice = reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(fn);
	}
	CHECK(pfnRequirements != nullptr && pfnInstanceExtensions != nullptr && pfnDeviceExtensions != nullptr);
	CHECK(pfnCreateInstance != nullptr && pfnCreateDevice != nullptr);

	XrGraphicsRequirementsVulkan2KHR requirements{.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR, .next = nullptr};
	CHECK_XR(pfnRequirements(instance, systemId, &requirements), XR_SUCCESS);
	std::printf(
	        "vulkan requirements: min %u.%u max %u.%u\n",
	        XR_VERSION_MAJOR(requirements.minApiVersionSupported),
	        XR_VERSION_MINOR(requirements.minApiVersionSupported),
	        XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
	        XR_VERSION_MINOR(requirements.maxApiVersionSupported));
	CHECK(XR_VERSION_MAJOR(requirements.minApiVersionSupported) == 1);
	CHECK(XR_VERSION_MAJOR(requirements.maxApiVersionSupported) == 1);
	CHECK(XR_VERSION_MINOR(requirements.maxApiVersionSupported) >= 3);

	// ---- Vulkan instance (MoltenVK): runtime extension list + flags ----
	uint32_t instExtBytes = 0;
	CHECK_XR(pfnInstanceExtensions(instance, systemId, 0, &instExtBytes, nullptr), XR_SUCCESS);
	CHECK(instExtBytes > 1);
	std::string instExts(instExtBytes, '\0');
	CHECK_XR(
	        pfnInstanceExtensions(instance, systemId, instExtBytes, &instExtBytes, instExts.data()),
	        XR_SUCCESS);
	std::printf("instance extensions: %s\n", instExts.c_str());
	CHECK(instExts.find("VK_KHR_portability_enumeration") != std::string::npos);

	std::vector<const char *> vkInstExts;
	{
		char * save = nullptr;
		for (char * tok = strtok_r(instExts.data(), " ", &save); tok != nullptr; tok = strtok_r(nullptr, " ", &save))
			vkInstExts.push_back(tok);
	}
	validationOn = has_layer("VK_LAYER_KHRONOS_validation");
	if (validationOn)
		vkInstExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	const char * validationLayer = "VK_LAYER_KHRONOS_validation";

	VkApplicationInfo appInfo{
	        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	        .pApplicationName = "graphics-test",
	        .applicationVersion = 1,
	        // 1.1+: get_physical_device_properties2 is core, satisfying
	        // the portability_subset device dependency (VUID-01387).
	        .apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo vkCreateInfo{
	        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	        .pApplicationInfo = &appInfo,
	        .enabledLayerCount = validationOn ? 1u : 0u,
	        .ppEnabledLayerNames = validationOn ? &validationLayer : nullptr,
	        .enabledExtensionCount = (uint32_t)vkInstExts.size(),
	        .ppEnabledExtensionNames = vkInstExts.data(),
	};
	vkCreateInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	VkInstance vkInstance = VK_NULL_HANDLE;
	CHECK_VK(vkCreateInstance(&vkCreateInfo, nullptr, &vkInstance));
	if (vkInstance == VK_NULL_HANDLE)
		return 1;
	std::printf("validation layers: %s\n", validationOn ? "on" : "OFF (unavailable)");

	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	if (validationOn)
	{
		auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
		CHECK(createMessenger != nullptr);
		if (createMessenger != nullptr)
		{
			VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
			        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			        .pfnUserCallback = debug_callback,
			};
			CHECK_VK(createMessenger(vkInstance, &messengerInfo, nullptr, &messenger));
		}
	}

	// ---- Physical device + logical device ----
	uint32_t physCount = 0;
	CHECK_VK(vkEnumeratePhysicalDevices(vkInstance, &physCount, nullptr));
	CHECK(physCount >= 1);
	std::vector<VkPhysicalDevice> physDevs(physCount);
	CHECK_VK(vkEnumeratePhysicalDevices(vkInstance, &physCount, physDevs.data()));
	VkPhysicalDevice phys = physDevs[0];
	VkPhysicalDeviceProperties physProps{};
	vkGetPhysicalDeviceProperties(phys, &physProps);
	std::printf("device: %s (api %u.%u)\n", physProps.deviceName, VK_VERSION_MAJOR(physProps.apiVersion), VK_VERSION_MINOR(physProps.apiVersion));

	uint32_t queueFamily = UINT32_MAX;
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, families.data());
		for (uint32_t i = 0; i < familyCount; ++i)
		{
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			{
				queueFamily = i;
				break;
			}
		}
	}
	CHECK(queueFamily != UINT32_MAX);

	uint32_t devExtBytes = 0;
	CHECK_XR(pfnDeviceExtensions(instance, systemId, 0, &devExtBytes, nullptr), XR_SUCCESS);
	std::string devExts(devExtBytes, '\0');
	CHECK_XR(pfnDeviceExtensions(instance, systemId, devExtBytes, &devExtBytes, devExts.data()), XR_SUCCESS);
	std::printf("device extensions: %s\n", devExts.c_str());
	CHECK(devExts.find("VK_KHR_portability_subset") != std::string::npos);
	std::vector<const char *> vkDevExts;
	{
		char * save = nullptr;
		for (char * tok = strtok_r(devExts.data(), " ", &save); tok != nullptr; tok = strtok_r(nullptr, " ", &save))
			vkDevExts.push_back(tok);
	}

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	        .queueFamilyIndex = queueFamily,
	        .queueCount = 1,
	        .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo devCreateInfo{
	        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	        .queueCreateInfoCount = 1,
	        .pQueueCreateInfos = &queueInfo,
	        .enabledExtensionCount = (uint32_t)vkDevExts.size(),
	        .ppEnabledExtensionNames = vkDevExts.data(),
	};
	VkDevice device = VK_NULL_HANDLE;
	CHECK_VK(vkCreateDevice(phys, &devCreateInfo, nullptr, &device));
	if (device == VK_NULL_HANDLE)
		return 1;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(device, queueFamily, 0, &queue);
	CHECK(queue != VK_NULL_HANDLE);

	// Enable2 create-functions path: the runtime builds instance + device.
	{
		XrVulkanInstanceCreateInfoKHR rtInstanceInfo{
		        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
		        .next = nullptr,
		        .systemId = systemId,
		        .createFlags = 0,
		        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
		        .vulkanCreateInfo = nullptr,
		        .vulkanAllocator = nullptr,
		};
		VkInstance rtInstance = VK_NULL_HANDLE;
		VkResult rtResult = VK_ERROR_UNKNOWN;
		CHECK_XR(pfnCreateInstance(instance, &rtInstanceInfo, &rtInstance, &rtResult), XR_SUCCESS);
		CHECK(rtResult == VK_SUCCESS && rtInstance != VK_NULL_HANDLE);

		uint32_t rtPhysCount = 0;
		CHECK_VK(vkEnumeratePhysicalDevices(rtInstance, &rtPhysCount, nullptr));
		CHECK(rtPhysCount >= 1);
		std::vector<VkPhysicalDevice> rtPhys(rtPhysCount);
		CHECK_VK(vkEnumeratePhysicalDevices(rtInstance, &rtPhysCount, rtPhys.data()));

		float rtPriority = 1.0f;
		VkDeviceQueueCreateInfo rtQueueInfo{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		        .queueFamilyIndex = queueFamily,
		        .queueCount = 1,
		        .pQueuePriorities = &rtPriority,
		};
		VkDeviceCreateInfo rtDevInfo{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		        .queueCreateInfoCount = 1,
		        .pQueueCreateInfos = &rtQueueInfo,
		};
		XrVulkanDeviceCreateInfoKHR rtDeviceInfo{
		        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
		        .next = nullptr,
		        .systemId = systemId,
		        .createFlags = 0,
		        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
		        .vulkanPhysicalDevice = rtPhys[0],
		        .vulkanCreateInfo = &rtDevInfo,
		        .vulkanAllocator = nullptr,
		};
		VkDevice rtDevice = VK_NULL_HANDLE;
		rtResult = VK_ERROR_UNKNOWN;
		CHECK_XR(pfnCreateDevice(instance, &rtDeviceInfo, &rtDevice, &rtResult), XR_SUCCESS);
		CHECK(rtResult == VK_SUCCESS && rtDevice != VK_NULL_HANDLE);
		std::printf("runtime-created instance+device OK\n");
		vkDestroyDevice(rtDevice, nullptr);
		vkDestroyInstance(rtInstance, nullptr);
	}

	// ---- Session with the Vulkan2 binding ----
	// NOTE: the enable2 binding struct is an alias of the v1 struct: it
	// carries family + index but no queue handle; the runtime resolves
	// the VkQueue itself.
	XrGraphicsBindingVulkan2KHR binding{
	        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
	        .next = nullptr,
	        .instance = vkInstance,
	        .physicalDevice = phys,
	        .device = device,
	        .queueFamilyIndex = queueFamily,
	        .queueIndex = 0,
	};
	XrSession session = XR_NULL_HANDLE;
	XrSessionCreateInfo sessionInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .createFlags = 0, .systemId = systemId};
	CHECK_XR(xrCreateSession(instance, &sessionInfo, &session), XR_SUCCESS);
	CHECK(session != XR_NULL_HANDLE);

	XrViewConfigurationType config = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	XrSessionBeginInfo beginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO, .next = nullptr, .primaryViewConfigurationType = config};
	CHECK_XR(xrBeginSession(session, &beginInfo), XR_SUCCESS);

	// ---- Swapchains (one per eye) ----
	uint32_t formatCount = 0;
	CHECK_XR(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), XR_SUCCESS);
	CHECK(formatCount >= 1);
	std::vector<int64_t> formats(formatCount);
	CHECK_XR(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), XR_SUCCESS);
	int64_t chosenFormat = formats[0];
	std::printf("swapchain formats: %d options, using %d\n", (int)formatCount, (int)chosenFormat);

	XrSwapchain swapchains[2] = {};
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainCreateInfo swapInfo{
		        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		        .next = nullptr,
		        .createFlags = 0,
		        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
		        .format = chosenFormat,
		        .sampleCount = 1,
		        .width = 512,
		        .height = 512,
		        .faceCount = 1,
		        .arraySize = 1,
		        .mipCount = 1,
		};
		CHECK_XR(xrCreateSwapchain(session, &swapInfo, &swapchains[eye]), XR_SUCCESS);

		uint32_t imageCount = 0;
		CHECK_XR(xrEnumerateSwapchainImages(swapchains[eye], 0, &imageCount, nullptr), XR_SUCCESS);
		CHECK(imageCount == 3);
		std::vector<XrSwapchainImageVulkan2KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		CHECK_XR(
		        xrEnumerateSwapchainImages(
		                swapchains[eye],
		                imageCount,
		                &imageCount,
		                reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
		        XR_SUCCESS);
		for (uint32_t i = 0; i < imageCount; ++i)
			CHECK(images[i].image != VK_NULL_HANDLE);
	}
	std::printf("swapchains: 2x triple-buffered 512x512\n");

	// ---- Reference space for view location ----
	XrReferenceSpaceCreateInfo spaceInfo{
	        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	        .next = nullptr,
	        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	        .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	};
	XrSpace local = XR_NULL_HANDLE;
	CHECK_XR(xrCreateReferenceSpace(session, &spaceInfo, &local), XR_SUCCESS);

	// ---- Frame loop ----
	XrSwapchainImageAcquireInfo acquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
	XrSwapchainImageWaitInfo waitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .next = nullptr, .timeout = XR_INFINITE_DURATION};
	XrSwapchainImageReleaseInfo releaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
	XrFrameWaitInfo frameWaitInfo{.type = XR_TYPE_FRAME_WAIT_INFO, .next = nullptr};
	XrFrameBeginInfo frameBeginInfo{.type = XR_TYPE_FRAME_BEGIN_INFO, .next = nullptr};

	int64_t lastPredicted = 0;
	for (int frame = 0; frame < 5; ++frame)
	{
		XrFrameState frameState{.type = XR_TYPE_FRAME_STATE, .next = nullptr};
		CHECK_XR(xrWaitFrame(session, &frameWaitInfo, &frameState), XR_SUCCESS);
		CHECK(frameState.predictedDisplayTime > lastPredicted);
		CHECK(frameState.shouldRender);
		lastPredicted = frameState.predictedDisplayTime;

		uint32_t indices[2] = {};
		for (int eye = 0; eye < 2; ++eye)
		{
			CHECK_XR(xrAcquireSwapchainImage(swapchains[eye], &acquireInfo, &indices[eye]), XR_SUCCESS);
			CHECK_XR(xrWaitSwapchainImage(swapchains[eye], &waitInfo), XR_SUCCESS);
			if (frame == 0 && eye == 0)
			{
				// The first wait consumed (reset) the fresh fence: an
				// immediate second zero-wait must report TIMEOUT (proves
				// real fences, not stubs).
				XrSwapchainImageWaitInfo zeroWait{
				        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .next = nullptr, .timeout = 0};
				CHECK_XR(xrWaitSwapchainImage(swapchains[eye], &zeroWait), XR_TIMEOUT_EXPIRED);
			}
		}

		CHECK_XR(xrBeginFrame(session, &frameBeginInfo), XR_SUCCESS);

		XrCompositionLayerProjectionView projViews[2] = {};
		XrCompositionLayerProjection proj{
		        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		        .next = nullptr,
		        .layerFlags = 0,
		        .space = local,
		        .viewCount = 2,
		        .views = projViews,
		};
		for (int eye = 0; eye < 2; ++eye)
		{
			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].next = nullptr;
			projViews[eye].pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}};
			projViews[eye].fov = {-0.86f, 0.86f, 0.86f, -0.86f}; // OpenXR signs: left/down negative
			projViews[eye].subImage = {
			        .swapchain = swapchains[eye],
			        .imageRect = {.offset = {0, 0}, .extent = {512, 512}},
			        .imageArrayIndex = 0,
			};
		}
		const XrCompositionLayerBaseHeader * layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj)};
		XrFrameEndInfo endInfo{
		        .type = XR_TYPE_FRAME_END_INFO,
		        .next = nullptr,
		        .displayTime = frameState.predictedDisplayTime,
		        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		        .layerCount = 1,
		        .layers = layers,
		};
		CHECK_XR(xrEndFrame(session, &endInfo), XR_SUCCESS);

		for (int eye = 0; eye < 2; ++eye)
			CHECK_XR(xrReleaseSwapchainImage(swapchains[eye], &releaseInfo), XR_SUCCESS);
	}
	std::printf("frame loop: 5 frames, predicted times advance\n");

	// Negative: quad layers are not composited in v1.
	{
		XrCompositionLayerQuad quad{.type = XR_TYPE_COMPOSITION_LAYER_QUAD, .next = nullptr};
		const XrCompositionLayerBaseHeader * layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad)};
		XrFrameWaitInfo wait{.type = XR_TYPE_FRAME_WAIT_INFO, .next = nullptr};
		XrFrameState state{.type = XR_TYPE_FRAME_STATE, .next = nullptr};
		CHECK_XR(xrWaitFrame(session, &wait, &state), XR_SUCCESS);
		CHECK_XR(xrBeginFrame(session, &frameBeginInfo), XR_SUCCESS);
		XrFrameEndInfo endInfo{
		        .type = XR_TYPE_FRAME_END_INFO,
		        .next = nullptr,
		        .displayTime = state.predictedDisplayTime,
		        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		        .layerCount = 1,
		        .layers = layers,
		};
		CHECK_XR(xrEndFrame(session, &endInfo), XR_ERROR_FEATURE_UNSUPPORTED);
	}

	CHECK_XR(xrDestroySpace(local), XR_SUCCESS);
	for (int eye = 0; eye < 2; ++eye)
		CHECK_XR(xrDestroySwapchain(swapchains[eye]), XR_SUCCESS);
	CHECK_XR(xrEndSession(session), XR_SUCCESS);
	CHECK_XR(xrDestroySession(session), XR_SUCCESS);
	CHECK_XR(xrDestroyInstance(instance), XR_SUCCESS);

	if (validationOn)
	{
		auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
		if (destroyMessenger != nullptr && messenger != VK_NULL_HANDLE)
			destroyMessenger(vkInstance, messenger, nullptr);
	}
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(vkInstance, nullptr);

	if (validationOn)
	{
		std::printf("validation errors: %d\n", validationErrors);
		CHECK(validationErrors == 0);
	}

	if (failures == 0)
		std::puts("graphics test PASS");
	else
		std::printf("graphics test FAIL (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
