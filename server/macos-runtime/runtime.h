// Internal shared state for the macOS bespoke runtime (v1 MVP).
// No Monado headers here, ever. Only Khronos openxr/ headers + STL.

#pragma once

#include <openxr/openxr.h>

#include <cstdint>
#include <mutex>

namespace wivrn_macos
{

// v1 MVP runtime version (informational, surfaced via instance properties).
constexpr uint32_t runtime_version_major = 0;
constexpr uint32_t runtime_version_minor = 1;
constexpr uint32_t runtime_version_patch = 0;

// Single HMD system (v1: fixed stereo description; live values from the
// headset info packet arrive in Phase 3/5).
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
};

struct Session
{
	Instance * instance = nullptr;
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
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
