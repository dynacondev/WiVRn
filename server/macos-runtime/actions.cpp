// Action sets/actions, path atoms, suggested bindings, sync + state.
// v1 models exactly the KHR_simple_controller component set; bindings
// from other profiles resolve through the same component names where
// they overlap (vive/index aliases work for the shared components).

#include "runtime.h"
#include "tracking.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace wivrn_macos
{
XrPath path_left()
{
	static XrPath path = XR_NULL_PATH;
	if (path == XR_NULL_PATH)
	{
		XrPath p = XR_NULL_PATH;
		extern XrResult string_to_path_global(const char *, XrPath *);
		string_to_path_global("/user/hand/left", &p);
		path = p;
	}
	return path;
}
XrPath path_right()
{
	static XrPath path = XR_NULL_PATH;
	if (path == XR_NULL_PATH)
	{
		XrPath p = XR_NULL_PATH;
		extern XrResult string_to_path_global(const char *, XrPath *);
		string_to_path_global("/user/hand/right", &p);
		path = p;
	}
	return path;
}
} // namespace wivrn_macos

namespace
{

// Process-wide path atoms (stable for the process lifetime; never freed).
std::mutex & paths_mutex()
{
	static std::mutex mutex;
	return mutex;
}
std::unordered_map<std::string, XrPath> & paths_by_name()
{
	static std::unordered_map<std::string, XrPath> map;
	return map;
}
std::vector<std::string> & paths_by_id()
{
	static std::vector<std::string> names;
	return names;
}

wivrn_macos::Instance * lookup(XrInstance instance)
{
	if (instance == nullptr)
		return nullptr;
	wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
	std::lock_guard lock(runtime.mutex);
	if (runtime.instance != &instance->data)
		return nullptr;
	return &instance->data;
}

wivrn_macos::Action * lookup_action(wivrn_macos::Instance * self, XrAction action)
{
	if (self == nullptr || action == nullptr)
		return nullptr;
	for (const auto & owned: self->ownedActions)
	{
		if (owned.get() == action)
			return &owned->data;
	}
	return nullptr;
}

const char * component_name(XrPath component)
{
	std::lock_guard lock(paths_mutex());
	auto & names = paths_by_id();
	if (component == XR_NULL_PATH || component > names.size())
		return nullptr;
	return names[component - 1].c_str();
}

// Resolve one (side, component) against the sample. Returns false when
// the component is outside the v1 simple-controller set (caller reports
// inactive rather than failing: the binding was accepted at suggest).
bool resolve_component(
        const wivrn_macos::TrackingSample & sample,
        bool left,
        const char * component,
        bool & boolean,
        float & scalar,
        XrVector2f & vec,
        XrPosef & pose)
{
	const wivrn_macos::ControllerSample & ctrl = left ? sample.left : sample.right;
	if (!ctrl.active)
		return false;
	// Component paths share names across simple/vive/index profiles, so
	// match the "/input/..." suffix wherever the user prefix ends
	// (e.g. "/user/hand/right/input/select/click" -> "select/click").
	const char * at = std::strstr(component, "/input/");
	if (at != nullptr)
	{
		const char * sub = at + std::strlen("/input/");
		auto is = [](const char * a, const char * b) { return std::strcmp(a, b) == 0; };
		if (is(sub, "select/click"))
		{
			boolean = ctrl.selectClick;
			return true;
		}
		if (is(sub, "menu/click"))
		{
			boolean = ctrl.menuClick;
			return true;
		}
		if (is(sub, "squeeze/click"))
		{
			boolean = ctrl.squeezeClick;
			return true;
		}
		if (is(sub, "trigger/value"))
		{
			scalar = ctrl.triggerValue;
			return true;
		}
		// Vive/index trigger/click aliases onto select.
		if (is(sub, "trigger/click"))
		{
			boolean = ctrl.selectClick;
			return true;
		}
		if (is(sub, "thumbstick/click"))
		{
			boolean = ctrl.thumbstickClick;
			return true;
		}
		if (is(sub, "thumbstick") || is(sub, "thumbstick/x") || is(sub, "thumbstick/y") || is(sub, "trackpad"))
		{
			vec = ctrl.thumbstick;
			return true;
		}
		if (is(sub, "grip/pose") || is(sub, "aim/pose"))
		{
			pose = ctrl.pose;
			return true;
		}
		return false;
	}
	boolean = false;
	scalar = 0;
	vec = {0, 0};
	pose = {{0, 0, 0, 1}, {0, 0, 0}};
	return false;
}

// Bindings matching (action, subaction-or-wildcard). Returns the first
// matching suggested binding or nullptr.
const wivrn_macos::SuggestedBinding * find_binding(
        const wivrn_macos::Instance * self,
        const wivrn_macos::Action * action,
        XrPath subaction)
{
	for (const auto & binding: self->bindings)
	{
		if (binding.action != action)
			continue;
		if (action->subactionPaths.empty() || subaction == XR_NULL_PATH)
			return &binding;
		for (XrPath p: action->subactionPaths)
		{
			if (p == subaction)
				return &binding;
		}
	}
	return nullptr;
}

bool session_uses(const wivrn_macos::Session & session, const wivrn_macos::Action * action)
{
	for (const wivrn_macos::ActionSet * set: session.attachedSets)
	{
		if (set == action->set)
			return true;
	}
	return false;
}

} // namespace

