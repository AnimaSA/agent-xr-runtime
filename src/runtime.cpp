#include "runtime.h"

#include <algorithm>
#include <bcrypt.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
namespace agentxr
{
bool IsAbsolutePath(std::string_view path)
{
	if (path.size() < 2 || path.size() >= XR_MAX_PATH_LENGTH || path.front() != '/' || path.back() == '/')
	{
		return false;
	}
	bool segmentHasCharacter = false;
	for (size_t index = 1; index < path.size(); ++index)
	{
		const char character = path[index];
		if (character == '/')
		{
			if (!segmentHasCharacter)
			{
				return false;
			}
			segmentHasCharacter = false;
			continue;
		}
		const bool lower = character >= 'a' && character <= 'z';
		const bool digit = character >= '0' && character <= '9';
		if (!lower && !digit && character != '-' && character != '_' && character != '.')
		{
			return false;
		}
		segmentHasCharacter = true;
	}
	return segmentHasCharacter;
}

namespace
{
std::mutex gActiveSessionMutex;
Session* gActiveSession = nullptr;
std::mutex gTombstoneMutex;
std::vector<void*> gTombstones;

void RetainTombstone(void* handle)
{
	std::lock_guard lock(gTombstoneMutex);
	gTombstones.push_back(handle);
}

void CopyString(char* destination, size_t capacity, std::string_view value)
{
	if (capacity == 0)
	{
		return;
	}
	size_t length = value.size();
	if (length >= capacity)
	{
		length = capacity - 1;
	}
	std::memcpy(destination, value.data(), length);
	destination[length] = '\0';
}

std::string MakeInstanceId()
{
	std::array<uint8_t, 16> bytes{};
	if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		std::memcpy(bytes.data(), &value.QuadPart, sizeof(value.QuadPart));
		const uint32_t pid = GetCurrentProcessId();
		std::memcpy(bytes.data() + sizeof(value.QuadPart), &pid, sizeof(pid));
	}
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(32);
	for (uint8_t byte : bytes)
	{
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0f]);
	}
	return result;
}

uint64_t ProcessCreationTime()
{
	FILETIME creation{}, exitTime{}, kernel{}, user{};
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
	{
		return 0;
	}
	ULARGE_INTEGER value{};
	value.LowPart = creation.dwLowDateTime;
	value.HighPart = creation.dwHighDateTime;
	return value.QuadPart;
}

std::filesystem::path ModulePath()
{
	std::array<wchar_t, 32768> buffer{};
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0)
	{
		return {};
	}
	return std::filesystem::path(buffer.data(), buffer.data() + length);
}

bool IsExpectedExtension(std::string_view name)
{
	return name == XR_KHR_D3D12_ENABLE_EXTENSION_NAME || name == XR_EXT_LOCAL_FLOOR_EXTENSION_NAME;
}

XrResult EnsureInstance(XrInstance instance)
{
	return IsValidInstance(instance) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult EnsureSession(XrSession session)
{
	return IsValidSession(session) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult EnsureType(const void* structure, XrStructureType expected)
{
	return CheckType(structure, expected);
}

bool FindD3D12Binding(const XrSessionCreateInfo* createInfo, const XrGraphicsBindingD3D12KHR*& binding)
{
	binding = nullptr;
	const auto* current = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
	while (current != nullptr)
	{
		if (current->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
		{
			binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(current);
			return true;
		}
		current = reinterpret_cast<const XrBaseInStructure*>(current->next);
	}
	return false;
}

bool GetHighPerformanceAdapter(LUID& luid, D3D_FEATURE_LEVEL& featureLevel)
{
	Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
	{
		return false;
	}
	Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
	for (UINT index = 0; factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++index)
	{
		DXGI_ADAPTER_DESC1 description{};
		if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
		{
			adapter.Reset();
			continue;
		}
		Microsoft::WRL::ComPtr<ID3D12Device> probe;
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&probe))))
		{
			luid = description.AdapterLuid;
			featureLevel = D3D_FEATURE_LEVEL_11_0;
			return true;
		}
		adapter.Reset();
	}
	return false;
}
bool SupportsFeatureLevel(ID3D12Device* device, D3D_FEATURE_LEVEL required)
{
	if (device == nullptr)
	{
		return false;
	}
	const std::array<D3D_FEATURE_LEVEL, 4> requested = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_12_1};
	D3D12_FEATURE_DATA_FEATURE_LEVELS data{};
	data.NumFeatureLevels = static_cast<UINT>(requested.size());
	data.pFeatureLevelsRequested = requested.data();
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data))))
	{
		return false;
	}
	return data.MaxSupportedFeatureLevel >= required;
}

void MarkDead(XrSpace handle)
{
	if (handle != XR_NULL_HANDLE)
	{
		handle->alive = false;
		handle->object = nullptr;
	}
}

void MarkDead(XrAction handle)
{
	if (handle != XR_NULL_HANDLE)
	{
		handle->alive = false;
		handle->object = nullptr;
	}
}

void MarkDead(XrActionSet handle)
{
	if (handle != XR_NULL_HANDLE)
	{
		handle->alive = false;
		handle->object = nullptr;
	}
}

void MarkDead(XrSwapchain handle)
{
	if (handle != XR_NULL_HANDLE)
	{
		handle->alive = false;
		handle->object = nullptr;
	}
}

} // namespace

SimState DefaultSimState()
{
	SimState state;
	state.head.position = {0.0f, 1.65f, 0.0f};
	state.leftGrip.position = {-0.25f, 1.20f, -0.35f};
	state.leftAim.position = {-0.25f, 1.20f, -0.35f};
	state.rightGrip.position = {0.25f, 1.20f, -0.35f};
	state.rightAim.position = {0.25f, 1.20f, -0.35f};
	return state;
}

XrPosef ToXrPose(const TrackedPose& pose)
{
	XrPosef result{};
	result.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
	result.position = {pose.position.x, pose.position.y, pose.position.z};
	return result;
}

TrackedPose FromXrPose(const XrPosef& pose)
{
	TrackedPose result;
	result.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
	result.position = {pose.position.x, pose.position.y, pose.position.z};
	return result;
}

Clock Clock::Create()
{
	Clock result;
	QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&result.qpcFrequency));
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&result.qpcOrigin));
	result.lastTime.store(0, std::memory_order_relaxed);
	return result;
}

