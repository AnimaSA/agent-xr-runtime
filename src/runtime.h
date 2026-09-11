#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define XR_USE_GRAPHICS_API_D3D12 1
#define XR_NO_PROTOTYPES 1
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <wrl.h>

#include <functional>

 
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "protocol.h"

namespace agentxr
{
class ControlServer;
class Compositor;
struct Instance;
struct Session;
struct Space;
struct ActionSet;
struct Action;
struct Swapchain;
struct TimelineEpoch;

inline constexpr uint64_t kInstanceMagic = 0x415852494E535441ULL;
inline constexpr uint64_t kSessionMagic = 0x415852534553534FULL;
inline constexpr uint64_t kSpaceMagic = 0x4158525350414345ULL;
inline constexpr uint64_t kActionSetMagic = 0x4158524153545345ULL;
inline constexpr uint64_t kActionMagic = 0x415852414354494FULL;
inline constexpr uint64_t kSwapchainMagic = 0x4158525357415043ULL;

struct Vec3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Quat
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 1.0f;
};

struct TrackedPose
{
	Vec3 position;
	Quat orientation;
	bool positionValid = true;
	bool orientationValid = true;
	bool positionTracked = true;
	bool orientationTracked = true;
};

struct ControllerState
{
	bool active = true;
	std::array<bool, 8> buttons{};
	std::array<bool, 16> touches{};
	float trigger = 0.0f;
	float squeeze = 0.0f;
	XrVector2f thumbstick{0.0f, 0.0f};
};

struct SimState
{
	TrackedPose head;
	TrackedPose leftGrip;
	TrackedPose leftAim;
	TrackedPose rightGrip;
	TrackedPose rightAim;
	ControllerState left;
	ControllerState right;
};

SimState DefaultSimState();
XrPosef ToXrPose(const TrackedPose& pose);
TrackedPose FromXrPose(const XrPosef& pose);

struct DiscreteChange
{
	int64_t offsetNs = 0;
	bool left = false;
	uint8_t channel = 0;
	bool oldValue = false;
	bool newValue = false;
};

struct TimelineSample
{
	int64_t offsetNs = 0;
	SimState state;
};

struct TimelineEpoch
{
	uint64_t id = 0;
	int64_t durationNs = 0;
	uint32_t sampleRateHz = 90;
	std::vector<TimelineSample> samples;
	std::vector<DiscreteChange> digitalChanges;
};

struct RetainedEpoch
{
	XrTime startTime = 0;
	std::shared_ptr<const TimelineEpoch> epoch;
};

struct FrameRecord
{
	uint64_t id = 0;
	XrTime displayTime = 0;
	XrDuration period = 0;
	uint32_t layerCount = 0;
	bool waited = false;
	bool begun = false;
	bool ended = false;
	bool discarded = false;
	bool late = false;
	bool presented = false;
	bool presentOccluded = false;
	int32_t presentResult = 0;
	uint32_t authoredSamples = 0;
	uint32_t appliedSamples = 0;
	uint32_t observedSamples = 0;
};


struct ActionSyncRecord
{
	XrTime time = 0;
	bool leftActive = true;
	bool rightActive = true;
	float leftTrigger = 0.0f;
	float rightTrigger = 0.0f;
	float leftSqueeze = 0.0f;
	float rightSqueeze = 0.0f;
	XrVector2f leftThumbstick{0.0f, 0.0f};
	XrVector2f rightThumbstick{0.0f, 0.0f};
	std::array<bool, 8> leftButtons{};
	std::array<bool, 8> rightButtons{};
	uint32_t observedSamples = 0;
	uint32_t unobservedDigitalTransitions = 0;
};

struct HapticRecord
{
	XrTime time = 0;
	bool left = false;
	float amplitude = 0.0f;
	float frequency = 0.0f;
	XrDuration duration = 0;
	bool stopped = false;
};

struct RunReport
{
	uint64_t timelineId = 0;
	std::string status = "idle";
	std::string error;
	XrTime authoredStart = 0;
	XrTime authoredEnd = 0;
	XrTime actualFirstFrame = 0;
	XrTime actualLastFrame = 0;
	XrDuration framePeriod = 0;
	XrDuration maxFrameGap = 0;
	uint32_t plannedSamples = 0;
	uint32_t appliedSamples = 0;
	uint32_t observedSamples = 0;
	uint32_t unobservedDigitalTransitions = 0;
	uint32_t lateSampleCount = 0;
	uint32_t missedFrameCount = 0;
	uint32_t validityTransitions = 0;
	uint32_t trackingTransitions = 0;
	bool overflow = false;
	bool undersampled = false;
	uint64_t submittedFrameId = 0;
	uint64_t composedFrameId = 0;
	uint64_t presentedFrameId = 0;
	std::vector<FrameRecord> frames;
	std::vector<ActionSyncRecord> actionSyncs;
	std::vector<HapticRecord> haptics;
};

