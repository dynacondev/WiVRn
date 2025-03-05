#include "application.h"
#include <memory>
#include <spdlog/sinks/android_sink.h>

#ifdef __ANDROID_LIB__

#define JNIFUNC extern "C" JNIEXPORT

class UnityLib
{
public:
	static UnityLib & instance();

	// Delete copy constructor and copy assignment operator
	UnityLib(const UnityLib &) = delete;
	UnityLib & operator=(const UnityLib &) = delete;

	// Delete move constructor and move assignment operator
	UnityLib(UnityLib &&) = delete;
	UnityLib & operator=(UnityLib &&) = delete;

	static std::shared_ptr<application> app;

	// External object pointers
	static uint64_t g_instance;
	static uint64_t g_systemId;
	static uint64_t g_session;

	// JNI related
	static JNIEnv * jnienv;
	static AAssetManager * assetManager;

	// OpenXR
	static const XrSessionCreateInfo * sessionCreateInfo;
	static PFN_xrGetInstanceProcAddr s_xrGetInstanceProcAddr;
	static std::vector<XrCompositionLayerBaseHeader *> layers;
	// static std::vector<XrCompositionLayerBaseHeader> rendered_layers;
	static XrFrameEndInfo modifiedFrameEndInfo;
	static PFN_xrEndFrame s_xrEndFrame;
	static PFN_xrCreateSession s_xrCreateSession;
	static PFN_xrCreateVulkanInstanceKHR s_xrCreateVulkanInstanceKHR;
	static PFN_xrWaitFrame s_xrWaitFrame;
	static XrFrameState * framestate;

	static std::mutex render_mtx;
	static std::mutex render_mtx2;

	void debugLog(std::string message);
	void setupOpenXRHooks();
	void setupMainAppSession(JNIEnv * env, jobject assetManager, jstring configPath, jstring cachePath, jlong instance, jlong systemId, jlong session);
	void begin();

	static XRAPI_ATTR XrResult XRAPI_CALL intercepted_xrWaitFrame(XrSession session, const XrFrameWaitInfo * frameWaitInfo, XrFrameState * frameState);
	static XRAPI_ATTR XrResult XRAPI_CALL intercepted_xrEndFrame(XrSession session, const XrFrameEndInfo * frameEndInfo);
	static XRAPI_ATTR XrResult XRAPI_CALL intercepted_xrCreateSession(XrInstance instance, const XrSessionCreateInfo * sessionCreateInfo, XrSession * session);
	static XRAPI_ATTR XrResult XRAPI_CALL intercepted_xrCreateVulkanInstanceKHR(XrInstance xrInstance, const XrVulkanInstanceCreateInfoKHR * vulkanCreateInfo, VkInstance * vulkanInstance, VkResult * vulkanResult);
	static XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr_intercepted(XrInstance instance, const char * name, PFN_xrVoidFunction * function);

private:
	// Private constructor & destructor to prevent direct instantiation
	UnityLib();
	~UnityLib();

	static std::shared_ptr<application_info> info;
	static std::shared_ptr<spdlog::logger> logger;

	static std::mutex app_thread_mtx;
	static std::condition_variable app_thread_cv;
	static bool app_thread_flag;
	static std::thread application_thread;
};

// A simple native function that prints a log message for debugging purposes
JNIFUNC void JNICALL
Java_org_meumeu_wivrn_EmbeddedPlugin_nativeLog(JNIEnv * env, const jobject * /* this */, jstring message);

// The main setup/begin function
JNIFUNC void JNICALL
Java_org_meumeu_wivrn_EmbeddedPlugin_androidLibMain(JNIEnv * env, const jobject * /* this */, jobject assetManager, jstring configPath, jstring cachePath, jlong instance, jlong systemId, jlong session);

// The function to configure intercepts for OpenXR calls
JNIFUNC jlong JNICALL
Java_org_meumeu_wivrn_EmbeddedPlugin_xrGetInstanceProcAddr(JNIEnv * env, const jobject * /* this */, jlong nextProcAddr);
#endif