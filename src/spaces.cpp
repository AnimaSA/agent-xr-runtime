#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace agentxr
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf Conjugate(const XrQuaternionf& value)
{
	return {-value.x, -value.y, -value.z, value.w};
}

XrVector3f Add(const XrVector3f& a, const XrVector3f& b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

XrVector3f Rotate(const XrQuaternionf& rotation, const XrVector3f& value)
{
	const XrQuaternionf vector{value.x, value.y, value.z, 0.0f};
	const XrQuaternionf result = Multiply(Multiply(rotation, vector), Conjugate(rotation));
	return {result.x, result.y, result.z};
}

XrPosef Compose(const XrPosef& parent, const XrPosef& child)
{
	return {Multiply(parent.orientation, child.orientation), Add(parent.position, Rotate(parent.orientation, child.position))};
}

XrPosef Inverse(const XrPosef& value)
{
	const XrQuaternionf rotation = Conjugate(value.orientation);
	const XrVector3f translated{-value.position.x, -value.position.y, -value.position.z};
	return {rotation, Rotate(rotation, translated)};
}

XrPosef Identity()
{
	return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

XrSpaceLocationFlags FlagsFor(const TrackedPose& pose)
{
	XrSpaceLocationFlags flags = 0;
	if (pose.orientationValid)
	{
		flags |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	}
	if (pose.positionValid)
	{
		flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
	}
	if (pose.orientationTracked)
	{
		flags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	}
	if (pose.positionTracked)
	{
		flags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	}
	return flags;
}
bool IsAttached(const Session& session, const Action& action)
{
	return action.actionSet != nullptr && std::find(session.actionSets.begin(), session.actionSets.end(), action.actionSet->handle) != session.actionSets.end();
}

TrackedPose PoseFromState(const SimState& state, std::string_view path)
{
	if (path == "/user/head")
	{
		return state.head;
	}
	if (path == "/user/hand/left/input/grip/pose")
	{
		return state.leftGrip;
	}
	if (path == "/user/hand/left/input/aim/pose")
	{
		return state.leftAim;
	}
	if (path == "/user/hand/right/input/grip/pose")
	{
		return state.rightGrip;
	}
	if (path == "/user/hand/right/input/aim/pose")
	{
		return state.rightAim;
	}
	TrackedPose invalid;
	invalid.positionValid = false;
	invalid.orientationValid = false;
	invalid.positionTracked = false;
	invalid.orientationTracked = false;
	return invalid;
}

XrPosef WorldReferencePose(const Space& space, XrTime time, const SimState& state, TrackedPose& tracked)
{
	Session& session = *space.session;
	switch (space.kind)
	{
	case SpaceKind::Stage:
		tracked = state.head;
		return Compose(session.stageOrigin, space.poseInParent);
	case SpaceKind::Local:
		tracked = state.head;
		return Compose(session.localOrigin, space.poseInParent);
	case SpaceKind::LocalFloor:
		tracked = state.head;
		return Compose(session.localFloorOrigin, space.poseInParent);
	case SpaceKind::View:
		tracked = state.head;
		return Compose(ToXrPose(state.head), space.poseInParent);
	case SpaceKind::Action:
		{
			XrPath bindingPath = XR_NULL_PATH;
			if (space.action != XR_NULL_HANDLE && IsValidAction(space.action) && space.action->object->instance == session.instance && IsAttached(session, *space.action->object))
			{
				for (const Binding& binding : space.action->object->bindings)
				{
					if (binding.input.kind == InputKind::Pose && (space.subactionPath == XR_NULL_PATH || binding.subactionPath == space.subactionPath))
					{
						bindingPath = binding.path;
						break;
					}
				}
			}
			const std::string path = session.instance->PathString(bindingPath);
			tracked = PoseFromState(state, path);
			const bool left = path.find("/user/hand/left/") == 0;
			const bool right = path.find("/user/hand/right/") == 0;
			if ((left && !state.left.active) || (right && !state.right.active))
			{
				tracked.positionValid = false;
				tracked.orientationValid = false;
				tracked.positionTracked = false;
				tracked.orientationTracked = false;
			}
			return Compose(ToXrPose(tracked), space.poseInParent);
		}
	}
	tracked = PoseFromState(state, {});
	return Identity();
}

XrResult ValidateLocateTime(const Session& session, XrTime time)
{
	if (time <= 0)
	{
		return XR_ERROR_TIME_INVALID;
	}
	const XrTime now = session.instance->clock.Now();
	if (time > now + 10000000000LL)
	{
		return XR_ERROR_TIME_INVALID;
	}
	return XR_SUCCESS;
}

XrResult LocateInternal(Space& space, Space& base, XrTime time, XrSpaceLocation& location)
{
	Session& session = *space.session;
	if (ValidateLocateTime(session, time) != XR_SUCCESS)
	{
		return XR_ERROR_TIME_INVALID;
	}
	const SimState state = session.StateAt(time);
	TrackedPose objectTracking;
	TrackedPose baseTracking;
	const XrPosef objectWorld = WorldReferencePose(space, time, state, objectTracking);
	const XrPosef baseWorld = WorldReferencePose(base, time, state, baseTracking);
	const XrPosef relative = Compose(Inverse(baseWorld), objectWorld);
	location.pose = relative;
	location.locationFlags = 0;
	if (objectTracking.orientationValid && baseTracking.orientationValid)
	{
		location.locationFlags |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	}
	if (objectTracking.positionValid && baseTracking.positionValid)
	{
		location.locationFlags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
	}
	if (objectTracking.orientationTracked && baseTracking.orientationTracked)
	{
		location.locationFlags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	}
	if (objectTracking.positionTracked && baseTracking.positionTracked)
	{
		location.locationFlags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	}
	for (auto* next = reinterpret_cast<XrBaseOutStructure*>(location.next); next != nullptr; next = next->next)
	{
		if (next->type == XR_TYPE_SPACE_VELOCITY)
		{
			auto* velocity = reinterpret_cast<XrSpaceVelocity*>(next);
			velocity->velocityFlags = 0;
			velocity->linearVelocity = {0.0f, 0.0f, 0.0f};
			velocity->angularVelocity = {0.0f, 0.0f, 0.0f};
			if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
			{
				velocity->velocityFlags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
			}
			if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
			{
				velocity->velocityFlags |= XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
			}
		}
	}
	return XR_SUCCESS;
}

bool ValidReferenceType(const Session& session, XrReferenceSpaceType type)
{
	return type == XR_REFERENCE_SPACE_TYPE_VIEW || type == XR_REFERENCE_SPACE_TYPE_LOCAL || type == XR_REFERENCE_SPACE_TYPE_STAGE || (type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && session.instance->localFloorEnabled);
}

} // namespace

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		const std::array<XrReferenceSpaceType, 4> supported = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR};
		const uint32_t sourceCount = session->object->instance->localFloorEnabled ? 4u : 3u;
		return EnumerateArray(capacity, count, spaces, sizeof(XrReferenceSpaceType), supported.data(), sourceCount);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(createInfo, XR_TYPE_REFERENCE_SPACE_CREATE_INFO) != XR_SUCCESS || space == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		if (!ValidReferenceType(owner, createInfo->referenceSpaceType))
		{
			return createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR ? XR_ERROR_REFERENCE_SPACE_UNSUPPORTED : XR_ERROR_VALIDATION_FAILURE;
		}
		if (!IsFinitePose(createInfo->poseInReferenceSpace))
		{
			return XR_ERROR_POSE_INVALID;
		}
		auto* state = new Space;
		state->session = &owner;
		state->referenceType = createInfo->referenceSpaceType;
		state->poseInParent = createInfo->poseInReferenceSpace;
		state->kind = createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? SpaceKind::View : createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? SpaceKind::Local : createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? SpaceKind::Stage : SpaceKind::LocalFloor;
		auto* handle = new XrSpace_T;
		handle->object = state;
		state->handle = handle;
		{
			std::lock_guard lock(owner.mutex);
			owner.spaces.push_back(handle);
		}
		*space = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType, XrExtent2Df* bounds)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (bounds == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE && referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)
		{
			return XR_SPACE_BOUNDS_UNAVAILABLE;
		}
		if (referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && !session->object->instance->localFloorEnabled)
		{
			return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
		}
		bounds->width = 4.0f;
		bounds->height = 4.0f;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(createInfo, XR_TYPE_ACTION_SPACE_CREATE_INFO) != XR_SUCCESS || space == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (!IsValidAction(createInfo->action))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		Action& action = *createInfo->action->object;
		if (action.instance != session->object->instance)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (action.type != XR_ACTION_TYPE_POSE_INPUT)
		{
			return XR_ERROR_ACTION_TYPE_MISMATCH;
		}
		if (!IsAttached(*session->object, action))
		{
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		}
		if (createInfo->subactionPath != XR_NULL_PATH && std::find(action.subactionPaths.begin(), action.subactionPaths.end(), createInfo->subactionPath) == action.subactionPaths.end())
		{
			return XR_ERROR_PATH_UNSUPPORTED;
		}
		if (!IsFinitePose(createInfo->poseInActionSpace))
		{
			return XR_ERROR_POSE_INVALID;
		}
		auto* state = new Space;
		state->session = session->object;
		state->kind = SpaceKind::Action;
		state->referenceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		state->poseInParent = createInfo->poseInActionSpace;
		state->action = createInfo->action;
		state->subactionPath = createInfo->subactionPath;
		auto* handle = new XrSpace_T;
		handle->object = state;
		state->handle = handle;
		{
			std::lock_guard lock(session->object->mutex);
			session->object->spaces.push_back(handle);
		}
		*space = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSpace(space) || !IsValidSpace(baseSpace))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (space->object->session != baseSpace->object->session)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckOutput(location, XR_TYPE_SPACE_LOCATION) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		return LocateInternal(*space->object, *baseSpace->object, time, *location);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSpace(space))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		Session* owner = space->object->session;
		{
			std::lock_guard lock(owner->mutex);
			owner->spaces.erase(std::remove(owner->spaces.begin(), owner->spaces.end(), space), owner->spaces.end());
		}
		delete space->object;
		space->object = nullptr;
		space->alive = false;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo* locateInfo, XrViewState* viewState, uint32_t capacity, uint32_t* count, XrView* views)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(locateInfo, XR_TYPE_VIEW_LOCATE_INFO) != XR_SUCCESS || CheckOutput(viewState, XR_TYPE_VIEW_STATE) != XR_SUCCESS || count == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (locateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		{
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		}
		if (!IsValidSpace(locateInfo->space) || locateInfo->space->object->session != session->object)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (ValidateLocateTime(*session->object, locateInfo->displayTime) != XR_SUCCESS)
		{
			return XR_ERROR_TIME_INVALID;
		}
		*count = 2;
		viewState->viewStateFlags = 0;
		const SimState state = session->object->StateAt(locateInfo->displayTime);
		TrackedPose headTracking = state.head;
		const XrPosef headWorld = ToXrPose(state.head);
		TrackedPose baseTracking;
		const XrPosef baseWorld = WorldReferencePose(*locateInfo->space->object, locateInfo->displayTime, state, baseTracking);
		const XrPosef headRelative = Compose(Inverse(baseWorld), headWorld);
		if (headTracking.orientationValid && baseTracking.orientationValid)
		{
			viewState->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		}
		if (headTracking.positionValid && baseTracking.positionValid)
		{
			viewState->viewStateFlags |= XR_VIEW_STATE_POSITION_VALID_BIT;
		}
		if (headTracking.orientationTracked && baseTracking.orientationTracked)
		{
			viewState->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
		}
		if (headTracking.positionTracked && baseTracking.positionTracked)
		{
			viewState->viewStateFlags |= XR_VIEW_STATE_POSITION_TRACKED_BIT;
		}
		if (capacity == 0)
		{
			return XR_SUCCESS;
		}
		if (views == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		for (uint32_t index = 0; index < std::min(capacity, 2u); ++index)
		{
			if (CheckOutput(&views[index], XR_TYPE_VIEW) != XR_SUCCESS)
			{
				return XR_ERROR_VALIDATION_FAILURE;
			}
			const float eyeOffset = index == 0 ? -0.032f : 0.032f;
			XrPosef eye = headRelative;
			eye.position = Add(eye.position, Rotate(eye.orientation, {eyeOffset, 0.0f, 0.0f}));
			views[index].pose = eye;
			views[index].fov = {-kPi * 0.25f, kPi * 0.25f, kPi * 0.25f, -kPi * 0.25f};
		}
		return capacity < 2 ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
	});
}

} // namespace agentxr
