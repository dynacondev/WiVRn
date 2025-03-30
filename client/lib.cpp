#include "lib.h"
#include "scenes/lobby.h"
#include "utils/named_thread.h"
#include "vk/check.h"
#include <android/asset_manager_jni.h>
#include <vulkan/vulkan_raii.hpp>

#ifdef __ANDROID_LIB__
// Variables
std::shared_ptr<application> UnityLib::app = nullptr;
uint64_t UnityLib::g_instance = 0;
uint64_t UnityLib::g_systemId = 0;
uint64_t UnityLib::g_session = 0;
JNIEnv * UnityLib::jnienv = nullptr;
AAssetManager * UnityLib::assetManager = nullptr;
std::shared_ptr<application_info> UnityLib::info = nullptr;
std::shared_ptr<spdlog::logger> UnityLib::logger = nullptr;
const XrSessionCreateInfo * UnityLib::sessionCreateInfo;
PFN_xrGetInstanceProcAddr UnityLib::s_xrGetInstanceProcAddr = nullptr;
// std::vector<XrCompositionLayerBaseHeader *> empty_layers; // Default empty vector
std::vector<XrCompositionLayerBaseHeader *> UnityLib::layers;
std::vector<const XrCompositionLayerBaseHeader *> UnityLib::special_layers;
std::vector<std::unique_ptr<XrCompositionLayerBaseHeader>> UnityLib::very_special_layers;
std::vector<std::unique_ptr<XrCompositionLayerBaseHeader>> UnityLib::noDisplayLayers;
std::vector<std::shared_ptr<XrCompositionLayerBaseHeader>> UnityLib::persistent_layers;
std::unique_ptr<XrSpace> UnityLib::space;
XrFrameEndInfo * UnityLib::frameEndinfo;
XrResult UnityLib::result;
XrSession UnityLib::session;
std::unique_ptr<std::pair<XrViewStateFlags, std::vector<XrView>>> UnityLib::flag_views;
std::vector<XrCompositionLayerBaseHeader> UnityLib::rendered_layers;
// std::vector<XrCompositionLayerBaseHeader> UnityLib::rendered_layers_safe;
// static std::vector<XrCompositionLayerBaseHeader> rendered_layers_safe;
static XrCompositionLayerBaseHeader single_layer;
XrFrameEndInfo UnityLib::modifiedFrameEndInfo;
PFN_xrEndFrame UnityLib::s_xrEndFrame = nullptr;
PFN_xrCreateSession UnityLib::s_xrCreateSession = nullptr;
PFN_xrCreateVulkanInstanceKHR UnityLib::s_xrCreateVulkanInstanceKHR = nullptr;
PFN_xrWaitFrame UnityLib::s_xrWaitFrame = nullptr;
XrFrameState * UnityLib::framestate = nullptr;
std::mutex UnityLib::render_mtx;
std::mutex UnityLib::render_mtx2;
std::mutex UnityLib::special_render_mtx;
std::mutex UnityLib::app_thread_mtx;
std::condition_variable UnityLib::app_thread_cv;
bool UnityLib::app_thread_flag = false;
std::thread UnityLib::application_thread;

XrCompositionLayerBaseHeader UnityLib::g_layerStorage[6];
size_t UnityLib::g_numLayers = 0;

// Private constructor to prevent direct instantiation
UnityLib::UnityLib()
{
	logger = spdlog::android_logger_mt("WiVRn", "WiVRn");
	spdlog::set_default_logger(logger);

	info = std::make_shared<application_info>();
	info->name = "WiVRn";
	info->version = VK_MAKE_VERSION(1, 0, 0);

	app = std::make_shared<application>(*info);
}

void UnityLib::debugLog(std::string message)
{
	spdlog::info(message);
}

void UnityLib::setupOpenXRHooks()
{
}

