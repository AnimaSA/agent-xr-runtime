#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

namespace agentxr
{
namespace
{
using Json = protocol::Json;

struct PosePatch
{
	std::optional<Vec3> position;
	std::optional<Quat> orientation;
	std::optional<bool> positionValid;
	std::optional<bool> orientationValid;
	std::optional<bool> positionTracked;
	std::optional<bool> orientationTracked;
};

struct ControllerPatch
{
	std::optional<bool> active;
	std::unordered_map<std::string, bool> buttons;
	std::unordered_map<std::string, bool> touches;
	std::optional<float> trigger;
	std::optional<float> squeeze;
	std::optional<XrVector2f> thumbstick;
};

struct StatePatch
{
	std::optional<PosePatch> head;
	std::optional<PosePatch> leftGrip;
	std::optional<PosePatch> leftAim;
	std::optional<PosePatch> rightGrip;
	std::optional<PosePatch> rightAim;
	std::optional<ControllerPatch> left;
	std::optional<ControllerPatch> right;
};

enum class Interpolation : uint8_t
{
	Step,
	Linear,
	EaseIn,
	EaseOut,
	EaseInOut
};

struct Keyframe
{
	int64_t timeNs = 0;
	Interpolation interpolation = Interpolation::Linear;
	uint32_t sampleRateHz = 90;
	StatePatch patch;
};

bool HasOnly(const Json& object, std::initializer_list<std::string_view> allowed, std::string& error)
{
	if (!object.is_object())
	{
		error = "expected object";
		return false;
	}
	for (const auto& [name, value] : object.items())
	{
		if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
		{
			error = "unknown field: " + name;
			return false;
		}
	}
	return true;
}

bool Number(const Json& value, double& result)
{
	if (!value.is_number())
	{
		return false;
	}
	result = value.get<double>();
	return std::isfinite(result);
}

bool Bool(const Json& value, bool& result)
{
	if (!value.is_boolean())
	{
		return false;
	}
	result = value.get<bool>();
	return true;
}

bool ParseVec3(const Json& value, Vec3& result)
{
	if (!value.is_array() || value.size() != 3)
	{
		return false;
	}
	double values[3]{};
	for (size_t index = 0; index < 3; ++index)
	{
		if (!Number(value[index], values[index]) || std::fabs(values[index]) > 10000.0)
		{
			return false;
		}
	}
	result = {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])};
	return true;
}

bool ParseQuat(const Json& value, Quat& result)
{
	if (!value.is_array() || value.size() != 4)
	{
		return false;
	}
	double values[4]{};
	for (size_t index = 0; index < 4; ++index)
	{
		if (!Number(value[index], values[index]))
		{
			return false;
		}
	}
	const double length = std::sqrt(values[0] * values[0] + values[1] * values[1] + values[2] * values[2] + values[3] * values[3]);
	if (!std::isfinite(length) || length < 1.0e-8 || std::fabs(length - 1.0) > 0.01)
	{
		return false;
	}
	result = {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2]), static_cast<float>(values[3])};
	return true;
}

bool ParsePosePatch(const Json& value, PosePatch& result, std::string& error, bool requireComplete)
{
	if (!HasOnly(value, {"position", "orientation", "positionValid", "orientationValid", "positionTracked", "orientationTracked"}, error))
	{
		return false;
	}
	if (value.empty() && requireComplete)
	{
		error = "pose must contain position and orientation";
		return false;
	}
	if (value.contains("position"))
	{
		Vec3 position;
		if (!ParseVec3(value.at("position"), position))
		{
			error = "position must contain three finite numbers";
			return false;
		}
		result.position = position;
	}
	if (value.contains("orientation"))
	{
		Quat orientation;
		if (!ParseQuat(value.at("orientation"), orientation))
		{
			error = "orientation must contain nonzero finite quaternion";
			return false;
		}
		result.orientation = orientation;
	}
	if (requireComplete && (!result.position.has_value() || !result.orientation.has_value()))
	{
		error = "pose must contain position and orientation";
		return false;
	}
	const auto parseFlag = [&](const char* name, std::optional<bool>& destination) -> bool
	{
		if (!value.contains(name))
		{
			return true;
		}
		bool flag = false;
		if (!Bool(value.at(name), flag))
		{
			error = std::string(name) + " must be boolean";
			return false;
		}
		destination = flag;
		return true;
	};
	return parseFlag("positionValid", result.positionValid) && parseFlag("orientationValid", result.orientationValid) && parseFlag("positionTracked", result.positionTracked) && parseFlag("orientationTracked", result.orientationTracked);
}

