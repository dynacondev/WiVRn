// Internal shared state for the macOS bespoke runtime (v1 MVP).
// No Monado headers here, ever. Only Khronos openxr/ headers + STL.

#pragma once

// Vulkan platform structs (binding parse is Vulkan-only in v1). Test
// apps must define XR_USE_GRAPHICS_API_VULKAN themselves if they need
// it (see test_runtime.cpp). vulkan.h must come first: the platform
// header expects Vk types to already be declared.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Opaque Khronos handle storage, completed after the namespace block.
// Instances own these (vectors below), so lookup is handle comparison.
struct XrInstance_T;
struct XrSession_T;
struct XrActionSet_T;
struct XrAction_T;
struct XrSpace_T;
struct XrSwapchain_T;

namespace wivrn_macos
{

// v1 MVP runtime version (informational, surfaced via instance properties).
constexpr uint32_t runtime_version_major = 0;
constexpr uint32_t runtime_version_minor = 1;
constexpr uint32_t runtime_version_patch = 0;

// Single HMD system (v1: fixed stereo description; live values from the
// headset info packet arrive in Phase 5).
struct SystemInfo
{
	XrSystemId id = 1;
	XrFormFactor formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	static constexpr uint32_t viewCount = 2;
	static constexpr uint32_t recommendedWidth = 2064;
	static constexpr uint32_t recommendedHeight = 2208;
	static constexpr uint32_t maxWidth = 2064;
	static constexpr uint32_t maxHeight = 2208;
};

struct Instance
{
	XrApplicationInfo appInfo{};
	XrVersion apiVersion = XR_CURRENT_API_VERSION;
	SystemInfo system;
	size_t liveSessions = 0;
	// Live sessions for event polling (raw; removed on destroy).
	std::vector<struct Session *> sessions;
	// Owned action/socket graph (freed on DestroyInstance).
	std::vector<std::unique_ptr<XrActionSet_T>> ownedSets;
	std::vector<std::unique_ptr<XrAction_T>> ownedActions;
	std::vector<std::unique_ptr<XrSpace_T>> ownedSpaces;
	std::vector<std::unique_ptr<XrSwapchain_T>> ownedSwapchains;
	// Suggested bindings, in suggest order (replaced per profile).
	std::vector<struct SuggestedBinding> bindings;
};

// One side of the simple-controller input state. v1 models exactly the
// KHR_simple_controller component set; other profiles resolve through
// the same component names where they overlap (documented limitation).
struct ControllerSample
{
	XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
	bool active = false;
	bool selectClick = false;
	bool menuClick = false;
	bool squeezeClick = false;
	bool thumbstickClick = false;
	float triggerValue = 0;
	XrVector2f thumbstick{0, 0};
};

struct TrackingSample
{
	int64_t timestampNs = 0;
	XrPosef hmd{{0, 0, 0, 1}, {0, 0, 0}};
	ControllerSample left;
	ControllerSample right;
};

struct ActionSet
{
	std::string name;
	std::string localizedName;
	std::vector<struct Action *> actions;
	bool attached = false;
};

struct Action
{
	ActionSet * set = nullptr;
	XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
	std::vector<XrPath> subactionPaths;
	std::string name;
};

struct Space
{
	enum class Kind
	{
		Reference,
		Action,
	};
	Kind kind = Kind::Reference;
	XrReferenceSpaceType refType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrPosef offset{{0, 0, 0, 1}, {0, 0, 0}};
	Action * action = nullptr;
	XrPath subaction = XR_NULL_PATH;
	Instance * owner = nullptr;
};

struct SuggestedBinding
{
	XrPath profile = XR_NULL_PATH;
	XrPath userPath = XR_NULL_PATH;
	XrPath componentPath = XR_NULL_PATH;
	Action * action = nullptr;
};

// Runtime-owned swapchain images on the application device (Phase 4).
// The app renders into these; Phase 5 snapshots them for encode.
struct Swapchain;

struct Session
{
	Instance * instance = nullptr;
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	// Action sets attached via xrAttachSessionActionSets (v1: attach once).
	std::vector<struct ActionSet *> attachedSets;
	// Latest synced tracking snapshot (xrSyncActions). GetActionState*
	// resolves against this; isActive requires at least one sync.
	bool everSynced = false;
	struct TrackingSample lastSample{};