void UnityLib::setupMainAppSession(JNIEnv * env, jobject javaAssetManager, jstring configPath, jstring cachePath, jlong instance, jlong systemId, jlong session)
{
	spdlog::info("WiVRn Library Started - Launching Main");

	jnienv = env;
	assetManager = AAssetManager_fromJava(jnienv, javaAssetManager);

	// Convert the Java strings to C++ strings
	const char * configPathChars = env->GetStringUTFChars(configPath, nullptr);
	const char * cachePathChars = env->GetStringUTFChars(cachePath, nullptr);

	auto config_path = std::string(configPathChars);
	auto cache_path = std::string(cachePathChars);

	g_instance = (uint64_t)instance;
	g_systemId = (uint64_t)systemId;
	g_session = (uint64_t)session;

	// Debug logging
	spdlog::info("Native OpenXR Handles Updated:\n");
	spdlog::info("Assigned g_instance: {}", std::to_string(g_instance));
	spdlog::info("Assigned g_systemId: {}", std::to_string(g_systemId));
	spdlog::info("Assigned g_session: {}", std::to_string(g_session));

	// Release the Java strings
	env->ReleaseStringUTFChars(configPath, configPathChars);
	env->ReleaseStringUTFChars(cachePath, cachePathChars);

	app->create(config_path, cache_path);
}

void UnityLib::begin()
{
	app->push_scene<scenes::lobby>();

	// application_thread = utils::named_thread("application_thread", []() {
	// 	app->push_scene<scenes::lobby>();

	// 	while (true)
	// 	{                                                          // Loop until we're done.
	// 		std::unique_lock<std::mutex> lock(app_thread_mtx); // Directly access the static mutex

	// 		// Wait until the flag becomes true
	// 		app_thread_cv.wait(lock, [] { return app_thread_flag; }); // Directly access the static flag

	// 		// Perform the work when the flag is true
	// 		if (app_thread_flag)
	// 		{
	// 			// std::unique_lock<std::mutex> lock2(render_mtx);
	// 			app->run_lib(UnityLib::framestate);
	// 			app_thread_flag = false;
	// 		}

	// 		// Notify other threads and release the lock
	// 		lock.unlock();
	// 		app_thread_cv.notify_all();
	// 	}
	// });
}

UnityLib & UnityLib::instance()
{
	static UnityLib instance;
	return instance;
}

// Private destructor
UnityLib::~UnityLib()
{
	// TODO tell application thread to stop looping
	application_thread.join();
}

XRAPI_ATTR XrResult XRAPI_CALL UnityLib::intercepted_xrWaitFrame(
        XrSession session,
        const XrFrameWaitInfo * frameWaitInfo,
        XrFrameState * frameState)
{
	// Call the xrWaitFrame function
	XrResult result = s_xrWaitFrame(session, frameWaitInfo, frameState);

	// // Signal the worker thread
	// if (frameState->shouldRender)
	// {
	// 	{
	// 		spdlog::info("lock 1");
	// 		render_mtx.lock();
	// 		std::lock_guard<std::mutex> lock(app_thread_mtx);
	// 		UnityLib::framestate = frameState;
	// 		app_thread_flag = true;
	// 	}
	// 	app_thread_cv.notify_all(); // Notify the worker thread to begin rendering}
	// } else {
	// 	render_mtx2.unlock();
	// }

	// app->run_lib(frameState);

	framestate = frameState;

	return result;
}