bool ParseControllerPatch(const Json& value, ControllerPatch& result, std::string& error, bool requireComplete)
{
	if (!HasOnly(value, {"active", "buttons", "touches", "trigger", "squeeze", "thumbstick"}, error))
	{
		return false;
	}
	if (value.contains("active"))
	{
		bool active = false;
		if (!Bool(value.at("active"), active))
		{
			error = "active must be boolean";
			return false;
		}
		result.active = active;
	}
	const auto parseDigital = [&](const char* name, std::unordered_map<std::string, bool>& destination) -> bool
	{
		if (!value.contains(name))
		{
			return true;
		}
		if (!value.at(name).is_object())
		{
			error = std::string(name) + " must be object";
			return false;
		}
		for (const auto& [key, item] : value.at(name).items())
		{
			bool state = false;
			if (!Bool(item, state))
			{
				error = std::string(name) + " values must be boolean";
				return false;
			}
			destination[key] = state;
		}
		return true;
	};
	if (!parseDigital("buttons", result.buttons) || !parseDigital("touches", result.touches))
	{
		return false;
	}
	const auto parseScalar = [&](const char* name, std::optional<float>& destination, float minimum, float maximum) -> bool
	{
		if (!value.contains(name))
		{
			return true;
		}
		double number = 0.0;
		if (!Number(value.at(name), number) || number < minimum || number > maximum)
		{
			error = std::string(name) + " outside allowed range";
			return false;
		}
		destination = static_cast<float>(number);
		return true;
	};
	if (!parseScalar("trigger", result.trigger, 0.0f, 1.0f) || !parseScalar("squeeze", result.squeeze, 0.0f, 1.0f))
	{
		return false;
	}
	if (value.contains("thumbstick"))
	{
		const Json& stick = value.at("thumbstick");
		if (!stick.is_array() || stick.size() != 2)
		{
			error = "thumbstick must contain two values";
			return false;
		}
		double x = 0.0;
		double y = 0.0;
		if (!Number(stick[0], x) || !Number(stick[1], y) || x < -1.0 || x > 1.0 || y < -1.0 || y > 1.0)
		{
			error = "thumbstick values outside allowed range";
			return false;
		}
		result.thumbstick = XrVector2f{static_cast<float>(x), static_cast<float>(y)};
	}
	if (requireComplete && result.active == std::nullopt)
	{
		result.active = true;
	}
	return true;
}

bool ParseStatePatch(const Json& value, StatePatch& result, std::string& error, bool requireComplete)
{
	if (!HasOnly(value, {"head", "left", "right"}, error))
	{
		return false;
	}
	if (value.contains("head"))
	{
		PosePatch patch;
		if (!ParsePosePatch(value.at("head"), patch, error, requireComplete))
		{
			return false;
		}
		result.head = std::move(patch);
	}
	else if (requireComplete)
	{
		error = "missing head";
		return false;
	}
	const auto parseHand = [&](const char* key, bool left, std::optional<ControllerPatch>& controller, std::optional<PosePatch>& grip, std::optional<PosePatch>& aim) -> bool
	{
		if (!value.contains(key))
		{
			if (requireComplete)
			{
				error = std::string("missing ") + key;
				return false;
			}
			return true;
		}
		const Json& hand = value.at(key);
		if (!hand.is_object())
		{
			error = std::string(key) + " must be object";
			return false;
		}
		if (!HasOnly(hand, {"grip", "aim", "active", "buttons", "touches", "trigger", "squeeze", "thumbstick"}, error))
		{
			return false;
		}
		if (!hand.contains("grip") || !hand.contains("aim"))
		{
			if (requireComplete)
			{
				error = std::string(key) + " requires grip and aim";
				return false;
			}
		}
		if (hand.contains("grip"))
		{
			PosePatch patch;
			if (!ParsePosePatch(hand.at("grip"), patch, error, requireComplete))
			{
				return false;
			}
			grip = std::move(patch);
		}
		if (hand.contains("aim"))
		{
			PosePatch patch;
			if (!ParsePosePatch(hand.at("aim"), patch, error, requireComplete))
			{
				return false;
			}
			aim = std::move(patch);
		}
		Json input = hand;
		input.erase("grip");
		input.erase("aim");
		ControllerPatch patch;
		if (!ParseControllerPatch(input, patch, error, requireComplete))
		{
			return false;
		}
		controller = std::move(patch);
		return true;
	};
	return parseHand("left", true, result.left, result.leftGrip, result.leftAim) && parseHand("right", false, result.right, result.rightGrip, result.rightAim);
}

bool ParseInterpolation(const Json& value, Interpolation& result, std::string& error)
{
	if (!value.is_string())
	{
		error = "interpolation must be string";
		return false;
	}
	const std::string name = value.get<std::string>();
	if (name == "step") result = Interpolation::Step;
	else if (name == "linear") result = Interpolation::Linear;
	else if (name == "ease_in") result = Interpolation::EaseIn;
	else if (name == "ease_out") result = Interpolation::EaseOut;
	else if (name == "ease_in_out") result = Interpolation::EaseInOut;
	else
	{
		error = "unsupported interpolation";
		return false;
	}
	return true;
}

bool QuantizeSeconds(const Json& value, int64_t& result, std::string& error)
{
	double seconds = 0.0;
	if (!Number(value, seconds) || seconds < 0.0 || seconds > 120.0)
	{
		error = "timestamp must be finite and between zero and 120 seconds";
		return false;
	}
	const long double nanos = static_cast<long double>(seconds) * 1000000000.0L;
	if (nanos > static_cast<long double>(std::numeric_limits<int64_t>::max()))
	{
		error = "timestamp overflow";
		return false;
	}
	result = static_cast<int64_t>(std::llround(nanos));
	return true;
}