namespace wivrn_macos
{
XrResult string_to_path_global(const char * pathString, XrPath * path)
{
	if (pathString == nullptr || path == nullptr)
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(paths_mutex());
	auto & byName = paths_by_name();
	auto & byId = paths_by_id();
	auto it = byName.find(pathString);
	if (it != byName.end())
	{
		*path = it->second;
		return XR_SUCCESS;
	}
	XrPath id = byId.size() + 1;
	byName.emplace(pathString, id);
	byId.emplace_back(pathString);
	*path = id;
	return XR_SUCCESS;
}
} // namespace wivrn_macos

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_StringToPath(XrInstance instance, const char * pathString, XrPath * path)
	{
		if (instance == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		(void)self;
		return wivrn_macos::string_to_path_global(pathString, path);
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_PathToString(XrInstance instance, XrPath path, uint32_t bufferCapacityInput, uint32_t * bufferCountOutput, char * buffer)
	{
		if (instance == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || bufferCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		(void)self;
		std::lock_guard lock(paths_mutex());
		auto & byId = paths_by_id();
		if (path == XR_NULL_PATH || path > byId.size())
			return XR_ERROR_PATH_INVALID;
		const std::string & name = byId[path - 1];
		uint32_t needed = (uint32_t)name.size() + 1;
		*bufferCountOutput = needed;
		if (bufferCapacityInput == 0)
			return XR_SUCCESS;
		if (buffer == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (bufferCapacityInput < needed)
			return XR_ERROR_SIZE_INSUFFICIENT;
		std::memcpy(buffer, name.c_str(), needed);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_CreateActionSet(XrInstance instance, const XrActionSetCreateInfo * createInfo, XrActionSet * actionSet)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || createInfo == nullptr || actionSet == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_ACTION_SET_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		auto handle = std::make_unique<XrActionSet_T>();
		handle->data.name = createInfo->actionSetName;
		handle->data.localizedName = createInfo->localizedActionSetName;
		*actionSet = handle.get();
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		self->ownedSets.push_back(std::move(handle));
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyActionSet(XrActionSet actionSet)
	{
		if (actionSet == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		// Find the owning instance by scan (sets are few; v1 favors clarity).
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		if (runtime.instance == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		for (auto it = runtime.instance->ownedSets.begin(); it != runtime.instance->ownedSets.end(); ++it)
		{
			if (it->get() == actionSet)
			{
				// v1: actions must already be destroyed by the app; sessions
				// must be destroyed first (spec-undefined otherwise).
				runtime.instance->ownedSets.erase(it);
				return XR_SUCCESS;
			}
		}
		return XR_ERROR_HANDLE_INVALID;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_CreateAction(XrActionSet actionSet, const XrActionCreateInfo * createInfo, XrAction * action)
	{
		if (actionSet == nullptr || createInfo == nullptr || action == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_ACTION_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		switch (createInfo->actionType)
		{
			case XR_ACTION_TYPE_BOOLEAN_INPUT:
			case XR_ACTION_TYPE_FLOAT_INPUT:
			case XR_ACTION_TYPE_VECTOR2F_INPUT:
			case XR_ACTION_TYPE_POSE_INPUT:
			case XR_ACTION_TYPE_VIBRATION_OUTPUT:
				break;
			default:
				return XR_ERROR_ACTION_TYPE_MISMATCH;
		}
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		if (runtime.instance == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		auto handle = std::make_unique<XrAction_T>();
		handle->data.set = &actionSet->data;
		handle->data.type = createInfo->actionType;
		handle->data.name = createInfo->actionName;
		for (uint32_t i = 0; i < createInfo->countSubactionPaths; ++i)
			handle->data.subactionPaths.push_back(createInfo->subactionPaths[i]);
		*action = handle.get();
		actionSet->data.actions.push_back(&handle->data);
		runtime.instance->ownedActions.push_back(std::move(handle));
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroyAction(XrAction action)
	{
		if (action == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		if (runtime.instance == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		for (auto it = runtime.instance->ownedActions.begin(); it != runtime.instance->ownedActions.end(); ++it)
		{
			if (it->get() == action)
			{
				runtime.instance->ownedActions.erase(it);
				return XR_SUCCESS;
			}
		}
		return XR_ERROR_HANDLE_INVALID;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SuggestInteractionProfileBindings(
	        XrInstance instance,
	        const XrInteractionProfileSuggestedBinding * suggestedBindings)
	{
		wivrn_macos::Instance * self = lookup(instance);
		if (self == nullptr || suggestedBindings == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)
			return XR_ERROR_VALIDATION_FAILURE;
		if (suggestedBindings->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		// Replace any earlier suggestion for the same profile (spec: most
		// recent wins). Unknown profiles/components are accepted; unmapped
		// components resolve inactive (documented v1 limitation).
		std::erase_if(self->bindings, [&](const wivrn_macos::SuggestedBinding & b) {
			return b.profile == suggestedBindings->interactionProfile;
		});
		for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i)
		{
			const XrActionSuggestedBinding & entry = suggestedBindings->suggestedBindings[i];
			if (lookup_action(self, entry.action) == nullptr)
				return XR_ERROR_HANDLE_INVALID;
			self->bindings.push_back({
			        .profile = suggestedBindings->interactionProfile,
			        .userPath = XR_NULL_PATH, // v1: user path comes from the action's subactions at resolve time
			        .componentPath = entry.binding,
			        .action = &entry.action->data,
			});
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo * attachInfo)
	{
		if (session == nullptr || attachInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (attachInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (!session->data.attachedSets.empty())
			return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		for (uint32_t i = 0; i < attachInfo->countActionSets; ++i)
		{
			if (attachInfo->actionSets[i] == nullptr)
				return XR_ERROR_HANDLE_INVALID;
			bool owned = false;
			for (const auto & ownedSet: self->ownedSets)
			{
				if (ownedSet.get() == attachInfo->actionSets[i])
				{
					owned = true;
					break;
				}
			}
			if (!owned)
				return XR_ERROR_HANDLE_INVALID;
			if (attachInfo->actionSets[i]->data.attached)
				return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
		}
		for (uint32_t i = 0; i < attachInfo->countActionSets; ++i)
		{
			attachInfo->actionSets[i]->data.attached = true;
			session->data.attachedSets.push_back(&attachInfo->actionSets[i]->data);
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_SyncActions(XrSession session, const XrActionsSyncInfo * syncInfo)
	{
		if (session == nullptr || syncInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (syncInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		for (uint32_t i = 0; i < syncInfo->countActiveActionSets; ++i)
		{
			if (syncInfo->activeActionSets[i].actionSet == nullptr)
				return XR_ERROR_HANDLE_INVALID;
			bool attached = false;
			for (const wivrn_macos::ActionSet * set: session->data.attachedSets)
			{
				if (set == &syncInfo->activeActionSets[i].actionSet->data)
				{
					attached = true;
					break;
				}
			}
			if (!attached)
				return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		}
		session->data.lastSample = wivrn_macos::current_sample();
		session->data.everSynced = true;
		return XR_SUCCESS;
	}

	namespace
	{
	// Validate (session, action, subaction) for the Get* family. On success,
	// *side is set for hand subactions (left=true) and *binding is the first
	// matching suggestion (or nullptr when the action has no bindings yet).
	XrResult resolve_query(
	        XrSession session,
	        XrAction action,
	        XrPath subactionPath,
	        const wivrn_macos::SuggestedBinding *& binding,
	        bool & left)
	{
		binding = nullptr;
		left = true;
		if (session == nullptr || action == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Action * act = lookup_action(self, action);
		if (act == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (!session_uses(session->data, act))
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		if (!act->subactionPaths.empty())
		{
			if (subactionPath == XR_NULL_PATH)
			{
				// Wildcard: resolved per-subaction by the caller.
			}
			else
			{
				bool known = false;
				for (XrPath p: act->subactionPaths)
				{
					if (p == subactionPath)
					{
						known = true;
						break;
					}
				}
				if (!known)
					return XR_ERROR_PATH_UNSUPPORTED;
			}
		}
		else if (subactionPath != XR_NULL_PATH)
			return XR_ERROR_PATH_UNSUPPORTED;
		binding = find_binding(self, act, subactionPath);
		if (subactionPath != XR_NULL_PATH)
			left = (subactionPath == wivrn_macos::path_left());
		return XR_SUCCESS;
	}

	void fill_active(XrSession session, const wivrn_macos::Action * act, bool & isActive, XrTime & lastChange)
	{
		isActive = session->data.everSynced && act != nullptr;
		lastChange = isActive ? session->data.lastSample.timestampNs : 0;
	}

	} // namespace

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_GetActionStateBoolean(XrSession session, const XrActionStateGetInfo * getInfo, XrActionStateBoolean * state)
	{
		if (getInfo == nullptr || state == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (state->type != XR_TYPE_ACTION_STATE_BOOLEAN)
			return XR_ERROR_VALIDATION_FAILURE;
		const wivrn_macos::SuggestedBinding * binding = nullptr;
		bool left = true;
		XrResult res = resolve_query(session, getInfo->action, getInfo->subactionPath, binding, left);
		if (res != XR_SUCCESS)
			return res;
		wivrn_macos::Action * act = lookup_action(session->data.instance, getInfo->action);
		bool active = false;
		XrTime when = 0;
		fill_active(session, act, active, when);
		bool value = false;
		bool bound = false;
		auto accumulate = [&](XrPath sub, bool side) {
			const wivrn_macos::SuggestedBinding * b = find_binding(session->data.instance, act, sub);
			if (b == nullptr)
				return;
			const char * name = component_name(b->componentPath);
			if (name == nullptr)
				return;
			bool boolean = false;
			float scalar = 0;
			XrVector2f vec{0, 0};
			XrPosef pose{};
			if (!resolve_component(session->data.lastSample, side, name, boolean, scalar, vec, pose))
				return;
			bound = true;
			value = value || boolean;
		};
		if (getInfo->subactionPath == XR_NULL_PATH && !act->subactionPaths.empty())
		{
			for (XrPath sub: act->subactionPaths)
				accumulate(sub, sub == wivrn_macos::path_left());
		}
		else
			accumulate(getInfo->subactionPath, left);
		state->currentState = bound && value;
		state->changedSinceLastSync = false; // v1: edge detection deferred
		state->lastChangeTime = when;
		state->isActive = active && (binding != nullptr || bound);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_GetActionStateFloat(XrSession session, const XrActionStateGetInfo * getInfo, XrActionStateFloat * state)
	{
		if (getInfo == nullptr || state == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (state->type != XR_TYPE_ACTION_STATE_FLOAT)
			return XR_ERROR_VALIDATION_FAILURE;
		const wivrn_macos::SuggestedBinding * binding = nullptr;
		bool left = true;
		XrResult res = resolve_query(session, getInfo->action, getInfo->subactionPath, binding, left);
		if (res != XR_SUCCESS)
			return res;
		wivrn_macos::Action * act = lookup_action(session->data.instance, getInfo->action);
		bool active = false;
		XrTime when = 0;
		fill_active(session, act, active, when);
		float value = 0;
		bool bound = false;
		auto accumulate = [&](XrPath sub, bool side) {
			const wivrn_macos::SuggestedBinding * b = find_binding(session->data.instance, act, sub);
			if (b == nullptr)
				return;
			const char * name = component_name(b->componentPath);
			if (name == nullptr)
				return;
			bool boolean = false;
			float scalar = 0;
			XrVector2f vec{0, 0};
			XrPosef pose{};
			if (!resolve_component(session->data.lastSample, side, name, boolean, scalar, vec, pose))
				return;
			bound = true;
			if (boolean)
				scalar = 1;
			if (scalar > value)
				value = scalar;
		};
		if (getInfo->subactionPath == XR_NULL_PATH && !act->subactionPaths.empty())
		{
			for (XrPath sub: act->subactionPaths)
				accumulate(sub, sub == wivrn_macos::path_left());
		}
		else
			accumulate(getInfo->subactionPath, left);
		state->currentState = bound ? value : 0;
		state->changedSinceLastSync = false;
		state->lastChangeTime = when;
		state->isActive = active && (binding != nullptr || bound);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_GetActionStateVector2f(XrSession session, const XrActionStateGetInfo * getInfo, XrActionStateVector2f * state)
	{
		if (getInfo == nullptr || state == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (state->type != XR_TYPE_ACTION_STATE_VECTOR2F)
			return XR_ERROR_VALIDATION_FAILURE;
		const wivrn_macos::SuggestedBinding * binding = nullptr;
		bool left = true;
		XrResult res = resolve_query(session, getInfo->action, getInfo->subactionPath, binding, left);
		if (res != XR_SUCCESS)
			return res;
		wivrn_macos::Action * act = lookup_action(session->data.instance, getInfo->action);
		bool active = false;
		XrTime when = 0;
		fill_active(session, act, active, when);
		XrVector2f value{0, 0};
		bool bound = false;
		auto accumulate = [&](XrPath sub, bool side) {
			const wivrn_macos::SuggestedBinding * b = find_binding(session->data.instance, act, sub);
			if (b == nullptr)
				return;
			const char * name = component_name(b->componentPath);
			if (name == nullptr)
				return;
			bool boolean = false;
			float scalar = 0;
			XrVector2f vec{0, 0};
			XrPosef pose{};
			if (!resolve_component(session->data.lastSample, side, name, boolean, scalar, vec, pose))
				return;
			bound = true;
			float have = vec.x * vec.x + vec.y * vec.y;
			float got = value.x * value.x + value.y * value.y;
			if (have > got)
				value = vec;
		};
		if (getInfo->subactionPath == XR_NULL_PATH && !act->subactionPaths.empty())
		{
			for (XrPath sub: act->subactionPaths)
				accumulate(sub, sub == wivrn_macos::path_left());
		}
		else
			accumulate(getInfo->subactionPath, left);
		state->currentState = bound ? value : XrVector2f{0, 0};
		state->changedSinceLastSync = false;
		state->lastChangeTime = when;
		state->isActive = active && (binding != nullptr || bound);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_GetActionStatePose(XrSession session, const XrActionStateGetInfo * getInfo, XrActionStatePose * state)
	{
		if (getInfo == nullptr || state == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (state->type != XR_TYPE_ACTION_STATE_POSE)
			return XR_ERROR_VALIDATION_FAILURE;
		const wivrn_macos::SuggestedBinding * binding = nullptr;
		bool left = true;
		XrResult res = resolve_query(session, getInfo->action, getInfo->subactionPath, binding, left);
		if (res != XR_SUCCESS)
			return res;
		wivrn_macos::Action * act = lookup_action(session->data.instance, getInfo->action);
		bool active = false;
		XrTime when = 0;
		fill_active(session, act, active, when);
		(void)when;
		state->isActive = active && binding != nullptr;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateBoundSourcesForAction(
	        XrSession session,
	        const XrBoundSourcesForActionEnumerateInfo * enumerateInfo,
	        uint32_t sourceCapacityInput,
	        uint32_t * sourceCountOutput,
	        XrPath * sources)
	{
		if (enumerateInfo == nullptr || sourceCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (enumerateInfo->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (session == nullptr || enumerateInfo->action == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Action * act = lookup_action(self, enumerateInfo->action);
		if (act == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		// v1: report one source per binding of this action.
		std::vector<XrPath> found;
		for (const auto & binding: self->bindings)
		{
			if (binding.action != act)
				continue;
			found.push_back(binding.componentPath);
		}
		if (sourceCapacityInput == 0)
		{
			*sourceCountOutput = (uint32_t)found.size();
			return XR_SUCCESS;
		}
		if (sources == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (sourceCapacityInput < found.size())
			return XR_ERROR_SIZE_INSUFFICIENT;
		for (size_t i = 0; i < found.size(); ++i)
			sources[i] = found[i];
		*sourceCountOutput = (uint32_t)found.size();
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_ApplyHapticFeedback(XrSession session, const XrHapticActionInfo * hapticActionInfo, const XrHapticBaseHeader * hapticFeedback)
	{
		if (session == nullptr || hapticActionInfo == nullptr || hapticFeedback == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		// v1: accepted and dropped (headset haptics arrive with Phase 5).
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_StopHapticFeedback(XrSession session, const XrHapticActionInfo * hapticActionInfo)
	{
		if (session == nullptr || hapticActionInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetCurrentInteractionProfile(
	        XrSession session,
	        XrPath topLevelUserPath,
	        XrInteractionProfileState * interactionProfile)
	{
		if (session == nullptr || interactionProfile == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (interactionProfile->type != XR_TYPE_INTERACTION_PROFILE_STATE)
			return XR_ERROR_VALIDATION_FAILURE;
		(void)topLevelUserPath;
		// v1 tracks no live profile switching (single suggested set resolves
		// everything); report none rather than invent one.
		return XR_ERROR_PATH_UNSUPPORTED;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetInputSourceLocalizedName(
	        XrSession session,
	        const XrInputSourceLocalizedNameGetInfo * getInfo,
	        uint32_t bufferCapacityInput,
	        uint32_t * bufferCountOutput,
	        char * buffer)
	{
		if (session == nullptr || getInfo == nullptr || bufferCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (getInfo->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		(void)bufferCapacityInput;
		(void)buffer;
		// v1 synthesizes no display names (see GetCurrentInteractionProfile).
		return XR_ERROR_PATH_UNSUPPORTED;
	}

} // extern "C"