XRAPI_ATTR XrResult XRAPI_CALL UnityLib::intercepted_xrEndFrame(
        XrSession session,
        const XrFrameEndInfo * frameEndInfo)
{
	UnityLib::frameEndinfo = &const_cast<XrFrameEndInfo &>(*frameEndInfo);
	UnityLib::session = session;
	app->run_lib(framestate);

	

	// 	// // Add rendered layers
	// 	// layers.insert(layers.end(), UnityLib::rendered_layers.begin(), UnityLib::rendered_layers.end());

	// 	// // static bool doOnce = false;
	// 	// // if (!doOnce)
	// 	// // {
	// 	// // 	rendered_layers_safe = rendered_layers; // TODO Make thread safe etc
	// 	// // 	doOnce = true;
	// 	// // }

	// 	// // spdlog::info("layers: {}", UnityLib::layers.size());

	// 	// // render_mtx2.lock();

	// 	// // for (uint32_t i = 0; i < UnityLib::layers.size(); ++i)
	// 	// // {
	// 	// // 	spdlog::info("Layer [{}]: Type: {}, Address: {}", i, static_cast<int>(UnityLib::layers[i]->type), reinterpret_cast<void *>(UnityLib::layers[i]));
	// 	// // }

	// 	// layers.clear();

	// 	// // render_mtx2.unlock();
	// 	// // render_mtx.unlock();

	// 	// // single_layer = *const_cast<XrCompositionLayerBaseHeader *>(frameEndInfo->layers[0]);
	// 	// // layers.push_back(&single_layer);

	// 	// // layers.push_back(const_cast<XrCompositionLayerBaseHeader *>(frameEndInfo->layers[0]));

	// 	// for (uint32_t i = 0; i < rendered_layers.size(); ++i)
	// 	// {
	// 	// 	spdlog::info("Planned Layer [{}]: Type: {}", i, static_cast<int>(rendered_layers[i].type));
	// 	// 	layers.push_back(&rendered_layers[i]); // Problematic?
	// 	// }

	// 	// Create a local vector to hold all layer data as copies
	// 	// std::vector<XrCompositionLayerBaseHeader> local_layers;

	// 	// Copy rendered_layers into local_layers
	// 	// local_layers.insert(local_layers.end(), rendered_layers.begin(), rendered_layers.end());

	// 	// Copy frameEndInfo->layers into local_layers
	// 	// for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
	// 	// {
	// 	//     local_layers.push_back(*frameEndInfo->layers[i]);
	// 	// }

	// 	// // Create a vector of pointers to the objects in local_layers
	// 	// std::vector<const XrCompositionLayerBaseHeader *> layer_pointers;
	// 	// // layer_pointers.reserve(frameEndInfo->layerCount + g_numLayers);
	// 	// for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
	// 	// {
	// 	// 	spdlog::info("Original Layer [{}]: Type: {}", i, static_cast<int>(frameEndInfo->layers[i]->type));
	// 	// 	layer_pointers.push_back(frameEndInfo->layers[i]);
	// 	// }
	// 	// for (uint32_t i = 0; i < g_numLayers; ++i)
	// 	// {
	// 	// 	spdlog::info("Generated Layer [{}]: Type: {}, Address: {}", i, static_cast<int>(g_layerStorage[i].type), reinterpret_cast<void *>(&g_layerStorage[i]));
	// 	// 	// layer_pointers.push_back(&g_layerStorage[i]);
	// 	// }
	// 	// // for (const auto& layer : local_layers)
	// 	// // {
	// 	// //     layer_pointers.push_back(&layer);
	// 	// // }

	// 	// for (uint32_t i = 0; i < modifiedFrameEndInfo.layerCount; ++i)
	// 	// {
	// 	// 	spdlog::info("Layer [{}]: Type: {}", i, static_cast<int>(modifiedFrameEndInfo.layers[i]->type));
	// 	// }

	// 	// for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
	// 	// {
	// 	// 	layers.push_back(const_cast<XrCompositionLayerBaseHeader *>(frameEndInfo->layers[i]));
	// 	// }

	// 	// modifiedFrameEndInfo = *frameEndInfo;
	// 	// // spdlog::info("Actual layer count: {}", modifiedFrameEndInfo.layerCount);
	// 	// // spdlog::info("Calculated layer count: {}", (uint32_t)layers.size());
	// 	// modifiedFrameEndInfo.layerCount = (uint32_t)layers.size();
	// 	// modifiedFrameEndInfo.layers = layers.data();

	// 	// for (uint32_t i = 0; i < layers.size(); ++i)
	// 	// {
	// 	// 	spdlog::info("Layer [{}]: Type: {}, Address: {}", i, static_cast<int>(layers[i]->type), reinterpret_cast<void *>(layers[i]));
	// 	// }

	// for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
	// {
	// 	special_layers.push_back(frameEndInfo->layers[i]);
	// }

	// // Update modifiedFrameEndInfo with the pointer to the array of pointers
	// modifiedFrameEndInfo = *frameEndInfo;
	// modifiedFrameEndInfo.layerCount = static_cast<uint32_t>(special_layers.size());
	// modifiedFrameEndInfo.layers = special_layers.data();

	// XrResult result;
	// try
	// {
	// 	for (uint32_t i = 0; i < modifiedFrameEndInfo.layerCount; ++i)
	// 	{
	// 		spdlog::info("A Layer [{}]: Type: {}", i, static_cast<int>(modifiedFrameEndInfo.layers[i]->type));
	// 	}

	// 	result = s_xrEndFrame(session, &modifiedFrameEndInfo);

	// 	for (uint32_t i = 0; i < modifiedFrameEndInfo.layerCount; ++i)
	// 	{
	// 		spdlog::info("B Layer [{}]: Type: {}", i, static_cast<int>(modifiedFrameEndInfo.layers[i]->type));
	// 	}

	// 	// for (uint32_t i = 0; i < special_layers.size(); ++i)
	// 	// {
	// 	// 	FreeCompositionLayer(special_layers[i]);
	// 	// }
	// 	special_layers.clear();

	// 	if (XR_FAILED(result))
	// 	{
	// 		spdlog::error("xrEndFrame failed with error: {}", xr::to_string(result));
	// 	}

	// 	// render_mtx.unlock();
	// }
	// catch (std::exception & e)
	// {
	// 	spdlog::error("xrEndFrame threw error: {}", e.what());
	// }
	// catch (...)
	// {
	// 	spdlog::error("xrEndFrame threw error");
	// }

	return result;
}