void ApplyPose(TrackedPose& destination, const PosePatch& patch)
{
	if (patch.position.has_value()) destination.position = *patch.position;
	if (patch.orientation.has_value()) destination.orientation = *patch.orientation;
	if (patch.positionValid.has_value()) destination.positionValid = *patch.positionValid;
	if (patch.orientationValid.has_value()) destination.orientationValid = *patch.orientationValid;
	if (patch.positionTracked.has_value()) destination.positionTracked = *patch.positionTracked;
	if (patch.orientationTracked.has_value()) destination.orientationTracked = *patch.orientationTracked;
}

uint8_t ButtonIndex(bool left, std::string_view key)
{
	if (left)
	{
		if (key == "x") return 0;
		if (key == "y") return 1;
		if (key == "menu") return 2;
		if (key == "thumbstick") return 3;
	}
	else
	{
		if (key == "a") return 0;
		if (key == "b") return 1;
		if (key == "thumbstick") return 2;
	}
	return UINT8_MAX;
}

uint8_t TouchIndex(std::string_view key)
{
	if (key == "x") return 0;
	if (key == "y") return 1;
	if (key == "a") return 0;
	if (key == "b") return 1;
	if (key == "thumbstick") return 2;
	if (key == "trigger") return 3;
	if (key == "thumbrest") return 4;
	return UINT8_MAX;
}

bool ApplyController(ControllerState& destination, const ControllerPatch& patch, bool left, std::string& error)
{
	if (patch.active.has_value()) destination.active = *patch.active;
	for (const auto& [key, value] : patch.buttons)
	{
		const uint8_t index = ButtonIndex(left, key);
		if (index == UINT8_MAX)
		{
			error = "unsupported button: " + key;
			return false;
		}
		destination.buttons[index] = value;
	}
	for (const auto& [key, value] : patch.touches)
	{
		const uint8_t index = TouchIndex(key);
		if (index == UINT8_MAX)
		{
			error = "unsupported touch: " + key;
			return false;
		}
		destination.touches[index] = value;
	}
	if (patch.trigger.has_value()) destination.trigger = *patch.trigger;
	if (patch.squeeze.has_value()) destination.squeeze = *patch.squeeze;
	if (patch.thumbstick.has_value()) destination.thumbstick = *patch.thumbstick;
	return true;
}

bool ApplyPatch(SimState& destination, const StatePatch& patch, std::string& error)
{
	if (patch.head.has_value()) ApplyPose(destination.head, *patch.head);
	if (patch.leftGrip.has_value()) ApplyPose(destination.leftGrip, *patch.leftGrip);
	if (patch.leftAim.has_value()) ApplyPose(destination.leftAim, *patch.leftAim);
	if (patch.rightGrip.has_value()) ApplyPose(destination.rightGrip, *patch.rightGrip);
	if (patch.rightAim.has_value()) ApplyPose(destination.rightAim, *patch.rightAim);
	if (patch.left.has_value() && !ApplyController(destination.left, *patch.left, true, error)) return false;
	if (patch.right.has_value() && !ApplyController(destination.right, *patch.right, false, error)) return false;
	const TrackedPose* poses[] = {&destination.head, &destination.leftGrip, &destination.leftAim, &destination.rightGrip, &destination.rightAim};
	for (const TrackedPose* pose : poses)
	{
		if ((pose->positionTracked && !pose->positionValid) || (pose->orientationTracked && !pose->orientationValid))
		{
			error = "tracked pose component must also be valid";
			return false;
		}
	}
	return true;
}

float Ease(Interpolation interpolation, float alpha)
{
	switch (interpolation)
	{
	case Interpolation::Step: return 1.0f;
	case Interpolation::Linear: return alpha;
	case Interpolation::EaseIn: return alpha * alpha;
	case Interpolation::EaseOut: return 1.0f - (1.0f - alpha) * (1.0f - alpha);
	case Interpolation::EaseInOut: return alpha * alpha * (3.0f - 2.0f * alpha);
	}
	return alpha;
}

Quat Nlerp(Quat a, Quat b, float alpha)
{
	const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (dot < 0.0f)
	{
		b.x = -b.x;
		b.y = -b.y;
		b.z = -b.z;
		b.w = -b.w;
	}
	Quat result{a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha, a.z + (b.z - a.z) * alpha, a.w + (b.w - a.w) * alpha};
	const float length = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
	if (length < 1.0e-8f) return a;
	result.x /= length;
	result.y /= length;
	result.z /= length;
	result.w /= length;
	return result;
}

TrackedPose InterpolatePose(const TrackedPose& a, const TrackedPose& b, float alpha)
{
	TrackedPose result = a;
	result.position = {a.position.x + (b.position.x - a.position.x) * alpha, a.position.y + (b.position.y - a.position.y) * alpha, a.position.z + (b.position.z - a.position.z) * alpha};
	result.orientation = Nlerp(a.orientation, b.orientation, alpha);
	return result;
}

