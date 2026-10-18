#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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

TrackedPose InvalidPose()
{
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
			tracked = ResolveInputPose(state, session.instance->PathString(bindingPath));
			return Compose(ToXrPose(tracked), space.poseInParent);
		}
	}
	tracked = InvalidPose();
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

bool ValidReferenceType(const Session& session, XrReferenceSpaceType type)
{
	return type == XR_REFERENCE_SPACE_TYPE_VIEW || type == XR_REFERENCE_SPACE_TYPE_LOCAL || type == XR_REFERENCE_SPACE_TYPE_STAGE || (type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && session.instance->localFloorEnabled);
}

} // namespace

namespace
{
constexpr XrDuration kVelocityHalfWindowNs = 500000;
constexpr std::string_view kProfileFile = "oculus-touch-controller.json";
constexpr std::string_view kProfileInteraction = "/interaction_profiles/oculus/touch_controller";
constexpr std::string_view kCalibrationHint = "measure it on a machine with a physical OpenXR runtime and Touch controllers: agent-xr-calibrate --out <this path> (see agent-xr/README.md, \"Device profile\")";

std::mutex gProfileMutex;
std::optional<DeviceProfile> gProfile;

std::filesystem::path ProfilePath(bool& fromEnvironment)
{
	std::vector<wchar_t> buffer(32768);
	const DWORD overrideLength = GetEnvironmentVariableW(L"AGENTXR_DEVICE_PROFILE", buffer.data(), static_cast<DWORD>(buffer.size()));
	fromEnvironment = overrideLength > 0;
	if (fromEnvironment)
	{
		return overrideLength < buffer.size() ? std::filesystem::path(buffer.data(), buffer.data() + overrideLength) : std::filesystem::path();
	}
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ProfilePath), &module))
	{
		return {};
	}
	const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size())
	{
		return {};
	}
	return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path() / "profiles" / std::string(kProfileFile);
}

bool ParseProfilePose(const protocol::Json& value, XrPosef& pose)
{
	if (!value.is_object() || !value.contains("position") || !value.contains("orientation"))
	{
		return false;
	}
	const protocol::Json& position = value.at("position");
	const protocol::Json& orientation = value.at("orientation");
	if (!position.is_array() || position.size() != 3 || !orientation.is_array() || orientation.size() != 4)
	{
		return false;
	}
	double p[3]{};
	double q[4]{};
	for (size_t index = 0; index < 3; ++index)
	{
		if (!position[index].is_number())
		{
			return false;
		}
		p[index] = position[index].get<double>();
		if (!std::isfinite(p[index]))
		{
			return false;
		}
	}
	for (size_t index = 0; index < 4; ++index)
	{
		if (!orientation[index].is_number())
		{
			return false;
		}
		q[index] = orientation[index].get<double>();
		if (!std::isfinite(q[index]))
		{
			return false;
		}
	}
	const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (std::fabs(length - 1.0) > 0.01)
	{
		return false;
	}
	pose.position = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
	pose.orientation = {static_cast<float>(q[0] / length), static_cast<float>(q[1] / length), static_cast<float>(q[2] / length), static_cast<float>(q[3] / length)};
	return true;
}

bool ParseProfileHand(const protocol::Json& root, const char* key, HandProfile& hand)
{
	return root.contains(key) && root.at(key).is_object() && ParseProfilePose(root.at(key).value("aimFromGrip", protocol::Json()), hand.aimFromGrip) && ParseProfilePose(root.at(key).value("palmFromGrip", protocol::Json()), hand.palmFromGrip);
}

bool ParseProfile(const protocol::Json& root, DeviceProfile& profile)
{
	return root.is_object() && root.value("interactionProfile", std::string()) == kProfileInteraction && root.contains("source") && root.at("source").is_object() && ParseProfileHand(root, "left", profile.left) && ParseProfileHand(root, "right", profile.right);
}

void LogProfileFailure(const std::filesystem::path& path, bool fromEnvironment, std::string_view reason)
{
	const std::string message = "AgentXR: device profile " + std::string(reason) + ": " + path.string() + (fromEnvironment ? " (from AGENTXR_DEVICE_PROFILE)" : " (default location beside the runtime DLL)") + "; " + std::string(kCalibrationHint) + "\n";
	std::fputs(message.c_str(), stderr);
	OutputDebugStringA(message.c_str());
}