XRAPI_ATTR XrResult XRAPI_CALL UnityLib::intercepted_xrCreateSession(
        XrInstance instance,
        const XrSessionCreateInfo * sessionInfo,
        XrSession * session)
{
	spdlog::info("Running xrCreateSession");
	sessionCreateInfo = sessionInfo;

	return s_xrCreateSession(instance, sessionCreateInfo, session);
}

XRAPI_ATTR XrResult XRAPI_CALL UnityLib::intercepted_xrCreateVulkanInstanceKHR(
        XrInstance xrInstance,
        const XrVulkanInstanceCreateInfoKHR * vulkanCreateInfo,
        VkInstance * vulkanInstance,
        VkResult * vulkanResult)
{
	spdlog::info("Running initialize_vulkan");
	return application::instance().initialize_vulkan(xrInstance, vulkanCreateInfo, vulkanInstance, vulkanResult);
}

XRAPI_ATTR XrResult XRAPI_CALL UnityLib::xrGetInstanceProcAddr_intercepted(
        XrInstance instance,
        const char * name,
        PFN_xrVoidFunction * function)
{
	XrResult result = s_xrGetInstanceProcAddr(instance, name, function);

	if (strcmp("xrWaitFrame", name) == 0)
	{
		s_xrWaitFrame = (PFN_xrWaitFrame)*function;
		*function = (PFN_xrVoidFunction)intercepted_xrWaitFrame;
	}
	else if (strcmp("xrEndFrame", name) == 0)
	{
		s_xrEndFrame = (PFN_xrEndFrame)*function;
		*function = (PFN_xrVoidFunction)intercepted_xrEndFrame;
	}
	else if (strcmp("xrCreateSession", name) == 0)
	{
		s_xrCreateSession = (PFN_xrCreateSession)*function;
		*function = (PFN_xrVoidFunction)intercepted_xrCreateSession;
	}
	else if (strcmp("xrCreateVulkanInstanceKHR", name) == 0)
	{
		s_xrCreateVulkanInstanceKHR = (PFN_xrCreateVulkanInstanceKHR)*function;
		*function = (PFN_xrVoidFunction)intercepted_xrCreateVulkanInstanceKHR;
	}

	return result;
}

JNIFUNC void JNICALL
Java_org_meumeu_wivrn_EmbeddedPlugin_nativeLog(JNIEnv * env, const jobject * /* this */, jstring message)
{
	const char * c_message = env->GetStringUTFChars(message, nullptr);
	UnityLib::instance().debugLog(std::string(c_message));
	env->ReleaseStringUTFChars(message, c_message);
}

JNIFUNC void JNICALL
Java_org_meumeu_wivrn_EmbeddedPlugin_androidLibMain(JNIEnv * env, const jobject * /* this */, jobject assetManager, jstring configPath, jstring cachePath, jlong instance, jlong systemId, jlong session)
{
	spdlog::info("Running begin");
	UnityLib::instance().setupMainAppSession(env, assetManager, configPath, cachePath, instance, systemId, session);
	UnityLib::instance().begin();
}

JNIFUNC jlong JNICALL
Java_org_meumeu_wivrn_EmbeddedPlugin_xrGetInstanceProcAddr(JNIEnv * env, const jobject * /* this */, jlong nextProcAddr)
{
	UnityLib::s_xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(nextProcAddr);
	return reinterpret_cast<jlong>(&UnityLib::xrGetInstanceProcAddr_intercepted);
}
#endif

// ginal Vulkan instance create info
//     VkInstanceCreateInfo modifiedCreateInfo = *(vulkanCreateInfo->vulkanCreateInfo);

//     // Vulkan layers to be used
//     std::vector<const char *> layers;

