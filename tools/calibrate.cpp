// Measures the rigid aim and palm offsets of physical Oculus Touch controllers against grip on the ACTIVE OpenXR runtime
// and writes the device calibration profile consumed by the AgentXR runtime.

#define XR_USE_GRAPHICS_API_D3D12 1

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr double kPositionToleranceMeters = 0.001;
constexpr double kOrientationToleranceDegrees = 0.5;
constexpr auto kInactivityTimeout = std::chrono::seconds(120);
constexpr char kTouchProfile[] = "/interaction_profiles/oculus/touch_controller";
constexpr char kHandNames[2][6] = {"left", "right"};
constexpr char kKindNames[2][5] = {"aim", "palm"};
constexpr uint32_t kRequiredFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;

struct Sample
{
	std::array<double, 3> position;
	std::array<double, 4> orientation;
};

struct Calibrator
{
	~Calibrator()
	{
		if (instance != XR_NULL_HANDLE)
		{
			xrDestroyInstance(instance);
		}
	}

	bool Check(XrResult result, const char* what)
	{
		if (XR_SUCCEEDED(result))
		{
			return true;
		}
		char text[XR_MAX_RESULT_STRING_SIZE] = {};
		if (instance != XR_NULL_HANDLE)
		{
			xrResultToString(instance, result, text);
		}
		std::cerr << "calibrate: " << what << " failed: " << text << " (" << result << ")\n";
		return false;
	}

	XrPath Path(const char* path)
	{
		XrPath handle = XR_NULL_PATH;
		return Check(xrStringToPath(instance, path, &handle), path) ? handle : XR_NULL_PATH;
	}