XrQuaternionf Normalized(const XrQuaternionf& value)
{
	const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
	return {value.x / length, value.y / length, value.z / length, value.w / length};
}

void SampleRelative(Space& space, Space& base, XrTime time, XrPosef& pose, XrSpaceLocationFlags& flags)
{
	const SimState state = space.session->StateAt(time);
	TrackedPose objectTracking;
	TrackedPose baseTracking;
	const XrPosef objectWorld = WorldReferencePose(space, time, state, objectTracking);
	const XrPosef baseWorld = WorldReferencePose(base, time, state, baseTracking);
	pose = Compose(Inverse(baseWorld), objectWorld);
	flags = 0;
	if (objectTracking.orientationValid && baseTracking.orientationValid)
	{
		flags |= XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	}
	if (objectTracking.positionValid && baseTracking.positionValid)
	{
		flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
	}
	if (objectTracking.orientationTracked && baseTracking.orientationTracked)
	{
		flags |= XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	}
	if (objectTracking.positionTracked && baseTracking.positionTracked)
	{
		flags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	}
}

TrackedPose ComposeTracked(const TrackedPose& grip, const XrPosef& offset)
{
	TrackedPose result = FromXrPose(Compose(ToXrPose(grip), offset));
	result.positionValid = grip.positionValid;
	result.orientationValid = grip.orientationValid;
	result.positionTracked = grip.positionTracked;
	result.orientationTracked = grip.orientationTracked;
	return result;
}
} // namespace

bool LoadDeviceProfile()
{
	bool fromEnvironment = false;
	const std::filesystem::path path = ProfilePath(fromEnvironment);
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		LogProfileFailure(path, fromEnvironment, "missing");
		return false;
	}
	const protocol::Json root = protocol::Json::parse(file, nullptr, false);
	DeviceProfile profile;
	if (root.is_discarded() || !ParseProfile(root, profile))
	{
		LogProfileFailure(path, fromEnvironment, "invalid");
		return false;
	}
	std::lock_guard lock(gProfileMutex);
	gProfile = profile;
	return true;
}

DeviceProfile GetDeviceProfile()
{
	std::lock_guard lock(gProfileMutex);
	return gProfile.value();
}

TrackedPose ResolveInputPose(const SimState& state, std::string_view path)
{
	if (path == "/user/head")
	{
		return state.head;
	}
	constexpr std::string_view leftPrefix = "/user/hand/left/input/";
	constexpr std::string_view rightPrefix = "/user/hand/right/input/";
	const bool left = path.starts_with(leftPrefix);
	if (!left && !path.starts_with(rightPrefix))
	{
		return InvalidPose();
	}
	if (!(left ? state.left : state.right).active)
	{
		return InvalidPose();
	}
	const std::string_view input = path.substr(left ? leftPrefix.size() : rightPrefix.size());
	const TrackedPose& grip = left ? state.leftGrip : state.rightGrip;
	if (input == "grip/pose")
	{
		return grip;
	}
	const DeviceProfile profile = GetDeviceProfile();
	const HandProfile& hand = left ? profile.left : profile.right;
	if (input == "aim/pose")
	{
		return ComposeTracked(grip, hand.aimFromGrip);
	}
	if (input == "palm_ext/pose" || input == "grip_surface/pose")
	{
		return ComposeTracked(grip, hand.palmFromGrip);
	}
	return InvalidPose();
}