// 	// Add existing layers
//     if (modifiedCreateInfo.enabledLayerCount > 0) {
//         layers.insert(layers.end(),
//                       modifiedCreateInfo.ppEnabledLayerNames,
//                       modifiedCreateInfo.ppEnabledLayerNames + modifiedCreateInfo.enabledLayerCount);
//     }

//     // Add existing extensions
//     if (modifiedCreateInfo.enabledExtensionCount > 0) {
//         instance_extensions.insert(instance_extensions.end(),
//                           modifiedCreateInfo.ppEnabledExtensionNames,
//                           modifiedCreateInfo.ppEnabledExtensionNames + modifiedCreateInfo.enabledExtensionCount);
//     }

// 	spdlog::info("Available Vulkan layers:");
// 	[[maybe_unused]] bool validation_layer_found = false;

// 	for (vk::LayerProperties & i: vk_context.enumerateInstanceLayerProperties())
// 	{
// 		spdlog::info("    {}", i.layerName.data());
// 		if (!strcmp(i.layerName, "VK_LAYER_KHRONOS_validation"))
// 		{
// 			validation_layer_found = true;
// 		}
// 	}
// #ifndef NDEBUG
// 	if (validation_layer_found)
// 	{
// 		spdlog::info("Using Vulkan validation layer");
// 		layers.push_back("VK_LAYER_KHRONOS_validation");
// 	}
// #endif

// 	std::vector<const char *> instance_extensions{};
// 	std::unordered_set<std::string_view> optional_device_extensions{};

// #ifndef NDEBUG
// 	bool debug_report_found = false;
// 	bool debug_utils_found = false;
// #endif
// 	spdlog::info("Available Vulkan instance extensions:");
// 	for (vk::ExtensionProperties & i: vk_context.enumerateInstanceExtensionProperties(nullptr))
// 	{
// 		spdlog::info("    {} (version {})", i.extensionName.data(), i.specVersion);

// #ifndef NDEBUG
// 		if (!strcmp(i.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
// 			debug_report_found = true;

// 		if (!strcmp(i.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
// 			debug_utils_found = true;
// #endif
// 	}

// #ifndef NDEBUG
// 	if (debug_utils_found && debug_report_found)
// 	{
// 		// debug_extensions_found = true;
// 		// spdlog::info("Using debug extensions");
// 		instance_extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
// 		// instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
// 	}
// #endif

// 	vk_device_extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
// 	optional_device_extensions.emplace(VK_IMG_FILTER_CUBIC_EXTENSION_NAME);

// #ifdef __ANDROID__
// 	vk_device_extensions.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME);
// 	vk_device_extensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
// 	instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
// 	instance_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
// #endif

//     // Update the create info with the modified layers and extensions
// 	modifiedCreateInfo.setPEnabledLayerNames(layers);
// 	modifiedCreateInfo.setPEnabledExtensionNames(instance_extensions);

//     // Create a new XrVulkanInstanceCreateInfoKHR with the modified Vulkan create info
//     XrVulkanInstanceCreateInfoKHR modifiedXrCreateInfo = *vulkanCreateInfo;
//     modifiedXrCreateInfo.vulkanCreateInfo = &modifiedCreateInfo;

// 	// Call the original `xrCreateVulkanInstanceKHR` with the modified create info
// 	XrResult xresult = s_xrCreateVulkanInstanceKHR(xrInstance, &modifiedXrCreateInfo, vulkanInstance, vulkanResult);
// 	CHECK_VK(vulkanResult, "xrCreateVulkanInstanceKHR");
// 	CHECK_XR(xresult, "xrCreateVulkanInstanceKHR");

// 	vk_instance = vk::raii::Instance(vk_context, vulkanInstance);

// #ifndef NDEBUG
// 	if (debug_report_found)
// 	{
// 		vk::DebugReportCallbackCreateInfoEXT debug_report_info{
// 		        .flags = vk::DebugReportFlagBitsEXT::eInformation |
// 		                 vk::DebugReportFlagBitsEXT::eWarning |
// 		                 vk::DebugReportFlagBitsEXT::ePerformanceWarning |
// 		                 vk::DebugReportFlagBitsEXT::eError |
// 		                 vk::DebugReportFlagBitsEXT::eDebug,
// 		        .pfnCallback = vulkan_debug_report_callback,
// 		};
// 		debug_report_callback = vk::raii::DebugReportCallbackEXT(vk_instance, debug_report_info);
// 	}
// #endif