void InterpolateChanged(SimState& destination, const SimState& start, const SimState& end, const StatePatch& patch, float alpha)
{
	if (patch.head.has_value()) destination.head = InterpolatePose(start.head, end.head, alpha);
	if (patch.leftGrip.has_value()) destination.leftGrip = InterpolatePose(start.leftGrip, end.leftGrip, alpha);
	if (patch.leftAim.has_value()) destination.leftAim = InterpolatePose(start.leftAim, end.leftAim, alpha);
	if (patch.rightGrip.has_value()) destination.rightGrip = InterpolatePose(start.rightGrip, end.rightGrip, alpha);
	if (patch.rightAim.has_value()) destination.rightAim = InterpolatePose(start.rightAim, end.rightAim, alpha);
}

bool ButtonValue(const ControllerState& state, size_t index)
{
	return index < state.buttons.size() && state.buttons[index];
}

void RecordDigitalChanges(TimelineEpoch& epoch, int64_t offset, const SimState& before, const SimState& after)
{
	for (size_t index = 0; index < after.left.buttons.size(); ++index)
	{
		if (ButtonValue(before.left, index) != ButtonValue(after.left, index)) epoch.digitalChanges.push_back({offset, true, static_cast<uint8_t>(index), ButtonValue(before.left, index), ButtonValue(after.left, index)});
	}
	for (size_t index = 0; index < after.right.buttons.size(); ++index)
	{
		if (ButtonValue(before.right, index) != ButtonValue(after.right, index)) epoch.digitalChanges.push_back({offset, false, static_cast<uint8_t>(index), ButtonValue(before.right, index), ButtonValue(after.right, index)});
	}
}

std::shared_ptr<TimelineEpoch> CompileTimeline(const Json& input, std::string& error)
{
	if (!HasOnly(input, {"schemaVersion", "durationSeconds", "sampleRateHz", "interpolation", "initial", "keyframes"}, error)) return {};
	if (!input.contains("schemaVersion") || !input.at("schemaVersion").is_number_integer() || input.at("schemaVersion").get<int>() != 1)
	{
		error = "schemaVersion must be 1";
		return {};
	}
	double durationSeconds = 0.0;
	if (!input.contains("durationSeconds") || !Number(input.at("durationSeconds"), durationSeconds) || durationSeconds <= 0.0 || durationSeconds > 120.0)
	{
		error = "durationSeconds must be in (0,120]";
		return {};
	}
	int64_t durationNs = static_cast<int64_t>(std::llround(durationSeconds * 1000000000.0));
	if (durationNs <= 0)
	{
		error = "durationSeconds is below one nanosecond";
		return {};
	}
	uint32_t sampleRate = 90;
	if (input.contains("sampleRateHz"))
	{
		double value = 0.0;
		if (!Number(input.at("sampleRateHz"), value) || value < 1.0 || value > 240.0 || std::floor(value) != value)
		{
			error = "sampleRateHz must be an integer in [1,240]";
			return {};
		}
		sampleRate = static_cast<uint32_t>(value);
	}
	Interpolation interpolation = Interpolation::Linear;
	if (input.contains("interpolation") && !ParseInterpolation(input.at("interpolation"), interpolation, error)) return {};
	if (!input.contains("initial") || !input.at("initial").is_object())
	{
		error = "initial is required";
		return {};
	}
	StatePatch initialPatch;
	if (!ParseStatePatch(input.at("initial"), initialPatch, error, true)) return {};
	SimState state = DefaultSimState();
	if (!ApplyPatch(state, initialPatch, error))
	{
		error = error.empty() ? "initial state invalid" : error;
		return {};
	}
	if (!input.contains("keyframes") || !input.at("keyframes").is_array() || input.at("keyframes").size() > 10000)
	{
		error = "keyframes must be array with at most 10000 entries";
		return {};
	}
	std::vector<Keyframe> keyframes;
	keyframes.reserve(input.at("keyframes").size());
	int64_t previousTime = -1;
	for (const Json& item : input.at("keyframes"))
	{
		if (!HasOnly(item, {"timeSeconds", "interpolation", "sampleRateHz", "state"}, error)) return {};
		if (!item.contains("timeSeconds") || !item.contains("state"))
		{
			error = "keyframe requires timeSeconds and state";
			return {};
		}
		Keyframe frame;
		if (!QuantizeSeconds(item.at("timeSeconds"), frame.timeNs, error) || frame.timeNs > durationNs || frame.timeNs <= previousTime)
		{
			if (error.empty()) error = "keyframe times must be strictly increasing and within duration";
			return {};
		}
		previousTime = frame.timeNs;
		frame.interpolation = interpolation;
		if (item.contains("interpolation") && !ParseInterpolation(item.at("interpolation"), frame.interpolation, error)) return {};
		frame.sampleRateHz = sampleRate;
		if (item.contains("sampleRateHz"))
		{
			double value = 0.0;
			if (!Number(item.at("sampleRateHz"), value) || value < 1.0 || value > 240.0 || std::floor(value) != value)
			{
				error = "keyframe sampleRateHz must be an integer in [1,240]";
				return {};
			}
			frame.sampleRateHz = static_cast<uint32_t>(value);
		}
		if (!ParseStatePatch(item.at("state"), frame.patch, error, false)) return {};
		keyframes.push_back(std::move(frame));
	}
	if (keyframes.empty())
	{
		error = "keyframes must not be empty";
		return {};
	}
	auto epoch = std::make_shared<TimelineEpoch>();
	epoch->durationNs = durationNs;
	epoch->sampleRateHz = sampleRate;
	epoch->samples.push_back({0, state});
	int64_t previous = 0;
	for (const Keyframe& frame : keyframes)
	{
		const SimState start = state;
		SimState target = state;
		if (!ApplyPatch(target, frame.patch, error))
		{
			if (error.empty()) error = "keyframe state invalid";
			return {};
		}
		RecordDigitalChanges(*epoch, frame.timeNs, start, target);
		if (epoch->digitalChanges.size() + epoch->samples.size() > 100000)
		{
			error = "compiled timeline exceeds 100000 samples and transitions";
			return {};
		}
		const int64_t interval = frame.timeNs - previous;
		const int64_t step = std::max<int64_t>(1, static_cast<int64_t>(std::llround(1000000000.0 / static_cast<double>(frame.sampleRateHz))));
		if (frame.interpolation != Interpolation::Step)
		{
			for (int64_t at = previous + step; at < frame.timeNs; at += step)
			{
				if (epoch->samples.size() + epoch->digitalChanges.size() >= 100000)
				{
					error = "compiled timeline exceeds 100000 samples and transitions";
					return {};
				}
				const float alpha = Ease(frame.interpolation, static_cast<float>(at - previous) / static_cast<float>(interval));
				SimState interpolated = start;
				InterpolateChanged(interpolated, start, target, frame.patch, alpha);
				epoch->samples.push_back({at, interpolated});
			}
		}
		if (!epoch->samples.empty() && epoch->samples.back().offsetNs == frame.timeNs)
		{
			epoch->samples.back().state = target;
		}
		else
		{
			epoch->samples.push_back({frame.timeNs, target});
		}
		state = target;
		previous = frame.timeNs;
	}
	if (epoch->samples.size() + epoch->digitalChanges.size() > 100000)
	{
		error = "compiled timeline exceeds 100000 samples and transitions";
		return {};
	}
	return epoch;
}