struct Clock
{
	int64_t qpcFrequency = 0;
	int64_t qpcOrigin = 0;
	mutable std::atomic<XrTime> lastTime{0};

	Clock() = default;
	Clock(const Clock& other)
		: qpcFrequency(other.qpcFrequency)
		, qpcOrigin(other.qpcOrigin)
		, lastTime(other.lastTime.load(std::memory_order_relaxed))
	{
	}
	Clock& operator=(const Clock& other)
	{
		qpcFrequency = other.qpcFrequency;
		qpcOrigin = other.qpcOrigin;
		lastTime.store(other.lastTime.load(std::memory_order_relaxed), std::memory_order_relaxed);
		return *this;
	}

	static Clock Create();
	XrTime Now() const;
	XrTime FromQpc(int64_t qpc) const;
	int64_t ToQpc(XrTime time) const;
	bool SleepUntilQpc(int64_t targetQpc) const;
};

enum class SpaceKind : uint8_t
{
	View,
	Local,
	Stage,
	LocalFloor,
	Action
};

enum class InputKind : uint8_t
{
	Button,
	Touch,
	Trigger,
	Squeeze,
	Thumbstick,
	ThumbstickX,
	ThumbstickY,
	Pose,
	Haptic
};

struct InputRef
{
	bool left = false;
	bool head = false;
	InputKind kind = InputKind::Button;
	uint8_t channel = 0;
};

struct Binding
{
	XrPath path = XR_NULL_PATH;
	XrPath subactionPath = XR_NULL_PATH;
	InputRef input;
};

struct ActionSnapshot
{
	bool active = false;
	bool changed = false;
	XrTime lastChangeTime = 0;
	bool booleanValue = false;
	float floatValue = 0.0f;
	XrVector2f vectorValue{0.0f, 0.0f};
};

struct Instance
{
	XrInstance_T* handle = nullptr;
	Clock clock;
	std::atomic<bool> closing{false};
	bool d3d12Enabled = false;
	std::atomic<bool> graphicsRequirementsQueried{false};
	bool localFloorEnabled = false;
	XrSystemId systemId = 1;
	std::string instanceId;
	std::filesystem::path manifestPath;
	uint64_t processCreationTime = 0;
	mutable std::mutex mutex;
	std::mutex eventMutex;
	std::deque<std::vector<uint8_t>> events;
	uint32_t lostEventCount = 0;
	std::vector<XrSession> sessions;
	std::vector<XrActionSet> actionSets;
	std::unordered_map<std::string, XrPath> paths;
	std::unordered_map<XrPath, std::string> pathStrings;
	XrPath nextPath = 1;
	std::unordered_map<XrPath, std::vector<std::pair<XrAction, XrPath>>> suggestions;
	std::unique_ptr<ControlServer> control;
	std::atomic<uint64_t> generationCounter{0};
	std::atomic<uint64_t> publicGeneration{0};
	std::atomic<uint64_t> nextConnection{1};
	std::mutex leaseMutex;
	uint64_t controllerLease = 0;

	XrPath InternPath(std::string_view path);
	std::string PathString(XrPath path) const;
	bool IsPath(XrPath path, std::string_view expected) const;
	void QueueEvent(const XrEventDataBaseHeader* eventData, size_t size);
	bool PopEvent(XrEventDataBuffer& eventData);
	XrResult HandleControl(uint64_t connectionId, const protocol::Json& request, protocol::PipeFrame& response);
	bool AcquireLease(uint64_t connectionId);
	void ReleaseLease(uint64_t connectionId, bool cancelRun);
	void InvalidateChildren();
};