// 	vk_physical_device = xr_system_id.physical_device(vk_instance);
// 	physical_device_properties = vk_physical_device.getProperties();

// 	spdlog::info("Available Vulkan device extensions:");
// 	for (vk::ExtensionProperties & i: vk_physical_device.enumerateDeviceExtensionProperties())
// 	{
// 		spdlog::info("    {}", i.extensionName.data());
// 		if (auto it = optional_device_extensions.find(i.extensionName); it != optional_device_extensions.end())
// 			vk_device_extensions.push_back(it->data());
// 	}

// 	spdlog::info("Initializing Vulkan with device {}", physical_device_properties.deviceName.data());
// 	spdlog::info("    Vendor ID: 0x{:04x}", physical_device_properties.vendorID);
// 	spdlog::info("    Device ID: 0x{:04x}", physical_device_properties.deviceID);
// 	spdlog::info("    Driver version: {}", parse_driver_sersion(physical_device_properties));

// XrSpace xrspace;       // Reference space
// XrSwapchain swapchain; // Swapchain handle
// VkInstance vulkanInstance = VK_NULL_HANDLE;
// VkPhysicalDevice vulkanPhysicalDevice = VK_NULL_HANDLE;
// VkDevice vulkanDevice = VK_NULL_HANDLE;
// VkQueue vulkanQueue = VK_NULL_HANDLE;
// uint32_t vulkanQueueFamilyIndex = 0;
// VkCommandPool commandPool;
// VkCommandBuffer commandBuffer;

// void setup_vulkan_x()
// {
// 	XrReferenceSpaceCreateInfo space_create_info{
// 	        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
// 	        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
// 	        .poseInReferenceSpace = {
// 	                // Default pose: centered in the local space
// 	                .orientation = {0, 0, 0, 1},
// 	                .position = {0, 0, -1} // Positioned 1 meter in front of the user
// 	        }};

// 	// Create a swapchain for the quad layer
// 	XrSwapchainCreateInfo swapchainCreateInfo = {};
// 	swapchainCreateInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
// 	swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
// 	swapchainCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // Vulkan-compatible format
// 	swapchainCreateInfo.sampleCount = 1;
// 	swapchainCreateInfo.width = 1024;
// 	swapchainCreateInfo.height = 1024;
// 	swapchainCreateInfo.faceCount = 1;
// 	swapchainCreateInfo.arraySize = 1;
// 	swapchainCreateInfo.mipCount = 1;
// 	CHECK_XR(xrCreateReferenceSpace(reinterpret_cast<XrSession>(UnityLib::g_session), &space_create_info, &xrspace));
// 	CHECK_XR(xrCreateSwapchain(reinterpret_cast<XrSession>(UnityLib::g_session), &swapchainCreateInfo, &swapchain));
// }

// void InitializeVulkanFromOpenXR(const XrSessionCreateInfo * sessionCreateInfo)
// {
// 	// Check if the createInfo contains Vulkan-specific graphics binding
// 	const XrGraphicsBindingVulkanKHR * vulkanBinding =
// 	        reinterpret_cast<const XrGraphicsBindingVulkanKHR *>(sessionCreateInfo->next);

// 	// Traverse the linked structure chain to find XrGraphicsBindingVulkanKHR
// 	while (vulkanBinding != nullptr && vulkanBinding->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)
// 	{
// 		vulkanBinding = reinterpret_cast<const XrGraphicsBindingVulkanKHR *>(vulkanBinding->next);
// 	}

// 	// Variables to store Vulkan objects
// 	commandPool = VK_NULL_HANDLE;
// 	commandBuffer = VK_NULL_HANDLE;
// 	vulkanQueue = VK_NULL_HANDLE;

// 	// If Vulkan binding is found, capture the Vulkan instance, device, etc.
// 	if (vulkanBinding != nullptr)
// 	{
// 		spdlog::info("Intercepted xrCreateSession: Capturing Vulkan objects\n");
// 		spdlog::info("VkInstance: {}", reinterpret_cast<long>(vulkanBinding->instance));
// 		spdlog::info("VkPhysicalDevice: {}", reinterpret_cast<long>(vulkanBinding->physicalDevice));
// 		spdlog::info("VkDevice: {}", reinterpret_cast<long>(vulkanBinding->device));
// 		spdlog::info("Queue Family Index: {}", vulkanBinding->queueFamilyIndex);