Json PoseJson(const TrackedPose& pose)
{
	return {{"position", {pose.position.x, pose.position.y, pose.position.z}}, {"orientation", {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}}, {"positionValid", pose.positionValid}, {"orientationValid", pose.orientationValid}, {"positionTracked", pose.positionTracked}, {"orientationTracked", pose.orientationTracked}};
}

Json ControllerJson(const ControllerState& state, bool left)
{
	Json buttons = Json::object();
	if (left)
	{
		buttons["x"] = state.buttons[0];
		buttons["y"] = state.buttons[1];
		buttons["menu"] = state.buttons[2];
		buttons["thumbstick"] = state.buttons[3];
	}
	else
	{
		buttons["a"] = state.buttons[0];
		buttons["b"] = state.buttons[1];
		buttons["thumbstick"] = state.buttons[2];
	}
	Json touches = Json::object();
	if (left)
	{
		touches["x"] = state.touches[0];
		touches["y"] = state.touches[1];
	}
	else
	{
		touches["a"] = state.touches[0];
		touches["b"] = state.touches[1];
	}
	touches["thumbstick"] = state.touches[2];
	touches["trigger"] = state.touches[3];
	touches["thumbrest"] = state.touches[4];
	return {{"active", state.active}, {"buttons", buttons}, {"touches", touches}, {"trigger", state.trigger}, {"squeeze", state.squeeze}, {"thumbstick", {state.thumbstick.x, state.thumbstick.y}}};
}

Json StateJson(const SimState& state)
{
	return {{"head", PoseJson(state.head)}, {"leftGrip", PoseJson(state.leftGrip)}, {"leftAim", PoseJson(state.leftAim)}, {"rightGrip", PoseJson(state.rightGrip)}, {"rightAim", PoseJson(state.rightAim)}, {"left", ControllerJson(state.left, true)}, {"right", ControllerJson(state.right, false)}};
}

Json FrameJson(const FrameRecord& frame)
{
	return {{"frameId", frame.id}, {"timelineId", frame.timelineId}, {"timelineStart", frame.timelineStart}, {"displayTime", frame.displayTime}, {"period", frame.period}, {"layerCount", frame.layerCount}, {"waited", frame.waited}, {"begun", frame.begun}, {"ended", frame.ended}, {"discarded", frame.discarded}, {"presented", frame.presented}, {"presentOccluded", frame.presentOccluded}, {"presentResult", frame.presentResult}, {"late", frame.late}, {"authoredSamples", frame.authoredSamples}, {"appliedSamples", frame.appliedSamples}, {"observedSamples", frame.observedSamples}};
}