	bool CreateInstance()
	{
		uint32_t count = 0;
		if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "enumerate extensions"))
		{
			return false;
		}
		std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
		if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()), "enumerate extensions"))
		{
			return false;
		}
		const auto has = [&](const char* name)
		{
			return std::any_of(available.begin(), available.end(), [&](const XrExtensionProperties& property) { return std::strcmp(property.extensionName, name) == 0; });
		};
		if (!has(XR_KHR_D3D12_ENABLE_EXTENSION_NAME))
		{
			std::cerr << "calibrate: active runtime lacks " << XR_KHR_D3D12_ENABLE_EXTENSION_NAME << '\n';
			return false;
		}
		std::vector<const char*> extensions = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
		if (has(XR_EXT_PALM_POSE_EXTENSION_NAME))
		{
			extensions.push_back(XR_EXT_PALM_POSE_EXTENSION_NAME);
			havePalmExtension = true;
		}
		XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(createInfo.applicationInfo.applicationName, "AgentXR Calibrate");
		std::strcpy(createInfo.applicationInfo.engineName, "AgentXR");
		createInfo.applicationInfo.applicationVersion = 1;
		createInfo.applicationInfo.engineVersion = 1;
		createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_1;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.enabledExtensionNames = extensions.data();
		XrResult result = xrCreateInstance(&createInfo, &instance);
		if (result == XR_ERROR_API_VERSION_UNSUPPORTED)
		{
			createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
			result = xrCreateInstance(&createInfo, &instance);
		}
		else
		{
			api11 = XR_SUCCEEDED(result);
		}
		if (!Check(result, "xrCreateInstance"))
		{
			instance = XR_NULL_HANDLE;
			return false;
		}
		if (!api11 && !havePalmExtension)
		{
			std::cerr << "calibrate: runtime lacks both OpenXR 1.1 (grip_surface) and XR_EXT_palm_pose; palm pose cannot be measured\n";
			return false;
		}
		XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
		if (!Check(xrGetInstanceProperties(instance, &properties), "xrGetInstanceProperties"))
		{
			return false;
		}
		runtimeName = properties.runtimeName;
		runtimeVersion = std::to_string(XR_VERSION_MAJOR(properties.runtimeVersion)) + "." + std::to_string(XR_VERSION_MINOR(properties.runtimeVersion)) + "." + std::to_string(XR_VERSION_PATCH(properties.runtimeVersion));
		std::string lowered = runtimeName;
		std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (lowered.find("agentxr") != std::string::npos)
		{
			std::cerr << "calibrate: active runtime is " << runtimeName << "; AgentXR cannot calibrate itself. Activate the physical runtime (SteamVR, Meta, Virtual Desktop, ...) and rerun.\n";
			return false;
		}
		XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
		systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (!Check(xrGetSystem(instance, &systemInfo, &systemId), "xrGetSystem (is the headset connected?)"))
		{
			return false;
		}
		XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
		if (!Check(xrGetSystemProperties(instance, systemId, &system), "xrGetSystemProperties"))
		{
			return false;
		}
		systemName = system.systemName;
		std::cout << "runtime: " << runtimeName << ' ' << runtimeVersion << " (OpenXR " << (api11 ? "1.1" : "1.0") << "), system: " << systemName << '\n';
		return true;
	}

	bool CreateSession()
	{
		PFN_xrGetD3D12GraphicsRequirementsKHR getRequirements = nullptr;
		if (!Check(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)), "load xrGetD3D12GraphicsRequirementsKHR"))
		{
			return false;
		}
		XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		if (!Check(getRequirements(instance, systemId, &requirements), "xrGetD3D12GraphicsRequirementsKHR"))
		{
			return false;
		}
		ComPtr<IDXGIFactory4> factory;
		ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(requirements.adapterLuid, IID_PPV_ARGS(&adapter))))
		{
			std::cerr << "calibrate: runtime-required D3D12 adapter not found\n";
			return false;
		}
		D3D12_COMMAND_QUEUE_DESC queueDescription{};
		queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(D3D12CreateDevice(adapter.Get(), requirements.minFeatureLevel, IID_PPV_ARGS(&device))) || FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) || FAILED(list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		{
			std::cerr << "calibrate: D3D12 device setup failed\n";
			return false;
		}
		fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
		binding.device = device.Get();
		binding.queue = queue.Get();
		XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
		createInfo.next = &binding;
		createInfo.systemId = systemId;
		if (!Check(xrCreateSession(instance, &createInfo, &session), "xrCreateSession"))
		{
			return false;
		}
		XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
		if (!Check(xrCreateReferenceSpace(session, &spaceInfo, &localSpace), "xrCreateReferenceSpace"))
		{
			return false;
		}
		return CreateSwapchain();
	}

	bool CreateSwapchain()
	{
		uint32_t viewCount = 0;
		if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr), "enumerate views"))
		{
			return false;
		}
		std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()), "enumerate views") || viewCount != 2)
		{
			std::cerr << "calibrate: expected 2 stereo views\n";
			return false;
		}
		width = static_cast<int32_t>(views[0].recommendedImageRectWidth);
		height = static_cast<int32_t>(views[0].recommendedImageRectHeight);
		uint32_t formatCount = 0;
		if (!Check(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), "enumerate swapchain formats"))
		{
			return false;
		}
		std::vector<int64_t> formats(formatCount);
		if (!Check(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), "enumerate swapchain formats"))
		{
			return false;
		}
		constexpr DXGI_FORMAT preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
		for (const DXGI_FORMAT candidate : preferred)
		{
			if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(candidate)) != formats.end())
			{
				format = candidate;
				break;
			}
		}
		if (format == DXGI_FORMAT_UNKNOWN)
		{
			std::cerr << "calibrate: runtime offers no 8-bit RGBA/BGRA swapchain format\n";
			return false;
		}
		XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		createInfo.format = format;
		createInfo.sampleCount = 1;
		createInfo.width = static_cast<uint32_t>(width);
		createInfo.height = static_cast<uint32_t>(height);
		createInfo.faceCount = 1;
		createInfo.arraySize = 1;
		createInfo.mipCount = 1;
		if (!Check(xrCreateSwapchain(session, &createInfo, &swapchain), "xrCreateSwapchain"))
		{
			return false;
		}
		uint32_t imageCount = 0;
		if (!Check(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr), "enumerate swapchain images"))
		{
			return false;
		}
		images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
		if (!Check(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "enumerate swapchain images"))
		{
			return false;
		}
		D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
		heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDescription.NumDescriptors = imageCount;
		if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&rtvHeap))))
		{
			std::cerr << "calibrate: RTV heap creation failed\n";
			return false;
		}
		rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_RENDER_TARGET_VIEW_DESC viewDescription{};
		viewDescription.Format = format;
		viewDescription.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		for (const XrSwapchainImageD3D12KHR& image : images)
		{
			device->CreateRenderTargetView(image.texture, &viewDescription, handle);
			handle.ptr += rtvStride;
		}
		return true;
	}

	bool CreateActions()
	{
		leftPath = Path("/user/hand/left");
		rightPath = Path("/user/hand/right");
		const XrPath hands[2] = {leftPath, rightPath};
		XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strcpy(setInfo.actionSetName, "calibrate");
		std::strcpy(setInfo.localizedActionSetName, "Calibrate");
		if (!Check(xrCreateActionSet(instance, &setInfo, &actionSet), "xrCreateActionSet"))
		{
			return false;
		}
		const char* actionNames[3] = {"grip", "aim", "palm"};
		std::vector<XrActionSuggestedBinding> bindings;
		for (int kind = 0; kind < 3; ++kind)
		{
			XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
			actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
			actionInfo.countSubactionPaths = 2;
			actionInfo.subactionPaths = hands;
			std::strcpy(actionInfo.actionName, actionNames[kind]);
			std::strcpy(actionInfo.localizedActionName, actionNames[kind]);
			if (!Check(xrCreateAction(actionSet, &actionInfo, &actions[kind]), "xrCreateAction"))
			{
				return false;
			}
			for (int hand = 0; hand < 2; ++hand)
			{
				const std::string root = std::string("/user/hand/") + kHandNames[hand] + "/input/";
				const std::string component = kind == 0 ? "grip/pose" : kind == 1 ? "aim/pose" : api11 ? "grip_surface/pose" : "palm_ext/pose";
				bindings.push_back({actions[kind], Path((root + component).c_str())});
				XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
				spaceInfo.action = actions[kind];
				spaceInfo.subactionPath = hands[hand];
				spaceInfo.poseInActionSpace.orientation.w = 1.0f;
				if (!Check(xrCreateActionSpace(session, &spaceInfo, &spaces[kind][hand]), "xrCreateActionSpace"))
				{
					return false;
				}
			}
		}
		touchProfile = Path(kTouchProfile);
		XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggested.interactionProfile = touchProfile;
		suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
		suggested.suggestedBindings = bindings.data();
		if (!Check(xrSuggestInteractionProfileBindings(instance, &suggested), "suggest touch_controller bindings (palm pose unsupported?)"))
		{
			return false;
		}
		XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attachInfo.countActionSets = 1;
		attachInfo.actionSets = &actionSet;
		return Check(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets");
	}

	// Returns false only on a fatal error; sets controllersReady when both hands report touch_controller.
	bool CheckProfiles(bool& controllersReady)
	{
		controllersReady = true;
		const XrPath hands[2] = {leftPath, rightPath};
		for (int hand = 0; hand < 2; ++hand)
		{
			XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
			if (!Check(xrGetCurrentInteractionProfile(session, hands[hand], &state), "xrGetCurrentInteractionProfile"))
			{
				return false;
			}
			if (state.interactionProfile == XR_NULL_PATH)
			{
				controllersReady = false;
			}
			else if (state.interactionProfile != touchProfile)
			{
				char text[XR_MAX_PATH_LENGTH] = {};
				uint32_t length = 0;
				xrPathToString(instance, state.interactionProfile, sizeof(text), &length, text);
				std::cerr << "calibrate: " << kHandNames[hand] << " hand reports interaction profile " << text << ", expected " << kTouchProfile << ". Calibration requires physical Oculus Touch controllers.\n";
				return false;
			}
		}
		return true;
	}

	bool RenderClear(const XrFrameState& frameState, XrCompositionLayerProjection& layer, std::array<XrCompositionLayerProjectionView, 2>& layerViews, bool& submit)
	{
		submit = false;
		std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
		XrViewState viewState{XR_TYPE_VIEW_STATE};
		XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = frameState.predictedDisplayTime;
		locateInfo.space = localSpace;
		uint32_t viewCount = 0;
		if (!Check(xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views.data()), "xrLocateViews"))
		{
			return false;
		}
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if (!Check(xrAcquireSwapchainImage(swapchain, &acquireInfo, &index), "xrAcquireSwapchainImage"))
		{
			return false;
		}
		XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		waitInfo.timeout = XR_INFINITE_DURATION;
		if (!Check(xrWaitSwapchainImage(swapchain, &waitInfo), "xrWaitSwapchainImage"))
		{
			return false;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(index) * rtvStride;
		constexpr float clearColor[4] = {0.02f, 0.02f, 0.02f, 1.0f};
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
		{
			std::cerr << "calibrate: command list reset failed\n";
			return false;
		}
		list->ClearRenderTargetView(handle, clearColor, 0, nullptr);
		ID3D12CommandList* lists[] = {list.Get()};
		if (FAILED(list->Close()))
		{
			return false;
		}
		queue->ExecuteCommandLists(1, lists);
		++fenceValue;
		if (FAILED(queue->Signal(fence.Get(), fenceValue)) || FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)))
		{
			return false;
		}
		WaitForSingleObject(fenceEvent, INFINITE);
		XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		if (!Check(xrReleaseSwapchainImage(swapchain, &releaseInfo), "xrReleaseSwapchainImage"))
		{
			return false;
		}
		constexpr XrViewStateFlags poseFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		if ((viewState.viewStateFlags & poseFlags) != poseFlags)
		{
			return true;
		}
		for (int eye = 0; eye < 2; ++eye)
		{
			layerViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			layerViews[eye].pose = views[eye].pose;
			layerViews[eye].fov = views[eye].fov;
			layerViews[eye].subImage.swapchain = swapchain;
			layerViews[eye].subImage.imageRect = {{0, 0}, {width, height}};
		}
		layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		layer.space = localSpace;
		layer.viewCount = 2;
		layer.views = layerViews.data();
		submit = true;
		return true;
	}

	// Locates aim and palm in grip space for both hands; returns true and fills result only if all four poses are fully tracked.
	bool Measure(XrTime time, Sample result[2][2])
	{
		for (int hand = 0; hand < 2; ++hand)
		{
			for (int kind = 0; kind < 2; ++kind)
			{
				XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
				if (XR_FAILED(xrLocateSpace(spaces[kind + 1][hand], spaces[0][hand], time, &location)) || (location.locationFlags & kRequiredFlags) != kRequiredFlags)
				{
					return false;
				}
				const XrPosef& pose = location.pose;
				result[hand][kind] = {{pose.position.x, pose.position.y, pose.position.z}, {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
			}
		}
		return true;
	}

	bool Run(int sampleTarget)
	{
		bool running = false;
		bool focused = false;
		bool announcedWait = false;
		Clock::time_point lastProgress = Clock::now();
		Clock::time_point lastNotice = lastProgress;
		std::cout << "waiting for session focus; put on the headset and hold both Touch controllers naturally in view of tracking\n";
		while (collected < sampleTarget)
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			while (true)
			{
				const XrResult polled = xrPollEvent(instance, &event);
				if (polled == XR_EVENT_UNAVAILABLE)
				{
					break;
				}
				if (!Check(polled, "xrPollEvent"))
				{
					return false;
				}
				if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
				{
					std::cerr << "calibrate: instance loss pending\n";
					return false;
				}
				if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
					switch (change.state)
					{
					case XR_SESSION_STATE_READY:
					{
						XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
						beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						if (!Check(xrBeginSession(session, &beginInfo), "xrBeginSession"))
						{
							return false;
						}
						running = true;
						break;
					}
					case XR_SESSION_STATE_STOPPING:
					{
						if (!Check(xrEndSession(session), "xrEndSession"))
						{
							return false;
						}
						running = false;
						focused = false;
						break;
					}
					case XR_SESSION_STATE_EXITING:
					case XR_SESSION_STATE_LOSS_PENDING:
					{
						std::cerr << "calibrate: session ended by runtime before calibration completed\n";
						return false;
					}
					case XR_SESSION_STATE_FOCUSED:
					{
						focused = true;
						lastProgress = Clock::now();
						std::cout << "session focused; collecting " << sampleTarget << " samples, keep both controllers tracked\n";
						break;
					}
					default:
					{
						focused = false;
						break;
					}
					}
				}
				event = {XR_TYPE_EVENT_DATA_BUFFER};
			}
			if (!running)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			XrFrameState frameState{XR_TYPE_FRAME_STATE};
			if (!Check(xrWaitFrame(session, nullptr, &frameState), "xrWaitFrame"))
			{
				return false;
			}
			XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
			if (!Check(xrBeginFrame(session, &beginInfo), "xrBeginFrame"))
			{
				return false;
			}
			XrCompositionLayerProjection layer{};
			std::array<XrCompositionLayerProjectionView, 2> layerViews{};
			bool submit = false;
			if (frameState.shouldRender && !RenderClear(frameState, layer, layerViews, submit))
			{
				return false;
			}
			const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
			XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
			endInfo.displayTime = frameState.predictedDisplayTime;
			endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			endInfo.layerCount = submit ? 1 : 0;
			endInfo.layers = submit ? layers : nullptr;
			if (!Check(xrEndFrame(session, &endInfo), "xrEndFrame"))
			{
				return false;
			}
			if (focused)
			{
				XrActiveActionSet active{actionSet, XR_NULL_PATH};
				XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
				syncInfo.countActiveActionSets = 1;
				syncInfo.activeActionSets = &active;
				if (!Check(xrSyncActions(session, &syncInfo), "xrSyncActions"))
				{
					return false;
				}
				bool controllersReady = false;
				if (!CheckProfiles(controllersReady))
				{
					return false;
				}
				Sample sample[2][2];
				if (controllersReady && Measure(frameState.predictedDisplayTime, sample))
				{
					for (int hand = 0; hand < 2; ++hand)
					{
						for (int kind = 0; kind < 2; ++kind)
						{
							samples[hand][kind].push_back(sample[hand][kind]);
						}
					}
					++collected;
					lastProgress = Clock::now();
					announcedWait = false;
					if (collected % 50 == 0 || collected == sampleTarget)
					{
						std::cout << "samples collected: " << collected << '/' << sampleTarget << '\n';
					}
					continue;
				}
				if (!announcedWait || Clock::now() - lastNotice > std::chrono::seconds(5))
				{
					std::cout << "waiting for both controllers to report fully tracked grip/aim/palm poses\n";
					announcedWait = true;
					lastNotice = Clock::now();
				}
			}
			if (Clock::now() - lastProgress > kInactivityTimeout)
			{
				std::cerr << "calibrate: no usable samples for " << std::chrono::duration_cast<std::chrono::seconds>(kInactivityTimeout).count() << " s (not focused, or controllers not tracked)\n";
				return false;
			}
		}
		return true;
	}

	static double Dot(const std::array<double, 4>& a, const std::array<double, 4>& b)
	{
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	}

	// Averages samples and verifies rigidity; returns false when any sample deviates beyond tolerance.
	bool Average(int hand, int kind, Sample& mean)
	{
		const std::vector<Sample>& set = samples[hand][kind];
		std::array<double, 3> position{};
		std::array<double, 4> orientation{};
		for (const Sample& sample : set)
		{
			const double sign = Dot(sample.orientation, set.front().orientation) < 0.0 ? -1.0 : 1.0;
			for (int i = 0; i < 3; ++i)
			{
				position[i] += sample.position[i];
			}
			for (int i = 0; i < 4; ++i)
			{
				orientation[i] += sign * sample.orientation[i];
			}
		}
		const double count = static_cast<double>(set.size());
		const double norm = std::sqrt(Dot(orientation, orientation));
		for (int i = 0; i < 3; ++i)
		{
			position[i] /= count;
		}
		for (int i = 0; i < 4; ++i)
		{
			orientation[i] /= norm;
		}
		double maxPosition = 0.0;
		double maxOrientation = 0.0;
		for (const Sample& sample : set)
		{
			const double dx = sample.position[0] - position[0];
			const double dy = sample.position[1] - position[1];
			const double dz = sample.position[2] - position[2];
			maxPosition = std::max(maxPosition, std::sqrt(dx * dx + dy * dy + dz * dz));
			const double dot = std::min(1.0, std::abs(Dot(sample.orientation, orientation)));
			maxOrientation = std::max(maxOrientation, 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846);
		}
		std::cout << kHandNames[hand] << ' ' << kKindNames[kind] << "FromGrip: max position deviation " << maxPosition * 1000.0 << " mm, max orientation deviation " << maxOrientation << " deg\n";
		mean = {position, orientation};
		if (maxPosition > kPositionToleranceMeters || maxOrientation > kOrientationToleranceDegrees)
		{
			std::cerr << "calibrate: " << kHandNames[hand] << ' ' << kKindNames[kind] << " offset is not rigid (tolerance " << kPositionToleranceMeters * 1000.0 << " mm, " << kOrientationToleranceDegrees << " deg); keep controllers tracked and rerun\n";
			return false;
		}
		return true;
	}

	bool Write(const std::filesystem::path& out)
	{
		Json document;
		document["interactionProfile"] = kTouchProfile;
		const std::time_t now = std::time(nullptr);
		std::tm utc{};
		gmtime_s(&utc, &now);
		char timestamp[32] = {};
		std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
		document["source"] = {{"runtimeName", runtimeName}, {"runtimeVersion", runtimeVersion}, {"systemName", systemName}, {"sampleCount", collected}, {"measuredAtUtc", timestamp}};
		bool rigid = true;
		for (int hand = 0; hand < 2; ++hand)
		{
			for (int kind = 0; kind < 2; ++kind)
			{
				Sample mean;
				rigid = Average(hand, kind, mean) && rigid;
				document[kHandNames[hand]][std::string(kKindNames[kind]) + "FromGrip"] = {{"position", mean.position}, {"orientation", mean.orientation}};
			}
		}
		if (!rigid)
		{
			return false;
		}
		std::error_code error;
		if (out.has_parent_path())
		{
			std::filesystem::create_directories(out.parent_path(), error);
		}
		std::ofstream file(out, std::ios::binary | std::ios::trunc);
		file << document.dump(2) << '\n';
		file.close();
		if (!file)
		{
			std::cerr << "calibrate: failed to write " << out << '\n';
			return false;
		}
		std::cout << "wrote " << std::filesystem::absolute(out) << '\n';
		return true;
	}

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction actions[3] = {};
	XrSpace spaces[3][2] = {};
	XrPath leftPath = XR_NULL_PATH;
	XrPath rightPath = XR_NULL_PATH;
	XrPath touchProfile = XR_NULL_PATH;
	bool api11 = false;
	bool havePalmExtension = false;
	std::string runtimeName;
	std::string runtimeVersion;
	std::string systemName;
	std::vector<XrSwapchainImageD3D12KHR> images;
	std::vector<Sample> samples[2][2];
	int collected = 0;
	int32_t width = 0;
	int32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	ComPtr<ID3D12DescriptorHeap> rtvHeap;
	UINT rtvStride = 0;
	UINT64 fenceValue = 0;
	HANDLE fenceEvent = nullptr;
};

void PrintUsage()
{
	std::cerr << "usage: agent-xr-calibrate --out <profile.json> [--samples N]\n"
			  << "  Measures aim/palm offsets of physical Oculus Touch controllers on the active OpenXR runtime.\n"
			  << "  Run once with the physical headset on the physical runtime; refuses to run on AgentXR.\n";
}
}

int main(int argc, char** argv)
{
	std::filesystem::path out;
	int sampleTarget = 300;
	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if (argument == "--out" && i + 1 < argc)
		{
			out = argv[++i];
		}
		else if (argument == "--samples" && i + 1 < argc)
		{
			sampleTarget = std::atoi(argv[++i]);
		}
		else
		{
			PrintUsage();
			return 2;
		}
	}
	if (out.empty() || sampleTarget < 2)
	{
		PrintUsage();
		return 2;
	}
	Calibrator calibrator;
	if (!calibrator.CreateInstance() || !calibrator.CreateSession() || !calibrator.CreateActions() || !calibrator.Run(sampleTarget) || !calibrator.Write(out))
	{
		return 1;
	}
	return 0;
}