// 		// Store or use these Vulkan objects as needed
// 		vulkanInstance = vulkanBinding->instance;
// 		vulkanDevice = vulkanBinding->device;
// 		vulkanQueueFamilyIndex = vulkanBinding->queueFamilyIndex;
// 		vulkanPhysicalDevice = vulkanBinding->physicalDevice;

// 		// Retrieve the Vulkan queue
// 		vkGetDeviceQueue(vulkanDevice, vulkanQueueFamilyIndex, 0, &vulkanQueue);
// 		if (vulkanQueue == VK_NULL_HANDLE)
// 		{
// 			spdlog::error("Failed to retrieve Vulkan queue");
// 			throw std::runtime_error("Failed to retrieve Vulkan queue");
// 		}
// 		spdlog::info("VkQueue retrieved successfully");

// 		// Create Vulkan Command Pool
// 		VkCommandPoolCreateInfo commandPoolInfo = {};
// 		commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
// 		commandPoolInfo.queueFamilyIndex = vulkanQueueFamilyIndex;
// 		commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

// 		VkResult result = vkCreateCommandPool(vulkanDevice, &commandPoolInfo, nullptr, &commandPool);
// 		if (result != VK_SUCCESS)
// 		{
// 			spdlog::error("Failed to create Vulkan command pool - enum code: {}", static_cast<int>(result));
// 			throw std::runtime_error("Failed to create Vulkan command pool");
// 		}
// 		spdlog::info("VkCommandPool created successfully");

// 		// Allocate a Vulkan Command Buffer
// 		VkCommandBufferAllocateInfo commandBufferInfo = {};
// 		commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
// 		commandBufferInfo.commandPool = commandPool;
// 		commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
// 		commandBufferInfo.commandBufferCount = 1;

// 		result = vkAllocateCommandBuffers(vulkanDevice, &commandBufferInfo, &commandBuffer);
// 		if (result != VK_SUCCESS)
// 		{
// 			spdlog::error("Failed to allocate Vulkan command buffer - enum code: {}", static_cast<int>(result));
// 			throw std::runtime_error("Failed to allocate Vulkan command buffer");
// 		}
// 		spdlog::info("VkCommandBuffer allocated successfully");
// 	}
// }

// static void RenderSolidRedBoxWithVulkan(XrSession session)
// {
// 	spdlog::info("Starting RenderSolidRedBoxWithVulkan");

// 	if (swapchain == XR_NULL_HANDLE)
// 	{
// 		spdlog::warn("Swapchain is null. Skipping rendering.");
// 		return;
// 	}

// 	if (commandBuffer == VK_NULL_HANDLE)
// 	{
// 		spdlog::warn("Command buffer is null. Skipping rendering.");
// 		return;
// 	}

// 	VkCommandBufferBeginInfo beginInfo = {};
// 	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

// 	VkResult vkResult = vkBeginCommandBuffer(commandBuffer, &beginInfo);
// 	if (vkResult != VK_SUCCESS)
// 	{
// 		spdlog::error("Failed to begin command buffer: VkResult {}", static_cast<int>(vkResult));
// 		return;
// 	}

// 	// Acquire the swapchain image
// 	uint32_t imageIndex;
// 	XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
// 	XrResult xrResult = xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex);
// 	if (XR_FAILED(xrResult))
// 	{
// 		spdlog::error("Failed to acquire swapchain image: XrResult {}", static_cast<int>(xrResult));
// 		vkEndCommandBuffer(commandBuffer);
// 		return;
// 	}
// 	spdlog::info("Acquired swapchain image with index {}", imageIndex);

// 	XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
// 	waitInfo.timeout = XR_INFINITE_DURATION;
// 	xrResult = xrWaitSwapchainImage(swapchain, &waitInfo);
// 	if (XR_FAILED(xrResult))
// 	{
// 		spdlog::error("Failed to wait for swapchain image: XrResult {}", static_cast<int>(xrResult));
// 		vkEndCommandBuffer(commandBuffer);
// 		return;
// 	}
// 	spdlog::info("Swapchain image ready for rendering");

// 	// Get Vulkan image from swapchain
// 	uint32_t imageCount = 0;
// 	xrResult = xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
// 	if (XR_FAILED(xrResult) || imageCount == 0)
// 	{
// 		spdlog::error("Failed to enumerate swapchain images or no images available: XrResult {}, ImageCount {}", static_cast<int>(xrResult), static_cast<int>(imageCount));
// 		vkEndCommandBuffer(commandBuffer);
// 		return;
// 	}