XrResult LocateRelative(Space& space, Space& base, XrTime time, bool withVelocity, RelativeMotion& motion)
{
	if (ValidateLocateTime(*space.session, time) != XR_SUCCESS)
	{
		return XR_ERROR_TIME_INVALID;
	}
	motion = {};
	SampleRelative(space, base, time, motion.pose, motion.locationFlags);
	if (!withVelocity)
	{
		return XR_SUCCESS;
	}
	XrPosef before;
	XrPosef after;
	XrSpaceLocationFlags beforeFlags = 0;
	XrSpaceLocationFlags afterFlags = 0;
	SampleRelative(space, base, time - kVelocityHalfWindowNs, before, beforeFlags);
	SampleRelative(space, base, time + kVelocityHalfWindowNs, after, afterFlags);
	const XrSpaceLocationFlags valid = motion.locationFlags & beforeFlags & afterFlags;
	const float seconds = static_cast<float>(2 * kVelocityHalfWindowNs) * 1.0e-9f;
	if ((valid & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
	{
		motion.velocityFlags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
		motion.linearVelocity = {(after.position.x - before.position.x) / seconds, (after.position.y - before.position.y) / seconds, (after.position.z - before.position.z) / seconds};
	}
	if ((valid & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
	{
		motion.velocityFlags |= XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
		// Rotation taking the earlier orientation to the later one, expressed in the base frame.
		XrQuaternionf delta = Normalized(Multiply(after.orientation, Conjugate(before.orientation)));
		if (delta.w < 0.0f)
		{
			delta = {-delta.x, -delta.y, -delta.z, -delta.w};
		}
		const float sine = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
		if (sine > 0.0f)
		{
			const float scale = 2.0f * std::atan2(sine, delta.w) / (sine * seconds);
			motion.angularVelocity = {delta.x * scale, delta.y * scale, delta.z * scale};
		}
	}
	return XR_SUCCESS;
}

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
		XrSpaceVelocity* velocity = nullptr;
		for (auto* next = reinterpret_cast<XrBaseOutStructure*>(location->next); next != nullptr; next = next->next)
		{
			if (next->type == XR_TYPE_SPACE_VELOCITY)
			{
				velocity = reinterpret_cast<XrSpaceVelocity*>(next);
			}
		}
		RelativeMotion motion;
		const XrResult located = LocateRelative(*space->object, *baseSpace->object, time, velocity != nullptr, motion);
		if (located != XR_SUCCESS)
		{
			return located;
		}
		location->pose = motion.pose;
		location->locationFlags = motion.locationFlags;
		if (velocity != nullptr)
		{
			velocity->velocityFlags = motion.velocityFlags;
			velocity->linearVelocity = motion.linearVelocity;
			velocity->angularVelocity = motion.angularVelocity;
		}
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* locateInfo, XrSpaceLocations* spaceLocations)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(locateInfo, XR_TYPE_SPACES_LOCATE_INFO) != XR_SUCCESS || CheckOutput(spaceLocations, XR_TYPE_SPACE_LOCATIONS) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (locateInfo->spaceCount == 0 || locateInfo->spaces == nullptr || spaceLocations->locationCount != locateInfo->spaceCount || spaceLocations->locations == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		XrSpaceVelocities* velocities = nullptr;
		for (auto* next = reinterpret_cast<XrBaseOutStructure*>(spaceLocations->next); next != nullptr; next = next->next)
		{
			if (next->type == XR_TYPE_SPACE_VELOCITIES)
			{
				velocities = reinterpret_cast<XrSpaceVelocities*>(next);
			}
		}
		if (velocities != nullptr && (velocities->velocityCount != locateInfo->spaceCount || velocities->velocities == nullptr))
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (!IsValidSpace(locateInfo->baseSpace) || locateInfo->baseSpace->object->session != session->object)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		for (uint32_t index = 0; index < locateInfo->spaceCount; ++index)
		{
			if (!IsValidSpace(locateInfo->spaces[index]) || locateInfo->spaces[index]->object->session != session->object)
			{
				return XR_ERROR_HANDLE_INVALID;
			}
		}
		if (ValidateLocateTime(*session->object, locateInfo->time) != XR_SUCCESS)
		{
			return XR_ERROR_TIME_INVALID;
		}
		for (uint32_t index = 0; index < locateInfo->spaceCount; ++index)
		{
			RelativeMotion motion;
			const XrResult located = LocateRelative(*locateInfo->spaces[index]->object, *locateInfo->baseSpace->object, locateInfo->time, velocities != nullptr, motion);
			if (located != XR_SUCCESS)
			{
				return located;
			}
			spaceLocations->locations[index].locationFlags = motion.locationFlags;
			spaceLocations->locations[index].pose = motion.pose;
			if (velocities != nullptr)
			{
				velocities->velocities[index].velocityFlags = motion.velocityFlags;
				velocities->velocities[index].linearVelocity = motion.linearVelocity;
				velocities->velocities[index].angularVelocity = motion.angularVelocity;
			}
		}
		return XR_SUCCESS;
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
