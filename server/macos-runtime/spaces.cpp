// Spaces: reference (LOCAL/VIEW/LOCAL_FLOOR/STAGE) + action spaces,
// xrLocateSpace/xrLocateViews over the active tracking source.
// v1 origins: STAGE/LOCAL_FLOOR at the floor identity, LOCAL raised to
// head height (0, 1.6, 0), VIEW/action spaces live from the sample.

#include "runtime.h"
#include "tracking.h"

#include <cmath>

// Path atoms (defined in actions.cpp).
namespace wivrn_macos
{
XrPath path_left();
XrPath path_right();
} // namespace wivrn_macos

namespace
{

XrQuaternionf qmul(const XrQuaternionf & a, const XrQuaternionf & b)
{
	return {
	        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

XrVector3f qrot(const XrQuaternionf & q, const XrVector3f & v)
{
	// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
	XrVector3f qv{q.x, q.y, q.z};
	XrVector3f t{
	        2 * (qv.y * v.z - qv.z * v.y),
	        2 * (qv.z * v.x - qv.x * v.z),
	        2 * (qv.x * v.y - qv.y * v.x),
	};
	XrVector3f u{
	        t.x + 2 * q.w * v.x,
	        t.y + 2 * q.w * v.y,
	        t.z + 2 * q.w * v.z,
	};
	return {
	        v.x + (qv.y * u.z - qv.z * u.y),
	        v.y + (qv.z * u.x - qv.x * u.z),
	        v.z + (qv.x * u.y - qv.y * u.x),
	};
}

XrPosef pmul(const XrPosef & a, const XrPosef & b)
{
	XrVector3f rotated = qrot(a.orientation, b.position);
	return {
	        qmul(a.orientation, b.orientation),
	        {a.position.x + rotated.x, a.position.y + rotated.y, a.position.z + rotated.z},
	};
}

XrPosef pinv(const XrPosef & p)
{
	XrQuaternionf inv{-p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w};
	XrVector3f neg{-p.position.x, -p.position.y, -p.position.z};
	return {inv, qrot(inv, neg)};
}

constexpr XrPosef identity_pose()
{
	return {{0, 0, 0, 1}, {0, 0, 0}};
}

constexpr XrPosef local_origin()
{
	return {{0, 0, 0, 1}, {0, 1.6f, 0}};
}

const wivrn_macos::ControllerSample & side_sample(const wivrn_macos::TrackingSample & sample, XrPath subaction, const char *& side)
{
	// v1: exactly two hand subaction paths (NULL defaults to left for
	// subaction-less pose actions). Anything else is rejected by the
	// caller before reaching here.
	if (subaction == XR_NULL_PATH || subaction == wivrn_macos::path_left())
	{
		side = "left";
		return sample.left;
	}
	side = "right";
	return sample.right;
}

// Pose of a space expressed in STAGE coordinates.
XrPosef stage_pose(const wivrn_macos::Space * space, const wivrn_macos::TrackingSample & sample)
{
	if (space->kind == wivrn_macos::Space::Kind::Action)
	{
		const char * side = nullptr;
		const wivrn_macos::ControllerSample & ctrl = side_sample(sample, space->subaction, side);
		(void)side;
		return pmul(ctrl.pose, space->offset);
	}
	switch (space->refType)
	{
		case XR_REFERENCE_SPACE_TYPE_LOCAL:
			return local_origin();
		case XR_REFERENCE_SPACE_TYPE_VIEW:
			return sample.hmd;
		case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
		case XR_REFERENCE_SPACE_TYPE_STAGE:
		default:
			return identity_pose();
	}
}

// Pose of `space` relative to `base` (both via STAGE). Reference-space
// creation offsets compose under the space origin.
XrPosef locate_pose(const wivrn_macos::Space * space, const wivrn_macos::Space * base, const wivrn_macos::TrackingSample & sample)
{
	XrPosef baseInStage = stage_pose(base, sample);
	XrPosef spaceInStage = stage_pose(space, sample);
	if (space->kind == wivrn_macos::Space::Kind::Reference)
		spaceInStage = pmul(spaceInStage, space->offset);
	return pmul(pinv(baseInStage), spaceInStage);
}

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EnumerateReferenceSpaces(
	        XrSession session,
	        uint32_t spaceCapacityInput,
	        uint32_t * spaceCountOutput,
	        XrReferenceSpaceType * spaces)
	{
		if (session == nullptr || spaceCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		static constexpr XrReferenceSpaceType supported[] = {
		        XR_REFERENCE_SPACE_TYPE_LOCAL,
		        XR_REFERENCE_SPACE_TYPE_VIEW,
		        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
		        XR_REFERENCE_SPACE_TYPE_STAGE,
		};
		static constexpr uint32_t count = sizeof(supported) / sizeof(supported[0]);
		if (spaceCapacityInput == 0)
		{
			*spaceCountOutput = count;
			return XR_SUCCESS;
		}
		if (spaces == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (spaceCapacityInput < count)
			return XR_ERROR_SIZE_INSUFFICIENT;
		for (uint32_t i = 0; i < count; ++i)
			spaces[i] = supported[i];
		*spaceCountOutput = count;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_CreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo * createInfo, XrSpace * space)
	{
		if (session == nullptr || createInfo == nullptr || space == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		switch (createInfo->referenceSpaceType)
		{
			case XR_REFERENCE_SPACE_TYPE_LOCAL:
			case XR_REFERENCE_SPACE_TYPE_VIEW:
			case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
			case XR_REFERENCE_SPACE_TYPE_STAGE:
				break;
			default:
				return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
		}
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		auto handle = std::make_unique<XrSpace_T>();
		handle->data.kind = wivrn_macos::Space::Kind::Reference;
		handle->data.refType = createInfo->referenceSpaceType;
		handle->data.offset = createInfo->poseInReferenceSpace;
		handle->data.owner = self;
		// Reference-space offsets apply at locate time (composed under the
		// space origin); stored verbatim here.
		*space = handle.get();
		{
			wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
			std::lock_guard lock(runtime.mutex);
			self->ownedSpaces.push_back(std::move(handle));
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo * createInfo, XrSpace * space)
	{
		if (session == nullptr || createInfo == nullptr || space == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->type != XR_TYPE_ACTION_SPACE_CREATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (createInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->action == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		// The action must belong to this instance and be a pose input.
		bool owned = false;
		for (const auto & a: self->ownedActions)
		{
			if (a.get() == createInfo->action)
			{
				owned = true;
				break;
			}
		}
		if (!owned)
			return XR_ERROR_HANDLE_INVALID;
		if (createInfo->action->data.type != XR_ACTION_TYPE_POSE_INPUT)
			return XR_ERROR_ACTION_TYPE_MISMATCH;
		// Subaction must be explicit when the action has subaction paths.
		const auto & subs = createInfo->action->data.subactionPaths;
		if (!subs.empty())
		{
			bool known = false;
			for (XrPath p: subs)
			{
				if (p == createInfo->subactionPath)
				{
					known = true;
					break;
				}
			}
			if (!known)
				return XR_ERROR_PATH_UNSUPPORTED;
		}
		else if (createInfo->subactionPath != XR_NULL_PATH)
			return XR_ERROR_PATH_UNSUPPORTED;
		auto handle = std::make_unique<XrSpace_T>();
		handle->data.kind = wivrn_macos::Space::Kind::Action;
		handle->data.action = &createInfo->action->data;
		handle->data.subaction = createInfo->subactionPath;
		handle->data.offset = createInfo->poseInActionSpace;
		handle->data.owner = self;
		*space = handle.get();
		{
			wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
			std::lock_guard lock(runtime.mutex);
			self->ownedSpaces.push_back(std::move(handle));
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_DestroySpace(XrSpace space)
	{
		if (space == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = space->data.owner;
		if (self == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Runtime & runtime = wivrn_macos::runtime_singleton();
		std::lock_guard lock(runtime.mutex);
		for (auto it = self->ownedSpaces.begin(); it != self->ownedSpaces.end(); ++it)
		{
			if (it->get() == space)
			{
				self->ownedSpaces.erase(it);
				return XR_SUCCESS;
			}
		}
		return XR_ERROR_HANDLE_INVALID;
	}

	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_LocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation * location)
	{
		if (location == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (time <= 0)
			return XR_ERROR_TIME_INVALID;
		if (location->type != XR_TYPE_SPACE_LOCATION)
			return XR_ERROR_VALIDATION_FAILURE;
		if (space == nullptr || baseSpace == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = space->data.owner;
		if (self == nullptr || baseSpace->data.owner != self)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::TrackingSample sample = wivrn_macos::current_sample();
		location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
		                          XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
		location->pose = locate_pose(&space->data, &baseSpace->data, sample);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_LocateSpaces(
	        XrSession session,
	        const XrSpacesLocateInfo * locateInfo,
	        XrSpaceLocations * spaceLocations)
	{
		if (session == nullptr || locateInfo == nullptr || spaceLocations == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (locateInfo->type != XR_TYPE_SPACES_LOCATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (locateInfo->next != nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (spaceLocations->type != XR_TYPE_SPACE_LOCATIONS)
			return XR_ERROR_VALIDATION_FAILURE;
		if (locateInfo->baseSpace == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr || locateInfo->baseSpace->data.owner != self)
			return XR_ERROR_HANDLE_INVALID;
		if (spaceLocations->locationCount != locateInfo->spaceCount)
			return XR_ERROR_VALIDATION_FAILURE;
		if (locateInfo->time <= 0)
			return XR_ERROR_TIME_INVALID;
		wivrn_macos::TrackingSample sample = wivrn_macos::current_sample();
		for (uint32_t i = 0; i < locateInfo->spaceCount; ++i)
		{
			XrSpace space = locateInfo->spaces[i];
			if (space == nullptr || space->data.owner != self)
				return XR_ERROR_HANDLE_INVALID;
			spaceLocations->locations[i].locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
			                                             XR_SPACE_LOCATION_POSITION_VALID_BIT |
			                                             XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
			                                             XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
			spaceLocations->locations[i].pose = locate_pose(&space->data, &locateInfo->baseSpace->data, sample);
		}
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_GetReferenceSpaceBoundsRect(
	        XrSession session,
	        XrReferenceSpaceType referenceSpaceType,
	        XrExtent2Df * bounds)
	{
		if (session == nullptr || bounds == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		switch (referenceSpaceType)
		{
			case XR_REFERENCE_SPACE_TYPE_LOCAL:
			case XR_REFERENCE_SPACE_TYPE_VIEW:
			case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
			case XR_REFERENCE_SPACE_TYPE_STAGE:
				break;
			default:
				return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
		}
		// v1 reports an unbounded play space (zeros).
		bounds->width = 0;
		bounds->height = 0;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_LocateViews(
	        XrSession session,
	        const XrViewLocateInfo * viewLocateInfo,
	        XrViewState * viewState,
	        uint32_t viewCapacityInput,
	        uint32_t * viewCountOutput,
	        XrView * views)
	{
		if (session == nullptr || viewLocateInfo == nullptr || viewState == nullptr || viewCountOutput == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		if (viewLocateInfo->displayTime <= 0)
			return XR_ERROR_TIME_INVALID;
		if (viewLocateInfo->space == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		wivrn_macos::Instance * self = session->data.instance;
		if (self == nullptr || viewLocateInfo->space->data.owner != self)
			return XR_ERROR_HANDLE_INVALID;
		if (viewCapacityInput == 0)
		{
			*viewCountOutput = wivrn_macos::SystemInfo::viewCount;
			return XR_SUCCESS;
		}
		if (views == nullptr)
			return XR_ERROR_VALIDATION_FAILURE;
		if (viewCapacityInput < wivrn_macos::SystemInfo::viewCount)
			return XR_ERROR_SIZE_INSUFFICIENT;

		wivrn_macos::TrackingSample sample = wivrn_macos::current_sample();
		XrPosef baseInStage = stage_pose(&viewLocateInfo->space->data, sample);
		if (viewLocateInfo->space->data.kind == wivrn_macos::Space::Kind::Reference)
			baseInStage = pmul(baseInStage, viewLocateInfo->space->data.offset);
		XrPosef stageInBase = pinv(baseInStage);

		static constexpr float eyeX[wivrn_macos::SystemInfo::viewCount] = {-0.032f, 0.032f};
		static constexpr float fovAngle = 0.86f; // ~49deg symmetric (v1 default)
		for (uint32_t i = 0; i < wivrn_macos::SystemInfo::viewCount; ++i)
		{
			XrPosef eyeInHead{{0, 0, 0, 1}, {eyeX[i], 0, 0}};
			XrPosef eyeInStage = pmul(sample.hmd, eyeInHead);
			views[i].type = XR_TYPE_VIEW;
			views[i].next = nullptr;
			views[i].pose = pmul(stageInBase, eyeInStage);
			views[i].fov = {fovAngle, fovAngle, fovAngle, fovAngle};
		}
		*viewCountOutput = wivrn_macos::SystemInfo::viewCount;
		viewState->type = XR_TYPE_VIEW_STATE;
		viewState->next = nullptr;
		viewState->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
		                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
		return XR_SUCCESS;
	}

} // extern "C"