XrTime Clock::FromQpc(int64_t qpc) const
{
	if (qpc <= qpcOrigin || qpcFrequency <= 0)
	{
		return 0;
	}
	const uint64_t delta = static_cast<uint64_t>(qpc - qpcOrigin);
	const uint64_t frequency = static_cast<uint64_t>(qpcFrequency);
	const uint64_t seconds = delta / frequency;
	const uint64_t remainder = delta % frequency;
	if (seconds > static_cast<uint64_t>(std::numeric_limits<XrTime>::max()) / 1000000000ULL)
	{
		return std::numeric_limits<XrTime>::max();
	}
	uint64_t nanos = seconds * 1000000000ULL;
	const uint64_t remainderNanos = (remainder * 1000000000ULL) / frequency;
	if (nanos > UINT64_MAX - remainderNanos)
	{
		nanos = UINT64_MAX;
	}
	else
	{
		nanos += remainderNanos;
	}
	const XrTime candidate = nanos > static_cast<uint64_t>(std::numeric_limits<XrTime>::max()) ? std::numeric_limits<XrTime>::max() : static_cast<XrTime>(nanos);
	XrTime previous = lastTime.load(std::memory_order_relaxed);
	while (candidate > previous && !lastTime.compare_exchange_weak(previous, candidate, std::memory_order_relaxed))
	{
	}
	return candidate < previous ? previous : candidate;
}

XrTime Clock::Now() const
{
	LARGE_INTEGER value{};
	QueryPerformanceCounter(&value);
	return FromQpc(value.QuadPart);
}

int64_t Clock::ToQpc(XrTime time) const
{
	if (time <= 0 || qpcFrequency <= 0)
	{
		return qpcOrigin;
	}
	const long double ticks = static_cast<long double>(time) * static_cast<long double>(qpcFrequency) / 1000000000.0L;
	const long double maximum = static_cast<long double>(INT64_MAX) - static_cast<long double>(qpcOrigin);
	if (!std::isfinite(ticks) || ticks >= maximum)
	{
		return INT64_MAX;
	}
	return qpcOrigin + static_cast<int64_t>(ticks);
}

bool Clock::SleepUntilQpc(int64_t targetQpc) const
{
	if (qpcFrequency <= 0)
	{
		return false;
	}
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	if (targetQpc <= now.QuadPart)
	{
		return true;
	}
	const uint64_t ticks = static_cast<uint64_t>(targetQpc - now.QuadPart);
	const long double duration = static_cast<long double>(ticks) * 10000000.0L / static_cast<long double>(qpcFrequency);
	if (!std::isfinite(duration) || duration >= static_cast<long double>(LONGLONG_MAX))
	{
		return false;
	}
	const auto duration100ns = static_cast<LONGLONG>(std::max<long double>(1.0L, std::ceil(duration)));
	thread_local HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
	if (timer == nullptr)
	{
		return false;
	}
	LARGE_INTEGER due{};
	due.QuadPart = -duration100ns;
	if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
	{
		return false;
	}
	return WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
}

XrPath Instance::InternPath(std::string_view path)
{
	if (!IsAbsolutePath(path))
	{
		return XR_NULL_PATH;
	}
	std::lock_guard lock(mutex);
	auto found = paths.find(std::string(path));
	if (found != paths.end())
	{
		return found->second;
	}
	if (nextPath == XR_NULL_PATH || nextPath == std::numeric_limits<XrPath>::max())
	{
		return XR_NULL_PATH;
	}
	const XrPath result = nextPath++;
	paths.emplace(std::string(path), result);
	pathStrings.emplace(result, std::string(path));
	return result;
}

std::string Instance::PathString(XrPath path) const
{
	std::lock_guard lock(mutex);
	auto found = pathStrings.find(path);
	return found == pathStrings.end() ? std::string{} : found->second;
}

bool Instance::IsPath(XrPath path, std::string_view expected) const
{
	std::lock_guard lock(mutex);
	auto found = pathStrings.find(path);
	return found != pathStrings.end() && found->second == expected;
}

void Instance::QueueEvent(const XrEventDataBaseHeader* eventData, size_t size)
{
	if (eventData == nullptr || size > sizeof(XrEventDataBuffer))
	{
		return;
	}
	std::vector<uint8_t> bytes(size);
	std::memcpy(bytes.data(), eventData, size);
	std::lock_guard lock(eventMutex);
	if (events.size() >= 1024)
	{
		++lostEventCount;
		return;
	}
	events.push_back(std::move(bytes));
}

bool Instance::PopEvent(XrEventDataBuffer& eventData)
{
	std::lock_guard lock(eventMutex);
	if (lostEventCount != 0)
	{
		XrEventDataEventsLost lost{XR_TYPE_EVENT_DATA_EVENTS_LOST, nullptr, lostEventCount};
		std::memset(&eventData, 0, sizeof(eventData));
		std::memcpy(&eventData, &lost, sizeof(lost));
		lostEventCount = 0;
		return true;
	}
	if (events.empty())
	{
		return false;
	}
	std::memset(&eventData, 0, sizeof(eventData));
	const auto& bytes = events.front();
	std::memcpy(&eventData, bytes.data(), bytes.size());
	events.pop_front();
	return true;
}

bool Instance::AcquireLease(uint64_t connectionId)
{
	std::lock_guard lock(leaseMutex);
	if (controllerLease == 0 || controllerLease == connectionId)
	{
		controllerLease = connectionId;
		return true;
	}
	return false;
}

void Instance::ReleaseLease(uint64_t connectionId, bool cancelRun)
{
	{
		std::lock_guard lock(leaseMutex);
		if (controllerLease != connectionId)
		{
			return;
		}
		controllerLease = 0;
	}
	if (!cancelRun)
	{
		return;
	}
	std::vector<Session*> sessionsToNeutralize;
	{
		std::lock_guard lock(mutex);
		sessionsToNeutralize.reserve(sessions.size());
		for (XrSession sessionHandle : sessions)
		{
			if (sessionHandle != XR_NULL_HANDLE && sessionHandle->alive && sessionHandle->object != nullptr && sessionHandle->object->instance == this && !sessionHandle->object->closing)
			{
				sessionsToNeutralize.push_back(sessionHandle->object);
			}
		}
	}
	for (Session* session : sessionsToNeutralize)
	{
		if (session != nullptr)
		{
			session->NeutralizeInputs();
		}
	}
}

