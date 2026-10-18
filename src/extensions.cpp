#include "runtime.h"

#include <algorithm>

namespace agentxr
{
namespace
{
constexpr uint32_t kHandJointCount = XR_HAND_JOINT_COUNT_EXT;
} // namespace

bool IsValidHandTracker(XrHandTrackerEXT handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kHandTrackerMagic && handle->alive && handle->object != nullptr && IsValidSession(handle->object->session->handle);
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateHandTrackerEXT(XrSession session, const XrHandTrackerCreateInfoEXT* createInfo, XrHandTrackerEXT* handTracker)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(createInfo, XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT) != XR_SUCCESS || handTracker == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if ((createInfo->hand != XR_HAND_LEFT_EXT && createInfo->hand != XR_HAND_RIGHT_EXT) || createInfo->handJointSet != XR_HAND_JOINT_SET_DEFAULT_EXT)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		auto* state = new HandTracker;
		state->session = &owner;
		state->hand = createInfo->hand;
		auto* handle = new XrHandTrackerEXT_T;
		handle->object = state;
		state->handle = handle;
		{
			std::lock_guard lock(owner.mutex);
			owner.handTrackers.push_back(handle);
		}
		*handTracker = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyHandTrackerEXT(XrHandTrackerEXT handTracker)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidHandTracker(handTracker))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		Session* owner = handTracker->object->session;
		{
			std::lock_guard lock(owner->mutex);
			owner->handTrackers.erase(std::remove(owner->handTrackers.begin(), owner->handTrackers.end(), handTracker), owner->handTrackers.end());
		}
		delete handTracker->object;
		handTracker->object = nullptr;
		handTracker->alive = false;
		return XR_SUCCESS;
	});
}

// Controllers are the only tracked devices, so a physical runtime reports hands as inactive while controllers are held.
extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrLocateHandJointsEXT(XrHandTrackerEXT handTracker, const XrHandJointsLocateInfoEXT* locateInfo, XrHandJointLocationsEXT* locations)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidHandTracker(handTracker))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(locateInfo, XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT) != XR_SUCCESS || CheckOutput(locations, XR_TYPE_HAND_JOINT_LOCATIONS_EXT) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (locations->jointCount != kHandJointCount || locations->jointLocations == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		XrHandJointVelocitiesEXT* velocities = nullptr;
		for (auto* next = reinterpret_cast<XrBaseOutStructure*>(locations->next); next != nullptr; next = next->next)
		{
			if (next->type == XR_TYPE_HAND_JOINT_VELOCITIES_EXT)
			{
				velocities = reinterpret_cast<XrHandJointVelocitiesEXT*>(next);
			}
		}
		if (velocities != nullptr && (velocities->jointCount != kHandJointCount || velocities->jointVelocities == nullptr))
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *handTracker->object->session;
		if (!IsValidSpace(locateInfo->baseSpace) || locateInfo->baseSpace->object->session != &owner)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (owner.ValidateTime(locateInfo->time) == 0)
		{
			return XR_ERROR_TIME_INVALID;
		}
		locations->isActive = XR_FALSE;
		for (uint32_t joint = 0; joint < kHandJointCount; ++joint)
		{
			locations->jointLocations[joint] = {};
			locations->jointLocations[joint].pose.orientation.w = 1.0f;
			if (velocities != nullptr)
			{
				velocities->jointVelocities[joint] = {};
			}
		}
		return XR_SUCCESS;
	});
}

} // namespace agentxr