struct Session
{
	XrSession_T* handle = nullptr;
	Instance* instance = nullptr;
	mutable std::mutex mutex;
	XrSessionState state = XR_SESSION_STATE_IDLE;
	XrViewConfigurationType viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	bool running = false;
	bool frameWaited = false;
	bool frameBegun = false;
	bool actionSetsAttached = false;
	bool closing = false;
	bool requestExit = false;
	XrTime nextDisplayTime = 0;
	int64_t nextDeadlineQpc = 0;
	uint64_t frameId = 0;
	uint64_t nextTimelineId = 1;
	uint64_t sessionGeneration = 0;
	XrTime lastSyncTime = 0;
	XrPosef localOrigin{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	XrPosef localFloorOrigin{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	XrPosef stageOrigin{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	bool inputsNeutralized = false;
	bool neutralizePending = false;
	SimState fallbackState = DefaultSimState();
	SimState lastPublishedState = DefaultSimState();
	RunReport report;
	std::shared_ptr<const TimelineEpoch> activeEpoch;
	XrTime timelineStart = 0;
	std::deque<RetainedEpoch> retainedEpochs;
	std::deque<std::pair<uint64_t, RunReport>> completedReports;
	std::optional<XrTime> canceledAt;
	std::vector<FrameRecord> frames;
	std::vector<ActionSyncRecord> actionSyncs;
	std::vector<HapticRecord> haptics;
	std::vector<XrSpace> spaces;
	std::vector<XrActionSet> actionSets;
	std::vector<XrSwapchain> swapchains;
	Compositor* compositor = nullptr;
	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	LUID adapterLuid{};

	bool IsFocused() const;
	void QueueState(XrSessionState newState);
	XrTime ValidateTime(XrTime time) const;
	SimState StateAt(XrTime time, std::shared_ptr<const TimelineEpoch>* epoch = nullptr) const;
	SimState StateAtLocked(XrTime time, std::shared_ptr<const TimelineEpoch>* epoch = nullptr) const;
	XrResult SubmitTimeline(const protocol::Json& timeline, uint64_t& timelineId, std::string& error);
	XrResult CancelTimeline(uint64_t timelineId, std::string& error);
	protocol::Json Snapshot() const;
	protocol::Json ReportPage(uint64_t timelineId, size_t cursor, size_t limit) const;
	XrResult Capture(uint64_t afterFrameId, protocol::Json& metadata, std::vector<uint8_t>& png, uint32_t timeoutMs);
	void NeutralizeInputs();
	void InvalidateChildren();
};

struct Space
{
	XrSpace_T* handle = nullptr;
	Session* session = nullptr;
	SpaceKind kind = SpaceKind::Local;
	XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrPosef poseInParent{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
	XrAction action = XR_NULL_HANDLE;
	XrPath subactionPath = XR_NULL_PATH;
};

struct ActionSet
{
	XrActionSet_T* handle = nullptr;
	Instance* instance = nullptr;
	std::string name;
	std::string localizedName;
	uint32_t priority = 0;
	bool destroyed = false;
	std::vector<XrAction> actions;
};

struct Action
{
	XrAction_T* handle = nullptr;
	ActionSet* actionSet = nullptr;
	Instance* instance = nullptr;
	std::string name;
	std::string localizedName;
	XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
	std::vector<XrPath> subactionPaths;
	std::vector<Binding> bindings;
	std::unordered_map<XrPath, ActionSnapshot> snapshots;
	ActionSnapshot aggregate;
	bool destroyed = false;
};

struct SwapchainImage
{
	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	uint64_t fenceValue = 0;
	bool acquired = false;
	bool waited = false;
	bool released = false;
	D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct Swapchain
{
	XrSwapchain_T* handle = nullptr;
	Session* session = nullptr;
	XrSwapchainCreateInfo info{};
	DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
	std::vector<SwapchainImage> images;
	uint32_t nextIndex = 0;
	std::deque<uint32_t> acquiredIndices;
	std::deque<uint32_t> waitedIndices;
	uint32_t lastReleasedIndex = UINT32_MAX;
	bool staticImage = false;
	bool destroyed = false;
};

class Compositor
{
public:
	explicit Compositor(Session& session);
	~Compositor();
	bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
	void Shutdown();
	XrResult CreateSwapchain(const XrSwapchainCreateInfo& info, Swapchain& swapchain);
	XrResult EnumerateSwapchainImages(Swapchain& swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images);
	XrResult AcquireSwapchainImage(Swapchain& swapchain, uint32_t* index);
	XrResult WaitSwapchainImage(Swapchain& swapchain, XrDuration timeout);
	XrResult ReleaseSwapchainImage(Swapchain& swapchain);
	XrResult Compose(const XrFrameEndInfo& endInfo, uint64_t frameId);
	XrResult Capture(uint64_t afterFrameId, protocol::Json& metadata, std::vector<uint8_t>& png, uint32_t timeoutMs);
	uint64_t LastCompletedFrame() const;
	uint64_t LastPresentedFrame() const;
	bool DeviceLost() const;

private:
	Session& session;
	HWND window = nullptr;
	std::string windowClassName;
	Microsoft::WRL::ComPtr<ID3D12Device> device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
	Microsoft::WRL::ComPtr<IDXGISwapChain3> windowSwapchain;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer;
	uint8_t* constantData = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> windowBuffers[2];
	UINT windowBufferCount = 0;
	Microsoft::WRL::ComPtr<ID3D12Resource> output;
	Microsoft::WRL::ComPtr<ID3D12Resource> readback;
	UINT readbackRowPitch = 0;
	UINT readbackHeight = 0;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> windowRtvHeap;
	HANDLE fenceEvent = nullptr;
	UINT rtvStride = 0;
	UINT srvStride = 0;
	D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	uint64_t nextFence = 1;
	uint64_t completedFrame = 0;
	uint64_t presentedFrame = 0;
	bool presentOccluded = false;
	int32_t presentResult = 0;
	uint64_t completedFence = 0;
	bool initialized = false;
	bool deviceLost = false;
	mutable std::mutex mutex;
	std::condition_variable captureCv;

	bool CreateWindowResources();
	bool CreatePipeline();
	bool WaitFence(uint64_t value, uint32_t timeoutMs);
	bool SubmitAndSignal(uint64_t frameId);
	bool EncodePng(std::vector<uint8_t>& png);
	void HideWindow();
};

class ControlServer
{
public:
	explicit ControlServer(Instance& instance);
	~ControlServer();
	bool Start();
	void Stop();
	std::wstring PipePath() const;

private:
	Instance& instance;
	std::wstring pipePath;
	std::atomic<bool> stopping{false};
	HANDLE listenThread = nullptr;
	HANDLE listenPipe = nullptr;
	std::mutex listenerMutex;
	std::mutex workersMutex;
	std::vector<HANDLE> workers;
	void ListenLoop();
	static DWORD WINAPI ListenThunk(void* context);
	static DWORD WINAPI ClientThunk(void* context);
	void ClientLoop(HANDLE pipe, uint64_t connectionId);
};

bool IsFinitePose(const XrPosef& pose);
bool IsAbsolutePath(std::string_view path);
bool IsValidInstance(XrInstance handle);
bool IsValidSession(XrSession handle);
bool IsValidSpace(XrSpace handle);
bool IsValidActionSet(XrActionSet handle);
bool IsValidAction(XrAction handle);
bool IsValidSwapchain(XrSwapchain handle);
XrResult CheckType(const void* structure, XrStructureType expected);
XrResult CheckOutput(void* structure, XrStructureType expected);
XrResult EnumerateArray(uint32_t capacity, uint32_t* count, void* values, size_t elementSize, const void* source, uint32_t sourceCount);

XrResult GuardResult(const std::function<XrResult()>& callback) noexcept;

} // namespace agentxr

struct XrInstance_T
{
	uint64_t magic = agentxr::kInstanceMagic;
	agentxr::Instance* object = nullptr;
	bool alive = true;
};

struct XrSession_T
{
	uint64_t magic = agentxr::kSessionMagic;
	agentxr::Session* object = nullptr;
	bool alive = true;
};

struct XrSpace_T
{
	uint64_t magic = agentxr::kSpaceMagic;
	agentxr::Space* object = nullptr;
	bool alive = true;
};

struct XrActionSet_T
{
	uint64_t magic = agentxr::kActionSetMagic;
	agentxr::ActionSet* object = nullptr;
	bool alive = true;
};

struct XrAction_T
{
	uint64_t magic = agentxr::kActionMagic;
	agentxr::Action* object = nullptr;
	bool alive = true;
};

struct XrSwapchain_T
{
	uint64_t magic = agentxr::kSwapchainMagic;
	agentxr::Swapchain* object = nullptr;
	bool alive = true;
};

#define AGENTXR_API __declspec(dllexport)

extern "C"
{
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t capacity, uint32_t* count, XrApiLayerProperties* properties);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t capacity, uint32_t* count, XrExtensionProperties* properties);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* properties);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType, uint32_t capacity, uint32_t* count, XrEnvironmentBlendMode* modes);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession session);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t capacity, uint32_t* count, XrReferenceSpaceType* spaces);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType, XrExtent2Df* bounds);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId, uint32_t capacity, uint32_t* count, XrViewConfigurationType* types);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type, XrViewConfigurationProperties* properties);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type, uint32_t capacity, uint32_t* count, XrViewConfigurationView* views);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t capacity, uint32_t* count, int64_t* formats);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession session);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrRequestExitSession(XrSession session);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo* waitInfo, XrFrameState* frameState);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo* beginInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* endInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo* locateInfo, XrViewState* viewState, uint32_t capacity, uint32_t* count, XrView* views);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString, XrPath* path);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t capacity, uint32_t* count, char* buffer);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath, XrInteractionProfileState* interactionProfile);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateBoolean* state);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo* enumerateInfo, uint32_t capacity, uint32_t* count, XrPath* sources);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo* getInfo, uint32_t capacity, uint32_t* count, char* buffer);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* actionInfo, const XrHapticBaseHeader* feedback);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* actionInfo);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrGetD3D12GraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* requirements);
AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo, XrNegotiateRuntimeRequest* runtimeRequest);
}