// 	std::vector<XrSwapchainImageVulkanKHR> swapchainImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
// 	xrResult = xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(swapchainImages.data()));
// 	if (XR_FAILED(xrResult))
// 	{
// 		spdlog::error("Failed to get swapchain images: XrResult {}", static_cast<int>(xrResult));
// 		vkEndCommandBuffer(commandBuffer);
// 		return;
// 	}

// 	if (imageIndex >= swapchainImages.size())
// 	{
// 		spdlog::error("Image index {} is out of bounds for swapchain images size {}", imageIndex, swapchainImages.size());
// 		vkEndCommandBuffer(commandBuffer);
// 		return;
// 	}

// 	VkImage vulkanImage = swapchainImages[imageIndex].image;
// 	if (vulkanImage == VK_NULL_HANDLE)
// 	{
// 		spdlog::warn("Swapchain image at index {} is null. Skipping rendering.", imageIndex);
// 		vkEndCommandBuffer(commandBuffer);
// 		return;
// 	}
// 	spdlog::info("Using Vulkan image: {}", reinterpret_cast<uint64_t>(vulkanImage));

// 	// Transition image layout to prepare for rendering
// 	VkImageMemoryBarrier barrier = {};
// 	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
// 	barrier.image = vulkanImage;
// 	barrier.srcAccessMask = 0;
// 	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
// 	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
// 	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
// 	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
// 	barrier.subresourceRange.levelCount = 1;
// 	barrier.subresourceRange.layerCount = 1;

// 	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
// 	spdlog::info("Image layout transitioned to TRANSFER_DST_OPTIMAL");

// 	// Clear image with red color
// 	VkClearColorValue clearColor = {{1.0f, 0.0f, 0.0f, 1.0f}};
// 	VkImageSubresourceRange range = {};
// 	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
// 	range.levelCount = 1;
// 	range.layerCount = 1;

// 	vkCmdClearColorImage(commandBuffer, vulkanImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
// 	spdlog::info("Image cleared with red color");

// 	// Transition image layout back for shader read
// 	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
// 	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
// 	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
// 	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

// 	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
// 	spdlog::info("Image layout transitioned to SHADER_READ_ONLY_OPTIMAL");

// 	// Submit the command buffer and release the swapchain image
// 	vkEndCommandBuffer(commandBuffer);
// 	spdlog::info("Command buffer recording ended");

// 	XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
// 	xrResult = xrReleaseSwapchainImage(swapchain, &releaseInfo);
// 	if (XR_FAILED(xrResult))
// 	{
// 		spdlog::error("Failed to release swapchain image: XrResult {}", static_cast<int>(xrResult));
// 		return;
// 	}
// 	spdlog::info("Swapchain image released");
// }

// static XrCompositionLayerQuad * RenderSolidRedBoxAndAppendLayer(
//         XrSession session)
// {
// 	spdlog::info("Starting RenderSolidRedBoxAndAppendLayer");

// 	// Render the solid red box
// 	RenderSolidRedBoxWithVulkan(session);

// 	// Populate layer
// 	XrCompositionLayerQuad * layer = new XrCompositionLayerQuad{
// 	        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
// 	        // .next = "",
// 	        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
// 	        .space = xrspace,
// 	        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
// 	        .subImage = {
// 	                .swapchain = swapchain,
// 	                .imageRect = {
// 	                        .offset = {
// 	                                .x = 0,
// 	                                .y = 0,
// 	                        },
// 	                        .extent = {
// 	                                .width = 1024,
// 	                                .height = 1024,
// 	                        }},
// 	        },
// 	        .pose = {
// 	                .orientation = {
// 	                        .x = 0,
// 	                        .y = 0,
// 	                        .z = 0,
// 	                        .w = 1,
// 	                },
// 	                .position = {
// 	                        .x = 0,
// 	                        .y = 0,
// 	                        .z = -1.0f,
// 	                },
// 	        },
// 	        .size = {
// 	                .width = 0.5f,
// 	                .height = 0.5f,
// 	        }};

// 	// layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer));
// 	spdlog::info("Layer appended successfully.");

// 	// Append the layer
// 	return layer;
// }