	// Graphics binding (Phase 4: Vulkan only, stored from CreateSession).
	// The enable2 binding struct is an alias of the v1 struct: it
	// carries instance/physicalDevice/device plus queue family + index,
	// but no queue handle, so the runtime resolves the VkQueue itself
	// (correct on single-family MoltenVK devices, which is all of them).
	bool hasGraphicsBinding = false;
	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
	VkDevice vkDevice = VK_NULL_HANDLE;
	VkQueue vkQueue = VK_NULL_HANDLE;
	uint32_t vkQueueFamily = 0;

	// Frame-loop call-order tracking (Wait -> Begin -> End).
	enum class FrameCall
	{
		None,
		Wait,
		Begin,
		End,
	};
	FrameCall lastCall = FrameCall::None;
	uint64_t frameIndex = 0;
	// Fixed-period pacing (90 Hz default; presentation feedback refines
	// this in Phase 5).
	int64_t periodNs = 1000000000 / 90;
	int64_t lastPredictedNs = 0;
	// Retained projection-layer count from the last EndFrame (Phase 5
	// composites from this intake).
	uint32_t lastLayerCount = 0;
	// Queued session-state events (IDLE+READY at creation, SYNCHRONIZED
	// at begin, IDLE at end, EXITING at request-exit). hello_xr waits
	// for READY before beginning, so these are load-bearing.
	std::deque<XrEventDataSessionStateChanged> events;
};

// Runtime-owned swapchain images on the application device (Phase 4).
// The app renders into these; Phase 5 snapshots them for encode.
struct Swapchain
{
	Instance * owner = nullptr;
	Session * session = nullptr;
	int64_t format = 0; // VkFormat, validated at creation
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t arraySize = 1;
	XrSwapchainUsageFlags usage = 0;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	std::vector<VkImage> images;
	std::vector<VkDeviceMemory> memories;
	std::vector<VkFence> fences;
	std::vector<bool> busy;
	// Per-image "waited since acquire" (release GPU-signals only then;
	// submitting against a signaled fence is illegal).
	std::vector<bool> waited;
	// FIFO of outstanding (acquired, unreleased) image indices. Wait and
	// Release apply to the front; single flight in v1.
	std::vector<uint32_t> outstanding;
	uint32_t nextIndex = 0;
	// Most recently acquired (-1 when nothing outstanding) and released
	// images. EndFrame encodes the released one (release-after-EndFrame
	// apps cost one frame).
	int32_t lastAcquired = -1;
	int32_t lastReleased = -1;
};

// Process-wide singleton: v1 supports exactly one instance at a time
// (matches the single-headset streaming model).
struct Runtime
{
	std::mutex mutex;
	Instance * instance = nullptr;
};

Runtime & runtime_singleton();

// stderr logging with a stable prefix (no Monado u_logging here).
void log(const char * fmt, ...);

} // namespace wivrn_macos

// Storage for the opaque Khronos handle types. The headers declare
// e.g. `typedef struct XrInstance_T *XrInstance;` (XR_DEFINE_HANDLE);
// defining the structs here completes them for the runtime. Test apps
// never include this header, so there is exactly one definition.
struct XrInstance_T
{
	wivrn_macos::Instance data;
};
struct XrSession_T
{
	wivrn_macos::Session data;
};
struct XrActionSet_T
{
	wivrn_macos::ActionSet data;
};
struct XrAction_T
{
	wivrn_macos::Action data;
};
struct XrSpace_T
{
	wivrn_macos::Space data;
};
struct XrSwapchain_T
{
	wivrn_macos::Swapchain data;
};