Json ActionSyncJson(const ActionSyncRecord& record)
{
	return {{"time", record.time}, {"leftActive", record.leftActive}, {"rightActive", record.rightActive}, {"leftTrigger", record.leftTrigger}, {"rightTrigger", record.rightTrigger}, {"leftSqueeze", record.leftSqueeze}, {"rightSqueeze", record.rightSqueeze}, {"leftThumbstick", {record.leftThumbstick.x, record.leftThumbstick.y}}, {"rightThumbstick", {record.rightThumbstick.x, record.rightThumbstick.y}}, {"observedSamples", record.observedSamples}, {"unobservedDigitalTransitions", record.unobservedDigitalTransitions}};
}

Json HapticJson(const HapticRecord& record)
{
	return {{"time", record.time}, {"hand", record.left ? "left" : "right"}, {"amplitude", record.amplitude}, {"frequency", record.frequency}, {"duration", record.duration}, {"stopped", record.stopped}};
}

Json ReportJson(const RunReport& report, size_t cursor, size_t limit)
{
	Json result = {{"timelineId", report.timelineId}, {"status", report.status}, {"error", report.error}, {"authoredStart", report.authoredStart}, {"authoredEnd", report.authoredEnd}, {"actualFirstFrame", report.actualFirstFrame}, {"actualLastFrame", report.actualLastFrame}, {"framePeriod", report.framePeriod}, {"maxFrameGap", report.maxFrameGap}, {"plannedSamples", report.plannedSamples}, {"appliedSamples", report.appliedSamples}, {"observedSamples", report.observedSamples}, {"unobservedDigitalTransitions", report.unobservedDigitalTransitions}, {"lateSampleCount", report.lateSampleCount}, {"missedFrameCount", report.missedFrameCount}, {"validityTransitions", report.validityTransitions}, {"trackingTransitions", report.trackingTransitions}, {"overflow", report.overflow}, {"undersampled", report.undersampled}, {"submittedFrameId", report.submittedFrameId}, {"composedFrameId", report.composedFrameId}, {"presentedFrameId", report.presentedFrameId}};
	const size_t end = std::min(cursor + limit, report.frames.size());
	result["cursor"] = cursor;
	result["nextCursor"] = end < report.frames.size() ? Json(end) : Json(nullptr);
	result["totalFrames"] = report.frames.size();
	result["frames"] = Json::array();
	for (size_t index = cursor; index < end; ++index) result["frames"].push_back(FrameJson(report.frames[index]));
	result["actionSyncs"] = Json::array();
	for (const ActionSyncRecord& action : report.actionSyncs) result["actionSyncs"].push_back(ActionSyncJson(action));
	result["haptics"] = Json::array();
	for (const HapticRecord& haptic : report.haptics) result["haptics"].push_back(HapticJson(haptic));
	return result;
}

} // namespace

XrTime Session::ValidateTime(XrTime time) const
{
	return time > 0 && time <= instance->clock.Now() + 10000000000LL ? time : 0;
}

SimState Session::StateAt(XrTime time, std::shared_ptr<const TimelineEpoch>* selectedEpoch) const
{
	std::lock_guard lock(mutex);
	return StateAtLocked(time, selectedEpoch);
}

SimState Session::StateAtLocked(XrTime time, std::shared_ptr<const TimelineEpoch>* selectedEpoch) const
{
	const auto neutralize = [](SimState& result)
	{
		result.left.buttons.fill(false);
		result.right.buttons.fill(false);
		result.left.touches.fill(false);
		result.right.touches.fill(false);
		result.left.trigger = 0.0f;
		result.right.trigger = 0.0f;
		result.left.squeeze = 0.0f;
		result.right.squeeze = 0.0f;
		result.left.thumbstick = {0.0f, 0.0f};
		result.right.thumbstick = {0.0f, 0.0f};
		result.left.active = true;
		result.right.active = true;
	};
	std::shared_ptr<const TimelineEpoch> epoch;
	XrTime startTime = 0;
	bool current = false;
	if (activeEpoch != nullptr && timelineStart > 0 && time >= timelineStart)
	{
		epoch = activeEpoch;
		startTime = timelineStart;
		current = true;
	}
	else
	{
		for (auto it = retainedEpochs.rbegin(); it != retainedEpochs.rend(); ++it)
		{
			if (it->epoch != nullptr && it->startTime > 0 && time >= it->startTime)
			{
				epoch = it->epoch;
				startTime = it->startTime;
				break;
			}
		}
	}
	if (epoch != nullptr)
	{
		const int64_t offset = std::max<int64_t>(0, std::min<int64_t>(epoch->durationNs, time - startTime));
		SimState result = epoch->samples.empty() ? fallbackState : epoch->samples.front().state;
		auto found = std::upper_bound(epoch->samples.begin(), epoch->samples.end(), offset, [](int64_t value, const TimelineSample& sample) { return value < sample.offsetNs; });
		if (found != epoch->samples.begin())
		{
			--found;
			result = found->state;
		}
		if (current && inputsNeutralized)
		{
			neutralize(result);
		}
		if (selectedEpoch != nullptr)
		{
			*selectedEpoch = epoch;
		}
		return result;
	}
	if (selectedEpoch != nullptr)
	{
		selectedEpoch->reset();
	}
	SimState result = previousFallbackState.has_value() ? previousFallbackState.value() : fallbackState;
	if (inputsNeutralized)
	{
		neutralize(result);
	}
	return result;
}

