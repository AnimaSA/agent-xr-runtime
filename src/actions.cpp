#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace agentxr
{
namespace
{
constexpr std::string_view kLeftPath = "/user/hand/left";
constexpr std::string_view kRightPath = "/user/hand/right";
constexpr std::string_view kHeadPath = "/user/head";
constexpr std::string_view kProfilePath = "/interaction_profiles/oculus/touch_controller";
template<size_t Size>
bool ReadName(const char (&value)[Size], std::string& result)
{
	const char* end = std::find(value, value + Size, '\0');
	if (end == value || end == value + Size)
	{
		return false;
	}
	result.assign(value, end);
	return true;
}

struct CoreProfile
{
	std::string_view name;
	std::string_view user;
	std::string_view components;
	std::string_view leftOnly;
	std::string_view rightOnly;
};

constexpr CoreProfile kCoreProfiles[] = {
	{"/interaction_profiles/khr/simple_controller", "", "/input/select/click /input/menu/click /input/grip/pose /input/aim/pose /output/haptic", "", ""},
	{"/interaction_profiles/google/daydream_controller", "", "/input/select/click /input/trackpad/x /input/trackpad/y /input/trackpad/click /input/trackpad/touch /input/grip/pose /input/aim/pose", "", ""},
	{"/interaction_profiles/htc/vive_controller", "", "/input/system/click /input/squeeze/click /input/menu/click /input/trigger/click /input/trigger/value /input/trackpad/x /input/trackpad/y /input/trackpad/click /input/trackpad/touch /input/grip/pose /input/aim/pose /output/haptic", "", ""},
	{"/interaction_profiles/htc/vive_pro", "/user/head", "/input/system/click /input/volume_up/click /input/volume_down/click /input/mute_mic/click", "", ""},
	{"/interaction_profiles/microsoft/motion_controller", "", "/input/menu/click /input/squeeze/click /input/trigger/value /input/thumbstick/x /input/thumbstick/y /input/thumbstick/click /input/trackpad/x /input/trackpad/y /input/trackpad/click /input/trackpad/touch /input/grip/pose /input/aim/pose /output/haptic", "", ""},
	{"/interaction_profiles/microsoft/xbox_controller", "/user/gamepad", "/input/menu/click /input/view/click /input/a/click /input/b/click /input/x/click /input/y/click /input/dpad_down/click /input/dpad_right/click /input/dpad_up/click /input/dpad_left/click /input/shoulder_left/click /input/shoulder_right/click /input/thumbstick_left/click /input/thumbstick_right/click /input/trigger_left/value /input/trigger_right/value /input/thumbstick_left/x /input/thumbstick_left/y /input/thumbstick_right/x /input/thumbstick_right/y /output/haptic_left /output/haptic_right /output/haptic_left_trigger /output/haptic_right_trigger", "", ""},
	{"/interaction_profiles/oculus/go_controller", "", "/input/system/click /input/trigger/click /input/back/click /input/trackpad/x /input/trackpad/y /input/trackpad/click /input/trackpad/touch /input/grip/pose /input/aim/pose", "", ""},
	{kProfilePath, "", "/input/squeeze/value /input/trigger/value /input/trigger/touch /input/thumbstick/x /input/thumbstick/y /input/thumbstick/click /input/thumbstick/touch /input/thumbrest/touch /input/grip/pose /input/aim/pose /output/haptic", "/input/x/click /input/x/touch /input/y/click /input/y/touch /input/menu/click", "/input/a/click /input/a/touch /input/b/click /input/b/touch /input/system/click"},
	{"/interaction_profiles/valve/index_controller", "", "/input/system/click /input/system/touch /input/a/click /input/a/touch /input/b/click /input/b/touch /input/squeeze/value /input/squeeze/force /input/trigger/click /input/trigger/value /input/trigger/touch /input/thumbstick/x /input/thumbstick/y /input/thumbstick/click /input/thumbstick/touch /input/trackpad/x /input/trackpad/y /input/trackpad/force /input/trackpad/touch /input/grip/pose /input/aim/pose /output/haptic", "", ""}
};

const CoreProfile* FindProfile(std::string_view name)
{
	for (const CoreProfile& profile : kCoreProfiles)
	{
		if (profile.name == name)
		{
			return &profile;
		}
	}
	return nullptr;
}

bool HasComponent(std::string_view components, std::string_view suffix, bool allowParent)
{
	while (!components.empty())
	{
		const size_t end = components.find(' ');
		const std::string_view component = components.substr(0, end);
		if (component == suffix || (allowParent && suffix.starts_with("/input/") && component.substr(0, component.rfind('/')) == suffix))
		{
			return true;
		}
		if (end == std::string_view::npos)
		{
			break;
		}
		components.remove_prefix(end + 1);
	}
	return false;
}

bool ProfilePathSupported(const CoreProfile& profile, std::string_view path, bool allowParent = true)
{
	std::string_view user = profile.user;
	if (user.empty())
	{
		if (path.starts_with(kLeftPath) && path.size() > kLeftPath.size() && path[kLeftPath.size()] == '/')
		{
			user = kLeftPath;
		}
		else
		{
			user = kRightPath;
		}
	}
	if (!path.starts_with(user) || path.size() <= user.size() || path[user.size()] != '/')
	{
		return false;
	}
	const std::string_view suffix = path.substr(user.size());
	return HasComponent(profile.components, suffix, allowParent) || HasComponent(user == kLeftPath ? profile.leftOnly : profile.rightOnly, suffix, allowParent);
}

bool IsKnownTopLevel(const Instance& instance, XrPath path)
{
	const std::string value = instance.PathString(path);
	return value == kLeftPath || value == kRightPath || value == kHeadPath || value == "/user/gamepad" || value == "/user/treadmill";
}

bool ResolveTouchPath(Instance& instance, XrPath path, XrActionType type, Binding& binding)
{
	const CoreProfile& profile = kCoreProfiles[7];
	const std::string original = instance.PathString(path);
	std::string resolved = original;
	binding.input = {};
	binding.input.left = original.starts_with(kLeftPath);
	const size_t userSize = binding.input.left ? kLeftPath.size() : kRightPath.size();
	std::string_view suffix = std::string_view(original).substr(userSize);
	if (!ProfilePathSupported(profile, original, false))
	{
		const auto select = [&](std::string_view component)
		{
			const std::string candidate = original + std::string(component);
			if (!ProfilePathSupported(profile, candidate, false))
			{
				return false;
			}
			resolved = candidate;
			return true;
		};
		switch (type)
		{
			case XR_ACTION_TYPE_BOOLEAN_INPUT:
			{
				if (!select("/click") && !select("/value"))
				{
					return false;
				}
				break;
			}
			case XR_ACTION_TYPE_FLOAT_INPUT:
			{
				if (!select("/value") && !select("/click"))
				{
					return false;
				}
				break;
			}
			case XR_ACTION_TYPE_VECTOR2F_INPUT:
			{
				if (!ProfilePathSupported(profile, original + "/x", false) || !ProfilePathSupported(profile, original + "/y", false))
				{
					return false;
				}
				binding.input.kind = InputKind::Thumbstick;
				binding.path = path;
				return true;
			}
			case XR_ACTION_TYPE_POSE_INPUT:
			{
				if (!select("/pose"))
				{
					return false;
				}
				break;
			}
			default:
			{
				return false;
			}
		}
		suffix = std::string_view(resolved).substr(userSize);
	}
	binding.path = instance.InternPath(resolved);
	if (suffix == "/input/grip/pose" || suffix == "/input/aim/pose")
	{
		binding.input.kind = InputKind::Pose;
		return type == XR_ACTION_TYPE_POSE_INPUT;
	}
	if (suffix == "/output/haptic")
	{
		binding.input.kind = InputKind::Haptic;
		return type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
	}
	if (type != XR_ACTION_TYPE_BOOLEAN_INPUT && type != XR_ACTION_TYPE_FLOAT_INPUT)
	{
		return false;
	}
	if (suffix == "/input/trigger/value")
	{
		binding.input.kind = InputKind::Trigger;
		return true;
	}
	if (suffix == "/input/squeeze/value")
	{
		binding.input.kind = InputKind::Squeeze;
		return true;
	}
	if (suffix == "/input/thumbstick/x")
	{
		binding.input.kind = InputKind::ThumbstickX;
		return true;
	}
	if (suffix == "/input/thumbstick/y")
	{
		binding.input.kind = InputKind::ThumbstickY;
		return true;
	}
	const bool touch = suffix.ends_with("/touch");
	binding.input.kind = touch ? InputKind::Touch : InputKind::Button;
	if (suffix == "/input/a/click" || suffix == "/input/a/touch" || suffix == "/input/x/click" || suffix == "/input/x/touch")
	{
		binding.input.channel = 0;
		return true;
	}
	if (suffix == "/input/b/click" || suffix == "/input/b/touch" || suffix == "/input/y/click" || suffix == "/input/y/touch")
	{
		binding.input.channel = 1;
		return true;
	}
	if (suffix == "/input/menu/click")
	{
		binding.input.channel = 2;
		return true;
	}
	if (suffix == "/input/thumbstick/click")
	{
		binding.input.channel = binding.input.left ? 3 : 2;
		return true;
	}
	if (suffix == "/input/thumbstick/touch")
	{
		binding.input.channel = 2;
		return true;
	}
	if (suffix == "/input/trigger/touch")
	{
		binding.input.channel = 3;
		return true;
	}
	if (suffix == "/input/thumbrest/touch")
	{
		binding.input.channel = 4;
		return true;
	}
	return false;
}

XrPath SourcePath(Instance& instance, const Binding& binding)
{
	if (binding.subactionPath != XR_NULL_PATH)
	{
		return binding.subactionPath;
	}
	const std::string value = instance.PathString(binding.path);
	if (value.find(kLeftPath) == 0)
	{
		return instance.InternPath(kLeftPath);
	}
	if (value.find(kRightPath) == 0)
	{
		return instance.InternPath(kRightPath);
	}
	return XR_NULL_PATH;
}

bool IsActionAttached(const Session& session, const Action& action)
{
	return std::find(session.actionSets.begin(), session.actionSets.end(), action.actionSet->handle) != session.actionSets.end();
}

struct ReadValue
{
	bool active = false;
	bool booleanValue = false;
	float floatValue = 0.0f;
	XrVector2f vectorValue{0.0f, 0.0f};
};

ReadValue ReadBinding(const SimState& state, const Binding& binding)
{
	ReadValue result;
	const ControllerState& controller = binding.input.left ? state.left : state.right;
	result.active = controller.active;
	switch (binding.input.kind)
	{
	case InputKind::Button:
		if (binding.input.channel < controller.buttons.size())
		{
			result.booleanValue = controller.buttons[binding.input.channel];
		}
		break;
	case InputKind::Touch:
		if (binding.input.channel < controller.touches.size())
		{
			result.booleanValue = controller.touches[binding.input.channel];
		}
		break;
	case InputKind::Trigger:
		result.floatValue = controller.trigger;
		break;
	case InputKind::Squeeze:
		result.floatValue = controller.squeeze;
		break;
	case InputKind::Thumbstick:
		result.vectorValue = controller.thumbstick;
		break;
	case InputKind::ThumbstickX:
		result.floatValue = controller.thumbstick.x;
		result.vectorValue = controller.thumbstick;
		break;
	case InputKind::ThumbstickY:
		result.floatValue = controller.thumbstick.y;
		result.vectorValue = controller.thumbstick;
		break;
	case InputKind::Pose:
	case InputKind::Haptic:
		break;
	}
	return result;
}
bool IsSupportedActionType(XrActionType type)
{
	return type == XR_ACTION_TYPE_BOOLEAN_INPUT || type == XR_ACTION_TYPE_FLOAT_INPUT || type == XR_ACTION_TYPE_VECTOR2F_INPUT || type == XR_ACTION_TYPE_POSE_INPUT || type == XR_ACTION_TYPE_VIBRATION_OUTPUT;
}


bool MatchesSubaction(const Binding& binding, XrPath subactionPath)
{
	return subactionPath == XR_NULL_PATH || binding.subactionPath == XR_NULL_PATH || binding.subactionPath == subactionPath;
}

ActionSnapshot ReadAction(const Action& action, const SimState& state, XrPath subactionPath)
{
	ActionSnapshot result;
	for (const Binding& binding : action.bindings)
	{
		if (!MatchesSubaction(binding, subactionPath))
		{
			continue;
		}
		const ReadValue value = ReadBinding(state, binding);
		const bool booleanBinding = binding.input.kind == InputKind::Button || binding.input.kind == InputKind::Touch;
		result.active = result.active || value.active;
		switch (action.type)
		{
			case XR_ACTION_TYPE_BOOLEAN_INPUT:
			{
				if (value.active)
				{
					result.booleanValue = result.booleanValue || (booleanBinding ? value.booleanValue : value.floatValue > 0.5f);
				}
				break;
			}
			case XR_ACTION_TYPE_FLOAT_INPUT:
			{
				if (value.active)
				{
					result.floatValue = booleanBinding ? (value.booleanValue ? 1.0f : 0.0f) : value.floatValue;
				}
				break;
			}
			case XR_ACTION_TYPE_VECTOR2F_INPUT:
			{
				if (value.active)
				{
					result.vectorValue = value.vectorValue;
				}
				break;
			}
			case XR_ACTION_TYPE_POSE_INPUT:
			{
				break;
			}
			default:
			{
				break;
			}
		}
	}
	return result;
}

bool Changed(const ActionSnapshot& before, const ActionSnapshot& after, XrActionType type)
{
	if (!before.active || !after.active)
	{
		return false;
	}
	switch (type)
	{
		case XR_ACTION_TYPE_BOOLEAN_INPUT:
		{
			return before.booleanValue != after.booleanValue;
		}
		case XR_ACTION_TYPE_FLOAT_INPUT:
		{
			return before.floatValue != after.floatValue;
		}
		case XR_ACTION_TYPE_VECTOR2F_INPUT:
		{
			return before.vectorValue.x != after.vectorValue.x || before.vectorValue.y != after.vectorValue.y;
		}
		case XR_ACTION_TYPE_POSE_INPUT:
		{
			return false;
		}
		default:
		{
			return false;
		}
	}
}

XrResult ValidateActionRequest(const Session& owner, const XrActionStateGetInfo& getInfo, XrActionType expectedType)
{
	if (!IsValidAction(getInfo.action))
	{
		return XR_ERROR_HANDLE_INVALID;
	}
	Action& action = *getInfo.action->object;
	if (action.instance != owner.instance)
	{
		return XR_ERROR_HANDLE_INVALID;
	}
	if (action.type != expectedType)
	{
		return XR_ERROR_ACTION_TYPE_MISMATCH;
	}
	if (!IsActionAttached(owner, action))
	{
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	}
	if (getInfo.subactionPath != XR_NULL_PATH && std::find(action.subactionPaths.begin(), action.subactionPaths.end(), getInfo.subactionPath) == action.subactionPaths.end())
	{
		return XR_ERROR_PATH_UNSUPPORTED;
	}
	return XR_SUCCESS;
}
XrResult ValidateHapticRequest(const Session& owner, const XrHapticActionInfo& info)
{
	if (!IsValidAction(info.action))
	{
		return XR_ERROR_HANDLE_INVALID;
	}
	Action& action = *info.action->object;
	if (action.instance != owner.instance)
	{
		return XR_ERROR_HANDLE_INVALID;
	}
	if (action.type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
	{
		return XR_ERROR_ACTION_TYPE_MISMATCH;
	}
	if (!IsActionAttached(owner, action))
	{
		return XR_ERROR_ACTIONSET_NOT_ATTACHED;
	}
	if (info.subactionPath != XR_NULL_PATH && std::find(action.subactionPaths.begin(), action.subactionPaths.end(), info.subactionPath) == action.subactionPaths.end())
	{
		return XR_ERROR_PATH_UNSUPPORTED;
	}
	return XR_SUCCESS;
}
} // namespace

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString, XrPath* path)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (pathString == nullptr || path == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		size_t length = 0;
		while (length < XR_MAX_PATH_LENGTH && pathString[length] != '\0')
		{
			++length;
		}
		if (length == 0 || length >= XR_MAX_PATH_LENGTH || !IsAbsolutePath(std::string_view(pathString, length)))
		{
			return XR_ERROR_PATH_FORMAT_INVALID;
		}
		const XrPath result = instance->object->InternPath(std::string_view(pathString, length));
		if (result == XR_NULL_PATH)
		{
			return XR_ERROR_PATH_COUNT_EXCEEDED;
		}
		*path = result;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t capacity, uint32_t* count, char* buffer)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (count == nullptr || path == XR_NULL_PATH)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		const std::string value = instance->object->PathString(path);
		if (value.empty())
		{
			return XR_ERROR_PATH_INVALID;
		}
		*count = static_cast<uint32_t>(value.size() + 1);
		if (capacity == 0)
		{
			return XR_SUCCESS;
		}
		if (buffer == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (capacity < value.size() + 1)
		{
			return XR_ERROR_SIZE_INSUFFICIENT;
		}
		std::memcpy(buffer, value.c_str(), value.size() + 1);
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(createInfo, XR_TYPE_ACTION_SET_CREATE_INFO) != XR_SUCCESS || actionSet == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		std::string actionSetName;
		std::string localizedName;
		if (!ReadName(createInfo->actionSetName, actionSetName) || !ReadName(createInfo->localizedActionSetName, localizedName))
		{
			return XR_ERROR_NAME_INVALID;
		}
		std::lock_guard lock(instance->object->mutex);
		for (XrActionSet existing : instance->object->actionSets)
		{
			if (IsValidActionSet(existing) && existing->object->name == actionSetName)
			{
				return XR_ERROR_NAME_DUPLICATED;
			}
		}
		auto* state = new ActionSet;
		state->instance = instance->object;
		state->name = std::move(actionSetName);
		state->localizedName = std::move(localizedName);
		state->priority = createInfo->priority;
		auto* handle = new XrActionSet_T;
		handle->object = state;
		state->handle = handle;
		instance->object->actionSets.push_back(handle);
		*actionSet = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidActionSet(actionSet))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		ActionSet* state = actionSet->object;
		{
			std::lock_guard lock(state->instance->mutex);
			state->instance->actionSets.erase(std::remove(state->instance->actionSets.begin(), state->instance->actionSets.end(), actionSet), state->instance->actionSets.end());
			for (XrSession sessionHandle : state->instance->sessions)
			{
				if (IsValidSession(sessionHandle))
				{
					std::lock_guard sessionLock(sessionHandle->object->mutex);
					sessionHandle->object->actionSets.erase(std::remove(sessionHandle->object->actionSets.begin(), sessionHandle->object->actionSets.end(), actionSet), sessionHandle->object->actionSets.end());
				}
			}
		}
		for (XrAction action : state->actions)
		{
			if (action != XR_NULL_HANDLE && action->object != nullptr)
			{
				action->object->destroyed = true;
				delete action->object;
				action->object = nullptr;
				action->alive = false;
			}
		}
		delete state;
		actionSet->object = nullptr;
		actionSet->alive = false;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidActionSet(actionSet))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(createInfo, XR_TYPE_ACTION_CREATE_INFO) != XR_SUCCESS || action == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		std::string actionName;
		std::string localizedName;
		if (!ReadName(createInfo->actionName, actionName) || !ReadName(createInfo->localizedActionName, localizedName))
		{
			return XR_ERROR_NAME_INVALID;
		}
		if (!IsSupportedActionType(createInfo->actionType))
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (createInfo->countSubactionPaths > 16 || (createInfo->countSubactionPaths != 0 && createInfo->subactionPaths == nullptr))
		{
			return XR_ERROR_PATH_COUNT_EXCEEDED;
		}
		ActionSet& owner = *actionSet->object;
		std::vector<XrPath> subactionPaths;
		subactionPaths.reserve(createInfo->countSubactionPaths);
		for (uint32_t index = 0; index < createInfo->countSubactionPaths; ++index)
		{
			const XrPath subactionPath = createInfo->subactionPaths[index];
			if (!IsKnownTopLevel(*owner.instance, subactionPath) || std::find(subactionPaths.begin(), subactionPaths.end(), subactionPath) != subactionPaths.end())
			{
				return XR_ERROR_PATH_UNSUPPORTED;
			}
			subactionPaths.push_back(subactionPath);
		}
		std::lock_guard lock(owner.instance->mutex);
		for (XrAction existing : owner.actions)
		{
			if (IsValidAction(existing) && existing->object->name == actionName)
			{
				return XR_ERROR_NAME_DUPLICATED;
			}
		}
		auto* state = new Action;
		state->actionSet = &owner;
		state->instance = owner.instance;
		state->name = std::move(actionName);
		state->localizedName = std::move(localizedName);
		state->type = createInfo->actionType;
		state->subactionPaths = std::move(subactionPaths);
		auto* handle = new XrAction_T;
		handle->object = state;
		state->handle = handle;
		owner.actions.push_back(handle);
		*action = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidAction(action))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		Action* state = action->object;
		state->destroyed = true;
		state->actionSet->actions.erase(std::remove(state->actionSet->actions.begin(), state->actionSet->actions.end(), action), state->actionSet->actions.end());
		delete state;
		action->object = nullptr;
		action->alive = false;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(suggestedBindings, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (suggestedBindings->countSuggestedBindings > 10000 || (suggestedBindings->countSuggestedBindings != 0 && suggestedBindings->suggestedBindings == nullptr))
		{
			return suggestedBindings->countSuggestedBindings > 10000 ? XR_ERROR_LIMIT_REACHED : XR_ERROR_VALIDATION_FAILURE;
		}
		for (uint32_t index = 0; index < suggestedBindings->countSuggestedBindings; ++index)
		{
			const XrActionSuggestedBinding& binding = suggestedBindings->suggestedBindings[index];
			if (!IsValidAction(binding.action) || binding.action->object->instance != instance->object)
			{
				return XR_ERROR_HANDLE_INVALID;
			}
			if (binding.action->object->actionSet->everAttached)
			{
				return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
			}
		}
		const std::string profileName = instance->object->PathString(suggestedBindings->interactionProfile);
		if (profileName.empty())
		{
			return XR_ERROR_PATH_INVALID;
		}
		const CoreProfile* profile = FindProfile(profileName);
		if (profile == nullptr)
		{
			return XR_ERROR_PATH_UNSUPPORTED;
		}
		std::vector<std::pair<XrAction, XrPath>> bindings;
		bindings.reserve(suggestedBindings->countSuggestedBindings);
		for (uint32_t index = 0; index < suggestedBindings->countSuggestedBindings; ++index)
		{
			const XrActionSuggestedBinding& binding = suggestedBindings->suggestedBindings[index];
			const std::string path = instance->object->PathString(binding.binding);
			if (path.empty())
			{
				return XR_ERROR_PATH_INVALID;
			}
			if (!ProfilePathSupported(*profile, path))
			{
				return XR_ERROR_PATH_UNSUPPORTED;
			}
			const std::pair<XrAction, XrPath> entry{binding.action, binding.binding};
			if (std::find(bindings.begin(), bindings.end(), entry) == bindings.end())
			{
				bindings.push_back(entry);
			}
		}
		{
			std::lock_guard lock(instance->object->mutex);
			instance->object->suggestions[suggestedBindings->interactionProfile] = std::move(bindings);
		}
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(attachInfo, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO) != XR_SUCCESS || (attachInfo->countActionSets != 0 && attachInfo->actionSets == nullptr))
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::vector<XrActionSet> requested;
		requested.reserve(attachInfo->countActionSets);
		std::unordered_set<XrActionSet> unique;
		for (uint32_t index = 0; index < attachInfo->countActionSets; ++index)
		{
			XrActionSet actionSet = attachInfo->actionSets[index];
			if (!IsValidActionSet(actionSet) || actionSet->object->instance != owner.instance || !unique.insert(actionSet).second)
			{
				return XR_ERROR_HANDLE_INVALID;
			}
			requested.push_back(actionSet);
		}
		const XrPath leftPath = owner.instance->InternPath(kLeftPath);
		const XrPath rightPath = owner.instance->InternPath(kRightPath);
		std::lock_guard lock(owner.mutex);
		if (owner.actionSetsAttached)
		{
			return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
		}
		for (XrActionSet actionSet : requested)
		{
			owner.actionSets.push_back(actionSet);
			for (XrAction action : actionSet->object->actions)
			{
				action->object->bindings.clear();
			}
		}
		for (const auto& [profile, suggested] : owner.instance->suggestions)
		{
			if (owner.instance->PathString(profile) != kProfilePath)
			{
				continue;
			}
			for (const auto& [actionHandle, bindingPath] : suggested)
			{
				if (actionHandle == XR_NULL_HANDLE || actionHandle->object == nullptr)
				{
					continue;
				}
				Action& action = *actionHandle->object;
				if (std::find(owner.actionSets.begin(), owner.actionSets.end(), action.actionSet->handle) == owner.actionSets.end())
				{
					continue;
				}
				Binding binding;
				if (!ResolveTouchPath(*owner.instance, bindingPath, action.type, binding))
				{
					continue;
				}
				binding.subactionPath = binding.input.left ? leftPath : rightPath;
				action.bindings.push_back(binding);
			}
		}
		for (XrActionSet actionSet : requested)
		{
			actionSet->object->everAttached = true;
		}
		owner.actionSetsAttached = true;
		XrEventDataInteractionProfileChanged profileChanged{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED, nullptr, session};
		owner.instance->QueueEvent(reinterpret_cast<const XrEventDataBaseHeader*>(&profileChanged), sizeof(profileChanged));
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath, XrInteractionProfileState* interactionProfile)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckOutput(interactionProfile, XR_TYPE_INTERACTION_PROFILE_STATE) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		const std::string path = owner.instance->PathString(topLevelUserPath);
		if (path.empty())
		{
			return XR_ERROR_PATH_INVALID;
		}
		if (!IsKnownTopLevel(*owner.instance, topLevelUserPath))
		{
			return XR_ERROR_PATH_UNSUPPORTED;
		}
		const XrPath touch = owner.instance->InternPath(kProfilePath);
		std::lock_guard lock(owner.mutex);
		interactionProfile->interactionProfile = owner.actionSetsAttached && (path == kLeftPath || path == kRightPath) && owner.instance->suggestions.contains(touch) ? touch : XR_NULL_PATH;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(syncInfo, XR_TYPE_ACTIONS_SYNC_INFO) != XR_SUCCESS || (syncInfo->countActiveActionSets != 0 && syncInfo->activeActionSets == nullptr))
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		for (uint32_t index = 0; index < syncInfo->countActiveActionSets; ++index)
		{
			const XrActionSet actionSet = syncInfo->activeActionSets[index].actionSet;
			if (!IsValidActionSet(actionSet) || actionSet->object->instance != owner.instance)
			{
				return XR_ERROR_HANDLE_INVALID;
			}
		}
		if (!owner.actionSetsAttached)
		{
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		}
		for (uint32_t index = 0; index < syncInfo->countActiveActionSets; ++index)
		{
			const XrActiveActionSet& active = syncInfo->activeActionSets[index];
			if (std::find(owner.actionSets.begin(), owner.actionSets.end(), active.actionSet) == owner.actionSets.end())
			{
				return XR_ERROR_ACTIONSET_NOT_ATTACHED;
			}
			if (active.subactionPath != XR_NULL_PATH && !IsKnownTopLevel(*owner.instance, active.subactionPath))
			{
				return XR_ERROR_PATH_UNSUPPORTED;
			}
			for (uint32_t prior = 0; prior < index; ++prior)
			{
				const XrActiveActionSet& previous = syncInfo->activeActionSets[prior];
				if (previous.actionSet == active.actionSet && previous.subactionPath == active.subactionPath)
				{
					return XR_ERROR_VALIDATION_FAILURE;
				}
			}
		}
		const XrTime sampleTime = owner.instance->clock.Now();
		const SimState previousState = owner.lastSyncTime == 0 ? owner.fallbackState : owner.StateAtLocked(owner.lastSyncTime);
		if (owner.neutralizePending)
		{
			owner.inputsNeutralized = true;
			owner.neutralizePending = false;
		}
		const SimState state = owner.StateAtLocked(sampleTime);
		ActionSyncRecord record;
		record.time = sampleTime;
		record.focused = owner.IsFocused();
		record.result = record.focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
		std::array<bool, 16> deliveredButtons{};
		record.leftActive = state.left.active;
		record.rightActive = state.right.active;
		record.leftTrigger = state.left.trigger;
		record.rightTrigger = state.right.trigger;
		record.leftSqueeze = state.left.squeeze;
		record.rightSqueeze = state.right.squeeze;
		record.leftThumbstick = state.left.thumbstick;
		record.rightThumbstick = state.right.thumbstick;
		record.leftButtons = state.left.buttons;
		record.rightButtons = state.right.buttons;
		for (XrActionSet actionSetHandle : owner.actionSets)
		{
			ActionSet& actionSet = *actionSetHandle->object;
			bool actionSetActive = false;
			for (uint32_t index = 0; index < syncInfo->countActiveActionSets; ++index)
			{
				if (syncInfo->activeActionSets[index].actionSet == actionSetHandle)
				{
					actionSetActive = true;
					break;
				}
			}
			for (XrAction actionHandle : actionSet.actions)
			{
				Action& action = *actionHandle->object;
				ActionSnapshot aggregate;
				const size_t subactionCount = action.subactionPaths.empty() ? 1 : action.subactionPaths.size();
				for (size_t index = 0; index < subactionCount; ++index)
				{
					const XrPath subactionPath = action.subactionPaths.empty() ? XR_NULL_PATH : action.subactionPaths[index];
					bool subactionActive = actionSetActive;
					if (subactionActive && !action.subactionPaths.empty())
					{
						subactionActive = false;
						for (uint32_t activeIndex = 0; activeIndex < syncInfo->countActiveActionSets; ++activeIndex)
						{
							const XrActiveActionSet& active = syncInfo->activeActionSets[activeIndex];
							if (active.actionSet == actionSetHandle && (active.subactionPath == XR_NULL_PATH || active.subactionPath == subactionPath))
							{
								subactionActive = true;
								break;
							}
						}
					}
					const ActionSnapshot next = subactionActive && record.focused ? ReadAction(action, state, subactionPath) : ActionSnapshot{};
					const auto previousIt = action.snapshots.find(subactionPath);
					const ActionSnapshot previous = owner.lastSyncTime == 0 || previousIt == action.snapshots.end() ? ActionSnapshot{} : previousIt->second;
					ActionSnapshot snapshot = next;
					snapshot.changed = Changed(previous, next, action.type);
					snapshot.lastChangeTime = !next.active || !previous.active ? 0 : snapshot.changed ? sampleTime : previous.lastChangeTime;
					action.snapshots[subactionPath] = snapshot;
					aggregate.active = aggregate.active || snapshot.active;
					aggregate.booleanValue = aggregate.booleanValue || snapshot.booleanValue;
					if (snapshot.active)
					{
						aggregate.floatValue = snapshot.floatValue;
						aggregate.vectorValue = snapshot.vectorValue;
					}
					if (action.type == XR_ACTION_TYPE_BOOLEAN_INPUT && snapshot.changed)
					{
						++record.observedSamples;
						for (const Binding& binding : action.bindings)
						{
							if (MatchesSubaction(binding, subactionPath) && binding.input.kind == InputKind::Button && binding.input.channel < 8)
							{
								const ControllerState& before = binding.input.left ? previousState.left : previousState.right;
								const ControllerState& after = binding.input.left ? state.left : state.right;
								if (owner.lastSyncFocused && before.active && after.active && before.buttons[binding.input.channel] != after.buttons[binding.input.channel])
								{
									deliveredButtons[(binding.input.left ? 0 : 8) + binding.input.channel] = true;
								}
							}
						}
					}
				}
				const ActionSnapshot previousAggregate = owner.lastSyncTime == 0 ? ActionSnapshot{} : action.aggregate;
				aggregate.changed = Changed(previousAggregate, aggregate, action.type);
				aggregate.lastChangeTime = !aggregate.active || !previousAggregate.active ? 0 : aggregate.changed ? sampleTime : previousAggregate.lastChangeTime;
				action.aggregate = aggregate;
			}
		}
		bool collectEvidence = false;
		if (owner.activeEpoch != nullptr && owner.report.timelineId == owner.activeEpoch->id && owner.timelineStart > 0 && sampleTime >= owner.timelineStart)
		{
			const XrTime timelineEnd = owner.timelineStart + owner.activeEpoch->durationNs;
			const XrTime cutoff = owner.canceledAt.has_value() ? std::min(timelineEnd, *owner.canceledAt) : timelineEnd;
			const bool terminalSyncAlreadyRecorded = !owner.report.actionSyncs.empty() && owner.report.actionSyncs.back().time >= cutoff;
			collectEvidence = owner.lastSyncTime <= cutoff && !terminalSyncAlreadyRecorded;
		}
		if (collectEvidence)
		{
			const int64_t relative = std::max<int64_t>(0, sampleTime - owner.timelineStart);
			const int64_t previous = owner.lastSyncTime == 0 ? 0 : std::max<int64_t>(0, owner.lastSyncTime - owner.timelineStart);
			std::array<uint32_t, 16> transitionCounts{};
			for (const DiscreteChange& change : owner.activeEpoch->digitalChanges)
			{
				if (change.offsetNs > previous && change.offsetNs <= relative && change.channel < transitionCounts.size())
				{
					const size_t offset = change.left ? 0 : 8;
					++transitionCounts[offset + change.channel];
				}
			}
			for (size_t index = 0; index < 8; ++index)
			{
				if (transitionCounts[index] != 0 && deliveredButtons[index])
				{
					--transitionCounts[index];
				}
				if (transitionCounts[index + 8] != 0 && deliveredButtons[index + 8])
				{
					--transitionCounts[index + 8];
				}
				record.unobservedDigitalTransitions += transitionCounts[index] + transitionCounts[index + 8];
			}
		}
		owner.lastSyncTime = sampleTime;
		owner.lastSyncFocused = record.focused;
		if (collectEvidence)
		{
			owner.report.actionSyncs.push_back(record);
			if (owner.report.actionSyncs.size() > protocol::kMaxRecordsPerRun)
			{
				owner.report.actionSyncs.erase(owner.report.actionSyncs.begin());
				owner.report.overflow = true;
			}
			owner.report.observedSamples += record.observedSamples;
			owner.report.unobservedDigitalTransitions += record.unobservedDigitalTransitions;
		}
		return record.result;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateBoolean* state)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(getInfo, XR_TYPE_ACTION_STATE_GET_INFO) != XR_SUCCESS || CheckOutput(state, XR_TYPE_ACTION_STATE_BOOLEAN) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		const XrResult validation = ValidateActionRequest(owner, *getInfo, XR_ACTION_TYPE_BOOLEAN_INPUT);
		if (validation != XR_SUCCESS)
		{
			return validation;
		}
		Action& action = *getInfo->action->object;
		const auto found = action.snapshots.find(getInfo->subactionPath);
		const ActionSnapshot snapshot = owner.IsFocused() ? (found == action.snapshots.end() ? action.aggregate : found->second) : ActionSnapshot{};
		state->currentState = snapshot.booleanValue ? XR_TRUE : XR_FALSE;
		state->changedSinceLastSync = snapshot.changed ? XR_TRUE : XR_FALSE;
		state->lastChangeTime = snapshot.lastChangeTime;
		state->isActive = snapshot.active ? XR_TRUE : XR_FALSE;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(getInfo, XR_TYPE_ACTION_STATE_GET_INFO) != XR_SUCCESS || CheckOutput(state, XR_TYPE_ACTION_STATE_FLOAT) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		const XrResult validation = ValidateActionRequest(owner, *getInfo, XR_ACTION_TYPE_FLOAT_INPUT);
		if (validation != XR_SUCCESS)
		{
			return validation;
		}
		Action& action = *getInfo->action->object;
		const auto found = action.snapshots.find(getInfo->subactionPath);
		const ActionSnapshot snapshot = owner.IsFocused() ? (found == action.snapshots.end() ? action.aggregate : found->second) : ActionSnapshot{};
		state->currentState = snapshot.floatValue;
		state->changedSinceLastSync = snapshot.changed ? XR_TRUE : XR_FALSE;
		state->lastChangeTime = snapshot.lastChangeTime;
		state->isActive = snapshot.active ? XR_TRUE : XR_FALSE;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(getInfo, XR_TYPE_ACTION_STATE_GET_INFO) != XR_SUCCESS || CheckOutput(state, XR_TYPE_ACTION_STATE_VECTOR2F) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		const XrResult validation = ValidateActionRequest(owner, *getInfo, XR_ACTION_TYPE_VECTOR2F_INPUT);
		if (validation != XR_SUCCESS)
		{
			return validation;
		}
		Action& action = *getInfo->action->object;
		const auto found = action.snapshots.find(getInfo->subactionPath);
		const ActionSnapshot snapshot = owner.IsFocused() ? (found == action.snapshots.end() ? action.aggregate : found->second) : ActionSnapshot{};
		state->currentState = snapshot.vectorValue;
		state->changedSinceLastSync = snapshot.changed ? XR_TRUE : XR_FALSE;
		state->lastChangeTime = snapshot.lastChangeTime;
		state->isActive = snapshot.active ? XR_TRUE : XR_FALSE;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(getInfo, XR_TYPE_ACTION_STATE_GET_INFO) != XR_SUCCESS || CheckOutput(state, XR_TYPE_ACTION_STATE_POSE) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		const XrResult validation = ValidateActionRequest(owner, *getInfo, XR_ACTION_TYPE_POSE_INPUT);
		if (validation != XR_SUCCESS)
		{
			return validation;
		}
		Action& action = *getInfo->action->object;
		const auto found = action.snapshots.find(getInfo->subactionPath);
		const ActionSnapshot snapshot = owner.IsFocused() ? (found == action.snapshots.end() ? action.aggregate : found->second) : ActionSnapshot{};
		state->isActive = snapshot.active ? XR_TRUE : XR_FALSE;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo* enumerateInfo, uint32_t capacity, uint32_t* count, XrPath* sources)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(enumerateInfo, XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (!IsValidAction(enumerateInfo->action))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		Action& action = *enumerateInfo->action->object;
		if (action.instance != session->object->instance)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (!IsActionAttached(*session->object, action))
		{
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		}
		std::vector<XrPath> unique;
		unique.reserve(action.bindings.size());
		for (const Binding& binding : action.bindings)
		{
			const XrPath source = SourcePath(*action.instance, binding);
			if (source != XR_NULL_PATH && std::find(unique.begin(), unique.end(), source) == unique.end())
			{
				unique.push_back(source);
			}
		}
		return EnumerateArray(capacity, count, sources, sizeof(XrPath), unique.data(), static_cast<uint32_t>(unique.size()));
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo* getInfo, uint32_t capacity, uint32_t* count, char* buffer)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(getInfo, XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO) != XR_SUCCESS || count == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		const std::string source = session->object->instance->PathString(getInfo->sourcePath);
		if (source.empty() || (source != kLeftPath && source != kRightPath && source != kHeadPath))
		{
			return XR_ERROR_PATH_INVALID;
		}
		std::string text = source == kLeftPath ? "Left Controller" : source == kRightPath ? "Right Controller" : "Head";
		if ((getInfo->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT) != 0)
		{
			text += " input";
		}
		*count = static_cast<uint32_t>(text.size() + 1);
		if (capacity == 0)
		{
			return XR_SUCCESS;
		}
		if (buffer == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (capacity < text.size() + 1)
		{
			return XR_ERROR_SIZE_INSUFFICIENT;
		}
		std::memcpy(buffer, text.c_str(), text.size() + 1);
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* actionInfo, const XrHapticBaseHeader* feedback)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(actionInfo, XR_TYPE_HAPTIC_ACTION_INFO) != XR_SUCCESS || feedback == nullptr || feedback->type != XR_TYPE_HAPTIC_VIBRATION)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		if (!owner.running)
		{
			return XR_ERROR_SESSION_NOT_RUNNING;
		}
		const XrResult validation = ValidateHapticRequest(owner, *actionInfo);
		if (validation != XR_SUCCESS)
		{
			return validation;
		}
		const auto* vibration = reinterpret_cast<const XrHapticVibration*>(feedback);
		if (!std::isfinite(vibration->amplitude) || vibration->amplitude < 0.0f || vibration->amplitude > 1.0f || !std::isfinite(vibration->frequency) || vibration->frequency < 0.0f || (vibration->duration < XR_MIN_HAPTIC_DURATION || vibration->duration > 120000000000LL))
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (!owner.IsFocused())
		{
			return XR_SESSION_NOT_FOCUSED;
		}
		HapticRecord record;
		record.time = owner.instance->clock.Now();
		record.left = actionInfo->subactionPath == owner.instance->InternPath(kLeftPath);
		record.amplitude = vibration->amplitude;
		record.frequency = vibration->frequency;
		record.duration = vibration->duration;
		if (owner.activeEpoch != nullptr && owner.report.timelineId == owner.activeEpoch->id && owner.timelineStart > 0)
		{
			const XrTime timelineEnd = owner.timelineStart + owner.activeEpoch->durationNs;
			const XrTime cutoff = owner.canceledAt.has_value() ? std::min(timelineEnd, *owner.canceledAt) : timelineEnd;
			if (record.time >= owner.timelineStart && record.time <= cutoff)
			{
				owner.report.haptics.push_back(record);
				if (owner.report.haptics.size() > protocol::kMaxRecordsPerRun)
				{
					owner.report.haptics.erase(owner.report.haptics.begin());
					owner.report.overflow = true;
				}
			}
		}
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* actionInfo)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (CheckType(actionInfo, XR_TYPE_HAPTIC_ACTION_INFO) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		Session& owner = *session->object;
		std::lock_guard lock(owner.mutex);
		if (!owner.running)
		{
			return XR_ERROR_SESSION_NOT_RUNNING;
		}
		const XrResult validation = ValidateHapticRequest(owner, *actionInfo);
		if (validation != XR_SUCCESS)
		{
			return validation;
		}
		HapticRecord record;
		record.time = owner.instance->clock.Now();
		record.left = actionInfo->subactionPath == owner.instance->InternPath(kLeftPath);
		record.stopped = true;
		if (owner.activeEpoch != nullptr && owner.report.timelineId == owner.activeEpoch->id && owner.timelineStart > 0)
		{
			const XrTime timelineEnd = owner.timelineStart + owner.activeEpoch->durationNs;
			const XrTime cutoff = owner.canceledAt.has_value() ? std::min(timelineEnd, *owner.canceledAt) : timelineEnd;
			if (record.time >= owner.timelineStart && record.time <= cutoff)
			{
				owner.report.haptics.push_back(record);
				if (owner.report.haptics.size() > protocol::kMaxRecordsPerRun)
				{
					owner.report.haptics.erase(owner.report.haptics.begin());
					owner.report.overflow = true;
				}
			}
		}
		return XR_SUCCESS;
	});
}

} // namespace agentxr