void Instance::InvalidateChildren()
{
	std::vector<XrSession> sessionCopy;
	std::vector<XrActionSet> actionSetCopy;
	{
		std::lock_guard lock(mutex);
		sessionCopy = sessions;
		actionSetCopy = actionSets;
		sessions.clear();
		actionSets.clear();
		publicGeneration.store(0, std::memory_order_release);
	}
	for (XrSession sessionHandle : sessionCopy)
	{
		if (sessionHandle == XR_NULL_HANDLE || !sessionHandle->alive || sessionHandle->object == nullptr)
		{
			continue;
		}
		Session* sessionState = sessionHandle->object;
		Compositor* sessionCompositor = nullptr;
		{
			std::lock_guard lock(sessionState->mutex);
			sessionState->closing = true;
			sessionState->frameWaited = false;
			sessionState->frameBegun = false;
			sessionState->waitedFrameId = 0;
			sessionState->begunFrameId = 0;
			sessionState->nextDisplayTime = 0;
			sessionState->nextDeadlineQpc = 0;
			sessionState->InvalidateChildren();
			sessionCompositor = sessionState->compositor;
			sessionState->compositor = nullptr;
		}
		if (sessionCompositor != nullptr)
		{
			sessionCompositor->Shutdown();
			delete sessionCompositor;
		}
		sessionHandle->alive = false;
		sessionHandle->object = nullptr;
		RetainTombstone(sessionHandle);
		delete sessionState;
	}
	for (XrActionSet actionSetHandle : actionSetCopy)
	{
		if (actionSetHandle == XR_NULL_HANDLE || !actionSetHandle->alive || actionSetHandle->object == nullptr)
		{
			continue;
		}
		ActionSet* actionSetState = actionSetHandle->object;
		for (XrAction actionHandle : actionSetState->actions)
		{
			if (actionHandle != XR_NULL_HANDLE && actionHandle->object != nullptr)
			{
				actionHandle->alive = false;
				actionHandle->object->destroyed = true;
				delete actionHandle->object;
				actionHandle->object = nullptr;
				RetainTombstone(actionHandle);
			}
		}
		actionSetState->actions.clear();
		actionSetHandle->alive = false;
		actionSetHandle->object = nullptr;
		RetainTombstone(actionSetHandle);
		delete actionSetState;
	}
}

bool Session::IsFocused() const
{
	return running && state == XR_SESSION_STATE_FOCUSED && !closing;
}
void Session::NotifyContentIdle()
{
	std::lock_guard lock(mutex);
	if (!running || closing || state != XR_SESSION_STATE_FOCUSED || requestExit)
	{
		return;
	}
	requestExit = true;
	QueueState(XR_SESSION_STATE_STOPPING);
}

void Session::QueueState(XrSessionState newState)
{
	state = newState;
	XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, nullptr, handle, newState, instance->clock.Now()};
	instance->QueueEvent(reinterpret_cast<const XrEventDataBaseHeader*>(&event), sizeof(event));
}

XrResult CheckType(const void* structure, XrStructureType expected)
{
	if (structure == nullptr)
	{
		return XR_ERROR_VALIDATION_FAILURE;
	}
	return *reinterpret_cast<const XrStructureType*>(structure) == expected ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}

XrResult CheckOutput(void* structure, XrStructureType expected)
{
	if (structure == nullptr)
	{
		return XR_ERROR_VALIDATION_FAILURE;
	}
	return *reinterpret_cast<XrStructureType*>(structure) == expected ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}

XrResult EnumerateArray(uint32_t capacity, uint32_t* count, void* values, size_t elementSize, const void* source, uint32_t sourceCount)
{
	if (count == nullptr)
	{
		return XR_ERROR_VALIDATION_FAILURE;
	}
	*count = sourceCount;
	if (capacity == 0)
	{
		return XR_SUCCESS;
	}
	if (values == nullptr)
	{
		return XR_ERROR_VALIDATION_FAILURE;
	}
	const uint32_t copyCount = capacity < sourceCount ? capacity : sourceCount;
	std::memcpy(values, source, static_cast<size_t>(copyCount) * elementSize);
	return capacity < sourceCount ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
}

