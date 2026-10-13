// Internal shared state for the macOS bespoke runtime (v1 MVP).
// No Monado headers here, ever. Only Khronos openxr/ headers + STL.

#pragma once

#include <openxr/openxr.h>

#include <cstdint>
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
	// Owned action/socket graph (freed on DestroyInstance).
	std::vector<std::unique_ptr<XrActionSet_T>> ownedSets;
	std::vector<std::unique_ptr<XrAction_T>> ownedActions;
	std::vector<std::unique_ptr<XrSpace_T>> ownedSpaces;
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
