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

bool IsKnownTopLevel(const Instance& instance, XrPath path)
{
	const std::string value = instance.PathString(path);
	return value == kLeftPath || value == kRightPath || value == kHeadPath;
}

bool IsSupportedInputPath(const Instance& instance, XrPath path, InputRef& input)
{
	const std::string value = instance.PathString(path);
	if (value.empty())
	{
		return false;
	}
	input = {};
	const bool left = value.size() > kLeftPath.size() && value.starts_with(kLeftPath) && value[kLeftPath.size()] == '/';
	const bool right = value.size() > kRightPath.size() && value.starts_with(kRightPath) && value[kRightPath.size()] == '/';
	if (left)
	{
		input.left = true;
	}
	else if (!right)
	{
		return false;
	}
	if (value.ends_with("/output/haptic"))
	{
		input.kind = InputKind::Haptic;
		return true;
	}
	if (value.ends_with("/trigger/value"))
	{
		input.kind = InputKind::Trigger;
		return true;
	}
	if (value.ends_with("/squeeze/value"))
	{
		input.kind = InputKind::Squeeze;
		return true;
	}
	if (value.ends_with("/thumbstick"))
	{
		input.kind = InputKind::Thumbstick;
		return true;
	}
	if (value.ends_with("/thumbstick/x"))
	{
		input.kind = InputKind::ThumbstickX;
		return true;
	}
	if (value.ends_with("/thumbstick/y"))
	{
		input.kind = InputKind::ThumbstickY;
		return true;
	}
	if (value.ends_with("/thumbstick/click"))
	{
		input.kind = InputKind::Button;
		input.channel = 3;
		return true;
	}
	if (value.ends_with("/thumbstick/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 2;
		return true;
	}
	if (value.ends_with("/thumbrest/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 4;
		return true;
	}
	if (value.ends_with("/trigger/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 3;
		return true;
	}
	if (value.ends_with("/x/click"))
	{
		input.kind = InputKind::Button;
		input.channel = 0;
		return input.left;
	}
	if (value.ends_with("/y/click"))
	{
		input.kind = InputKind::Button;
		input.channel = 1;
		return input.left;
	}
	if (value.ends_with("/menu/click"))
	{
		input.kind = InputKind::Button;
		input.channel = 2;
		return input.left;
	}
	if (value.ends_with("/a/click"))
	{
		input.kind = InputKind::Button;
		input.channel = 0;
		return !input.left;
	}
	if (value.ends_with("/b/click"))
	{
		input.kind = InputKind::Button;
		input.channel = 1;
		return !input.left;
	}
	if (value.ends_with("/x/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 0;
		return input.left;
	}
	if (value.ends_with("/y/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 1;
		return input.left;
	}
	if (value.ends_with("/a/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 0;
		return !input.left;
	}
	if (value.ends_with("/b/touch"))
	{
		input.kind = InputKind::Touch;
		input.channel = 1;
		return !input.left;
	}
	return false;
}

bool IsPosePath(const Instance& instance, XrPath path)
{
	const std::string value = instance.PathString(path);
	return value == kHeadPath || value == "/user/hand/left/input/grip/pose" || value == "/user/hand/right/input/grip/pose" || value == "/user/hand/left/input/aim/pose" || value == "/user/hand/right/input/aim/pose";
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
	return instance.InternPath(kHeadPath);
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
	if (binding.input.kind == InputKind::Pose && binding.input.head)
	{
		result.active = state.head.positionValid && state.head.orientationValid;
		return result;
	}
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

bool BindingTypeCompatible(InputKind kind, XrActionType actionType)
{
	switch (kind)
	{
	case InputKind::Button:
	case InputKind::Touch:
	case InputKind::Trigger:
	case InputKind::Squeeze:
	case InputKind::ThumbstickX:
	case InputKind::ThumbstickY:
		return actionType == XR_ACTION_TYPE_BOOLEAN_INPUT || actionType == XR_ACTION_TYPE_FLOAT_INPUT;
	case InputKind::Thumbstick:
		return actionType == XR_ACTION_TYPE_VECTOR2F_INPUT;
	case InputKind::Pose:
		return actionType == XR_ACTION_TYPE_POSE_INPUT;
	case InputKind::Haptic:
		return actionType == XR_ACTION_TYPE_VIBRATION_OUTPUT;
	}
	return false;
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
			result.booleanValue = result.booleanValue || (booleanBinding ? value.booleanValue : value.floatValue > 0.5f);
			break;
		case XR_ACTION_TYPE_FLOAT_INPUT:
			if (value.active)
			{
				result.floatValue = booleanBinding ? (value.booleanValue ? 1.0f : 0.0f) : value.floatValue;
			}
			break;
		case XR_ACTION_TYPE_VECTOR2F_INPUT:
			if (value.active)
			{
				result.vectorValue = value.vectorValue;
			}
			break;
		case XR_ACTION_TYPE_POSE_INPUT:
			break;
		default:
			break;
		}
	}
	return result;
}

bool Changed(const ActionSnapshot& before, const ActionSnapshot& after, XrActionType type)
{
	if (before.active != after.active)
	{
		return true;
	}
	switch (type)
	{
	case XR_ACTION_TYPE_BOOLEAN_INPUT:
		return before.booleanValue != after.booleanValue;
	case XR_ACTION_TYPE_FLOAT_INPUT:
		return before.floatValue != after.floatValue;
	case XR_ACTION_TYPE_VECTOR2F_INPUT:
		return before.vectorValue.x != after.vectorValue.x || before.vectorValue.y != after.vectorValue.y;
	case XR_ACTION_TYPE_POSE_INPUT:
		return false;
	default:
		return false;
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
		}
		const std::string profile = instance->object->PathString(suggestedBindings->interactionProfile);
		if (profile.empty())
		{
			return XR_ERROR_PATH_INVALID;
		}
		if (profile != kProfilePath)
		{
			return XR_SUCCESS;
		}
		std::vector<std::pair<XrAction, XrPath>> bindings;
		bindings.reserve(suggestedBindings->countSuggestedBindings);
		for (uint32_t index = 0; index < suggestedBindings->countSuggestedBindings; ++index)
		{
			const XrActionSuggestedBinding& binding = suggestedBindings->suggestedBindings[index];
			InputRef input;
			const bool pose = IsPosePath(*instance->object, binding.binding);
			const bool value = IsSupportedInputPath(*instance->object, binding.binding, input);
			if (!pose && !value)
			{
				continue;
			}
			if (pose)
			{
				input = {};
				input.kind = InputKind::Pose;
				const std::string path = instance->object->PathString(binding.binding);
				input.head = path == kHeadPath;
				input.left = path.find(kLeftPath) == 0;
			}
			if (!BindingTypeCompatible(pose ? InputKind::Pose : input.kind, binding.action->object->type))
			{
				return XR_ERROR_ACTION_TYPE_MISMATCH;
			}
			if (std::find(bindings.begin(), bindings.end(), std::pair<XrAction, XrPath>{binding.action, binding.binding}) != bindings.end())
			{
				return XR_ERROR_NAME_DUPLICATED;
			}
			bindings.emplace_back(binding.action, binding.binding);
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
		const XrPath headPath = owner.instance->InternPath(kHeadPath);
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
				binding.path = bindingPath;
				const std::string path = owner.instance->PathString(bindingPath);
				binding.subactionPath = path.find(kLeftPath) == 0 ? leftPath : path.find(kRightPath) == 0 ? rightPath : headPath;
				if (IsPosePath(*owner.instance, bindingPath))
				{
					binding.input.kind = InputKind::Pose;
					binding.input.head = path == kHeadPath;
					binding.input.left = path.find(kLeftPath) == 0;
				}
				else if (!IsSupportedInputPath(*owner.instance, bindingPath, binding.input))
				{
					continue;
				}
				action.bindings.push_back(binding);
			}
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
		if (!IsKnownTopLevel(*session->object->instance, topLevelUserPath) || topLevelUserPath == session->object->instance->InternPath(kHeadPath))
		{
			return XR_ERROR_PATH_UNSUPPORTED;
		}
		interactionProfile->interactionProfile = session->object->instance->InternPath(kProfilePath);
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
		if (!owner.IsFocused() || !owner.actionSetsAttached)
		{
			return XR_ERROR_ACTIONSET_NOT_ATTACHED;
		}
		for (uint32_t index = 0; index < syncInfo->countActiveActionSets; ++index)
		{
			const XrActiveActionSet& active = syncInfo->activeActionSets[index];
			if (!IsValidActionSet(active.actionSet) || std::find(owner.actionSets.begin(), owner.actionSets.end(), active.actionSet) == owner.actionSets.end())
			{
				return XR_ERROR_HANDLE_INVALID;
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
					const ActionSnapshot next = subactionActive ? ReadAction(action, state, subactionPath) : ActionSnapshot{};
					const auto previousIt = action.snapshots.find(subactionPath);
					const ActionSnapshot previous = previousIt == action.snapshots.end() ? ActionSnapshot{} : previousIt->second;
					ActionSnapshot snapshot = next;
					snapshot.changed = Changed(previous, next, action.type);
					snapshot.lastChangeTime = snapshot.changed ? sampleTime : previous.lastChangeTime;
					action.snapshots[subactionPath] = snapshot;
					aggregate.active = aggregate.active || snapshot.active;
					aggregate.booleanValue = aggregate.booleanValue || snapshot.booleanValue;
					if (snapshot.active)
					{
						aggregate.floatValue = snapshot.floatValue;
						aggregate.vectorValue = snapshot.vectorValue;
					}
					aggregate.changed = aggregate.changed || snapshot.changed;
					if (snapshot.changed)
					{
						aggregate.lastChangeTime = std::max(aggregate.lastChangeTime, snapshot.lastChangeTime);
					}
					if (action.type == XR_ACTION_TYPE_BOOLEAN_INPUT && snapshot.changed)
					{
						++record.observedSamples;
					}
				}
				action.aggregate = aggregate;
			}
		}
		if (owner.activeEpoch != nullptr && owner.timelineStart > 0)
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
				if (transitionCounts[index] != 0 && previousState.left.buttons[index] != state.left.buttons[index])
				{
					--transitionCounts[index];
				}
				if (transitionCounts[index + 8] != 0 && previousState.right.buttons[index] != state.right.buttons[index])
				{
					--transitionCounts[index + 8];
				}
				record.unobservedDigitalTransitions += transitionCounts[index] + transitionCounts[index + 8];
			}
		}
		owner.lastSyncTime = sampleTime;
		owner.actionSyncs.push_back(record);
		if (owner.actionSyncs.size() > protocol::kMaxRecordsPerRun)
		{
			owner.actionSyncs.erase(owner.actionSyncs.begin());
			owner.report.overflow = true;
		}
		owner.report.observedSamples += record.observedSamples;
		owner.report.unobservedDigitalTransitions += record.unobservedDigitalTransitions;
		return XR_SUCCESS;
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
		const ActionSnapshot& snapshot = found == action.snapshots.end() ? action.aggregate : found->second;
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
		const ActionSnapshot& snapshot = found == action.snapshots.end() ? action.aggregate : found->second;
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
		const ActionSnapshot& snapshot = found == action.snapshots.end() ? action.aggregate : found->second;
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
		const ActionSnapshot& snapshot = found == action.snapshots.end() ? action.aggregate : found->second;
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
		HapticRecord record;
		record.time = owner.instance->clock.Now();
		record.left = actionInfo->subactionPath == owner.instance->InternPath(kLeftPath);
		record.amplitude = vibration->amplitude;
		record.frequency = vibration->frequency;
		record.duration = vibration->duration;
		owner.haptics.push_back(record);
		if (owner.haptics.size() > protocol::kMaxRecordsPerRun)
		{
			owner.haptics.erase(owner.haptics.begin());
			owner.report.overflow = true;
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
		owner.haptics.push_back(record);
		if (owner.haptics.size() > protocol::kMaxRecordsPerRun)
		{
			owner.haptics.erase(owner.haptics.begin());
			owner.report.overflow = true;
		}
		return XR_SUCCESS;
	});
}

} // namespace agentxr