bool IsFinitePose(const XrPosef& pose)
{
	const float values[] = {pose.position.x, pose.position.y, pose.position.z, pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
	for (float value : values)
	{
		if (!std::isfinite(value))
		{
			return false;
		}
	}
	const float length = pose.orientation.x * pose.orientation.x + pose.orientation.y * pose.orientation.y + pose.orientation.z * pose.orientation.z + pose.orientation.w * pose.orientation.w;
	return length > 1.0e-8f && std::fabs(length - 1.0f) < 0.01f;
}

bool IsValidInstance(XrInstance handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kInstanceMagic && handle->alive && handle->object != nullptr && !handle->object->closing.load(std::memory_order_acquire);
}

bool IsValidSession(XrSession handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kSessionMagic && handle->alive && handle->object != nullptr && !handle->object->closing;
}

bool IsValidSpace(XrSpace handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kSpaceMagic && handle->alive && handle->object != nullptr && IsValidSession(handle->object->session->handle);
}

bool IsValidActionSet(XrActionSet handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kActionSetMagic && handle->alive && handle->object != nullptr && IsValidInstance(handle->object->instance->handle) && !handle->object->destroyed;
}

bool IsValidAction(XrAction handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kActionMagic && handle->alive && handle->object != nullptr && IsValidActionSet(handle->object->actionSet->handle) && !handle->object->destroyed;
}

bool IsValidSwapchain(XrSwapchain handle)
{
	return handle != XR_NULL_HANDLE && handle->magic == kSwapchainMagic && handle->alive && handle->object != nullptr && IsValidSession(handle->object->session->handle) && !handle->object->destroyed;
}

XrResult GuardResult(const std::function<XrResult()>& callback) noexcept
{
	try
	{
		return callback();
	}
	catch (const std::bad_alloc&)
	{
		return XR_ERROR_OUT_OF_MEMORY;
	}
	catch (...)
	{
		return XR_ERROR_RUNTIME_FAILURE;
	}
}

} // namespace agentxr

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (loaderInfo == nullptr || runtimeRequest == nullptr || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO || runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST || loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || loaderInfo->structSize < sizeof(XrNegotiateLoaderInfo) || runtimeRequest->structSize < sizeof(XrNegotiateRuntimeRequest))
		{
			return XR_ERROR_INITIALIZATION_FAILED;
		}
		if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION || loaderInfo->minApiVersion > XR_API_VERSION_1_0 || loaderInfo->maxApiVersion < XR_API_VERSION_1_0)
		{
			return XR_ERROR_API_VERSION_UNSUPPORTED;
		}
		runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
		runtimeRequest->runtimeApiVersion = XR_API_VERSION_1_0;
		runtimeRequest->getInstanceProcAddr = &xrGetInstanceProcAddr;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (name == nullptr || function == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		*function = nullptr;
		const bool global = instance == XR_NULL_HANDLE;
		if (!global && !agentxr::IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
#define AGENTXR_GLOBAL_PROC(symbol) if (std::strcmp(name, #symbol) == 0) { *function = reinterpret_cast<PFN_xrVoidFunction>(&symbol); return XR_SUCCESS; }
		AGENTXR_GLOBAL_PROC(xrGetInstanceProcAddr)
		AGENTXR_GLOBAL_PROC(xrEnumerateApiLayerProperties)
		AGENTXR_GLOBAL_PROC(xrEnumerateInstanceExtensionProperties)
		AGENTXR_GLOBAL_PROC(xrCreateInstance)
#undef AGENTXR_GLOBAL_PROC
		if (global)
		{
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		}
#define AGENTXR_INSTANCE_PROC(symbol) if (std::strcmp(name, #symbol) == 0) { *function = reinterpret_cast<PFN_xrVoidFunction>(&symbol); return XR_SUCCESS; }
		AGENTXR_INSTANCE_PROC(xrDestroyInstance)
		AGENTXR_INSTANCE_PROC(xrGetInstanceProperties)
		AGENTXR_INSTANCE_PROC(xrPollEvent)
		AGENTXR_INSTANCE_PROC(xrResultToString)
		AGENTXR_INSTANCE_PROC(xrStructureTypeToString)
		AGENTXR_INSTANCE_PROC(xrGetSystem)
		AGENTXR_INSTANCE_PROC(xrGetSystemProperties)
		AGENTXR_INSTANCE_PROC(xrEnumerateEnvironmentBlendModes)
		AGENTXR_INSTANCE_PROC(xrCreateSession)
		AGENTXR_INSTANCE_PROC(xrDestroySession)
		AGENTXR_INSTANCE_PROC(xrEnumerateReferenceSpaces)
		AGENTXR_INSTANCE_PROC(xrCreateReferenceSpace)
		AGENTXR_INSTANCE_PROC(xrGetReferenceSpaceBoundsRect)
		AGENTXR_INSTANCE_PROC(xrCreateActionSpace)
		AGENTXR_INSTANCE_PROC(xrLocateSpace)
		AGENTXR_INSTANCE_PROC(xrDestroySpace)
		AGENTXR_INSTANCE_PROC(xrEnumerateViewConfigurations)
		AGENTXR_INSTANCE_PROC(xrGetViewConfigurationProperties)
		AGENTXR_INSTANCE_PROC(xrEnumerateViewConfigurationViews)
		AGENTXR_INSTANCE_PROC(xrEnumerateSwapchainFormats)
		AGENTXR_INSTANCE_PROC(xrCreateSwapchain)
		AGENTXR_INSTANCE_PROC(xrDestroySwapchain)
		AGENTXR_INSTANCE_PROC(xrEnumerateSwapchainImages)
		AGENTXR_INSTANCE_PROC(xrAcquireSwapchainImage)
		AGENTXR_INSTANCE_PROC(xrWaitSwapchainImage)
		AGENTXR_INSTANCE_PROC(xrReleaseSwapchainImage)
		AGENTXR_INSTANCE_PROC(xrBeginSession)
		AGENTXR_INSTANCE_PROC(xrEndSession)
		AGENTXR_INSTANCE_PROC(xrRequestExitSession)
		AGENTXR_INSTANCE_PROC(xrWaitFrame)
		AGENTXR_INSTANCE_PROC(xrBeginFrame)
		AGENTXR_INSTANCE_PROC(xrEndFrame)
		AGENTXR_INSTANCE_PROC(xrLocateViews)
		AGENTXR_INSTANCE_PROC(xrStringToPath)
		AGENTXR_INSTANCE_PROC(xrPathToString)
		AGENTXR_INSTANCE_PROC(xrCreateActionSet)
		AGENTXR_INSTANCE_PROC(xrDestroyActionSet)
		AGENTXR_INSTANCE_PROC(xrCreateAction)
		AGENTXR_INSTANCE_PROC(xrDestroyAction)
		AGENTXR_INSTANCE_PROC(xrSuggestInteractionProfileBindings)
		AGENTXR_INSTANCE_PROC(xrAttachSessionActionSets)
		AGENTXR_INSTANCE_PROC(xrGetCurrentInteractionProfile)
		AGENTXR_INSTANCE_PROC(xrGetActionStateBoolean)
		AGENTXR_INSTANCE_PROC(xrGetActionStateFloat)
		AGENTXR_INSTANCE_PROC(xrGetActionStateVector2f)
		AGENTXR_INSTANCE_PROC(xrGetActionStatePose)
		AGENTXR_INSTANCE_PROC(xrSyncActions)
		AGENTXR_INSTANCE_PROC(xrEnumerateBoundSourcesForAction)
		AGENTXR_INSTANCE_PROC(xrGetInputSourceLocalizedName)
		AGENTXR_INSTANCE_PROC(xrApplyHapticFeedback)
		AGENTXR_INSTANCE_PROC(xrStopHapticFeedback)
		if (std::strcmp(name, "xrGetD3D12GraphicsRequirementsKHR") == 0 && instance->object->d3d12Enabled)
		{
			*function = reinterpret_cast<PFN_xrVoidFunction>(&xrGetD3D12GraphicsRequirementsKHR);
			return XR_SUCCESS;
		}
#undef AGENTXR_INSTANCE_PROC
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t capacity, uint32_t* count, XrApiLayerProperties* properties)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (count == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		*count = 0;
		if (capacity != 0 && properties == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t capacity, uint32_t* count, XrExtensionProperties* properties)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (layerName != nullptr)
		{
			return XR_ERROR_API_LAYER_NOT_PRESENT;
		}
		std::array<XrExtensionProperties, 2> supported{};
		supported[0].type = XR_TYPE_EXTENSION_PROPERTIES;
		supported[0].extensionVersion = XR_KHR_D3D12_enable_SPEC_VERSION;
		agentxr::CopyString(supported[0].extensionName, sizeof(supported[0].extensionName), XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
		supported[1].type = XR_TYPE_EXTENSION_PROPERTIES;
		supported[1].extensionVersion = XR_EXT_local_floor_SPEC_VERSION;
		agentxr::CopyString(supported[1].extensionName, sizeof(supported[1].extensionName), XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
		return agentxr::EnumerateArray(capacity, count, properties, sizeof(XrExtensionProperties), supported.data(), static_cast<uint32_t>(supported.size()));
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::CheckType(createInfo, XR_TYPE_INSTANCE_CREATE_INFO) != XR_SUCCESS || instance == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (createInfo->applicationInfo.apiVersion != 0 && (XR_VERSION_MAJOR(createInfo->applicationInfo.apiVersion) != 1 || XR_VERSION_MINOR(createInfo->applicationInfo.apiVersion) > 0))
		{
			return XR_ERROR_API_VERSION_UNSUPPORTED;
		}
		std::unordered_set<std::string> requested;
		for (uint32_t index = 0; index < createInfo->enabledApiLayerCount; ++index)
		{
			if (createInfo->enabledApiLayerNames == nullptr || createInfo->enabledApiLayerNames[index] == nullptr)
			{
				return XR_ERROR_VALIDATION_FAILURE;
			}
			return XR_ERROR_API_LAYER_NOT_PRESENT;
		}
		bool d3d12 = false;
		bool localFloor = false;
		for (uint32_t index = 0; index < createInfo->enabledExtensionCount; ++index)
		{
			if (createInfo->enabledExtensionNames == nullptr || createInfo->enabledExtensionNames[index] == nullptr)
			{
				return XR_ERROR_VALIDATION_FAILURE;
			}
			const std::string extension(createInfo->enabledExtensionNames[index]);
			if (!requested.insert(extension).second)
			{
				return XR_ERROR_NAME_DUPLICATED;
			}
			if (!agentxr::IsExpectedExtension(extension))
			{
				return XR_ERROR_EXTENSION_NOT_PRESENT;
			}
			d3d12 = d3d12 || extension == XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
			localFloor = localFloor || extension == XR_EXT_LOCAL_FLOOR_EXTENSION_NAME;
		}
		auto* state = new agentxr::Instance;
		state->clock = agentxr::Clock::Create();
		state->d3d12Enabled = d3d12;
		state->localFloorEnabled = localFloor;
		state->instanceId = agentxr::MakeInstanceId();
		state->processCreationTime = agentxr::ProcessCreationTime();
		state->manifestPath = agentxr::ModulePath().parent_path() / L"agent-xr.json";
		auto* handle = new XrInstance_T;
		handle->object = state;
		state->handle = handle;
		state->InternPath("/user/head");
		state->InternPath("/user/hand/left");
		state->InternPath("/user/hand/right");
		state->InternPath("/interaction_profiles/oculus/touch_controller");
		const char* paths[] = {
			"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose", "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose",
			"/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value", "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch",
			"/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value",
			"/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick", "/user/hand/left/input/thumbstick/x", "/user/hand/left/input/thumbstick/y",
			"/user/hand/right/input/thumbstick/x", "/user/hand/right/input/thumbstick/y", "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click",
			"/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch", "/user/hand/left/input/thumbrest/touch", "/user/hand/right/input/thumbrest/touch",
			"/user/hand/left/input/x/click", "/user/hand/left/input/y/click", "/user/hand/left/input/menu/click", "/user/hand/right/input/a/click", "/user/hand/right/input/b/click",
			"/user/hand/left/input/x/touch", "/user/hand/left/input/y/touch", "/user/hand/right/input/a/touch", "/user/hand/right/input/b/touch",
			"/user/hand/left/output/haptic", "/user/hand/right/output/haptic"};
		for (const char* path : paths)
		{
			state->InternPath(path);
		}
		state->control = std::make_unique<agentxr::ControlServer>(*state);
		if (!state->control->Start())
		{
			state->control.reset();
			delete handle;
			delete state;
			return XR_ERROR_INITIALIZATION_FAILED;
		}
		*instance = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (instance == XR_NULL_HANDLE || instance->magic != agentxr::kInstanceMagic || !instance->alive || instance->object == nullptr)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		agentxr::Instance* state = instance->object;
		state->closing.store(true, std::memory_order_release);
		state->publicGeneration.store(0, std::memory_order_release);
		{
			std::lock_guard lock(agentxr::gActiveSessionMutex);
			if (agentxr::gActiveSession != nullptr && agentxr::gActiveSession->instance == state)
			{
				agentxr::gActiveSession = nullptr;
			}
		}
		if (state->control)
		{
			state->control->Stop();
			state->control.reset();
		}
		state->InvalidateChildren();
		instance->alive = false;
		instance->object = nullptr;
		agentxr::RetainTombstone(instance);
		delete state;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* properties)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS || agentxr::CheckOutput(properties, XR_TYPE_INSTANCE_PROPERTIES) != XR_SUCCESS)
		{
			return agentxr::IsValidInstance(instance) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
		}
		properties->runtimeVersion = XR_API_VERSION_1_0;
		agentxr::CopyString(properties->runtimeName, sizeof(properties->runtimeName), "AgentXR");
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (eventData == nullptr || eventData->type != XR_TYPE_EVENT_DATA_BUFFER)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		return instance->object->PopEvent(*eventData) ? XR_SUCCESS : XR_EVENT_UNAVAILABLE;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE])
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS || buffer == nullptr)
		{
			return agentxr::IsValidInstance(instance) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
		}
		const char* text = "XR_UNKNOWN";
		switch (value)
		{
		case XR_SUCCESS: text = "XR_SUCCESS"; break;
		case XR_TIMEOUT_EXPIRED: text = "XR_TIMEOUT_EXPIRED"; break;
		case XR_EVENT_UNAVAILABLE: text = "XR_EVENT_UNAVAILABLE"; break;
		case XR_FRAME_DISCARDED: text = "XR_FRAME_DISCARDED"; break;
		case XR_ERROR_VALIDATION_FAILURE: text = "XR_ERROR_VALIDATION_FAILURE"; break;
		case XR_ERROR_HANDLE_INVALID: text = "XR_ERROR_HANDLE_INVALID"; break;
		case XR_ERROR_API_VERSION_UNSUPPORTED: text = "XR_ERROR_API_VERSION_UNSUPPORTED"; break;
		case XR_ERROR_EXTENSION_NOT_PRESENT: text = "XR_ERROR_EXTENSION_NOT_PRESENT"; break;
		case XR_ERROR_CALL_ORDER_INVALID: text = "XR_ERROR_CALL_ORDER_INVALID"; break;
		case XR_ERROR_TIME_INVALID: text = "XR_ERROR_TIME_INVALID"; break;
		default: break;
		}
		agentxr::CopyString(buffer, XR_MAX_RESULT_STRING_SIZE, text);
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE])
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS || buffer == nullptr)
		{
			return agentxr::IsValidInstance(instance) ? XR_ERROR_VALIDATION_FAILURE : XR_ERROR_HANDLE_INVALID;
		}
		std::string result = "XR_TYPE_UNKNOWN";
		switch (value)
		{
#define XR_TYPE_CASE(name) case name: result = #name; break;
			XR_TYPE_CASE(XR_TYPE_INSTANCE_CREATE_INFO)
			XR_TYPE_CASE(XR_TYPE_SESSION_CREATE_INFO)
			XR_TYPE_CASE(XR_TYPE_FRAME_STATE)
			XR_TYPE_CASE(XR_TYPE_FRAME_END_INFO)
			XR_TYPE_CASE(XR_TYPE_SPACE_LOCATION)
			XR_TYPE_CASE(XR_TYPE_VIEW)
			XR_TYPE_CASE(XR_TYPE_COMPOSITION_LAYER_PROJECTION)
			XR_TYPE_CASE(XR_TYPE_COMPOSITION_LAYER_QUAD)
#undef XR_TYPE_CASE
		default: break;
		}
		agentxr::CopyString(buffer, XR_MAX_STRUCTURE_NAME_SIZE, result);
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (agentxr::CheckType(getInfo, XR_TYPE_SYSTEM_GET_INFO) != XR_SUCCESS || systemId == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
		{
			return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
		}
		*systemId = instance->object->systemId;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (systemId != instance->object->systemId)
		{
			return XR_ERROR_SYSTEM_INVALID;
		}
		if (agentxr::CheckOutput(properties, XR_TYPE_SYSTEM_PROPERTIES) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		properties->systemId = systemId;
		properties->vendorId = 0x415852;
		agentxr::CopyString(properties->systemName, sizeof(properties->systemName), "AgentXR");
		properties->graphicsProperties = {4096, 4096, XR_MIN_COMPOSITION_LAYERS_SUPPORTED};
		properties->trackingProperties = {XR_TRUE, XR_TRUE};
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType, uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* modes)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (systemId != instance->object->systemId)
		{
			return XR_ERROR_SYSTEM_INVALID;
		}
		if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		{
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		}
		const XrEnvironmentBlendMode mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		return agentxr::EnumerateArray(capacity, count, modes, sizeof(mode), &mode, 1);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (agentxr::EnsureInstance(instance) != XR_SUCCESS)
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (agentxr::CheckType(createInfo, XR_TYPE_SESSION_CREATE_INFO) != XR_SUCCESS || session == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (createInfo->systemId != instance->object->systemId)
		{
			return XR_ERROR_SYSTEM_INVALID;
		}
		if (!instance->object->d3d12Enabled)
		{
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		if (!instance->object->graphicsRequirementsQueried.load(std::memory_order_acquire))
		{
			return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
		}
		const XrGraphicsBindingD3D12KHR* binding = nullptr;
		if (!agentxr::FindD3D12Binding(createInfo, binding) || binding == nullptr || binding->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR || binding->device == nullptr || binding->queue == nullptr)
		{
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		LUID requiredLuid{};
		D3D_FEATURE_LEVEL requiredLevel = D3D_FEATURE_LEVEL_11_0;
		if (!agentxr::GetHighPerformanceAdapter(requiredLuid, requiredLevel))
		{
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		const LUID suppliedLuid = binding->device->GetAdapterLuid();
		if (suppliedLuid.LowPart != requiredLuid.LowPart || suppliedLuid.HighPart != requiredLuid.HighPart || !agentxr::SupportsFeatureLevel(binding->device, requiredLevel))
		{
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		const D3D12_COMMAND_QUEUE_DESC queueDescription = binding->queue->GetDesc();
		if (queueDescription.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
		{
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		std::scoped_lock sessionCreationLock(instance->object->mutex, agentxr::gActiveSessionMutex);
		if (agentxr::gActiveSession != nullptr)
		{
			return XR_ERROR_LIMIT_REACHED;
		}
		const uint64_t currentGeneration = instance->object->generationCounter.load(std::memory_order_acquire);
		if (currentGeneration >= std::numeric_limits<uint64_t>::max() - 1)
		{
			return XR_ERROR_LIMIT_REACHED;
		}
		const uint64_t nextGeneration = currentGeneration + 1;
		auto* state = new agentxr::Session;
		state->instance = instance->object;
		state->device = binding->device;
		state->queue = binding->queue;
		state->adapterLuid = suppliedLuid;
		state->sessionGeneration = nextGeneration;
		state->localOrigin = agentxr::ToXrPose(state->fallbackState.head);
		state->localFloorOrigin = state->localOrigin;
		state->localFloorOrigin.position.y = 0.0f;
		state->stageOrigin = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		state->report.status = "ready";
		auto* handle = new XrSession_T;
		handle->object = state;
		state->handle = handle;
		state->compositor = new agentxr::Compositor(*state);
		if (!state->compositor->Initialize(binding->device, binding->queue))
		{
			delete state->compositor;
			delete handle;
			delete state;
			return XR_ERROR_INITIALIZATION_FAILED;
		}
		instance->object->sessions.push_back(handle);
		instance->object->generationCounter.store(nextGeneration, std::memory_order_release);
		agentxr::gActiveSession = state;
		instance->object->publicGeneration.store(state->sessionGeneration, std::memory_order_release);
		state->QueueState(XR_SESSION_STATE_IDLE);
		state->QueueState(XR_SESSION_STATE_READY);
		*session = handle;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		agentxr::Session* state = session->object;
		agentxr::Instance* owner = state->instance;
		{
			std::scoped_lock sessionLocks(owner->mutex, state->mutex, agentxr::gActiveSessionMutex);
			state->closing = true;
			state->frameWaited = false;
			state->frameBegun = false;
			state->waitedFrameId = 0;
			state->begunFrameId = 0;
			state->nextDisplayTime = 0;
			state->nextDeadlineQpc = 0;
			state->InvalidateChildren();
			owner->sessions.erase(std::remove(owner->sessions.begin(), owner->sessions.end(), session), owner->sessions.end());
			if (agentxr::gActiveSession == state)
			{
				agentxr::gActiveSession = nullptr;
			}
		}
		if (state->compositor != nullptr)
		{
			state->compositor->Shutdown();
			delete state->compositor;
			state->compositor = nullptr;
		}
		session->alive = false;
		session->object = nullptr;
		agentxr::RetainTombstone(session);
		delete state;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (agentxr::CheckType(beginInfo, XR_TYPE_SESSION_BEGIN_INFO) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		agentxr::Session& state = *session->object;
		std::lock_guard lock(state.mutex);
		if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		{
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		}
		if (state.state != XR_SESSION_STATE_READY || state.running)
		{
			return XR_ERROR_CALL_ORDER_INVALID;
		}
		if (state.compositor == nullptr || !state.compositor->StartPresentation())
		{
			return XR_ERROR_INITIALIZATION_FAILED;
		}
		state.viewConfiguration = beginInfo->primaryViewConfigurationType;
		state.running = true;
		state.requestExit = false;
		state.nextDisplayTime = state.instance->clock.Now() + 11111111;
		state.nextDeadlineQpc = state.instance->clock.ToQpc(state.nextDisplayTime);
		state.QueueState(XR_SESSION_STATE_SYNCHRONIZED);
		state.QueueState(XR_SESSION_STATE_VISIBLE);
		state.QueueState(XR_SESSION_STATE_FOCUSED);
		state.report.status = "running";
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		agentxr::Session& state = *session->object;
		agentxr::Compositor* compositor = nullptr;
		{
			std::unique_lock lock(state.mutex);
			if (!state.running || state.state != XR_SESSION_STATE_STOPPING)
			{
				return XR_ERROR_SESSION_NOT_STOPPING;
			}
			state.running = false;
			state.frameWaited = false;
			state.frameBegun = false;
			state.waitedFrameId = 0;
			state.begunFrameId = 0;
			state.nextDisplayTime = 0;
			state.nextDeadlineQpc = 0;
			state.QueueState(XR_SESSION_STATE_IDLE);
			state.QueueState(XR_SESSION_STATE_READY);
			state.report.status = "ready";
			compositor = state.compositor;
		}
		if (compositor != nullptr)
		{
			compositor->StopPresentation();
		}
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession session)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		agentxr::Session& state = *session->object;
		std::lock_guard lock(state.mutex);
		if (!state.running)
		{
			return XR_ERROR_SESSION_NOT_RUNNING;
		}
		state.requestExit = true;
		state.QueueState(XR_SESSION_STATE_STOPPING);
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo* waitInfo, XrFrameState* frameState)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (agentxr::CheckType(waitInfo, XR_TYPE_FRAME_WAIT_INFO) != XR_SUCCESS || agentxr::CheckOutput(frameState, XR_TYPE_FRAME_STATE) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		agentxr::Session& state = *session->object;
		std::unique_lock lock(state.mutex);
		if (!state.IsFocused())
		{
			return XR_ERROR_SESSION_NOT_RUNNING;
		}
		if (state.waitedFrameId != 0)
		{
			return XR_ERROR_CALL_ORDER_INVALID;
		}
		const int64_t deadline = state.nextDeadlineQpc;
		lock.unlock();
		const bool waited = state.instance->clock.SleepUntilQpc(deadline);
		lock.lock();
		if (!state.IsFocused())
		{
			state.frameWaited = false;
			state.waitedFrameId = 0;
			return XR_ERROR_SESSION_NOT_RUNNING;
		}
		if (!waited)
		{
			return XR_ERROR_RUNTIME_FAILURE;
		}
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		const bool late = now.QuadPart > deadline;
		XrTime predicted = state.instance->clock.FromQpc(deadline);
		if (predicted <= state.nextDisplayTime)
		{
			predicted = state.nextDisplayTime + 1;
		}
		state.nextDisplayTime = predicted;
		state.nextDeadlineQpc = deadline + state.instance->clock.qpcFrequency / 90;
		if (state.nextDeadlineQpc < now.QuadPart)
		{
			state.nextDeadlineQpc = now.QuadPart;
		}
		state.frameId++;
		agentxr::FrameRecord frameRecord;
		frameRecord.id = state.frameId;
		frameRecord.displayTime = predicted;
		frameRecord.period = 11111111;
		frameRecord.waited = true;
		frameRecord.late = late;
		state.frames.push_back(frameRecord);
		state.waitedFrameId = frameRecord.id;
		state.frameWaited = true;
		if (state.frames.size() > agentxr::protocol::kMaxRecordsPerRun)
		{
			state.frames.erase(state.frames.begin());
			state.report.overflow = true;
		}
		if (late)
		{
			++state.report.missedFrameCount;
		}
		if (state.activeEpoch != nullptr && state.timelineStart == 0)
		{
			state.timelineStart = predicted;
			state.report.authoredStart = predicted;
			state.report.status = "running";
			state.lastPublishedState = state.activeEpoch->samples.empty() ? state.fallbackState : state.activeEpoch->samples.front().state;
		}
		agentxr::FrameRecord& waitedRecord = state.frames.back();
		if (state.activeEpoch != nullptr && predicted >= state.timelineStart + state.activeEpoch->durationNs)
		{
			state.report.status = state.canceledAt.has_value() ? "canceled" : "completed";
			state.report.authoredEnd = state.timelineStart + state.activeEpoch->durationNs;
		}
		if (state.activeEpoch != nullptr && state.timelineStart > 0)
		{
			const int64_t offset = std::clamp<int64_t>(predicted - state.timelineStart, 0, state.activeEpoch->durationNs);
			const auto sampleEnd = std::upper_bound(state.activeEpoch->samples.begin(), state.activeEpoch->samples.end(), offset, [](int64_t value, const agentxr::TimelineSample& sample) { return value < sample.offsetNs; });
			const uint32_t applied = static_cast<uint32_t>(sampleEnd - state.activeEpoch->samples.begin());
			state.lastPublishedState = state.StateAtLocked(predicted);
			waitedRecord.authoredSamples = static_cast<uint32_t>(state.activeEpoch->samples.size());
			waitedRecord.appliedSamples = applied;
			state.report.appliedSamples = std::max(state.report.appliedSamples, applied);
			state.report.undersampled = state.report.undersampled || (state.activeEpoch->samples.size() > 1 && applied < state.activeEpoch->samples.size() && predicted >= state.timelineStart + state.activeEpoch->durationNs);
		}
		frameState->predictedDisplayTime = predicted;
		frameState->predictedDisplayPeriod = 11111111;
		frameState->shouldRender = state.requestExit ? XR_FALSE : XR_TRUE;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo* beginInfo)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (agentxr::CheckType(beginInfo, XR_TYPE_FRAME_BEGIN_INFO) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		agentxr::Session& state = *session->object;
		std::lock_guard lock(state.mutex);
		if (!state.running || (state.state != XR_SESSION_STATE_FOCUSED && state.state != XR_SESSION_STATE_STOPPING) || state.waitedFrameId == 0 || state.begunFrameId != 0)
		{
			return XR_ERROR_CALL_ORDER_INVALID;
		}
		const uint64_t begunFrameId = state.waitedFrameId;
		auto frameRecord = std::find_if(state.frames.begin(), state.frames.end(), [begunFrameId](const agentxr::FrameRecord& record)
		{
			return record.id == begunFrameId;
		});
		if (frameRecord == state.frames.end())
		{
			return XR_ERROR_RUNTIME_FAILURE;
		}
		state.waitedFrameId = 0;
		state.frameWaited = false;
		state.begunFrameId = begunFrameId;
		state.frameBegun = true;
		frameRecord->begun = true;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* endInfo)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidSession(session))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (agentxr::CheckType(endInfo, XR_TYPE_FRAME_END_INFO) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		agentxr::Session& state = *session->object;
		std::unique_lock lock(state.mutex);
		if (state.begunFrameId == 0)
		{
			return XR_ERROR_CALL_ORDER_INVALID;
		}
		const uint64_t frameId = state.begunFrameId;
		auto frameRecord = std::find_if(state.frames.begin(), state.frames.end(), [frameId](const agentxr::FrameRecord& record)
		{
			return record.id == frameId;
		});
		if (frameRecord == state.frames.end())
		{
			return XR_ERROR_RUNTIME_FAILURE;
		}
		if (endInfo->displayTime != frameRecord->displayTime)
		{
			return XR_ERROR_TIME_INVALID;
		}
		if (endInfo->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
		{
			return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
		}
		if (endInfo->layerCount > XR_MIN_COMPOSITION_LAYERS_SUPPORTED || (endInfo->layerCount != 0 && endInfo->layers == nullptr))
		{
			return XR_ERROR_LAYER_LIMIT_EXCEEDED;
		}
		for (uint32_t index = 0; index < endInfo->layerCount; ++index)
		{
			if (endInfo->layers[index] == nullptr || (endInfo->layers[index]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION && endInfo->layers[index]->type != XR_TYPE_COMPOSITION_LAYER_QUAD))
			{
				return XR_ERROR_LAYER_INVALID;
			}
		}
		const XrTime displayTime = endInfo->displayTime;
		lock.unlock();
		const XrResult composeResult = state.compositor == nullptr ? XR_ERROR_GRAPHICS_DEVICE_INVALID : state.compositor->Compose(*endInfo, frameId);
		lock.lock();
		frameRecord = std::find_if(state.frames.begin(), state.frames.end(), [frameId](const agentxr::FrameRecord& record)
		{
			return record.id == frameId;
		});
		if (frameRecord == state.frames.end())
		{
			state.begunFrameId = 0;
			state.frameBegun = false;
			return XR_ERROR_RUNTIME_FAILURE;
		}
		frameRecord->presentResult = static_cast<int32_t>(composeResult);
		if (composeResult != XR_SUCCESS)
		{
			state.report.error = "compositor failed";
			state.report.status = "failed";
			frameRecord->discarded = true;
			state.begunFrameId = 0;
			state.frameBegun = false;
			return composeResult;
		}
		frameRecord->ended = true;
		frameRecord->layerCount = endInfo->layerCount;
		frameRecord->presented = state.compositor != nullptr && state.compositor->LastPresentedFrame() == frameId;
		state.report.submittedFrameId = frameId;
		state.report.composedFrameId = frameId;
		if (frameRecord->presented)
		{
			state.report.presentedFrameId = frameId;
		}
		if (state.report.actualFirstFrame == 0)
		{
			state.report.actualFirstFrame = displayTime;
		}
		state.report.actualLastFrame = displayTime;
		auto previous = std::find_if(state.frames.rbegin(), state.frames.rend(), [frameId](const agentxr::FrameRecord& record)
		{
			return record.id < frameId;
		});
		if (previous != state.frames.rend() && displayTime >= previous->displayTime)
		{
			state.report.maxFrameGap = std::max(state.report.maxFrameGap, displayTime - previous->displayTime);
		}
		state.begunFrameId = 0;
		state.frameBegun = false;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId, uint32_t capacity, uint32_t* count, XrViewConfigurationType* types)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (systemId != instance->object->systemId)
		{
			return XR_ERROR_SYSTEM_INVALID;
		}
		const XrViewConfigurationType type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		return agentxr::EnumerateArray(capacity, count, types, sizeof(type), &type, 1);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type, XrViewConfigurationProperties* properties)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (systemId != instance->object->systemId || type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		{
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		}
		if (agentxr::CheckOutput(properties, XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) != XR_SUCCESS)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		properties->viewConfigurationType = type;
		properties->fovMutable = XR_FALSE;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type, uint32_t capacity, uint32_t* count, XrViewConfigurationView* views)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (systemId != instance->object->systemId || type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
		{
			return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
		}
		if (count == nullptr)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		*count = 2;
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
			if (agentxr::CheckOutput(&views[index], XR_TYPE_VIEW_CONFIGURATION_VIEW) != XR_SUCCESS)
			{
				return XR_ERROR_VALIDATION_FAILURE;
			}
			views[index].recommendedImageRectWidth = 1024;
			views[index].maxImageRectWidth = 4096;
			views[index].recommendedImageRectHeight = 1024;
			views[index].maxImageRectHeight = 4096;
			views[index].recommendedSwapchainSampleCount = 1;
			views[index].maxSwapchainSampleCount = 1;
		}
		return capacity < 2 ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetD3D12GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* requirements)
{
	return agentxr::GuardResult([&]() -> XrResult
	{
		if (!agentxr::IsValidInstance(instance))
		{
			return XR_ERROR_HANDLE_INVALID;
		}
		if (!instance->object->d3d12Enabled)
		{
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		}
		if (systemId != instance->object->systemId)
		{
			return XR_ERROR_SYSTEM_INVALID;
		}
		if (requirements == nullptr || requirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR)
		{
			return XR_ERROR_VALIDATION_FAILURE;
		}
		D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		if (!agentxr::GetHighPerformanceAdapter(requirements->adapterLuid, level))
		{
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
		requirements->minFeatureLevel = level;
		instance->object->graphicsRequirementsQueried.store(true, std::memory_order_release);
		return XR_SUCCESS;
	});
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
	return TRUE;
}