void Session::PruneRetainedEpochs()
{
	auto hasOutstandingFrame = [this](uint64_t barrierFrameId)
	{
		for (uint64_t frameId : waitedFrameIds)
		{
			if (frameId <= barrierFrameId)
			{
				return true;
			}
		}
		for (uint64_t frameId : begunFrameIds)
		{
			if (frameId <= barrierFrameId)
			{
				return true;
			}
		}
		for (const FrameRecord& frame : frames)
		{
			if (frame.id <= barrierFrameId && frame.begun && !frame.ended && !frame.discarded)
			{
				return true;
			}
		}
		return false;
	};
	for (auto it = retainedEpochs.begin(); it != retainedEpochs.end();)
	{
		if (!hasOutstandingFrame(it->barrierFrameId))
		{
			it = retainedEpochs.erase(it);
		}
		else
		{
			++it;
		}
	}
}

bool Session::ActivatePendingTimeline(XrTime startTime)
{
	if (pendingEpoch == nullptr)
	{
		return true;
	}
	PruneRetainedEpochs();
	const bool retainActive = activeEpoch != nullptr && timelineStart > 0;
	if (retainActive && retainedEpochs.size() >= 4)
	{
		return false;
	}
	std::shared_ptr<const TimelineEpoch> next = std::move(pendingEpoch);
	pendingTimelineId = 0;
	if (activeEpoch != nullptr)
	{
		report.frames.clear();
		for (const FrameRecord& frame : frames)
		{
			if (frame.timelineId == activeEpoch->id)
			{
				report.frames.push_back(frame);
			}
		}
		report.actionSyncs = actionSyncs;
		report.haptics = haptics;
		completedReports.emplace_back(activeEpoch->id, report);
		if (completedReports.size() > 4)
		{
			completedReports.pop_front();
		}
		if (retainActive)
		{
			retainedEpochs.push_back({timelineStart, frameId, activeEpoch});
		}
	}
	if (!previousFallbackState.has_value())
	{
		previousFallbackState = fallbackState;
	}
	activeEpoch = std::move(next);
	timelineStart = startTime;
	canceledAt.reset();
	inputsNeutralized = false;
	neutralizePending = false;
	fallbackState = activeEpoch->samples.front().state;
	lastPublishedState = fallbackState;
	lastSyncTime = 0;
	actionSyncs.clear();
	haptics.clear();
	report = {};
	report.timelineId = activeEpoch->id;
	report.status = "running";
	report.authoredStart = startTime;
	report.plannedSamples = static_cast<uint32_t>(activeEpoch->samples.size());
	report.framePeriod = 11111111;
	return true;
}

XrResult Session::SubmitTimeline(const Json& timeline, uint64_t& timelineId, std::string& error)
{
	std::shared_ptr<TimelineEpoch> compiled = CompileTimeline(timeline, error);
	if (compiled == nullptr)
	{
		return XR_ERROR_VALIDATION_FAILURE;
	}
	std::lock_guard lock(mutex);
	if (!IsFocused())
	{
		error = "session must be focused";
		return XR_SESSION_NOT_FOCUSED;
	}
	if (pendingEpoch != nullptr)
	{
		error = "timeline already pending";
		return XR_ERROR_LIMIT_REACHED;
	}
	const bool hasOutstandingFrames = !waitedFrameIds.empty() || !begunFrameIds.empty();
	const bool activeRun = activeEpoch != nullptr && (report.status == "armed" || report.status == "running");
	if (nextTimelineId == 0 || nextTimelineId == std::numeric_limits<uint64_t>::max())
	{
		error = "timeline id exhausted";
		return XR_ERROR_LIMIT_REACHED;
	}
	PruneRetainedEpochs();
	const bool retainActive = activeEpoch != nullptr && timelineStart > 0;
	if (retainActive && retainedEpochs.size() >= 4)
	{
		error = "four unreleased timeline epochs retained";
		return XR_ERROR_LIMIT_REACHED;
	}
	compiled->id = nextTimelineId++;
	timelineId = compiled->id;
	// Running or still-owned frames require activation at the next frame boundary.
	if (activeRun || hasOutstandingFrames)
	{
		pendingTimelineId = timelineId;
		pendingEpoch = std::move(compiled);
		return XR_SUCCESS;
	}
	if (activeEpoch != nullptr)
	{
		report.frames.clear();
		for (const FrameRecord& frame : frames)
		{
			if (frame.timelineId == activeEpoch->id)
			{
				report.frames.push_back(frame);
			}
		}
		report.actionSyncs = actionSyncs;
		report.haptics = haptics;
		completedReports.emplace_back(activeEpoch->id, report);
		if (completedReports.size() > 4)
		{
			completedReports.pop_front();
		}
		if (retainActive)
		{
			retainedEpochs.push_back({timelineStart, frameId, activeEpoch});
		}
	}
	previousFallbackState.reset();
	activeEpoch = std::move(compiled);
	timelineStart = 0;
	canceledAt.reset();
	inputsNeutralized = false;
	neutralizePending = false;
	fallbackState = activeEpoch->samples.front().state;
	lastPublishedState = fallbackState;
	lastSyncTime = 0;
	frames.clear();
	actionSyncs.clear();
	haptics.clear();
	report = {};
	report.timelineId = activeEpoch->id;
	report.status = "armed";
	report.plannedSamples = static_cast<uint32_t>(activeEpoch->samples.size());
	report.framePeriod = 11111111;
	return XR_SUCCESS;
}

XrResult Session::CancelTimeline(uint64_t timelineId, std::string& error)
{
	std::lock_guard lock(mutex);
	if (pendingEpoch != nullptr && pendingTimelineId == timelineId)
	{
		pendingEpoch.reset();
		pendingTimelineId = 0;
		return XR_SUCCESS;
	}
	if (activeEpoch == nullptr || activeEpoch->id != timelineId)
	{
		error = "timeline not active";
		return XR_ERROR_HANDLE_INVALID;
	}
	if (canceledAt.has_value())
	{
		return XR_SUCCESS;
	}
	canceledAt = instance->clock.Now();
	neutralizePending = true;
	report.status = "canceled";
	report.error = "canceled by controller";
	return XR_SUCCESS;
}

void Session::NeutralizeInputs()
{
	std::lock_guard lock(mutex);
	neutralizePending = true;
	if (activeEpoch != nullptr && !canceledAt.has_value())
	{
		canceledAt = instance->clock.Now();
		report.status = "canceled";
		report.error = "controlling connection lost";
	}
}

protocol::Json Session::Snapshot() const
{
	std::lock_guard lock(mutex);
	Json result = {{"sessionState", static_cast<int>(state)}, {"running", running}, {"focused", IsFocused()}, {"sessionGeneration", sessionGeneration}, {"frameId", frameId}, {"frameWaited", frameWaited}, {"frameBegun", frameBegun}, {"graphics", {{"d3d12", device != nullptr}, {"adapterLuidLow", adapterLuid.LowPart}, {"adapterLuidHigh", adapterLuid.HighPart}, {"deviceLost", compositor != nullptr && compositor->DeviceLost()}}}, {"tracking", StateJson(lastPublishedState)}, {"frame", {{"submitted", report.submittedFrameId}, {"composed", report.composedFrameId}, {"presented", report.presentedFrameId}}}, {"timeline", {{"id", report.timelineId}, {"status", report.status}, {"start", report.authoredStart}, {"end", report.authoredEnd}, {"plannedSamples", report.plannedSamples}, {"appliedSamples", report.appliedSamples}, {"observedSamples", report.observedSamples}, {"unobservedDigitalTransitions", report.unobservedDigitalTransitions}}}};
	result["timeline"]["pendingId"] = pendingTimelineId;
	result["timeline"]["pending"] = pendingEpoch != nullptr;
	if (compositor != nullptr)
	{
		result["compositor"]["lastCompletedFrame"] = compositor->LastCompletedFrame();
	}
	return result;
}

protocol::Json Session::ReportPage(uint64_t timelineId, size_t cursor, size_t limit) const
{
	std::lock_guard lock(mutex);
	if (limit == 0 || limit > 1000) limit = 1000;
	if (pendingEpoch != nullptr && pendingTimelineId == timelineId)
	{
		RunReport pendingReport;
		pendingReport.timelineId = pendingTimelineId;
		pendingReport.status = "armed";
		pendingReport.plannedSamples = static_cast<uint32_t>(pendingEpoch->samples.size());
		pendingReport.framePeriod = 11111111;
		return ReportJson(pendingReport, cursor, limit);
	}
	if (report.timelineId == timelineId)
	{
		RunReport current = report;
		current.frames.clear();
		for (const FrameRecord& frame : frames)
		{
			if (frame.timelineId == report.timelineId)
			{
				current.frames.push_back(frame);
			}
		}
		current.actionSyncs = actionSyncs;
		current.haptics = haptics;
		return ReportJson(current, cursor, limit);
	}
	for (const auto& [id, completed] : completedReports)
	{
		if (id == timelineId)
		{
			return ReportJson(completed, cursor, limit);
		}
	}
	return {{"status", "report_expired"}, {"timelineId", timelineId}};
}

XrResult Session::Capture(uint64_t afterFrameId, Json& metadata, std::vector<uint8_t>& png, uint32_t timeoutMs)
{
	std::lock_guard lock(mutex);
	if (compositor == nullptr || compositor->DeviceLost())
	{
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	return compositor->Capture(afterFrameId, metadata, png, timeoutMs);
}

void Session::InvalidateChildren()
{
	for (XrSpace handle : spaces)
	{
		if (handle != XR_NULL_HANDLE && handle->object != nullptr)
		{
			handle->alive = false;
			delete handle->object;
			handle->object = nullptr;
		}
	}
	spaces.clear();
	for (XrSwapchain handle : swapchains)
	{
		if (handle != XR_NULL_HANDLE && handle->object != nullptr)
		{
			handle->alive = false;
			delete handle->object;
			handle->object = nullptr;
		}
	}
	swapchains.clear();
	actionSets.clear();
}

} // namespace agentxr
