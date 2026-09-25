#define XR_USE_GRAPHICS_API_D3D12 1
#define XR_NO_PROTOTYPES 1

#include "protocol.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <dbghelp.h>
#include <wincodec.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <future>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

namespace
{
using Json = agentxr::protocol::Json;
using Microsoft::WRL::ComPtr;
LONG WINAPI SmokeUnhandledExceptionFilter(EXCEPTION_POINTERS* exception)
{
	const DWORD code = exception != nullptr && exception->ExceptionRecord != nullptr ? exception->ExceptionRecord->ExceptionCode : 0;
	const void* address = exception != nullptr && exception->ExceptionRecord != nullptr ? exception->ExceptionRecord->ExceptionAddress : nullptr;
	const DWORD64 addressValue = reinterpret_cast<DWORD64>(address);
	std::cerr << "smoke: unhandled exception code=0x" << std::hex << code << " address=0x" << addressValue << std::dec;
	if (address != nullptr)
	{
		HANDLE process = GetCurrentProcess();
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
		if (SymInitialize(process, nullptr, TRUE))
		{
			IMAGEHLP_MODULE64 module{sizeof(IMAGEHLP_MODULE64)};
			if (SymGetModuleInfo64(process, addressValue, &module))
			{
				std::cerr << " module=" << module.ModuleName;
			}
			alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolStorage{};
			auto* symbol = reinterpret_cast<PSYMBOL_INFO>(symbolStorage.data());
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = MAX_SYM_NAME;
			DWORD64 displacement = 0;
			if (SymFromAddr(process, addressValue, &displacement, symbol))
			{
				std::cerr << " symbol=" << symbol->Name;
			}
			IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
			DWORD lineDisplacement = 0;
			if (SymGetLineFromAddr64(process, addressValue, &lineDisplacement, &line))
			{
				std::cerr << " location=" << line.FileName << ':' << line.LineNumber;
			}
			SymCleanup(process);
		}
	}
	std::cerr << '\n';
	return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void SmokeTerminateHandler()
{
	std::cerr << "smoke: std::terminate invoked\n";
	std::abort();
}

struct RuntimeEndpoint
{
	std::wstring pipe;
	uint32_t processId = 0;
	std::string instanceId;
	uint64_t sessionGeneration = 0;
	Json handshake;
};

bool Check(XrResult result, std::string_view operation)
{
	if (result == XR_SUCCESS)
	{
		return true;
	}
	std::cerr << operation << " failed: " << static_cast<int>(result) << '\n';
	return false;
}

bool Expect(XrResult result, XrResult expected, std::string_view operation)
{
	if (result == expected)
	{
		return true;
	}
	std::cerr << operation << " returned " << static_cast<int>(result) << ", expected " << static_cast<int>(expected) << '\n';
	return false;
}

uint32_t CountAgentXRSessionWindows()
{
	struct Query
	{
		DWORD processId = 0;
		std::wstring titlePrefix;
		uint32_t count = 0;
	};

	const DWORD processId = GetCurrentProcessId();
	Query query{processId, L"AgentXR PID " + std::to_wstring(processId) + L" Session", 0};
	EnumWindows(
		[](HWND window, LPARAM parameter) -> BOOL
		{
			auto& query = *reinterpret_cast<Query*>(parameter);
			DWORD windowProcessId = 0;
			if (GetWindowThreadProcessId(window, &windowProcessId) == 0 || windowProcessId != query.processId)
			{
				return TRUE;
			}

			std::array<wchar_t, 256> title{};
			const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
			if (length > 0 && std::wstring_view(title.data(), static_cast<size_t>(length)).starts_with(query.titlePrefix))
			{
				++query.count;
			}
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&query));
	return query.count;
}

bool ReadJsonFile(const std::filesystem::path& path, Json& value)
{
	std::ifstream stream(path);
	if (!stream)
	{
		return false;
	}
	try
	{
		stream >> value;
		return value.is_object();
	}
	catch (...)
	{
		return false;
	}
}

bool Handshake(const std::wstring& pipeName, RuntimeEndpoint& endpoint, DWORD* createFileError = nullptr)
{
	HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (pipe == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		if (createFileError != nullptr)
		{
			*createFileError = error;
		}
		return false;
	}
	const Json request = {{"op", "handshake"}};
	agentxr::protocol::PipeFrame response;
	const bool ok = agentxr::protocol::WriteFrame(pipe, request, {}, 1000) && agentxr::protocol::ReadFrame(pipe, response, 1000) && response.message.value("ok", false);
	if (ok)
	{
		endpoint.pipe = pipeName;
		endpoint.processId = response.message.value("processId", 0u);
		endpoint.instanceId = response.message.value("instanceId", std::string{});
		endpoint.sessionGeneration = response.message.value("sessionGeneration", 0ull);
		endpoint.handshake = response.message;
	}
	CloseHandle(pipe);
	return ok;
}

bool FindEndpoint(RuntimeEndpoint& endpoint)
{
	WIN32_FIND_DATAW data{};
	HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\AgentXR.*", &data);
	if (find == INVALID_HANDLE_VALUE)
	{
		const DWORD error = GetLastError();
		std::cerr << "FindEndpoint enumeration failed (GetLastError=" << error << ")\n";
		return false;
	}
	bool found = false;
	DWORD lastHandshakeError = ERROR_SUCCESS;
	do
	{
		const std::wstring name = std::wstring(L"\\\\.\\pipe\\") + data.cFileName;
		RuntimeEndpoint candidate;
		if (Handshake(name, candidate, &lastHandshakeError) && candidate.processId == GetCurrentProcessId() && candidate.sessionGeneration != 0)
		{
			endpoint = std::move(candidate);
			found = true;
			break;
		}
	}
	while (FindNextFileW(find, &data));
	const DWORD enumerationError = GetLastError();
	FindClose(find);
	if (!found)
	{
		std::cerr << "FindEndpoint found no matching endpoint (FindNextFile GetLastError=" << enumerationError << ", handshake CreateFile GetLastError=" << lastHandshakeError << ")\n";
	}
	return found;
}

class Control
{
public:
	~Control()
	{
		Disconnect();
	}

	void Disconnect()
	{
		if (pipe != INVALID_HANDLE_VALUE)
		{
			CloseHandle(pipe);
			pipe = INVALID_HANDLE_VALUE;
		}
	}

	bool Connect(const RuntimeEndpoint& endpoint)
	{
		Disconnect();
		pipe = CreateFileW(endpoint.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		processId = endpoint.processId;
		instanceId = endpoint.instanceId;
		generation = endpoint.sessionGeneration;
		return pipe != INVALID_HANDLE_VALUE;
	}

	bool Request(const Json& request, agentxr::protocol::PipeFrame& response)
	{
		return pipe != INVALID_HANDLE_VALUE && agentxr::protocol::WriteFrame(pipe, request, {}, 2000) && agentxr::protocol::ReadFrame(pipe, response, 2500);
	}

	Json Identity() const
	{
		return {{"processId", processId}, {"instanceId", instanceId}, {"sessionGeneration", generation}};
	}

	bool Snapshot(Json& snapshot)
	{
		Json request = Identity();
		request["op"] = "snapshot";
		agentxr::protocol::PipeFrame response;
		if (!Request(request, response) || !response.message.value("ok", false) || !response.message.contains("result"))
		{
			return false;
		}
		snapshot = response.message.at("result");
		return true;
	}

	bool StaleSnapshot()
	{
		Json request = Identity();
		request["op"] = "snapshot";
		agentxr::protocol::PipeFrame response;
		return Request(request, response) && !response.message.value("ok", false) && response.message.value("error", Json{}).value("code", std::string{}) == "stale_session";
	}

	bool Report(uint64_t timelineId, Json& report)
	{
		Json request = Identity();
		request["op"] = "get_report";
		request["timelineId"] = timelineId;
		request["cursor"] = 0;
		request["limit"] = 1000;
		agentxr::protocol::PipeFrame response;
		if (!Request(request, response) || !response.message.value("ok", false) || !response.message.contains("result"))
		{
			return false;
		}
		report = response.message.at("result");
		return report.value("status", std::string{}) != "report_expired";
	}

	bool CancelTimeline(uint64_t timelineId)
	{
		Json request = Identity();
		request["op"] = "cancel_timeline";
		request["timelineId"] = timelineId;
		agentxr::protocol::PipeFrame response;
		if (!Request(request, response) || !response.message.value("ok", false) || !response.message.contains("result") || !response.message.at("result").is_object())
		{
			std::cerr << "cancel_timeline: request failed for timeline " << timelineId << '\n';
			return false;
		}
		const Json result = response.message.at("result");
		const uint64_t resultId = result.value("timelineId", 0ull);
		const std::string status = result.value("status", std::string{});
		if (resultId != timelineId || status != "canceled")
		{
			std::cerr << "cancel_timeline: expected id=" << timelineId << " status=canceled, got id=" << resultId << " status=" << status << '\n';
			return false;
		}
		return true;
	}

	uint32_t processId = 0;
	std::string instanceId;
	uint64_t generation = 0;

private:
	HANDLE pipe = INVALID_HANDLE_VALUE;
};

struct Graphics
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	uint64_t fenceValue = 1;
	ComPtr<ID3D12DescriptorHeap> rtvHeap;
	UINT rtvStride = 0;

	~Graphics()
	{
		if (fenceEvent != nullptr)
		{
			CloseHandle(fenceEvent);
		}
	}

	bool Initialize()
	{
		ComPtr<IDXGIFactory6> factory;
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
		{
			return false;
		}
		for (UINT index = 0; factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&selectedAdapter)) != DXGI_ERROR_NOT_FOUND; ++index)
		{
			DXGI_ADAPTER_DESC1 description{};
			if (FAILED(selectedAdapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
			{
				selectedAdapter.Reset();
				continue;
			}
			if (SUCCEEDED(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
			{
				break;
			}
			selectedAdapter.Reset();
		}
		if (device == nullptr)
		{
			return false;
		}
		D3D12_COMMAND_QUEUE_DESC queueDescription{};
		queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) || FAILED(list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		{
			return false;
		}
		fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (fenceEvent == nullptr)
		{
			return false;
		}
		D3D12_DESCRIPTOR_HEAP_DESC heap{};
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap.NumDescriptors = 8;
		if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap))))
		{
			return false;
		}
		rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		return true;
	}

	bool Submit()
	{
		if (FAILED(list->Close()))
		{
			return false;
		}
		ID3D12CommandList* lists[] = {list.Get()};
		queue->ExecuteCommandLists(1, lists);
		const uint64_t value = fenceValue++;
		if (FAILED(queue->Signal(fence.Get(), value)) || FAILED(fence->SetEventOnCompletion(value, fenceEvent)) || WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0)
		{
			return false;
		}
		return true;
	}

	DXGI_ADAPTER_DESC1 AdapterDescription() const
	{
		DXGI_ADAPTER_DESC1 description{};
		if (selectedAdapter != nullptr)
		{
			selectedAdapter->GetDesc1(&description);
		}
		return description;
	}

private:
	ComPtr<IDXGIAdapter1> selectedAdapter;
};

class OpenXR
{
public:
	~OpenXR()
	{
		DestroySessionOnly();
		if (instance != XR_NULL_HANDLE && xrDestroyInstance != nullptr)
		{
			xrDestroyInstance(instance);
		}
	}

	bool Load(const std::wstring& manifest)
	{
		if (!SetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest.c_str()))
		{
			return false;
		}
		getInstanceProcAddr = &::xrGetInstanceProcAddr;
		if (getInstanceProcAddr == nullptr)
		{
			return false;
		}
		if (!Check(getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties)), "get extension enumeration") || xrEnumerateInstanceExtensionProperties == nullptr)
		{
			return false;
		}
		if (!Check(getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance)), "get xrCreateInstance") || xrCreateInstance == nullptr)
		{
			return false;
		}
		return CreateInstance() && LoadFunctions();
	}

	bool CreateInstance()
	{
		uint32_t count = 0;
		if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "enumerate extensions count"))
		{
			return false;
		}
		std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
		if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()), "enumerate extensions"))
		{
			return false;
		}
		bool haveD3D12 = false;
		bool haveLocalFloor = false;
		for (const XrExtensionProperties& property : available)
		{
			haveD3D12 = haveD3D12 || std::strcmp(property.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
			haveLocalFloor = haveLocalFloor || std::strcmp(property.extensionName, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME) == 0;
		}
		if (!haveD3D12 || !haveLocalFloor)
		{
			std::cerr << "required extension missing\n";
			return false;
		}
		const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME};
		XrApplicationInfo application{};
		std::strcpy(application.applicationName, "AgentXR Smoke");
		std::strcpy(application.engineName, "AgentXR");
		application.applicationVersion = 1;
		application.engineVersion = 1;
		XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
		createInfo.applicationInfo = application;
		createInfo.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
		createInfo.enabledExtensionNames = extensions;
		createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
		XrResult result = xrCreateInstance(&createInfo, &instance);
		if (result != XR_ERROR_API_VERSION_UNSUPPORTED)
		{
			if (result == XR_SUCCESS)
			{
				xrDestroyInstance(instance);
				instance = XR_NULL_HANDLE;
			}
			return Expect(result, XR_ERROR_API_VERSION_UNSUPPORTED, "API 1.1 fallback probe");
		}
		apiFallbackObserved = true;
		createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
		return Check(xrCreateInstance(&createInfo, &instance), "create API 1.0 instance");
	}

	bool LoadFunctions()
	{
#define LOAD(name) \
		if (!Check(getInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name)), #name) || name == nullptr) \
		{ \
			return false; \
		}
		LOAD(xrDestroyInstance);
		LOAD(xrGetInstanceProperties);
		LOAD(xrPollEvent);
		LOAD(xrGetSystem);
		LOAD(xrGetSystemProperties);
		LOAD(xrEnumerateEnvironmentBlendModes);
		LOAD(xrEnumerateViewConfigurations);
		LOAD(xrGetViewConfigurationProperties);
		LOAD(xrEnumerateViewConfigurationViews);
		LOAD(xrCreateSession);
		LOAD(xrDestroySession);
		LOAD(xrBeginSession);
		LOAD(xrEndSession);
		LOAD(xrRequestExitSession);
		LOAD(xrWaitFrame);
		LOAD(xrBeginFrame);
		LOAD(xrEndFrame);
		LOAD(xrEnumerateReferenceSpaces);
		LOAD(xrCreateReferenceSpace);
		LOAD(xrCreateActionSpace);
		LOAD(xrDestroySpace);
		LOAD(xrLocateSpace);
		LOAD(xrLocateViews);
		LOAD(xrStringToPath);
		LOAD(xrPathToString);
		LOAD(xrCreateActionSet);
		LOAD(xrDestroyActionSet);
		LOAD(xrCreateAction);
		LOAD(xrDestroyAction);
		LOAD(xrSuggestInteractionProfileBindings);
		LOAD(xrAttachSessionActionSets);
		LOAD(xrSyncActions);
		LOAD(xrGetActionStateBoolean);
		LOAD(xrGetActionStateFloat);
		LOAD(xrGetActionStatePose);
		LOAD(xrEnumerateSwapchainFormats);
		LOAD(xrCreateSwapchain);
		LOAD(xrDestroySwapchain);
		LOAD(xrEnumerateSwapchainImages);
		LOAD(xrAcquireSwapchainImage);
		LOAD(xrWaitSwapchainImage);
		LOAD(xrReleaseSwapchainImage);
		LOAD(xrGetD3D12GraphicsRequirementsKHR);
#undef LOAD
		return true;
	}

	bool InitializeGraphics(bool createDefaultResources = true)
	{
		if (!apiFallbackObserved)
		{
			std::cerr << "initialize graphics: API 1.1 fallback not observed\n";
			return false;
		}
		if (!graphics.Initialize())
		{
			std::cerr << "initialize graphics: D3D12 initialization failed\n";
			return false;
		}
		XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
		if (!Check(xrGetInstanceProperties(instance, &properties), "instance properties"))
		{
			return false;
		}
		if (std::string_view(properties.runtimeName).find("AgentXR") != 0)
		{
			std::cerr << "initialize graphics: unexpected runtime " << properties.runtimeName << '\n';
			return false;
		}
		XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
		systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (!Check(xrGetSystem(instance, &systemInfo, &systemId), "get system"))
		{
			return false;
		}
		XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
		if (!Check(xrGetSystemProperties(instance, systemId, &systemProperties), "system properties"))
		{
			return false;
		}
		if (systemProperties.graphicsProperties.maxSwapchainImageWidth < 1024 || systemProperties.graphicsProperties.maxSwapchainImageHeight < 1024)
		{
			std::cerr << "initialize graphics: swapchain limit too small (" << systemProperties.graphicsProperties.maxSwapchainImageWidth << "x" << systemProperties.graphicsProperties.maxSwapchainImageHeight << ")\n";
			return false;
		}
		uint32_t viewConfigurationCount = 0;
		if (!Check(xrEnumerateViewConfigurations(instance, systemId, 0, &viewConfigurationCount, nullptr), "view configuration count"))
		{
			return false;
		}
		std::vector<XrViewConfigurationType> viewConfigurations(viewConfigurationCount);
		if (!Check(xrEnumerateViewConfigurations(instance, systemId, viewConfigurationCount, &viewConfigurationCount, viewConfigurations.data()), "view configurations"))
		{
			return false;
		}
		if (std::find(viewConfigurations.begin(), viewConfigurations.end(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == viewConfigurations.end())
		{
			std::cerr << "initialize graphics: PRIMARY_STEREO view configuration unavailable\n";
			return false;
		}
		XrViewConfigurationProperties viewProperties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
		if (!Check(xrGetViewConfigurationProperties(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &viewProperties), "view configuration properties"))
		{
			return false;
		}
		uint32_t viewCount = 0;
		if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr), "view count"))
		{
			return false;
		}
		std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		if (!Check(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()), "views"))
		{
			return false;
		}
		if (viewCount != 2)
		{
			std::cerr << "initialize graphics: expected 2 views, got " << viewCount << '\n';
			return false;
		}
		if (views[0].recommendedImageRectWidth != 1024 || views[0].recommendedImageRectHeight != 1024)
		{
			std::cerr << "initialize graphics: expected 1024x1024 views, got " << views[0].recommendedImageRectWidth << "x" << views[0].recommendedImageRectHeight << '\n';
			return false;
		}
		uint32_t blendCount = 0;
		if (!Check(xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendCount, nullptr), "blend mode count"))
		{
			return false;
		}
		std::vector<XrEnvironmentBlendMode> blendModes(blendCount);
		if (!Check(xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendCount, &blendCount, blendModes.data()), "blend modes"))
		{
			return false;
		}
		if (std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == blendModes.end())
		{
			std::cerr << "initialize graphics: opaque blend mode unavailable\n";
			return false;
		}
		XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
		if (!Check(xrGetD3D12GraphicsRequirementsKHR(instance, systemId, &requirements), "graphics requirements"))
		{
			return false;
		}
		if (requirements.minFeatureLevel > D3D_FEATURE_LEVEL_11_0)
		{
			std::cerr << "initialize graphics: unsupported minimum feature level " << static_cast<unsigned>(requirements.minFeatureLevel) << '\n';
			return false;
		}
		const LUID suppliedLuid = graphics.device->GetAdapterLuid();
		if (requirements.adapterLuid.LowPart != suppliedLuid.LowPart || requirements.adapterLuid.HighPart != suppliedLuid.HighPart)
		{
			std::cerr << "initialize graphics: adapter LUID mismatch (runtime " << requirements.adapterLuid.HighPart << ':' << requirements.adapterLuid.LowPart << ", client " << suppliedLuid.HighPart << ':' << suppliedLuid.LowPart << ")\n";
			return false;
		}
		graphicsRequirementsQueried = true;
		if (!CreateSessionOnly(createDefaultResources))
		{
			std::cerr << "initialize graphics: session creation failed\n";
			return false;
		}
		return true;
	}

	bool Begin()
	{
		bool began = false;
		bool focused = false;
		for (int attempt = 0; attempt < 100; ++attempt)
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			XrResult pollResult = XR_EVENT_UNAVAILABLE;
			while ((pollResult = xrPollEvent(instance, &event)) == XR_SUCCESS)
			{
				if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
					if (changed->state == XR_SESSION_STATE_READY && !began)
					{
						XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
						beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						if (!Check(xrBeginSession(session, &beginInfo), "begin session"))
						{
							return false;
						}
						began = true;
					}
					focused = focused || changed->state == XR_SESSION_STATE_FOCUSED;
				}
				event = {XR_TYPE_EVENT_DATA_BUFFER};
			}
			if (pollResult != XR_EVENT_UNAVAILABLE)
			{
				std::cerr << "begin: event polling failed: " << static_cast<int>(pollResult) << '\n';
				return false;
			}
			if (began && focused)
			{
				running = true;
				return true;
			}
			Sleep(1);
		}
		std::cerr << "begin: timed out waiting for focused session\n";
		return false;
	}

	bool End()
	{
		if (session == XR_NULL_HANDLE || !running)
		{
			return true;
		}
		if (!Check(xrRequestExitSession(session), "request exit session"))
		{
			return false;
		}
		bool stopping = false;
		for (int attempt = 0; attempt < 100 && !stopping; ++attempt)
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			XrResult pollResult = XR_EVENT_UNAVAILABLE;
			while ((pollResult = xrPollEvent(instance, &event)) == XR_SUCCESS)
			{
				if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
					stopping = stopping || changed->state == XR_SESSION_STATE_STOPPING;
				}
				event = {XR_TYPE_EVENT_DATA_BUFFER};
			}
			if (pollResult != XR_EVENT_UNAVAILABLE)
			{
				return false;
			}
			if (!stopping)
			{
				Sleep(1);
			}
		}
		if (!stopping || !Check(xrEndSession(session), "end session"))
		{
			return false;
		}
		running = false;
		return true;
	}

	bool Frame(bool render, uint32_t& frameCount, XrViewState* stateOut = nullptr, XrTime* displayTimeOut = nullptr, float alpha = 1.0f)
	{
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState{XR_TYPE_FRAME_STATE};
		const XrResult waitResult = xrWaitFrame(session, &waitInfo, &frameState);
		if (!Check(waitResult, "wait frame"))
		{
			return false;
		}
		if (frameState.predictedDisplayTime <= lastDisplayTime)
		{
			std::cerr << "frame: predicted display time did not increase\n";
			return false;
		}
		if (frameState.predictedDisplayPeriod <= 0)
		{
			std::cerr << "frame: predicted display period is invalid\n";
			return false;
		}
		lastDisplayTime = frameState.predictedDisplayTime;
		if (displayTimeOut != nullptr)
		{
			*displayTimeOut = frameState.predictedDisplayTime;
		}
		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		if (!Check(xrBeginFrame(session, &beginInfo), "begin frame"))
		{
			return false;
		}
		XrViewState viewState{XR_TYPE_VIEW_STATE};
		std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
		uint32_t viewCount = 0;
		XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = frameState.predictedDisplayTime;
		locateInfo.space = localSpace;
		if (!Check(xrLocateViews(session, &locateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data()), "locate views"))
		{
			return false;
		}
		if (viewCount != 2)
		{
			std::cerr << "frame: expected 2 located views, got " << viewCount << '\n';
			return false;
		}
		if (stateOut != nullptr)
		{
			*stateOut = viewState;
		}
		XrCompositionLayerProjectionView projectionViews[2]{};
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
		const bool submitLayer = render && frameState.shouldRender;
		if (submitLayer)
		{
			uint32_t imageIndex = 0;
			XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (!Check(xrAcquireSwapchainImage(swapchain, &acquire, &imageIndex), "acquire image"))
			{
				return false;
			}
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 2000000000LL;
			if (!Check(xrWaitSwapchainImage(swapchain, &wait), "wait image"))
			{
				return false;
			}
			const HRESULT allocatorReset = graphics.allocator->Reset();
			if (FAILED(allocatorReset))
			{
				std::cerr << "frame: command allocator reset failed\n";
				return false;
			}
			const HRESULT listReset = graphics.list->Reset(graphics.allocator.Get(), nullptr);
			if (FAILED(listReset))
			{
				std::cerr << "frame: command list reset failed\n";
				return false;
			}
			const D3D12_CPU_DESCRIPTOR_HANDLE start = graphics.rtvHeap->GetCPUDescriptorHandleForHeapStart();
			for (uint32_t eye = 0; eye < 2; ++eye)
			{
				XrSwapchainImageD3D12KHR& image = images[imageIndex];
				D3D12_RENDER_TARGET_VIEW_DESC view{};
				view.Format = selectedFormat;
				view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
				view.Texture2DArray.MipSlice = 0;
				view.Texture2DArray.ArraySize = 1;
				view.Texture2DArray.FirstArraySlice = eye;
				D3D12_CPU_DESCRIPTOR_HANDLE target = start;
				target.ptr += static_cast<SIZE_T>(eye) * graphics.rtvStride;
				graphics.device->CreateRenderTargetView(image.texture, &view, target);
				const float color[4] = {eye == 0 ? 1.0f : 0.0f, 0.0f, eye == 0 ? 0.0f : 1.0f, alpha};
				graphics.list->ClearRenderTargetView(target, color, 0, nullptr);
			}
			if (!graphics.Submit())
			{
				std::cerr << "frame: GPU submission failed\n";
				return false;
			}
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			if (!Check(xrReleaseSwapchainImage(swapchain, &release), "release image"))
			{
				return false;
			}
			for (uint32_t eye = 0; eye < 2; ++eye)
			{
				projectionViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				projectionViews[eye].pose = views[eye].pose;
				projectionViews[eye].fov = views[eye].fov;
				projectionViews[eye].subImage = {swapchain, {{0, 0}, {1024, 1024}}, eye};
			}
			projection.space = localSpace;
			projection.viewCount = 2;
			projection.views = projectionViews;
		}
		XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = submitLayer ? 1 : 0;
		endInfo.layers = submitLayer ? layers : nullptr;
		const XrResult result = xrEndFrame(session, &endInfo);
		if (result != XR_SUCCESS && result != XR_FRAME_DISCARDED)
		{
			return Check(result, "end frame");
		}
		++frameCount;
		return true;
	}

	bool RequestActiveSessionExit()
	{
		using RequestExitFunction = XrResult (XRAPI_CALL*)();
		const HMODULE runtimeModule = GetModuleHandleW(L"AgentXRRuntime.dll");
		if (runtimeModule == nullptr)
		{
			std::cerr << "request active session exit: runtime module unavailable\n";
			return false;
		}
		const auto requestExit = reinterpret_cast<RequestExitFunction>(GetProcAddress(runtimeModule, "agentxrRequestExitActiveSession"));
		if (requestExit == nullptr)
		{
			std::cerr << "request active session exit: export unavailable\n";
			return false;
		}
		return Check(requestExit(), "request active session exit");
	}

	bool HoldSwapchainImage()
	{
		if (swapchain == XR_NULL_HANDLE || heldSwapchainImage)
		{
			return false;
		}
		uint32_t imageIndex = 0;
		XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if (!Check(xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex), "hold swapchain image acquire"))
		{
			return false;
		}
		XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		waitInfo.timeout = 2000000000LL;
		if (!Check(xrWaitSwapchainImage(swapchain, &waitInfo), "hold swapchain image wait"))
		{
			return false;
		}
		heldSwapchainImage = true;
		return true;
	}

	bool DestroyHeldSwapchain()
	{
		if (swapchain == XR_NULL_HANDLE || !heldSwapchainImage)
		{
			return false;
		}
		if (!Check(xrDestroySwapchain(swapchain), "destroy held swapchain"))
		{
			return false;
		}
		swapchain = XR_NULL_HANDLE;
		images.clear();
		heldSwapchainImage = false;
		return true;
	}



	bool StaleProjectionFrames(uint32_t& frameCount)
	{
		if (swapchain == XR_NULL_HANDLE || localSpace == XR_NULL_HANDLE || images.empty())
		{
			return false;
		}

		XrCompositionLayerProjectionView projectionViews[2]{};
		for (uint32_t eye = 0; eye < 2; ++eye)
		{
			projectionViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			projectionViews[eye].pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			projectionViews[eye].fov = {-0.785398163f, 0.785398163f, 0.785398163f, -0.785398163f};
			projectionViews[eye].subImage = {swapchain, {{0, 0}, {1024, 1024}}, eye};
		}
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		projection.space = localSpace;
		projection.viewCount = 2;
		projection.views = projectionViews;
		const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
		const ULONGLONG start = GetTickCount64();
		bool submitted = false;
		do
		{
			XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
			XrFrameState frameState{XR_TYPE_FRAME_STATE};
			const XrResult waitResult = xrWaitFrame(session, &waitInfo, &frameState);
			if (!Check(waitResult, "stale layer wait frame"))
			{
				return false;
			}
			if (frameState.predictedDisplayTime <= lastDisplayTime || frameState.predictedDisplayPeriod <= 0)
			{
				std::cerr << "stale layer frame timing is invalid\n";
				return false;
			}
			lastDisplayTime = frameState.predictedDisplayTime;
			XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
			if (!Check(xrBeginFrame(session, &beginInfo), "stale layer begin frame"))
			{
				return false;
			}
			XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
			endInfo.displayTime = frameState.predictedDisplayTime;
			endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			endInfo.layerCount = 1;
			endInfo.layers = layers;
			const XrResult endResult = xrEndFrame(session, &endInfo);
			if (endResult != XR_SUCCESS && endResult != XR_FRAME_DISCARDED)
			{
				return Check(endResult, "stale layer end frame");
			}
			submitted = true;
			++frameCount;
		}
		while (GetTickCount64() - start < 1300);
		return submitted;
	}


	bool StuckFrameAfterIdle()
	{
		if (session == XR_NULL_HANDLE || !running)
		{
			return false;
		}
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState{XR_TYPE_FRAME_STATE};
		if (!Check(xrWaitFrame(session, &waitInfo, &frameState), "stuck frame wait"))
		{
			return false;
		}
		if (frameState.predictedDisplayTime <= lastDisplayTime || frameState.predictedDisplayPeriod <= 0)
		{
			std::cerr << "stuck frame timing is invalid\n";
			return false;
		}
		lastDisplayTime = frameState.predictedDisplayTime;

		const ULONGLONG waitStart = GetTickCount64();
		while (GetTickCount64() - waitStart < 1200)
		{
			Sleep(10);
		}
		while (CountAgentXRSessionWindows() != 0 && GetTickCount64() - waitStart < 2500)
		{
			Sleep(10);
		}
		if (CountAgentXRSessionWindows() != 0)
		{
			std::cerr << "stuck frame: window did not expire\n";
			return false;
		}
		if (!RequestActiveSessionExit())
		{
			return false;
		}
		return RestartAfterForcedReady();
	}




	bool Sync(bool& pressed, bool* poseActive = nullptr, bool* triggerValue = nullptr, float* aClickValue = nullptr)
	{
		XrActiveActionSet active{actionSet, XR_NULL_PATH};
		XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		if (!Check(xrSyncActions(session, &sync), "sync actions"))
		{
			return false;
		}
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = action;
		info.subactionPath = rightPath;
		XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
		if (!Check(xrGetActionStateBoolean(session, &info, &state), "read action"))
		{
			return false;
		}
		pressed = state.isActive != XR_FALSE && state.currentState == XR_TRUE;

		if (poseActive != nullptr)
		{
			XrActionStateGetInfo poseInfo{XR_TYPE_ACTION_STATE_GET_INFO};
			poseInfo.action = poseAction;
			poseInfo.subactionPath = rightPath;
			XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
			if (!Check(xrGetActionStatePose(session, &poseInfo, &poseState), "read pose action"))
			{
				return false;
			}
			*poseActive = poseState.isActive == XR_TRUE;
		}
		if (triggerValue != nullptr)
		{
			XrActionStateGetInfo triggerInfo{XR_TYPE_ACTION_STATE_GET_INFO};
			triggerInfo.action = triggerValueAction;
			triggerInfo.subactionPath = rightPath;
			XrActionStateBoolean triggerState{XR_TYPE_ACTION_STATE_BOOLEAN};
			if (!Check(xrGetActionStateBoolean(session, &triggerInfo, &triggerState), "read trigger boolean action"))
			{
				return false;
			}
			*triggerValue = triggerState.currentState == XR_TRUE;
		}
		if (aClickValue != nullptr)
		{
			XrActionStateGetInfo aClickInfo{XR_TYPE_ACTION_STATE_GET_INFO};
			aClickInfo.action = aClickFloatAction;
			aClickInfo.subactionPath = rightPath;
			XrActionStateFloat aClickState{XR_TYPE_ACTION_STATE_FLOAT};
			if (!Check(xrGetActionStateFloat(session, &aClickInfo, &aClickState), "read A click float action"))
			{
				return false;
			}
			*aClickValue = aClickState.currentState;
		}
		return true;
	}

	bool LocateRightGrip(XrTime time, XrSpaceLocation& location)
	{
		location = {XR_TYPE_SPACE_LOCATION};
		return Check(xrLocateSpace(rightGripSpace, localSpace, time, &location), "locate right grip");
	}
	bool CheckReferenceOrigins(XrTime time)
	{
		XrSpaceLocation localLocation{XR_TYPE_SPACE_LOCATION};
		XrSpaceLocation floorLocation{XR_TYPE_SPACE_LOCATION};
		if (!Check(xrLocateSpace(localSpace, stageSpace, time, &localLocation), "locate local origin") || !Check(xrLocateSpace(localFloorSpace, stageSpace, time, &floorLocation), "locate local floor origin"))
		{
			return false;
		}
		const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
		return (localLocation.locationFlags & required) == required && (floorLocation.locationFlags & required) == required && std::fabs(localLocation.pose.position.y - 1.65f) < 0.05f && std::fabs(floorLocation.pose.position.y) < 0.05f;
	}

	bool InvalidCallOrder()
	{
		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		return Expect(xrBeginFrame(session, &beginInfo), XR_ERROR_CALL_ORDER_INVALID, "begin frame before wait");
	}

	bool WaitFrameOnly(XrFrameState& frameState, std::string_view operation)
	{
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		frameState = {XR_TYPE_FRAME_STATE};
		if (!Check(xrWaitFrame(session, &waitInfo, &frameState), operation))
		{
			return false;
		}
		if (frameState.predictedDisplayTime <= lastDisplayTime || frameState.predictedDisplayPeriod <= 0)
		{
			std::cerr << operation << ": frame timing is invalid\n";
			return false;
		}
		lastDisplayTime = frameState.predictedDisplayTime;
		return true;
	}

	bool BeginFrameOnly(std::string_view operation)
	{
		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		return Check(xrBeginFrame(session, &beginInfo), operation);
	}

	bool EndFrameOnly(XrTime displayTime, std::string_view operation)
	{
		XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
		endInfo.displayTime = displayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		return Expect(xrEndFrame(session, &endInfo), XR_SUCCESS, operation);
	}

	bool CheckTimelinePose(XrTime displayTime, float expectedHeadX, float expectedGripX, std::string_view operation)
	{
		XrViewState viewState{XR_TYPE_VIEW_STATE};
		std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
		XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = displayTime;
		locateInfo.space = localSpace;
		uint32_t viewCount = 0;
		if (!Check(xrLocateViews(session, &locateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data()), operation) || viewCount != views.size())
		{
			std::cerr << operation << ": expected two valid views\n";
			return false;
		}
		XrSpaceLocation gripLocation{XR_TYPE_SPACE_LOCATION};
		if (!Check(xrLocateSpace(rightGripSpace, localSpace, displayTime, &gripLocation), operation))
		{
			return false;
		}
		const XrViewStateFlags requiredViewFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
		const XrSpaceLocationFlags requiredGripFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
		const float headX = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
		if ((viewState.viewStateFlags & requiredViewFlags) != requiredViewFlags || (gripLocation.locationFlags & requiredGripFlags) != requiredGripFlags || std::fabs(headX - expectedHeadX) > 0.01f || std::fabs(gripLocation.pose.position.x - expectedGripX) > 0.01f)
		{
			std::cerr << operation << ": old/new timeline pose mismatch head=" << headX << " grip=" << gripLocation.pose.position.x << '\n';
			return false;
		}
		return true;
	}

	bool PipelinedFrame()
	{
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		const auto waitFrame = [&](XrFrameState& state, std::string_view operation)
		{
			state = {XR_TYPE_FRAME_STATE};
			if (!Check(xrWaitFrame(session, &waitInfo, &state), operation))
			{
				return false;
			}
			if (state.predictedDisplayTime <= lastDisplayTime)
			{
				std::cerr << operation << ": predicted display time did not increase\n";
				return false;
			}
			if (state.predictedDisplayPeriod <= 0)
			{
				std::cerr << operation << ": predicted display period is invalid\n";
				return false;
			}
			lastDisplayTime = state.predictedDisplayTime;
			return true;
		};
		const auto locateViews = [&](XrTime displayTime, std::string_view operation)
		{
			XrViewState viewState{XR_TYPE_VIEW_STATE};
			std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
			XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
			locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			locateInfo.displayTime = displayTime;
			locateInfo.space = localSpace;
			uint32_t viewCount = 0;
			if (!Check(xrLocateViews(session, &locateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data()), operation))
			{
				return false;
			}
			if (viewCount != views.size())
			{
				std::cerr << operation << ": expected 2 located views, got " << viewCount << '\n';
				return false;
			}
			return true;
		};
		const auto beginFrame = [&](std::string_view operation)
		{
			return Check(xrBeginFrame(session, &beginInfo), operation);
		};
		const auto endFrame = [&](XrTime displayTime, std::string_view operation)
		{
			XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
			endInfo.displayTime = displayTime;
			endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			const XrResult result = xrEndFrame(session, &endInfo);
			if (result == XR_SUCCESS || result == XR_FRAME_DISCARDED)
			{
				return true;
			}
			return Check(result, operation);
		};

		if (!Expect(xrBeginFrame(session, &beginInfo), XR_ERROR_CALL_ORDER_INVALID, "pipelined-frame empty begin"))
		{
			return false;
		}
		XrFrameEndInfo emptyEndInfo{XR_TYPE_FRAME_END_INFO};
		emptyEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (!Expect(xrEndFrame(session, &emptyEndInfo), XR_ERROR_CALL_ORDER_INVALID, "pipelined-frame empty end"))
		{
			return false;
		}

		std::array<XrFrameState, 3> firstStates{};
		if (!waitFrame(firstStates[0], "pipelined-frame W1") || !waitFrame(firstStates[1], "pipelined-frame W2") || !waitFrame(firstStates[2], "pipelined-frame W3"))
		{
			return false;
		}
		if (!locateViews(firstStates[0].predictedDisplayTime, "pipelined-frame locate W1") || !locateViews(firstStates[1].predictedDisplayTime, "pipelined-frame locate W2") || !locateViews(firstStates[2].predictedDisplayTime, "pipelined-frame locate W3"))
		{
			return false;
		}
		if (!beginFrame("pipelined-frame B1") || !beginFrame("pipelined-frame B2") || !beginFrame("pipelined-frame B3"))
		{
			return false;
		}
		if (!endFrame(firstStates[0].predictedDisplayTime, "pipelined-frame E1") || !endFrame(firstStates[1].predictedDisplayTime, "pipelined-frame E2") || !endFrame(firstStates[2].predictedDisplayTime, "pipelined-frame E3"))
		{
			return false;
		}

		std::array<XrFrameState, 3> interleaveStates{};
		if (!waitFrame(interleaveStates[0], "interleave W1") || !waitFrame(interleaveStates[1], "interleave W2") || !beginFrame("interleave B1") || !waitFrame(interleaveStates[2], "interleave W3") || !beginFrame("interleave B2"))
		{
			return false;
		}
		if (!locateViews(interleaveStates[0].predictedDisplayTime, "interleave locate W1") || !locateViews(interleaveStates[1].predictedDisplayTime, "interleave locate W2") || !locateViews(interleaveStates[2].predictedDisplayTime, "interleave locate W3"))
		{
			return false;
		}
		if (!endFrame(interleaveStates[0].predictedDisplayTime, "interleave E1") || !beginFrame("interleave B3") || !endFrame(interleaveStates[1].predictedDisplayTime, "interleave E2") || !endFrame(interleaveStates[2].predictedDisplayTime, "interleave E3"))
		{
			return false;
		}

		std::array<XrFrameState, 2> beginCreditStates{};
		if (!waitFrame(beginCreditStates[0], "begin-credit W1") || !waitFrame(beginCreditStates[1], "begin-credit W2") || !beginFrame("begin-credit B1"))
		{
			return false;
		}
		if (!endFrame(beginCreditStates[1].predictedDisplayTime, "begin-credit E2 before E1"))
		{
			return false;
		}
		XrFrameEndInfo waitedEndInfo{XR_TYPE_FRAME_END_INFO};
		waitedEndInfo.displayTime = beginCreditStates[0].predictedDisplayTime;
		waitedEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (!Expect(xrEndFrame(session, &waitedEndInfo), XR_ERROR_CALL_ORDER_INVALID, "begin-credit E1 before second Begin"))
		{
			return false;
		}
		if (!beginFrame("begin-credit B2") || !endFrame(beginCreditStates[0].predictedDisplayTime, "begin-credit E1 after second Begin"))
		{
			return false;
		}

		std::array<XrFrameState, 2> endStates{};
		if (!waitFrame(endStates[0], "end-order W1") || !waitFrame(endStates[1], "end-order W2") || !beginFrame("end-order B1") || !beginFrame("end-order B2"))
		{
			return false;
		}
		if (!locateViews(endStates[0].predictedDisplayTime, "end-order locate W1") || !locateViews(endStates[1].predictedDisplayTime, "end-order locate W2"))
		{
			return false;
		}
		if (!endFrame(endStates[1].predictedDisplayTime, "end-order E2 before E1"))
		{
			return false;
		}
		XrFrameEndInfo duplicateEndInfo{XR_TYPE_FRAME_END_INFO};
		duplicateEndInfo.displayTime = endStates[1].predictedDisplayTime;
		duplicateEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (!Expect(xrEndFrame(session, &duplicateEndInfo), XR_ERROR_TIME_INVALID, "end-order duplicate E2"))
		{
			return false;
		}
		XrFrameEndInfo unknownEndInfo{XR_TYPE_FRAME_END_INFO};
		unknownEndInfo.displayTime = endStates[0].predictedDisplayTime + 1;
		unknownEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (!Expect(xrEndFrame(session, &unknownEndInfo), XR_ERROR_TIME_INVALID, "end-order unknown current-epoch time"))
		{
			return false;
		}
		return endFrame(endStates[0].predictedDisplayTime, "end-order E1 after invalid ends");

	}

	bool PipelinedSwapchain()
	{
		XrSwapchain pipelinedSwapchain = XR_NULL_HANDLE;
		uint32_t unawaitedCount = 0;
		uint32_t waitedCount = 0;
		const auto destroy = [&]() -> bool
		{
			if (pipelinedSwapchain == XR_NULL_HANDLE)
			{
				return true;
			}
			const XrResult result = xrDestroySwapchain(pipelinedSwapchain);
			pipelinedSwapchain = XR_NULL_HANDLE;
			return Check(result, "destroy pipelined swapchain");
		};
		const auto cleanup = [&]()
		{
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			while (waitedCount > 0)
			{
				if (xrReleaseSwapchainImage(pipelinedSwapchain, &release) != XR_SUCCESS)
				{
					break;
				}
				--waitedCount;
			}
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 2000000000LL;
			while (unawaitedCount > 0)
			{
				if (xrWaitSwapchainImage(pipelinedSwapchain, &wait) != XR_SUCCESS)
				{
					break;
				}
				--unawaitedCount;
				++waitedCount;
				if (xrReleaseSwapchainImage(pipelinedSwapchain, &release) != XR_SUCCESS)
				{
					break;
				}
				--waitedCount;
			}
			while (waitedCount > 0)
			{
				if (xrReleaseSwapchainImage(pipelinedSwapchain, &release) != XR_SUCCESS)
				{
					break;
				}
				--waitedCount;
			}
			destroy();
		};
		XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		createInfo.createFlags = 0;
		createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		createInfo.format = static_cast<int64_t>(selectedFormat);
		createInfo.sampleCount = 1;
		createInfo.width = 1;
		createInfo.height = 1;
		createInfo.arraySize = 1;
		createInfo.faceCount = 1;
		createInfo.mipCount = 1;
		if (!Check(xrCreateSwapchain(session, &createInfo, &pipelinedSwapchain), "create pipelined swapchain"))
		{
			cleanup();
			return false;
		}
		uint32_t imageCount = 0;
		if (!Check(xrEnumerateSwapchainImages(pipelinedSwapchain, 0, &imageCount, nullptr), "pipelined image count"))
		{
			cleanup();
			return false;
		}
		if (imageCount < 3)
		{
			std::cerr << "pipelined-swapchain: expected at least 3 images, got " << imageCount << '\n';
			cleanup();
			return false;
		}
		std::vector<XrSwapchainImageD3D12KHR> pipelinedImages;
		pipelinedImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
		if (!Check(xrEnumerateSwapchainImages(pipelinedSwapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(pipelinedImages.data())), "pipelined images"))
		{
			cleanup();
			return false;
		}
		const auto acquire = [&](uint32_t& index, std::string_view operation)
		{
			XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			if (!Check(xrAcquireSwapchainImage(pipelinedSwapchain, &acquireInfo, &index), operation))
			{
				return false;
			}
			++unawaitedCount;
			return true;
		};
		const auto waitImage = [&](std::string_view operation)
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 2000000000LL;
			if (!Check(xrWaitSwapchainImage(pipelinedSwapchain, &wait), operation))
			{
				return false;
			}
			--unawaitedCount;
			++waitedCount;
			return true;
		};
		const auto release = [&](std::string_view operation)
		{
			XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			if (!Check(xrReleaseSwapchainImage(pipelinedSwapchain, &releaseInfo), operation))
			{
				return false;
			}
			--waitedCount;
			return true;
		};
		uint32_t firstIndex = 0;
		uint32_t secondIndex = 0;
		uint32_t thirdIndex = 0;
		if (!acquire(firstIndex, "acquire pipelined image 1") || !acquire(secondIndex, "acquire pipelined image 2"))
		{
			cleanup();
			return false;
		}
		if (firstIndex == secondIndex)
		{
			std::cerr << "pipelined-swapchain: first two acquired images were not distinct\n";
			cleanup();
			return false;
		}
		if (!waitImage("wait pipelined image 1") || !release("release pipelined image 1"))
		{
			cleanup();
			return false;
		}
		if (!acquire(thirdIndex, "acquire pipelined image 3"))
		{
			cleanup();
			return false;
		}
		if (thirdIndex == secondIndex)
		{
			std::cerr << "pipelined-swapchain: third acquire reused still-outstanding image " << secondIndex << '\n';
			cleanup();
			return false;
		}
		if (!waitImage("wait pipelined image 2") || !release("release pipelined image 2") || !waitImage("wait pipelined image 3") || !release("release pipelined image 3"))
		{
			cleanup();
			return false;
		}
		std::cerr << "pipelined-swapchain: first=" << firstIndex << " second=" << secondIndex << " third=" << thirdIndex << '\n';
		return destroy();
	}

	bool InvalidHandle()
	{
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState{XR_TYPE_FRAME_STATE};
		return Expect(xrWaitFrame(XR_NULL_HANDLE, &waitInfo, &frameState), XR_ERROR_HANDLE_INVALID, "invalid session handle");
	}

	bool RecreateSession()
	{
		DestroySessionOnly();
		return CreateSessionOnly();
	}

	bool PrepareUnrealActionSetup()
	{
		return CreateActions(true, true, true) && CreateSwapchain();
	}

	bool ValidatePathContracts()
	{
		const auto roundTrip = [&](const char* source, XrPath& output, std::string_view operation)
		{
			if (!Check(xrStringToPath(instance, source, &output), operation))
			{
				return false;
			}
			std::array<char, XR_MAX_PATH_LENGTH> buffer{};
			uint32_t count = 0;
			if (!Check(xrPathToString(instance, output, static_cast<uint32_t>(buffer.size()), &count, buffer.data()), "path to string"))
			{
				return false;
			}
			if (count != std::strlen(source) + 1 || std::strcmp(buffer.data(), source) != 0)
			{
				std::cerr << operation << " did not round-trip exactly\n";
				return false;
			}
			return true;
		};

		const char* unknownProfileString = "/interaction_profiles/agentxr/unknown_controller";
		XrPath unknownProfile = XR_NULL_PATH;
		if (!roundTrip(unknownProfileString, unknownProfile, "unknown profile path"))
		{
			return false;
		}
		const char* unsupportedComponentString = "/user/hand/right/input/unsupported_component";
		XrPath unsupportedComponent = XR_NULL_PATH;
		if (!roundTrip(unsupportedComponentString, unsupportedComponent, "unsupported component path"))
		{
			return false;
		}
		const XrPath knownBinding = Path("/user/hand/right/input/a/click");
		const XrPath oculusProfile = Path("/interaction_profiles/oculus/touch_controller");
		if (knownBinding == XR_NULL_PATH || oculusProfile == XR_NULL_PATH)
		{
			return false;
		}

		const XrActionSuggestedBinding unknownProfileBinding{action, knownBinding};
		XrInteractionProfileSuggestedBinding unknownProfileSuggestions{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		unknownProfileSuggestions.interactionProfile = unknownProfile;
		unknownProfileSuggestions.countSuggestedBindings = 1;
		unknownProfileSuggestions.suggestedBindings = &unknownProfileBinding;
		if (!Check(xrSuggestInteractionProfileBindings(instance, &unknownProfileSuggestions), "suggest unknown profile"))
		{
			return false;
		}

		const std::array<XrActionSuggestedBinding, 2> duplicateBindings = {unknownProfileBinding, unknownProfileBinding};
		XrInteractionProfileSuggestedBinding duplicateSuggestions{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		duplicateSuggestions.interactionProfile = oculusProfile;
		duplicateSuggestions.countSuggestedBindings = static_cast<uint32_t>(duplicateBindings.size());
		duplicateSuggestions.suggestedBindings = duplicateBindings.data();
		if (!Check(xrSuggestInteractionProfileBindings(instance, &duplicateSuggestions), "suggest duplicate identical bindings"))
		{
			return false;
		}

		const XrActionSuggestedBinding unsupportedComponentBinding{action, unsupportedComponent};
		XrInteractionProfileSuggestedBinding unsupportedComponentSuggestions{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		unsupportedComponentSuggestions.interactionProfile = oculusProfile;
		unsupportedComponentSuggestions.countSuggestedBindings = 1;
		unsupportedComponentSuggestions.suggestedBindings = &unsupportedComponentBinding;
		if (!Check(xrSuggestInteractionProfileBindings(instance, &unsupportedComponentSuggestions), "suggest unsupported Oculus component"))
		{
			return false;
		}

		XrPath malformed = XR_NULL_PATH;
		return Expect(xrStringToPath(instance, "interaction_profiles/agentxr/malformed", &malformed), XR_ERROR_PATH_FORMAT_INVALID, "malformed path");
	}

	bool RestartAfterForcedReady()
	{
		const auto consumeStates = [&](std::string_view operation)
		{
			const std::array<XrSessionState, 3> expected = {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED};
			size_t next = 0;
			for (int attempt = 0; attempt < 100 && next < expected.size(); ++attempt)
			{
				XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
				XrResult pollResult = XR_EVENT_UNAVAILABLE;
				while ((pollResult = xrPollEvent(instance, &event)) == XR_SUCCESS)
				{
					if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
					{
						const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
						if (changed->session == session && changed->state == XR_SESSION_STATE_STOPPING)
						{
							std::cerr << operation << " received unexpected STOPPING state\n";
							return false;
						}
						if (changed->session == session && next < expected.size() && changed->state == expected[next])
						{
							++next;
						}
					}
					event = {XR_TYPE_EVENT_DATA_BUFFER};
				}
				if (pollResult != XR_EVENT_UNAVAILABLE)
				{
					std::cerr << operation << " event polling failed: " << static_cast<int>(pollResult) << '\n';
					return false;
				}
				if (next < expected.size())
				{
					Sleep(1);
				}
			}
			if (next != expected.size())
			{
				std::cerr << operation << " timed out waiting for session states\n";
				return false;
			}
			return true;
		};

		if (session == XR_NULL_HANDLE || !running)
		{
			return false;
		}
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frameState{XR_TYPE_FRAME_STATE};
		if (!Expect(xrWaitFrame(session, &waitInfo, &frameState), XR_ERROR_SESSION_NOT_RUNNING, "forced exit session not running"))
		{
			return false;
		}
		running = false;
		priorEpochDisplayTime = lastDisplayTime;
		lastDisplayTime = 0;
		XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
		beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		if (!Check(xrBeginSession(session, &beginInfo), "forced-ready restart session"))
		{
			return false;
		}
		if (!consumeStates("forced-ready synchronized/visible/focused"))
		{
			return false;
		}
		running = true;
		return true;
	}

	bool RecreateSwapchain()
	{
		return CreateSwapchain();
	}

	bool EndPriorEpochFrame()
	{
		if (session == XR_NULL_HANDLE || !running || priorEpochDisplayTime <= 0)
		{
			return false;
		}
		XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState currentState{XR_TYPE_FRAME_STATE};
		if (!Check(xrWaitFrame(session, &waitInfo, &currentState), "prior-epoch current wait"))
		{
			return false;
		}
		if (currentState.predictedDisplayTime <= lastDisplayTime || currentState.predictedDisplayPeriod <= 0)
		{
			std::cerr << "prior-epoch current frame timing is invalid\n";
			return false;
		}
		lastDisplayTime = currentState.predictedDisplayTime;
		XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
		if (!Check(xrBeginFrame(session, &beginInfo), "prior-epoch current begin"))
		{
			return false;
		}
		XrFrameEndInfo priorEndInfo{XR_TYPE_FRAME_END_INFO};
		priorEndInfo.displayTime = priorEpochDisplayTime;
		priorEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (!Expect(xrEndFrame(session, &priorEndInfo), XR_SUCCESS, "prior-epoch end"))
		{
			return false;
		}
		XrFrameEndInfo currentEndInfo{XR_TYPE_FRAME_END_INFO};
		currentEndInfo.displayTime = currentState.predictedDisplayTime;
		currentEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		return Expect(xrEndFrame(session, &currentEndInfo), XR_SUCCESS, "prior-epoch current end");
	}




	bool DestroyCurrentSession()
	{
		DestroySessionOnly();
		return session == XR_NULL_HANDLE;
	}

	bool HasApiFallback() const
	{
		return apiFallbackObserved;
	}

	bool ReleaseInstance(XrInstance& releasedInstance, PFN_xrDestroyInstance& destroyFunction)
	{
		if (instance == XR_NULL_HANDLE || xrDestroyInstance == nullptr)
		{
			return false;
		}
		releasedInstance = instance;
		destroyFunction = xrDestroyInstance;
		instance = XR_NULL_HANDLE;
		return true;
	}

private:
	bool CreateSessionOnly(bool createDefaultResources = true)
	{
		if (!graphicsRequirementsQueried)
		{
			return false;
		}
		XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
		binding.device = graphics.device.Get();
		binding.queue = graphics.queue.Get();
		XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
		createInfo.next = &binding;
		createInfo.systemId = systemId;
		if (!Check(xrCreateSession(instance, &createInfo, &session), "create session"))
		{
			return false;
		}
		if (!CreateSpaces() || (createDefaultResources && (!CreateActions() || !CreateSwapchain())))
		{
			DestroySessionOnly();
			return false;
		}
		return true;
	}

	void DestroySessionOnly()
	{
		if (rightGripSpace != XR_NULL_HANDLE && xrDestroySpace != nullptr)
		{
			xrDestroySpace(rightGripSpace);
			rightGripSpace = XR_NULL_HANDLE;
		}
		if (viewSpace != XR_NULL_HANDLE && xrDestroySpace != nullptr)
		{
			xrDestroySpace(viewSpace);
			viewSpace = XR_NULL_HANDLE;
		}
		if (localSpace != XR_NULL_HANDLE && xrDestroySpace != nullptr)
		{
			xrDestroySpace(localSpace);
			localSpace = XR_NULL_HANDLE;
		}
		if (localFloorSpace != XR_NULL_HANDLE && xrDestroySpace != nullptr)
		{
			xrDestroySpace(localFloorSpace);
			localFloorSpace = XR_NULL_HANDLE;
		}
		if (stageSpace != XR_NULL_HANDLE && xrDestroySpace != nullptr)
		{
			xrDestroySpace(stageSpace);
			stageSpace = XR_NULL_HANDLE;
		}
		if (swapchain != XR_NULL_HANDLE && xrDestroySwapchain != nullptr)
		{
			xrDestroySwapchain(swapchain);
			swapchain = XR_NULL_HANDLE;
			heldSwapchainImage = false;
		}
		if (session != XR_NULL_HANDLE && xrDestroySession != nullptr)
		{
			xrDestroySession(session);
			session = XR_NULL_HANDLE;
		}
		if (aClickFloatAction != XR_NULL_HANDLE && xrDestroyAction != nullptr)
		{
			xrDestroyAction(aClickFloatAction);
			aClickFloatAction = XR_NULL_HANDLE;
		}
		if (triggerValueAction != XR_NULL_HANDLE && xrDestroyAction != nullptr)
		{
			xrDestroyAction(triggerValueAction);
			triggerValueAction = XR_NULL_HANDLE;
		}
		if (poseAction != XR_NULL_HANDLE && xrDestroyAction != nullptr)
		{
			xrDestroyAction(poseAction);
			poseAction = XR_NULL_HANDLE;
		}
		if (action != XR_NULL_HANDLE && xrDestroyAction != nullptr)
		{
			xrDestroyAction(action);
			action = XR_NULL_HANDLE;
		}
		if (actionSet != XR_NULL_HANDLE && xrDestroyActionSet != nullptr)
		{
			xrDestroyActionSet(actionSet);
			actionSet = XR_NULL_HANDLE;
		}
		running = false;
		lastDisplayTime = 0;
	}

	bool CreateSpaces()
	{
		uint32_t count = 0;
		if (!Check(xrEnumerateReferenceSpaces(session, 0, &count, nullptr), "reference space count"))
		{
			return false;
		}
		std::vector<XrReferenceSpaceType> supported(count);
		if (!Check(xrEnumerateReferenceSpaces(session, count, &count, supported.data()), "reference spaces") || std::find(supported.begin(), supported.end(), XR_REFERENCE_SPACE_TYPE_VIEW) == supported.end() || std::find(supported.begin(), supported.end(), XR_REFERENCE_SPACE_TYPE_LOCAL) == supported.end() || std::find(supported.begin(), supported.end(), XR_REFERENCE_SPACE_TYPE_STAGE) == supported.end() || std::find(supported.begin(), supported.end(), XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT) == supported.end())
		{
			return false;
		}
		const XrPosef identity{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		const auto create = [&](XrReferenceSpaceType type, XrSpace& output)
		{
			XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			info.referenceSpaceType = type;
			info.poseInReferenceSpace = identity;
			return Check(xrCreateReferenceSpace(session, &info, &output), "create reference space");
		};
		return create(XR_REFERENCE_SPACE_TYPE_VIEW, viewSpace) && create(XR_REFERENCE_SPACE_TYPE_LOCAL, localSpace) && create(XR_REFERENCE_SPACE_TYPE_STAGE, stageSpace) && create(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT, localFloorSpace);
	}

	bool CreateActions(bool createPoseSpaceBeforeAttach = false, bool useSimpleControllerProfile = false, bool includeScalarConversionBindings = false)
	{
		XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strcpy(setInfo.actionSetName, "smoke");
		std::strcpy(setInfo.localizedActionSetName, "Smoke");
		if (!Check(xrCreateActionSet(instance, &setInfo, &actionSet), "create action set"))
		{
			return false;
		}
		rightPath = Path("/user/hand/right");
		if (rightPath == XR_NULL_PATH)
		{
			return false;
		}
		const XrPath subactions[] = {rightPath};
		XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(actionInfo.actionName, "smoke_a");
		std::strcpy(actionInfo.localizedActionName, "Smoke A");
		actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
		actionInfo.countSubactionPaths = 1;
		actionInfo.subactionPaths = subactions;
		if (!Check(xrCreateAction(actionSet, &actionInfo, &action), "create boolean action"))
		{
			return false;
		}
		std::strcpy(actionInfo.actionName, "smoke_grip_pose");
		std::strcpy(actionInfo.localizedActionName, "Smoke Grip Pose");
		actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
		if (!Check(xrCreateAction(actionSet, &actionInfo, &poseAction), "create pose action"))
		{
			return false;
		}
		if (includeScalarConversionBindings)
		{
			std::strcpy(actionInfo.actionName, "smoke_trigger_value");
			std::strcpy(actionInfo.localizedActionName, "Smoke Trigger Value");
			actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
			if (!Check(xrCreateAction(actionSet, &actionInfo, &triggerValueAction), "create trigger boolean action"))
			{
				return false;
			}
			std::strcpy(actionInfo.actionName, "smoke_a_click_float");
			std::strcpy(actionInfo.localizedActionName, "Smoke A Click Float");
			actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
			if (!Check(xrCreateAction(actionSet, &actionInfo, &aClickFloatAction), "create A click float action"))
			{
				return false;
			}
		}
		const XrPath profile = Path(useSimpleControllerProfile ? "/interaction_profiles/khr/simple_controller" : "/interaction_profiles/oculus/touch_controller");
		const XrPath bindingPath = Path("/user/hand/right/input/a/click");
		const XrPath poseBindingPath = Path("/user/hand/right/input/grip/pose");
		const XrPath oculusProfile = includeScalarConversionBindings ? Path("/interaction_profiles/oculus/touch_controller") : XR_NULL_PATH;
		const XrPath triggerValuePath = includeScalarConversionBindings ? Path("/user/hand/right/input/trigger/value") : XR_NULL_PATH;
		if (profile == XR_NULL_PATH || bindingPath == XR_NULL_PATH || poseBindingPath == XR_NULL_PATH || (includeScalarConversionBindings && (oculusProfile == XR_NULL_PATH || triggerValuePath == XR_NULL_PATH)))
		{
			return false;
		}
		const XrActionSuggestedBinding suggestedBindings[] = {{action, bindingPath}, {poseAction, poseBindingPath}};
		const XrActionSuggestedBinding simpleBinding{poseAction, poseBindingPath};
		const XrActionSuggestedBinding extendedBindings[] = {{action, bindingPath}, {poseAction, poseBindingPath}, {triggerValueAction, triggerValuePath}, {aClickFloatAction, bindingPath}};
		XrInteractionProfileSuggestedBinding suggestions{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		suggestions.interactionProfile = profile;
		if (useSimpleControllerProfile)
		{
			suggestions.countSuggestedBindings = 1;
			suggestions.suggestedBindings = &simpleBinding;
		}
		else if (includeScalarConversionBindings)
		{
			suggestions.countSuggestedBindings = static_cast<uint32_t>(std::size(extendedBindings));
			suggestions.suggestedBindings = extendedBindings;
		}
		else
		{
			suggestions.countSuggestedBindings = static_cast<uint32_t>(std::size(suggestedBindings));
			suggestions.suggestedBindings = suggestedBindings;
		}
		if (!Check(xrSuggestInteractionProfileBindings(instance, &suggestions), "suggest bindings"))
		{
			return false;
		}
		if (useSimpleControllerProfile && includeScalarConversionBindings)
		{
			suggestions.interactionProfile = oculusProfile;
			suggestions.countSuggestedBindings = static_cast<uint32_t>(std::size(extendedBindings));
			suggestions.suggestedBindings = extendedBindings;
			if (!Check(xrSuggestInteractionProfileBindings(instance, &suggestions), "suggest Oculus Touch scalar bindings"))
			{
				return false;
			}
		}
		const auto createPoseSpace = [&]()
		{
			XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
			spaceInfo.action = poseAction;
			spaceInfo.subactionPath = rightPath;
			spaceInfo.poseInActionSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
			return Check(xrCreateActionSpace(session, &spaceInfo, &rightGripSpace), "create right grip space");
		};
		if (createPoseSpaceBeforeAttach && !createPoseSpace())
		{
			return false;
		}
		XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attach.countActionSets = 1;
		attach.actionSets = &actionSet;
		if (!Check(xrAttachSessionActionSets(session, &attach), "attach action set"))
		{
			return false;
		}
		if (!createPoseSpaceBeforeAttach && !createPoseSpace())
		{
			return false;
		}
		return true;
	}

	bool CreateSwapchain()
	{
		uint32_t formatCount = 0;
		if (!Check(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr), "format count"))
		{
			return false;
		}
		std::vector<int64_t> formats(formatCount);
		if (!Check(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()), "formats"))
		{
			return false;
		}
		const std::array<DXGI_FORMAT, 4> preferred = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
		selectedFormat = DXGI_FORMAT_UNKNOWN;
		for (DXGI_FORMAT candidate : preferred)
		{
			if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(candidate)) != formats.end())
			{
				selectedFormat = candidate;
				break;
			}
		}
		if (selectedFormat == DXGI_FORMAT_UNKNOWN)
		{
			return false;
		}
		XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
		createInfo.format = static_cast<int64_t>(selectedFormat);
		createInfo.sampleCount = 1;
		createInfo.width = 1024;
		createInfo.height = 1024;
		createInfo.arraySize = 2;
		createInfo.faceCount = 1;
		createInfo.mipCount = 1;
		if (!Check(xrCreateSwapchain(session, &createInfo, &swapchain), "create swapchain"))
		{
			return false;
		}
		heldSwapchainImage = false;
		uint32_t imageCount = 0;
		if (!Check(xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr), "image count"))
		{
			return false;
		}
		images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
		return Check(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "images");
	}

	XrPath Path(const char* path)
	{
		XrPath result = XR_NULL_PATH;
		if (!Check(xrStringToPath(instance, path, &result), "string path"))
		{
			return XR_NULL_PATH;
		}
		return result;
	}

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace viewSpace = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE;
	XrSpace localFloorSpace = XR_NULL_HANDLE;
	XrSpace stageSpace = XR_NULL_HANDLE;
	XrSpace rightGripSpace = XR_NULL_HANDLE;
	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction action = XR_NULL_HANDLE;
	XrAction poseAction = XR_NULL_HANDLE;
	XrAction triggerValueAction = XR_NULL_HANDLE;
	XrAction aClickFloatAction = XR_NULL_HANDLE;
	XrPath rightPath = XR_NULL_PATH;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageD3D12KHR> images;
	DXGI_FORMAT selectedFormat = DXGI_FORMAT_UNKNOWN;
	Graphics graphics;
	PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
	PFN_xrCreateInstance xrCreateInstance = nullptr;
	PFN_xrDestroyInstance xrDestroyInstance = nullptr;
	PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
	PFN_xrPollEvent xrPollEvent = nullptr;
	PFN_xrGetSystem xrGetSystem = nullptr;
	PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
	PFN_xrEnumerateEnvironmentBlendModes xrEnumerateEnvironmentBlendModes = nullptr;
	PFN_xrEnumerateViewConfigurations xrEnumerateViewConfigurations = nullptr;
	PFN_xrGetViewConfigurationProperties xrGetViewConfigurationProperties = nullptr;
	PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = nullptr;
	PFN_xrCreateSession xrCreateSession = nullptr;
	PFN_xrDestroySession xrDestroySession = nullptr;
	PFN_xrBeginSession xrBeginSession = nullptr;
	PFN_xrEndSession xrEndSession = nullptr;
	PFN_xrRequestExitSession xrRequestExitSession = nullptr;
	PFN_xrWaitFrame xrWaitFrame = nullptr;
	PFN_xrBeginFrame xrBeginFrame = nullptr;
	PFN_xrEndFrame xrEndFrame = nullptr;
	PFN_xrEnumerateReferenceSpaces xrEnumerateReferenceSpaces = nullptr;
	PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
	PFN_xrCreateActionSpace xrCreateActionSpace = nullptr;
	PFN_xrDestroySpace xrDestroySpace = nullptr;
	PFN_xrLocateSpace xrLocateSpace = nullptr;
	PFN_xrLocateViews xrLocateViews = nullptr;
	PFN_xrStringToPath xrStringToPath = nullptr;
	PFN_xrPathToString xrPathToString = nullptr;
	PFN_xrCreateActionSet xrCreateActionSet = nullptr;
	PFN_xrDestroyActionSet xrDestroyActionSet = nullptr;
	PFN_xrCreateAction xrCreateAction = nullptr;
	PFN_xrDestroyAction xrDestroyAction = nullptr;
	PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = nullptr;
	PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
	PFN_xrSyncActions xrSyncActions = nullptr;
	PFN_xrGetActionStateBoolean xrGetActionStateBoolean = nullptr;
	PFN_xrGetActionStateFloat xrGetActionStateFloat = nullptr;
	PFN_xrGetActionStatePose xrGetActionStatePose = nullptr;
	PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
	PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
	PFN_xrDestroySwapchain xrDestroySwapchain = nullptr;
	PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
	PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
	PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
	PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
	PFN_xrGetD3D12GraphicsRequirementsKHR xrGetD3D12GraphicsRequirementsKHR = nullptr;
	bool apiFallbackObserved = false;
	bool graphicsRequirementsQueried = false;
	bool running = false;
	XrTime lastDisplayTime = 0;
	bool heldSwapchainImage = false;
	XrTime priorEpochDisplayTime = 0;
};

bool SubmitTimeline(Control& control, const Json& timeline, uint64_t& timelineId, bool expectSuccess)
{
	Json request = control.Identity();
	request["op"] = "submit_timeline";
	request["timeline"] = timeline;
	agentxr::protocol::PipeFrame response;
	if (!control.Request(request, response))
	{
		std::cerr << "submit_timeline: transport request failed\n";
		return false;
	}
	const bool success = response.message.value("ok", false);
	if (success != expectSuccess)
	{
		const Json error = response.message.contains("error") && response.message.at("error").is_object() ? response.message.at("error") : Json::object();
		std::cerr << "submit_timeline: expected ok=" << (expectSuccess ? 1 : 0) << " got ok=" << (success ? 1 : 0) << " code=" << error.value("code", std::string{}) << " message=" << error.value("message", std::string{}) << '\n';
		return false;
	}
	if (success)
	{
		if (!response.message.contains("result") || !response.message.at("result").is_object())
		{
			std::cerr << "submit_timeline: successful response omitted result\n";
			return false;
		}
		timelineId = response.message.at("result").value("timelineId", 0ull);
		if (timelineId == 0)
		{
			std::cerr << "submit_timeline: successful response returned zero timeline ID\n";
			return false;
		}
		return true;
	}
	const Json error = response.message.contains("error") && response.message.at("error").is_object() ? response.message.at("error") : Json::object();
	const std::string code = error.value("code", std::string{});
	if (code != "invalid_timeline")
	{
		std::cerr << "submit_timeline: expected invalid_timeline rejection, got code=" << code << " message=" << error.value("message", std::string{}) << '\n';
		return false;
	}
	return true;
}

bool RunLiveTimelineRegression(OpenXR& client, Control& control, const Json& replacementTimeline, uint64_t& activeTimelineId)
{
	const auto checkSnapshot = [&](uint64_t expectedId, std::string_view expectedStatus, bool expectedWaited, bool expectedBegun, bool expectedPending, uint64_t expectedPendingId, std::string_view operation)
	{
		Json snapshot;
		if (!control.Snapshot(snapshot))
		{
			std::cerr << operation << ": snapshot request failed\n";
			return false;
		}
		const Json timeline = snapshot.value("timeline", Json::object());
		const uint64_t actualId = timeline.value("id", 0ull);
		const std::string actualStatus = timeline.value("status", std::string{});
		const bool actualWaited = snapshot.value("frameWaited", false);
		const bool actualBegun = snapshot.value("frameBegun", false);
		const bool actualPending = timeline.value("pending", false);
		const uint64_t actualPendingId = timeline.value("pendingId", 0ull);
		if (actualId != expectedId || actualStatus != expectedStatus || actualWaited != expectedWaited || actualBegun != expectedBegun || actualPending != expectedPending || actualPendingId != expectedPendingId)
		{
			std::cerr << operation << ": expected id=" << expectedId << " status=" << expectedStatus << " waited=" << (expectedWaited ? 1 : 0) << " begun=" << (expectedBegun ? 1 : 0) << " pending=" << (expectedPending ? 1 : 0) << " pendingId=" << expectedPendingId << " got id=" << actualId << " status=" << actualStatus << " waited=" << (actualWaited ? 1 : 0) << " begun=" << (actualBegun ? 1 : 0) << " pending=" << (actualPending ? 1 : 0) << " pendingId=" << actualPendingId << '\n';
			return false;
		}
		return true;
	};

	Json oldTimeline = replacementTimeline;
	const float newHeadX = replacementTimeline.at("initial").at("head").at("position").at(0).get<float>();
	const float newGripX = replacementTimeline.at("initial").at("right").at("grip").at("position").at(0).get<float>();
	oldTimeline["initial"]["head"]["position"][0] = newHeadX + 0.5f;
	oldTimeline["initial"]["right"]["grip"]["position"][0] = newGripX + 0.5f;
	oldTimeline["initial"]["right"]["aim"]["position"][0] = newGripX + 0.5f;

	uint64_t oldTimelineId = 0;
	if (!SubmitTimeline(control, oldTimeline, oldTimelineId, true))
	{
		std::cerr << "live submit old pending: submit assertion failed\n";
		return false;
	}
	if (!checkSnapshot(oldTimelineId, "armed", false, false, false, 0, "live submit old pending"))
	{
		return false;
	}
	XrFrameState oldFrame{XR_TYPE_FRAME_STATE};
	if (!client.WaitFrameOnly(oldFrame, "live submit old wait"))
	{
		return false;
	}
	if (!client.BeginFrameOnly("live submit old begin"))
	{
		return false;
	}
	if (!checkSnapshot(oldTimelineId, "running", false, true, false, 0, "live submit old active"))
	{
		return false;
	}
	if (!client.CheckTimelinePose(oldFrame.predictedDisplayTime, newHeadX + 0.5f, newGripX + 0.5f, "live submit old frame pose"))
	{
		return false;
	}

	uint64_t newTimelineId = 0;
	if (!SubmitTimeline(control, replacementTimeline, newTimelineId, true))
	{
		std::cerr << "live submit replacement pending: submit assertion failed\n";
		return false;
	}
	if (newTimelineId == oldTimelineId)
	{
		std::cerr << "live submit replacement pending: replacement reused old timeline ID " << oldTimelineId << '\n';
		return false;
	}
	if (!checkSnapshot(oldTimelineId, "running", false, true, true, newTimelineId, "live submit replacement pending"))
	{
		return false;
	}
	Json pendingReportRequest = control.Identity();
	pendingReportRequest["op"] = "get_report";
	pendingReportRequest["timelineId"] = newTimelineId;
	pendingReportRequest["cursor"] = 0;
	pendingReportRequest["limit"] = 1000;
	agentxr::protocol::PipeFrame pendingReportResponse;
	if (!control.Request(pendingReportRequest, pendingReportResponse) || !pendingReportResponse.message.value("ok", false) || !pendingReportResponse.message.contains("result") || !pendingReportResponse.message.at("result").is_object())
	{
		std::cerr << "live submit replacement pending report: expected armed result, got response failure\n";
		return false;
	}
	const Json pendingReport = pendingReportResponse.message.at("result");
	const uint64_t pendingReportId = pendingReport.value("timelineId", 0ull);
	const std::string pendingReportStatus = pendingReport.value("status", std::string{});
	if (pendingReportId != newTimelineId || pendingReportStatus != "armed")
	{
		std::cerr << "live submit replacement pending report: expected id=" << newTimelineId << " status=armed, got id=" << pendingReportId << " status=" << pendingReportStatus << '\n';
		return false;
	}
	if (!client.CheckTimelinePose(oldFrame.predictedDisplayTime, newHeadX + 0.5f, newGripX + 0.5f, "live submit retained old pose"))
	{
		return false;
	}

	XrFrameState newFrame{XR_TYPE_FRAME_STATE};
	if (!client.WaitFrameOnly(newFrame, "live submit replacement wait"))
	{
		return false;
	}
	if (newFrame.predictedDisplayTime <= oldFrame.predictedDisplayTime)
	{
		std::cerr << "live submit replacement wait: predicted display time did not increase old=" << oldFrame.predictedDisplayTime << " new=" << newFrame.predictedDisplayTime << '\n';
		return false;
	}
	if (!checkSnapshot(newTimelineId, "running", true, true, false, 0, "live submit replacement active"))
	{
		return false;
	}
	if (!client.BeginFrameOnly("live submit replacement begin"))
	{
		return false;
	}
	if (!client.CheckTimelinePose(newFrame.predictedDisplayTime, newHeadX, newGripX, "live submit replacement pose"))
	{
		return false;
	}
	if (!checkSnapshot(newTimelineId, "running", false, true, false, 0, "live submit replacement begun"))
	{
		return false;
	}
	if (!client.EndFrameOnly(oldFrame.predictedDisplayTime, "live submit old end"))
	{
		return false;
	}
	if (!client.EndFrameOnly(newFrame.predictedDisplayTime, "live submit replacement end"))
	{
		return false;
	}

	Json oldReport;
	if (!control.Report(oldTimelineId, oldReport))
	{
		std::cerr << "live submit old report: request failed\n";
		return false;
	}
	const uint64_t oldReportId = oldReport.value("timelineId", 0ull);
	const std::string oldReportStatus = oldReport.value("status", std::string{});
	if (oldReportId != oldTimelineId || oldReportStatus == "report_expired")
	{
		std::cerr << "live submit old report: expected id=" << oldTimelineId << " retained, got id=" << oldReportId << " status=" << oldReportStatus << '\n';
		return false;
	}
	Json newReport;
	if (!control.Report(newTimelineId, newReport))
	{
		std::cerr << "live submit replacement report: request failed\n";
		return false;
	}
	const uint64_t newReportId = newReport.value("timelineId", 0ull);
	const std::string newReportStatus = newReport.value("status", std::string{});
	if (newReportId != newTimelineId || newReportStatus != "running")
	{
		std::cerr << "live submit replacement report: expected id=" << newTimelineId << " status=running, got id=" << newReportId << " status=" << newReportStatus << '\n';
		return false;
	}
	activeTimelineId = newTimelineId;
	return true;
}

struct CaptureResult
{
	Json metadata;
	std::vector<uint8_t> png;
};

uint32_t ReadBigEndian(const uint8_t* bytes)
{
	return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) | (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

bool Capture(Control& control, uint64_t afterFrameId, CaptureResult& capture)
{
	Json request = control.Identity();
	request["op"] = "capture";
	request["afterFrameId"] = afterFrameId;
	agentxr::protocol::PipeFrame response;
	if (!control.Request(request, response) || !response.message.value("ok", false) || !response.message.contains("result") || response.binary.size() < 24)
	{
		return false;
	}
	capture.metadata = response.message.at("result");
	capture.png = std::move(response.binary);
	if (capture.png[0] != 0x89 || capture.png[1] != 'P' || capture.png[2] != 'N' || capture.png[3] != 'G' || capture.png[12] != 'I' || capture.png[13] != 'H' || capture.png[14] != 'D' || capture.png[15] != 'R')
	{
		return false;
	}
	return capture.metadata.value("binaryLength", 0ull) == capture.png.size() && capture.metadata.value("frameId", 0ull) > afterFrameId && capture.metadata.value("width", 0u) == 2048u && capture.metadata.value("height", 0u) == 1024u && ReadBigEndian(capture.png.data() + 16) == 2048u && ReadBigEndian(capture.png.data() + 20) == 1024u;
}

bool CheckHresult(HRESULT result, std::string_view operation)
{
	if (SUCCEEDED(result))
	{
		return true;
	}
	std::cerr << operation << " failed HRESULT=0x" << std::hex << static_cast<uint32_t>(result) << std::dec << '\n';
	return false;
}

struct ComInitialization
{
	HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool uninitialize = SUCCEEDED(result);

	~ComInitialization()
	{
		if (uninitialize)
		{
			CoUninitialize();
		}
	}

	bool IsReady() const
	{
		if (result == RPC_E_CHANGED_MODE)
		{
			return true;
		}
		return CheckHresult(result, "CoInitializeEx");
	}
};

bool DecodeCapturePng(CaptureResult& capture, std::vector<uint8_t>& rgba)
{
	constexpr UINT expectedWidth = 2048;
	constexpr UINT expectedHeight = 1024;
	if (capture.png.empty() || capture.png.size() > std::numeric_limits<DWORD>::max())
	{
		std::cerr << "WIC PNG stream has invalid byte length " << capture.png.size() << '\n';
		return false;
	}

	ComInitialization com;
	if (!com.IsReady())
	{
		return false;
	}

	ComPtr<IWICImagingFactory> factory;
	if (!CheckHresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC imaging factory creation"))
	{
		return false;
	}
	ComPtr<IWICStream> stream;
	if (!CheckHresult(factory->CreateStream(&stream), "WIC stream creation") ||
		!CheckHresult(stream->InitializeFromMemory(capture.png.data(), static_cast<DWORD>(capture.png.size())), "WIC stream initialization"))
	{
		return false;
	}
	ComPtr<IWICBitmapDecoder> decoder;
	if (!CheckHresult(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "WIC PNG decoder creation"))
	{
		return false;
	}
	ComPtr<IWICBitmapFrameDecode> frame;
	if (!CheckHresult(decoder->GetFrame(0, &frame), "WIC PNG frame retrieval"))
	{
		return false;
	}
	UINT width = 0;
	UINT height = 0;
	if (!CheckHresult(frame->GetSize(&width, &height), "WIC PNG frame dimensions"))
	{
		return false;
	}
	if (width != expectedWidth || height != expectedHeight)
	{
		std::cerr << "WIC PNG dimensions were " << width << 'x' << height << ", expected " << expectedWidth << 'x' << expectedHeight << '\n';
		return false;
	}

	ComPtr<IWICFormatConverter> converter;
	if (!CheckHresult(factory->CreateFormatConverter(&converter), "WIC format converter creation") ||
		!CheckHresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "WIC RGBA32 conversion initialization"))
	{
		return false;
	}
	UINT convertedWidth = 0;
	UINT convertedHeight = 0;
	if (!CheckHresult(converter->GetSize(&convertedWidth, &convertedHeight), "WIC converted image dimensions"))
	{
		return false;
	}
	if (convertedWidth != expectedWidth || convertedHeight != expectedHeight)
	{
		std::cerr << "WIC converted dimensions were " << convertedWidth << 'x' << convertedHeight << ", expected " << expectedWidth << 'x' << expectedHeight << '\n';
		return false;
	}

	const UINT stride = expectedWidth * 4;
	const UINT bufferSize = stride * expectedHeight;
	rgba.resize(bufferSize);
	return CheckHresult(converter->CopyPixels(nullptr, stride, bufferSize, rgba.data()), "WIC RGBA32 pixel copy");
}

bool CheckCapturePixel(const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y, const std::array<uint8_t, 3>& expected, std::string_view phase)
{
	constexpr uint32_t imageWidth = 2048;
	constexpr uint32_t imageHeight = 1024;
	if (rgba.size() != static_cast<size_t>(imageWidth) * imageHeight * 4 || x >= imageWidth || y >= imageHeight)
	{
		std::cerr << phase << " has invalid RGBA dimensions or sample coordinate: bytes=" << rgba.size() << " x=" << x << " y=" << y << '\n';
		return false;
	}

	const size_t offset = (static_cast<size_t>(y) * imageWidth + x) * 4;
	const std::array<uint8_t, 3> actual = {rgba[offset], rgba[offset + 1], rgba[offset + 2]};
	bool matches = true;
	for (size_t channel = 0; channel < actual.size(); ++channel)
	{
		matches = matches && std::abs(static_cast<int>(actual[channel]) - static_cast<int>(expected[channel])) <= 2;
	}
	if (!matches)
	{
		std::cerr << phase << " pixel (" << x << ',' << y << ") was [" << static_cast<unsigned>(actual[0]) << ',' << static_cast<unsigned>(actual[1]) << ',' << static_cast<unsigned>(actual[2])
			<< "], expected [" << static_cast<unsigned>(expected[0]) << ',' << static_cast<unsigned>(expected[1]) << ',' << static_cast<unsigned>(expected[2]) << "] tolerance=2\n";
	}
	return matches;
}

bool SameRgbaPixels(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual, std::string_view phase)
{
	if (expected.size() != actual.size())
	{
		std::cerr << phase << " RGBA byte lengths differ: actual=" << actual.size() << " expected=" << expected.size() << '\n';
		return false;
	}
	const auto mismatch = std::mismatch(expected.cbegin(), expected.cend(), actual.cbegin(), actual.cend());
	if (mismatch.first == expected.cend())
	{
		return true;
	}

	const size_t byteOffset = static_cast<size_t>(mismatch.first - expected.cbegin());
	const size_t pixelOffset = byteOffset / 4;
	std::cerr << phase << " first pixel mismatch at (" << pixelOffset % 2048 << ',' << pixelOffset / 2048 << ") channel=" << byteOffset % 4
		<< " actual=" << static_cast<unsigned>(actual[byteOffset]) << " expected=" << static_cast<unsigned>(expected[byteOffset]) << '\n';
	return false;
}

bool CaptureFrameMetadataMatches(const Json& actual, const Json& expected, std::string_view phase)
{
	static constexpr std::array<const char*, 12> fields = {
		"timelineId",
		"frameId",
		"displayTime",
		"layerCount",
		"ended",
		"composed",
		"composeResult",
		"presentAttempted",
		"presented",
		"presentResult",
		"presentOccluded",
		"presentStillDrawing"};
	for (const char* field : fields)
	{
		if (!actual.contains(field) || !expected.contains(field) || actual.at(field) != expected.at(field))
		{
			const std::string actualValue = actual.contains(field) ? actual.at(field).dump() : "<missing>";
			const std::string expectedValue = expected.contains(field) ? expected.at(field).dump() : "<missing>";
			std::cerr << phase << " frame metadata " << field << " was " << actualValue << ", expected " << expectedValue << '\n';
			return false;
		}
	}
	return true;
}

bool ValidateRenderedCaptureMetadata(const Json& metadata, std::string_view phase)
{
	const auto fieldText = [&](const char* field)
	{
		return metadata.contains(field) ? metadata.at(field).dump() : std::string("<missing>");
	};
	const auto readBoolean = [&](const char* field, bool& value)
	{
		if (!metadata.contains(field) || !metadata.at(field).is_boolean())
		{
			return false;
		}
		value = metadata.at(field).get<bool>();
		return true;
	};

	const bool composeResultValid = metadata.contains("composeResult") && metadata.at("composeResult").is_number_integer();
	const bool composeSucceeded = composeResultValid && metadata.at("composeResult") == static_cast<int32_t>(XR_SUCCESS);
	const bool presentResultValid = metadata.contains("presentResult") && metadata.at("presentResult").is_number_integer();
	bool presentAttempted = false;
	bool presented = false;
	bool presentOccluded = false;
	bool presentStillDrawing = false;
	const bool presentBooleansValid =
		readBoolean("presentAttempted", presentAttempted) &&
		readBoolean("presented", presented) &&
		readBoolean("presentOccluded", presentOccluded) &&
		readBoolean("presentStillDrawing", presentStillDrawing);
	if (!composeSucceeded || !presentResultValid || !presentBooleansValid || !presentAttempted)
	{
		std::cerr << phase << " metadata invalid: composeResult=" << fieldText("composeResult")
			<< " presentAttempted=" << fieldText("presentAttempted")
			<< " presentResult=" << fieldText("presentResult")
			<< " presented=" << fieldText("presented")
			<< " presentOccluded=" << fieldText("presentOccluded")
			<< " presentStillDrawing=" << fieldText("presentStillDrawing")
			<< " expected composeResult=XR_SUCCESS and presentAttempted=true\n";
		return false;
	}

	bool expectedPresented = false;
	bool expectedOccluded = false;
	bool expectedStillDrawing = false;
	const Json& presentResult = metadata.at("presentResult");
	if (presentResult == static_cast<int32_t>(S_OK))
	{
		expectedPresented = true;
	}
	else if (presentResult == static_cast<int32_t>(DXGI_STATUS_OCCLUDED))
	{
		expectedOccluded = true;
	}
	else if (presentResult == static_cast<int32_t>(DXGI_ERROR_WAS_STILL_DRAWING))
	{
		expectedStillDrawing = true;
	}
	else
	{
		std::cerr << phase << " rejected presentResult=" << fieldText("presentResult")
			<< "; expected signed HRESULT S_OK=" << static_cast<int32_t>(S_OK)
			<< ", DXGI_STATUS_OCCLUDED=" << static_cast<int32_t>(DXGI_STATUS_OCCLUDED)
			<< ", or DXGI_ERROR_WAS_STILL_DRAWING=" << static_cast<int32_t>(DXGI_ERROR_WAS_STILL_DRAWING) << '\n';
		return false;
	}

	if (presented != expectedPresented || presentOccluded != expectedOccluded || presentStillDrawing != expectedStillDrawing)
	{
		std::cerr << phase << " presentation flags mismatch for presentResult=" << fieldText("presentResult")
			<< ": presented=" << fieldText("presented") << " expected=" << (expectedPresented ? "true" : "false")
			<< " presentOccluded=" << fieldText("presentOccluded") << " expected=" << (expectedOccluded ? "true" : "false")
			<< " presentStillDrawing=" << fieldText("presentStillDrawing") << " expected=" << (expectedStillDrawing ? "true" : "false") << '\n';
		return false;
	}
	return true;
}

std::filesystem::path ResolveExample(const std::filesystem::path& executable, std::string_view name)
{
	const std::wstring wideName = agentxr::protocol::WideFromUtf8(name);
	const std::filesystem::path fileName = wideName + L".json";
	const std::array<std::filesystem::path, 4> candidates = {std::filesystem::current_path() / L"examples" / fileName, std::filesystem::current_path() / L"agent-xr" / L"examples" / fileName, executable.parent_path() / L"examples" / fileName, executable.parent_path().parent_path() / L"examples" / fileName};
	for (const auto& candidate : candidates)
	{
		if (std::filesystem::exists(candidate))
		{
			return candidate;
		}
	}
	return {};
}

bool CheckNeutralInputs(const Json& snapshot, std::string_view operation)
{
	const Json tracking = snapshot.value("tracking", Json::object());
	const auto isNeutralController = [](const Json& controller)
	{
		if (!controller.is_object() || !controller.value("active", false))
		{
			return false;
		}
		const Json buttons = controller.value("buttons", Json::object());
		const Json touches = controller.value("touches", Json::object());
		if (!buttons.is_object() || !touches.is_object())
		{
			return false;
		}
		for (const auto& entry : buttons.items())
		{
			if (!entry.value().is_boolean() || entry.value().get<bool>())
			{
				return false;
			}
		}
		for (const auto& entry : touches.items())
		{
			if (!entry.value().is_boolean() || entry.value().get<bool>())
			{
				return false;
			}
		}
		const Json thumbstick = controller.value("thumbstick", Json::array());
		if (!thumbstick.is_array() || thumbstick.size() != 2 || !thumbstick[0].is_number() || !thumbstick[1].is_number() || std::fabs(thumbstick[0].get<double>()) > 1.0e-6 || std::fabs(thumbstick[1].get<double>()) > 1.0e-6)
		{
			return false;
		}
		return controller.contains("trigger") && controller.at("trigger").is_number() && std::fabs(controller.at("trigger").get<double>()) <= 1.0e-6 && controller.contains("squeeze") && controller.at("squeeze").is_number() && std::fabs(controller.at("squeeze").get<double>()) <= 1.0e-6;
	};
	if (!tracking.is_object() || !isNeutralController(tracking.value("left", Json::object())) || !isNeutralController(tracking.value("right", Json::object())))
	{
		std::cerr << operation << ": final tracking inputs were not neutral\n";
		return false;
	}
	return true;
}

bool RequireSameReport(const Json& expected, const Json& actual, std::string_view operation)
{
	if (expected == actual)
	{
		return true;
	}

	bool foundDifference = false;
	const auto describe = [](const Json* value)
	{
		if (value == nullptr)
		{
			return std::string("<missing>");
		}
		if (value->is_array())
		{
			return std::string("count=") + std::to_string(value->size());
		}
		return value->dump();
	};
	const auto reportDifference = [&](const std::string& field, const Json* expectedValue, const Json* actualValue)
	{
		foundDifference = true;
		std::cerr << operation << ": report field=" << field
			<< " expected=" << describe(expectedValue)
			<< " actual=" << describe(actualValue);
		if (expectedValue != nullptr && actualValue != nullptr && expectedValue->is_array() && actualValue->is_array())
		{
			const size_t sharedCount = std::min(expectedValue->size(), actualValue->size());
			for (size_t index = 0; index < sharedCount; ++index)
			{
				if ((*expectedValue)[index] != (*actualValue)[index])
				{
					std::cerr << " firstDifferingIndex=" << index
						<< " expectedEntry=" << (*expectedValue)[index].dump()
						<< " actualEntry=" << (*actualValue)[index].dump();
					break;
				}
			}
			if (expectedValue->size() < actualValue->size())
			{
				std::cerr << " firstAddedIndex=" << sharedCount << " actualEntry=" << (*actualValue)[sharedCount].dump();
			}
			else if (actualValue->size() < expectedValue->size())
			{
				std::cerr << " firstRemovedIndex=" << sharedCount << " expectedEntry=" << (*expectedValue)[sharedCount].dump();
			}
		}
		std::cerr << '\n';
	};

	if (expected.is_object() && actual.is_object())
	{
		for (const auto& item : expected.items())
		{
			const std::string& field = item.key();
			const Json& expectedValue = item.value();
			if (!actual.contains(field) || actual.at(field) != expectedValue)
			{
				reportDifference(field, &expectedValue, actual.contains(field) ? &actual.at(field) : nullptr);
			}
		}
		for (const auto& item : actual.items())
		{
			const std::string& field = item.key();
			if (!expected.contains(field))
			{
				reportDifference(field, nullptr, &item.value());
			}
		}
	}
	else
	{
		reportDifference("root", &expected, &actual);
	}
	if (!foundDifference)
	{
		std::cerr << operation << ": report category=root differed\n";
	}
	return false;
}


int RunScenario(const std::wstring& runtimeManifest, const std::filesystem::path& executable, std::string_view scenarioName)
{
	OpenXR client;
	if (!client.Load(runtimeManifest))
	{
		std::cerr << "scenario[" << scenarioName << "]: Load failed\n";
		return 1;
	}
	if (!client.InitializeGraphics())
	{
		std::cerr << "scenario[" << scenarioName << "]: InitializeGraphics failed\n";
		return 1;
	}
	if (!client.Begin())
	{
		std::cerr << "scenario[" << scenarioName << "]: Begin failed\n";
		return 1;
	}
	RuntimeEndpoint endpoint;
	if (!FindEndpoint(endpoint))
	{
		std::cerr << "scenario[" << scenarioName << "]: FindEndpoint failed\n";
		return 1;
	}
	Control control;
	if (!control.Connect(endpoint))
	{
		std::cerr << "scenario[" << scenarioName << "]: Control.Connect failed\n";
		return 1;
	}
	if (!client.InvalidCallOrder())
	{
		std::cerr << "scenario[" << scenarioName << "]: InvalidCallOrder failed\n";
		return 1;
	}
	if (scenarioName == "pipelined-swapchain")
	{
		const bool swapchainPassed = client.PipelinedSwapchain();
		if (!client.End())
		{
			std::cerr << "scenario[" << scenarioName << "]: End failed\n";
			return 1;
		}
		return swapchainPassed ? 0 : 1;
	}

	if (scenarioName == "pipelined-frame")
	{
		const bool framePassed = client.PipelinedFrame();
		if (!client.End())
		{
			std::cerr << "scenario[" << scenarioName << "]: End failed\n";
			return 1;
		}
		return framePassed ? 0 : 1;
	}

	if (scenarioName == "invalid-input")
	{
		Json invalid;
		const std::filesystem::path validPath = ResolveExample(executable, "neutral");
		if (validPath.empty() || !ReadJsonFile(validPath, invalid))
		{
			return 1;
		}
		invalid["unknownField"] = true;
		uint64_t ignoredTimelineId = 0;
		if (!SubmitTimeline(control, invalid, ignoredTimelineId, false))
		{
			return 1;
		}

		if (!client.InvalidHandle())
		{
			return 1;
		}
		if (!client.End())
		{
			std::cerr << "scenario[" << scenarioName << "]: End failed\n";
			return 1;
		}
		return 0;
	}
	if (scenarioName == "capture-wait")
	{
		uint32_t frames = 0;
		if (!client.Frame(true, frames))
		{
			std::cerr << "capture-wait: rendered warm-up Frame failed\n";
			return 1;
		}

		CaptureResult baseline;
		if (!Capture(control, 0, baseline) || !ValidateRenderedCaptureMetadata(baseline.metadata, "capture-wait: baseline capture"))
		{
			std::cerr << "capture-wait: baseline Capture failed\n";
			return 1;
		}
		const Json& baselineMetadata = baseline.metadata;
		const uint64_t baselineFrameId = baselineMetadata.value("frameId", 0ull);
		const bool baselineGenerationMatches = baselineMetadata.contains("sessionGeneration") &&
			baselineMetadata.at("sessionGeneration").is_number_integer() &&
			baselineMetadata.at("sessionGeneration") == endpoint.sessionGeneration;
		if (baselineFrameId == 0 || !baselineMetadata.value("ended", false) || !baselineMetadata.value("composed", false) || !baselineGenerationMatches)
		{
			std::cerr << "capture-wait: baseline metadata mismatch frameId=" << baselineFrameId
				<< " ended=" << (baselineMetadata.value("ended", false) ? "true" : "false")
				<< " composed=" << (baselineMetadata.value("composed", false) ? "true" : "false")
				<< " sessionGeneration=" << baselineMetadata.value("sessionGeneration", 0ull)
				<< " expectedSessionGeneration=" << endpoint.sessionGeneration << '\n';
			return 1;
		}

		struct CaptureAttempt
		{
			bool succeeded = false;
			CaptureResult capture;
		};
		std::promise<void> captureStartedPromise;
		std::future<void> captureStartedFuture = captureStartedPromise.get_future();
		std::future<CaptureAttempt> captureFuture;
		try
		{
			captureFuture = std::async(std::launch::async, [&control, baselineFrameId, &captureStartedPromise]()
			{
				captureStartedPromise.set_value();
				CaptureAttempt attempt;
				attempt.succeeded = Capture(control, baselineFrameId, attempt.capture);
				return attempt;
			});
		}
		catch (const std::exception& error)
		{
			std::cerr << "capture-wait: starting asynchronous Capture failed: " << error.what() << '\n';
			return 1;
		}

		bool passed = true;
		const bool captureTaskStarted = captureStartedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		if (!captureTaskStarted)
		{
			std::cerr << "capture-wait: asynchronous Capture task did not start within 2 seconds\n";
			passed = false;
		}
		const bool remainedPending = captureFuture.wait_for(std::chrono::milliseconds(60)) == std::future_status::timeout;
		if (!remainedPending)
		{
			std::cerr << "capture-wait: Capture completed before next rendered Frame; expected pending for 60ms\n";
			passed = false;
		}

		const bool renderedFrameSucceeded = client.Frame(true, frames);
		if (!renderedFrameSucceeded)
		{
			std::cerr << "capture-wait: rendered Frame after pending Capture failed\n";
			passed = false;
		}

		const bool captureReadyWithinGuard = captureFuture.wait_for(std::chrono::seconds(8)) == std::future_status::ready;
		if (!captureReadyWithinGuard)
		{
			std::cerr << "capture-wait: asynchronous Capture exceeded 8-second completion guard\n";
			passed = false;
			captureFuture.wait();
		}
		CaptureAttempt attempt;
		try
		{
			attempt = captureFuture.get();
		}
		catch (const std::exception& error)
		{
			std::cerr << "capture-wait: asynchronous Capture threw: " << error.what() << '\n';
			return 1;
		}
		catch (...)
		{
			std::cerr << "capture-wait: asynchronous Capture threw an unknown exception\n";
			return 1;
		}

		if (!attempt.succeeded)
		{
			std::cerr << "capture-wait: Capture did not return a successful PNG result after frameId=" << baselineFrameId << '\n';
			passed = false;
		}
		if (attempt.succeeded)
		{
			const Json& metadata = attempt.capture.metadata;
			const uint64_t frameId = metadata.value("frameId", 0ull);
			const bool generationMatches = metadata.contains("sessionGeneration") &&
				metadata.at("sessionGeneration").is_number_integer() &&
				metadata.at("sessionGeneration") == endpoint.sessionGeneration;
			if (!ValidateRenderedCaptureMetadata(metadata, "capture-wait: fresh capture") ||
				frameId <= baselineFrameId ||
				!metadata.value("ended", false) ||
				!metadata.value("composed", false) ||
				!generationMatches)
			{
				std::cerr << "capture-wait: fresh metadata mismatch frameId=" << frameId
					<< " baselineFrameId=" << baselineFrameId
					<< " ended=" << (metadata.value("ended", false) ? "true" : "false")
					<< " composed=" << (metadata.value("composed", false) ? "true" : "false")
					<< " sessionGeneration=" << metadata.value("sessionGeneration", 0ull)
					<< " expectedSessionGeneration=" << endpoint.sessionGeneration << '\n';
				passed = false;
			}

			std::vector<uint8_t> rgba;
			if (!DecodeCapturePng(attempt.capture, rgba) ||
				!CheckCapturePixel(rgba, 512, 512, {255, 0, 0}, "capture-wait: left eye") ||
				!CheckCapturePixel(rgba, 1536, 512, {0, 0, 255}, "capture-wait: right eye"))
			{
				passed = false;
			}
		}
		uint64_t latestFrameId = attempt.succeeded ? attempt.capture.metadata.value("frameId", 0ull) : 0;
		if (latestFrameId == 0)
		{
			CaptureResult latest;
			if (!Capture(control, 0, latest) || !ValidateRenderedCaptureMetadata(latest.metadata, "capture-wait: latest capture"))
			{
				std::cerr << "capture-wait: could not identify latest completed frame after asynchronous Capture\n";
				passed = false;
			}
			else
			{
				latestFrameId = latest.metadata.value("frameId", 0ull);
				if (latestFrameId == 0)
				{
					std::cerr << "capture-wait: latest completed Capture returned frameId=0\n";
					passed = false;
				}
			}
		}
		if (latestFrameId != 0)
		{
			Json timeoutRequest = control.Identity();
			timeoutRequest["op"] = "capture";
			timeoutRequest["afterFrameId"] = latestFrameId;
			agentxr::protocol::PipeFrame timeoutResponse;
			const auto timeoutStarted = std::chrono::steady_clock::now();
			const bool timeoutResponseReceived = control.Request(timeoutRequest, timeoutResponse);
			const auto timeoutElapsed = std::chrono::steady_clock::now() - timeoutStarted;
			constexpr auto timeoutReturnBound = std::chrono::seconds(8);
			const bool timeoutReturnedInBound = timeoutElapsed <= timeoutReturnBound;
			const Json timeoutError = timeoutResponseReceived && timeoutResponse.message.is_object()
				? timeoutResponse.message.value("error", Json::object())
				: Json::object();
			const bool gotTimeoutError = timeoutResponseReceived &&
				timeoutResponse.message.is_object() &&
				!timeoutResponse.message.value("ok", true) &&
				timeoutError.is_object() &&
				timeoutError.value("code", std::string{}) == "timeout";
			if (!gotTimeoutError || !timeoutReturnedInBound)
			{
				std::cerr << "capture-wait: no-new-frame Capture after frameId=" << latestFrameId
					<< " did not return bounded timeout: received=" << (timeoutResponseReceived ? "true" : "false")
					<< " error=" << timeoutError.dump()
					<< " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(timeoutElapsed).count()
					<< " boundMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(timeoutReturnBound).count() << '\n';
				passed = false;
			}
		}
		if (!passed)
		{
			return 1;
		}
		if (!client.End())
		{
			std::cerr << "capture-wait: End failed\n";
			return 1;
		}
		return 0;
	}
	if (scenarioName == "stereo-composition")
	{
		uint32_t frames = 0;
		for (uint32_t index = 0; index < 10; ++index)
		{
			if (!client.Frame(true, frames, nullptr, nullptr, 0.0f))
			{
				std::cerr << "stereo-composition: initial Frame failed at index " << index << '\n';
				return 1;
			}
		}

		CaptureResult pattern;
		if (!Capture(control, 0, pattern))
		{
			std::cerr << "stereo-composition: initial Capture failed for afterFrameId=0\n";
			return 1;
		}
		const uint64_t patternFrame = pattern.metadata.value("frameId", 0ull);
		const int64_t patternDisplayTime = pattern.metadata.value("displayTime", int64_t{0});
		const uint32_t patternLayerCount = pattern.metadata.value("layerCount", 0u);
		const bool patternEnded = pattern.metadata.value("ended", false);
		const bool patternComposed = pattern.metadata.value("composed", false);
		if (patternLayerCount != 1u || !patternEnded || !patternComposed)
		{
			std::cerr << "stereo-composition: initial frame metadata mismatch: frameId=" << patternFrame
				<< " displayTime=" << patternDisplayTime
				<< " layerCount=" << patternLayerCount
				<< " ended=" << (patternEnded ? "true" : "false")
				<< " composed=" << (patternComposed ? "true" : "false")
				<< "; expected layerCount=1 ended=true composed=true\n";
			return 1;
		}
		if (!ValidateRenderedCaptureMetadata(pattern.metadata, "stereo-composition: initial capture"))
		{
			return 1;
		}

		std::vector<uint8_t> patternRgba;
		if (!DecodeCapturePng(pattern, patternRgba))
		{
			std::cerr << "stereo-composition: initial PNG decode failed\n";
			return 1;
		}
		constexpr uint32_t centerY = 512;
		if (!CheckCapturePixel(patternRgba, 512, centerY, {255, 0, 0}, "stereo-composition: left eye") ||
			!CheckCapturePixel(patternRgba, 1536, centerY, {0, 0, 255}, "stereo-composition: right eye"))
		{
			return 1;
		}

		XrFrameState pendingFrame{XR_TYPE_FRAME_STATE};
		if (!client.WaitFrameOnly(pendingFrame, "stereo-composition newer unended wait"))
		{
			std::cerr << "stereo-composition: waiting newer unended frame failed\n";
			return 1;
		}
		if (pendingFrame.predictedDisplayTime <= patternDisplayTime)
		{
			std::cerr << "stereo-composition: unended frame display time was " << pendingFrame.predictedDisplayTime
				<< ", not newer than captured display time " << patternDisplayTime << '\n';
			return 1;
		}
		CaptureResult retained;
		if (!Capture(control, 0, retained))
		{
			std::cerr << "stereo-composition: Capture with unended frame failed for afterFrameId=0\n";
			return 1;
		}
		if (!CaptureFrameMetadataMatches(retained.metadata, pattern.metadata, "stereo-composition: capture with unended frame"))
		{
			return 1;
		}
		std::vector<uint8_t> retainedRgba;
		if (!DecodeCapturePng(retained, retainedRgba))
		{
			std::cerr << "stereo-composition: retained PNG decode failed\n";
			return 1;
		}
		if (!SameRgbaPixels(patternRgba, retainedRgba, "stereo-composition: capture with unended frame"))
		{
			return 1;
		}

		if (!client.BeginFrameOnly("stereo-composition drain pending begin") ||
			!client.EndFrameOnly(pendingFrame.predictedDisplayTime, "stereo-composition drain pending end"))
		{
			std::cerr << "stereo-composition: draining unended frame at display time " << pendingFrame.predictedDisplayTime << " failed\n";
			return 1;
		}
		if (!client.Frame(false, frames))
		{
			std::cerr << "stereo-composition: zero-layer Frame failed\n";
			return 1;
		}
		CaptureResult black;
		if (!Capture(control, patternFrame, black))
		{
			std::cerr << "stereo-composition: zero-layer Capture failed after frameId=" << patternFrame << '\n';
			return 1;
		}
		const uint64_t blackFrame = black.metadata.value("frameId", 0ull);
		const uint32_t blackLayerCount = black.metadata.value("layerCount", 1u);
		if (blackLayerCount != 0u || blackFrame <= patternFrame)
		{
			std::cerr << "stereo-composition: zero-layer metadata mismatch: frameId=" << blackFrame
				<< " layerCount=" << blackLayerCount << "; expected newer frameId and layerCount=0\n";
			return 1;
		}
		std::vector<uint8_t> blackRgba;
		if (!DecodeCapturePng(black, blackRgba))
		{
			std::cerr << "stereo-composition: zero-layer PNG decode failed\n";
			return 1;
		}
		if (!CheckCapturePixel(blackRgba, 512, centerY, {0, 0, 0}, "stereo-composition: zero-layer left eye") ||
			!CheckCapturePixel(blackRgba, 1536, centerY, {0, 0, 0}, "stereo-composition: zero-layer right eye"))
		{
			return 1;
		}
		if (!client.End())
		{
			std::cerr << "stereo-composition: End failed\n";
			return 1;
		}
		return 0;
	}
	if (scenarioName == "report-lifetime")
	{
		const std::filesystem::path examplePath = ResolveExample(executable, "neutral");
		Json timeline;
		if (examplePath.empty() || !ReadJsonFile(examplePath, timeline))
		{
			std::cerr << "report-lifetime: neutral example read failed\n";
			return 1;
		}
		timeline["durationSeconds"] = 0.25;
		timeline["sampleRateHz"] = 90;
		timeline["interpolation"] = "step";
		timeline["keyframes"] = Json::array({Json{{"timeSeconds", 0.25}, {"state", Json::object()}}});

		uint32_t frames = 0;
		if (!client.Frame(true, frames))
		{
			std::cerr << "report-lifetime: rendered warm-up Frame failed\n";
			return 1;
		}
		uint64_t timelineId = 0;
		if (!SubmitTimeline(control, timeline, timelineId, true))
		{
			std::cerr << "report-lifetime: SubmitTimeline failed\n";
			return 1;
		}

		const auto reportDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		Json report;
		bool reportCompleted = false;
		bool deliberateStall = false;
		uint32_t runFrames = 0;
		while (std::chrono::steady_clock::now() < reportDeadline)
		{
			if (!client.Frame(true, frames))
			{
				std::cerr << "report-lifetime: rendered run Frame failed at index " << runFrames << " timelineId=" << timelineId << '\n';
				return 1;
			}
			bool pressed = false;
			if (!client.Sync(pressed))
			{
				std::cerr << "report-lifetime: run Sync failed at index " << runFrames << " timelineId=" << timelineId << '\n';
				return 1;
			}
			++runFrames;
			if (!deliberateStall)
			{
				Sleep(60);
				deliberateStall = true;
			}
			if (std::chrono::steady_clock::now() >= reportDeadline)
			{
				break;
			}
			if (!control.Report(timelineId, report))
			{
				std::cerr << "report-lifetime: Report failed before completion for timelineId=" << timelineId << " runFrames=" << runFrames << '\n';
				return 1;
			}
			if (std::chrono::steady_clock::now() >= reportDeadline)
			{
				break;
			}
			const std::string status = report.is_object() ? report.value("status", std::string{}) : std::string{};
			if (status == "completed")
			{
				reportCompleted = true;
				break;
			}
			if (status != "running")
			{
				std::cerr << "report-lifetime: report status=" << status << " expected=running or completed timelineId=" << timelineId << '\n';
				return 1;
			}
		}
		if (!reportCompleted)
		{
			const std::string status = report.is_object() ? report.value("status", std::string("<unavailable>")) : std::string("<unavailable>");
			std::cerr << "report-lifetime: completion exceeded 3-second monotonic bound timelineId=" << timelineId
				<< " runFrames=" << runFrames << " lastStatus=" << status << '\n';
			return 1;
		}

		const bool totalFramesValid = report.contains("totalFrames") && report.at("totalFrames").is_number_integer();
		const uint64_t totalFrames = totalFramesValid ? report.at("totalFrames").get<uint64_t>() : 0;
		if (!totalFramesValid || totalFrames == 0 || totalFrames >= 1000)
		{
			const std::string actual = report.contains("totalFrames") ? report.at("totalFrames").dump() : "<missing>";
			std::cerr << "report-lifetime: report field=totalFrames actual=" << actual << " expected=nonzero and less than 1000\n";
			return 1;
		}
		if (!report.contains("frames") || !report.at("frames").is_array() || report.at("frames").empty())
		{
			const std::string actual = report.contains("frames") ? report.at("frames").dump() : "<missing>";
			std::cerr << "report-lifetime: report field=frames actual=" << actual << " expected=nonempty array\n";
			return 1;
		}

		uint64_t missedPeriodTotal = 0;
		const Json& reportedFrames = report.at("frames");
		for (size_t index = 0; index < reportedFrames.size(); ++index)
		{
			const Json& frameReport = reportedFrames[index];
			if (!frameReport.is_object() || !frameReport.contains("timelineId") || !frameReport.at("timelineId").is_number_integer())
			{
				const std::string actual = frameReport.is_object() && frameReport.contains("timelineId") ? frameReport.at("timelineId").dump() : "<missing>";
				std::cerr << "report-lifetime: report field=frames[" << index << "].timelineId actual=" << actual << " expected=" << timelineId << '\n';
				return 1;
			}
			const uint64_t frameTimelineId = frameReport.at("timelineId").get<uint64_t>();
			if (frameTimelineId != timelineId)
			{
				std::cerr << "report-lifetime: report field=frames[" << index << "].timelineId actual=" << frameTimelineId << " expected=" << timelineId << '\n';
				return 1;
			}
			if (!frameReport.contains("missedPeriods") || !frameReport.at("missedPeriods").is_number_integer())
			{
				const std::string actual = frameReport.contains("missedPeriods") ? frameReport.at("missedPeriods").dump() : "<missing>";
				std::cerr << "report-lifetime: report field=frames[" << index << "].missedPeriods actual=" << actual << '\n';
				return 1;
			}
			const uint64_t missedPeriods = frameReport.at("missedPeriods").get<uint64_t>();
			if (missedPeriodTotal > std::numeric_limits<uint64_t>::max() - missedPeriods)
			{
				std::cerr << "report-lifetime: report field=frames[].missedPeriods sum overflow at frameIndex=" << index << '\n';
				return 1;
			}
			missedPeriodTotal += missedPeriods;
			if (!frameReport.contains("late") || !frameReport.at("late").is_boolean())
			{
				const std::string actual = frameReport.contains("late") ? frameReport.at("late").dump() : "<missing>";
				std::cerr << "report-lifetime: report field=frames[" << index << "].late actual=" << actual << " expected=boolean\n";
				return 1;
			}
			if (!frameReport.at("late").get<bool>() && missedPeriods != 0)
			{
				const std::string frameId = frameReport.contains("frameId") ? frameReport.at("frameId").dump() : "<missing>";
				std::cerr << "report-lifetime: report field=frames[" << index << "].missedPeriods frameId=" << frameId
					<< " actual=" << missedPeriods << " expected=0 because late=false\n";
				return 1;
			}
		}
		const bool missedFrameCountValid = report.contains("missedFrameCount") && report.at("missedFrameCount").is_number_integer();
		const uint64_t missedFrameCount = missedFrameCountValid ? report.at("missedFrameCount").get<uint64_t>() : 0;
		if (!missedFrameCountValid || missedFrameCount == 0 || missedPeriodTotal != missedFrameCount)
		{
			const std::string actual = report.contains("missedFrameCount") ? report.at("missedFrameCount").dump() : "<missing>";
			std::cerr << "report-lifetime: report field=missedFrameCount actual=" << actual
				<< " framesMissedPeriodsSum=" << missedPeriodTotal << " expected=equal and nonzero\n";
			return 1;
		}

		Json baselineSnapshot;
		if (!control.Snapshot(baselineSnapshot))
		{
			std::cerr << "report-lifetime: baseline Snapshot failed after completed report timelineId=" << timelineId << '\n';
			return 1;
		}
		const Json baselineFrame = baselineSnapshot.value("frame", Json::object());
		const bool baselineIdsValid = baselineFrame.is_object() &&
			baselineFrame.contains("submitted") && baselineFrame.at("submitted").is_number_integer() &&
			baselineFrame.contains("composed") && baselineFrame.at("composed").is_number_integer();
		if (!baselineIdsValid)
		{
			const std::string submitted = baselineFrame.is_object() && baselineFrame.contains("submitted") ? baselineFrame.at("submitted").dump() : "<missing>";
			const std::string composed = baselineFrame.is_object() && baselineFrame.contains("composed") ? baselineFrame.at("composed").dump() : "<missing>";
			std::cerr << "report-lifetime: snapshot fields submitted/composed unavailable baselineSubmitted=" << submitted << " baselineComposed=" << composed << '\n';
			return 1;
		}
		const uint64_t baselineSubmitted = baselineFrame.at("submitted").get<uint64_t>();
		const uint64_t baselineComposed = baselineFrame.at("composed").get<uint64_t>();

		CaptureResult baselineCapture;
		if (!Capture(control, 0, baselineCapture))
		{
			std::cerr << "report-lifetime: baseline Capture failed after frameId=0 timelineId=" << timelineId << '\n';
			return 1;
		}
		if (!ValidateRenderedCaptureMetadata(baselineCapture.metadata, "report-lifetime: baseline capture"))
		{
			return 1;
		}
		const uint64_t baselineCaptureFrameId = baselineCapture.metadata.value("frameId", 0ull);

		Sleep(60);
		for (uint32_t index = 0; index < 10; ++index)
		{
			if (!client.Frame(true, frames))
			{
				std::cerr << "report-lifetime: post-run rendered Frame failed at index=" << index << " timelineId=" << timelineId << '\n';
				return 1;
			}
			bool pressed = false;
			if (!client.Sync(pressed))
			{
				std::cerr << "report-lifetime: post-run Sync failed at index=" << index << " timelineId=" << timelineId << '\n';
				return 1;
			}
		}

		Json laterReport;
		if (!control.Report(timelineId, laterReport))
		{
			std::cerr << "report-lifetime: later Report failed for timelineId=" << timelineId << '\n';
			return 1;
		}
		if (!RequireSameReport(report, laterReport, "report-lifetime after live frames"))
		{
			return 1;
		}

		Json liveSnapshot;
		if (!control.Snapshot(liveSnapshot))
		{
			std::cerr << "report-lifetime: later Snapshot failed after 10 live frames timelineId=" << timelineId << '\n';
			return 1;
		}
		const Json liveFrame = liveSnapshot.value("frame", Json::object());
		const bool liveIdsValid = liveFrame.is_object() &&
			liveFrame.contains("submitted") && liveFrame.at("submitted").is_number_integer() &&
			liveFrame.contains("composed") && liveFrame.at("composed").is_number_integer();
		if (!liveIdsValid)
		{
			const std::string submitted = liveFrame.is_object() && liveFrame.contains("submitted") ? liveFrame.at("submitted").dump() : "<missing>";
			const std::string composed = liveFrame.is_object() && liveFrame.contains("composed") ? liveFrame.at("composed").dump() : "<missing>";
			std::cerr << "report-lifetime: later snapshot fields submitted/composed unavailable actualSubmitted=" << submitted << " actualComposed=" << composed << '\n';
			return 1;
		}
		const uint64_t liveSubmitted = liveFrame.at("submitted").get<uint64_t>();
		const uint64_t liveComposed = liveFrame.at("composed").get<uint64_t>();
		if (liveSubmitted <= baselineSubmitted || liveComposed <= baselineComposed)
		{
			std::cerr << "report-lifetime: snapshot live frame IDs did not advance baselineSubmitted=" << baselineSubmitted
				<< " actualSubmitted=" << liveSubmitted << " baselineComposed=" << baselineComposed << " actualComposed=" << liveComposed << '\n';
			return 1;
		}

		CaptureResult liveCapture;
		if (!Capture(control, baselineCaptureFrameId, liveCapture))
		{
			std::cerr << "report-lifetime: later Capture failed after frameId=" << baselineCaptureFrameId << '\n';
			return 1;
		}
		if (!ValidateRenderedCaptureMetadata(liveCapture.metadata, "report-lifetime: later capture"))
		{
			return 1;
		}
		const uint64_t liveCaptureFrameId = liveCapture.metadata.value("frameId", 0ull);
		if (liveCaptureFrameId <= baselineCaptureFrameId)
		{
			std::cerr << "report-lifetime: capture frameId did not advance baselineFrameId=" << baselineCaptureFrameId
				<< " actualFrameId=" << liveCaptureFrameId << '\n';
			return 1;
		}
		if (!liveCapture.metadata.contains("timelineId") || !liveCapture.metadata.at("timelineId").is_number_integer() || liveCapture.metadata.at("timelineId") != 0)
		{
			const std::string actual = liveCapture.metadata.contains("timelineId") ? liveCapture.metadata.at("timelineId").dump() : "<missing>";
			std::cerr << "report-lifetime: capture field=timelineId actual=" << actual << " expected=0 liveFrameId=" << liveCaptureFrameId << '\n';
			return 1;
		}
		if (!client.End())
		{
			std::cerr << "report-lifetime: End failed\n";
			return 1;
		}
		return 0;
	}
	const std::string exampleName = scenarioName == "timeline-actions" ? "motion-and-input" : std::string(scenarioName);
	const std::filesystem::path examplePath = ResolveExample(executable, exampleName);
	Json timeline;
	if (examplePath.empty())
	{
		std::cerr << "scenario[" << scenarioName << "]: example file not found for " << exampleName << '\n';
		return 1;
	}
	if (!ReadJsonFile(examplePath, timeline))
	{
		std::cerr << "scenario[" << scenarioName << "]: example file read failed: " << examplePath.string() << '\n';
		return 1;
	}
	uint64_t timelineId = 0;
	if (scenarioName == "timeline-actions")
	{
		if (!RunLiveTimelineRegression(client, control, timeline, timelineId))
		{
			std::cerr << "scenario[" << scenarioName << "]: live SubmitTimeline regression failed\n";
			return 1;
		}
	}
	else if (!SubmitTimeline(control, timeline, timelineId, true))
	{
		std::cerr << "scenario[" << scenarioName << "]: SubmitTimeline failed\n";
		return 1;
	}

	if (scenarioName == "timeline-actions")
	{
		control.Disconnect();
		if (!control.Connect(endpoint))
		{
			std::cerr << "scenario[" << scenarioName << "]: read-only reconnect failed\n";
			return 1;
		}
		Json reconnectSnapshot;
		if (!control.Snapshot(reconnectSnapshot))
		{
			std::cerr << "scenario[" << scenarioName << "]: read-only reconnect snapshot failed\n";
			return 1;
		}
		const Json reconnectTimeline = reconnectSnapshot.value("timeline", Json::object());
		const uint64_t reconnectTimelineId = reconnectTimeline.value("id", 0ull);
		const std::string reconnectStatus = reconnectTimeline.value("status", std::string{});
		if (reconnectTimelineId != timelineId || reconnectStatus != "running" || reconnectTimeline.value("pending", false))
		{
			std::cerr << "scenario[" << scenarioName << "]: disconnect canceled or replaced active timeline; expected id=" << timelineId << " status=running, got id=" << reconnectTimelineId << " status=" << reconnectStatus << '\n';
			return 1;
		}
		Json reconnectReport;
		if (!control.Report(timelineId, reconnectReport) || reconnectReport.value("status", std::string{}) != "running")
		{
			std::cerr << "scenario[" << scenarioName << "]: read-only reconnect report did not remain running\n";
			return 1;
		}
	}
	const double durationSeconds = timeline.value("durationSeconds", 1.0);
	const uint32_t frameTarget = std::max<uint32_t>(10u, static_cast<uint32_t>(std::ceil(durationSeconds * 90.0)) + 20u);
	uint32_t frames = 0;
	bool sawPress = false;
	bool sawRelease = false;
	bool previousPressed = false;
	bool sawUntrackedHead = false;
	bool sawRecoveredHead = false;
	bool sawInactiveGrip = false;
	bool sawActiveGripAfterRecovery = false;
	for (uint32_t index = 0; index < frameTarget; ++index)
	{
		XrViewState viewState{XR_TYPE_VIEW_STATE};
		XrTime displayTime = 0;
		if (!client.Frame(scenarioName != "tracking-recovery", frames, &viewState, &displayTime))
		{
			std::cerr << "scenario[" << scenarioName << "]: Frame failed at index " << index << '\n';
			return 1;
		}
		bool pressed = false;
		bool poseActive = true;
		if (!client.Sync(pressed, &poseActive))
		{
			std::cerr << "scenario[" << scenarioName << "]: Sync failed at index " << index << '\n';
			return 1;
		}
		if (pressed && !previousPressed)
		{
			sawPress = true;
		}
		if (!pressed && previousPressed)
		{
			sawRelease = true;
		}
		previousPressed = pressed;
		if (scenarioName == "tracking-recovery")
		{
			const bool headTracked = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0 && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT) != 0;
			if (!headTracked)
			{
				sawUntrackedHead = true;
			}
			if (sawUntrackedHead && headTracked)
			{
				sawRecoveredHead = true;
			}
			XrSpaceLocation gripLocation{XR_TYPE_SPACE_LOCATION};
			if (!client.LocateRightGrip(displayTime, gripLocation))
			{
				std::cerr << "scenario[" << scenarioName << "]: LocateRightGrip failed at index " << index << '\n';
				return 1;
			}
			const bool gripTracked = (gripLocation.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0 && (gripLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
			if (!poseActive || !gripTracked)
			{
				sawInactiveGrip = true;
			}
			if (sawInactiveGrip && poseActive && gripTracked)
			{
				sawActiveGripAfterRecovery = true;
			}
		}
	}
	Json snapshot;
	Json report;
	if (!control.Snapshot(snapshot))
	{
		std::cerr << "scenario[" << scenarioName << "]: Snapshot failed\n";
		return 1;
	}
	if (!control.Report(timelineId, report))
	{
		std::cerr << "scenario[" << scenarioName << "]: Report failed\n";
		return 1;
	}
	const Json frame = snapshot.is_object() ? snapshot.value("frame", Json::object()) : Json::object();
	const uint64_t submitted = frame.is_object() ? frame.value("submitted", 0ull) : 0ull;
	const uint64_t reportTimelineId = report.is_object() ? report.value("timelineId", 0ull) : 0ull;
	const std::string reportStatus = report.is_object() ? report.value("status", std::string{}) : std::string{};
	std::cerr << "scenario[" << scenarioName << "]: final status=" << reportStatus << " submitted=" << submitted << " timelineId=" << reportTimelineId << " expectedTimelineId=" << timelineId << '\n';
	if (submitted == 0 || reportTimelineId != timelineId || reportStatus != "completed")
	{
		std::cerr << "scenario[" << scenarioName << "]: final predicate failed\n";
		return 1;
	}
	if (scenarioName == "timeline-actions")
	{
		const uint32_t unobservedDigitalTransitions = report.is_object() ? report.value("unobservedDigitalTransitions", 1u) : 1u;
		const bool timelineActionsPassed = sawPress && sawRelease && unobservedDigitalTransitions == 0;
		std::cerr << "timeline-actions verdict: sawPress=" << (sawPress ? 1 : 0) << " sawRelease=" << (sawRelease ? 1 : 0) << " unobservedDigitalTransitions=" << unobservedDigitalTransitions << '\n';
		if (!timelineActionsPassed)
		{
			return 1;
		}
		if (!CheckNeutralInputs(snapshot, "timeline-actions disconnect completion"))
		{
			return 1;
		}
	}

	if (scenarioName == "timeline-actions")
	{
		Json cancelTimeline = timeline;
		cancelTimeline["initial"]["right"]["buttons"]["a"] = true;
		uint64_t cancelTimelineId = 0;
		if (!SubmitTimeline(control, cancelTimeline, cancelTimelineId, true))
		{
			std::cerr << "scenario[" << scenarioName << "]: second-connection submit did not acquire released lease\n";
			return 1;
		}
		uint32_t cancelFrames = frames;
		if (!client.Frame(false, cancelFrames))
		{
			std::cerr << "scenario[" << scenarioName << "]: explicit-cancel setup frame failed\n";
			return 1;
		}
		if (!control.CancelTimeline(cancelTimelineId))
		{
			std::cerr << "scenario[" << scenarioName << "]: second-connection explicit cancel failed\n";
			return 1;
		}
		bool canceledPressed = true;
		if (!client.Frame(false, cancelFrames) || !client.Sync(canceledPressed))
		{
			std::cerr << "scenario[" << scenarioName << "]: canceled-input neutralization frame failed\n";
			return 1;
		}
		if (canceledPressed)
		{
			std::cerr << "scenario[" << scenarioName << "]: explicit cancellation left button input active\n";
			return 1;
		}
		Json canceledReport;
		if (!control.Report(cancelTimelineId, canceledReport) || canceledReport.value("status", std::string{}) != "canceled")
		{
			std::cerr << "scenario[" << scenarioName << "]: explicit cancellation report was not retained as canceled\n";
			return 1;
		}
		uint32_t postCancelFrames = cancelFrames;
		for (uint32_t index = 0; index < 5; ++index)
		{
			bool postCancelPressed = true;
			if (!client.Frame(false, postCancelFrames) || !client.Sync(postCancelPressed))
			{
				std::cerr << "scenario[" << scenarioName << "]: post-cancel Frame+Sync failed at index=" << index << '\n';
				return 1;
			}
			if (postCancelPressed)
			{
				std::cerr << "scenario[" << scenarioName << "]: explicit cancellation input remained active at post-cancel index=" << index << '\n';
				return 1;
			}
		}
		Json postCancelSnapshot;
		if (!control.Snapshot(postCancelSnapshot))
		{
			std::cerr << "scenario[" << scenarioName << "]: post-cancel Snapshot failed\n";
			return 1;
		}
		if (!CheckNeutralInputs(postCancelSnapshot, "timeline-actions explicit cancellation"))
		{
			return 1;
		}
		Json postCancelReport;
		if (!control.Report(cancelTimelineId, postCancelReport))
		{
			std::cerr << "scenario[" << scenarioName << "]: post-cancel Report failed for timelineId=" << cancelTimelineId << '\n';
			return 1;
		}
		if (!RequireSameReport(canceledReport, postCancelReport, "timeline-actions explicit-cancel stability"))
		{
			return 1;
		}
	}
	if (scenarioName == "tracking-recovery")
	{
		const bool trackingRecoveryPassed = sawUntrackedHead && sawRecoveredHead && sawInactiveGrip && sawActiveGripAfterRecovery;
		std::cerr << "tracking-recovery verdict: sawUntrackedHead=" << (sawUntrackedHead ? 1 : 0) << " sawRecoveredHead=" << (sawRecoveredHead ? 1 : 0) << " sawInactiveGrip=" << (sawInactiveGrip ? 1 : 0) << " sawActiveGripAfterRecovery=" << (sawActiveGripAfterRecovery ? 1 : 0) << '\n';
		if (!trackingRecoveryPassed)
		{
			return 1;
		}
	}
	if (!client.End())
	{
		std::cerr << "scenario[" << scenarioName << "]: End failed\n";
		return 1;
	}
	return 0;
}

int RunUnrealActionSetup(const std::wstring& runtimeManifest)
{
	OpenXR client;
	if (!client.Load(runtimeManifest) || !client.InitializeGraphics(false))
	{
		std::cerr << "unreal-action-setup: initialization failed\n";
		return 1;
	}
	if (!client.PrepareUnrealActionSetup() || !client.ValidatePathContracts())
	{
		std::cerr << "unreal-action-setup: action/path setup failed\n";
		return 1;
	}
	if (!client.Begin())
	{
		std::cerr << "unreal-action-setup: session begin failed\n";
		return 1;
	}
	bool pressed = false;
	bool poseActive = false;
	bool triggerValue = true;
	float aClickValue = 1.0f;
	if (!client.Sync(pressed, &poseActive, &triggerValue, &aClickValue))
	{
		std::cerr << "unreal-action-setup: sync failed\n";
		return 1;
	}
	if (triggerValue || aClickValue != 0.0f)
	{
		std::cerr << "unreal-action-setup: scalar neutral conversion failed trigger=" << (triggerValue ? 1 : 0) << " aClick=" << aClickValue << '\n';
		return 1;
	}
	if (!client.End())
	{
		std::cerr << "unreal-action-setup: session end failed\n";
		return 1;
	}
	std::cerr << "unreal-action-setup: poseActive=" << (poseActive ? 1 : 0) << " triggerValue=" << (triggerValue ? 1 : 0) << " aClick=" << aClickValue << '\n';
	return 0;
}

int RunSessionRestart(const std::wstring& runtimeManifest)
{
	const auto expectWindowCount = [](uint32_t expected, std::string_view phase)
	{
		const uint32_t actual = CountAgentXRSessionWindows();
		if (actual == expected)
		{
			return true;
		}
		std::cerr << "session-restart: " << phase << " found " << actual << " AgentXR session windows, expected " << expected << '\n';
		return false;
	};
	const auto expectDiscovery = [](const RuntimeEndpoint& endpoint, std::string_view phase, bool expectRunning)
	{
		const Json& message = endpoint.handshake;
		const bool hasGeneration = message.contains("sessionGeneration") && message.at("sessionGeneration").is_number_integer();
		const bool hasRunning = message.contains("sessionRunning") && message.at("sessionRunning").is_boolean();
		const bool hasState = message.contains("sessionState");
		uint64_t actualGeneration = 0;
		if (hasGeneration)
		{
			const Json& generation = message.at("sessionGeneration");
			if (generation.is_number_unsigned())
			{
				actualGeneration = generation.get<uint64_t>();
			}
			else
			{
				const int64_t signedGeneration = generation.get<int64_t>();
				if (signedGeneration >= 0)
				{
					actualGeneration = static_cast<uint64_t>(signedGeneration);
				}
			}
		}
		const bool actualRunning = hasRunning ? message.at("sessionRunning").get<bool>() : false;
		const bool generationMatches = hasGeneration && (expectRunning ? actualGeneration != 0 : actualGeneration == 0);
		if (!generationMatches || !hasRunning || actualRunning != expectRunning || !hasState)
		{
			const std::string generationText = message.contains("sessionGeneration") ? message.at("sessionGeneration").dump() : "<missing>";
			const std::string runningText = message.contains("sessionRunning") ? message.at("sessionRunning").dump() : "<missing>";
			const std::string stateText = message.contains("sessionState") ? message.at("sessionState").dump() : "<missing>";
			std::cerr << "session-restart: " << phase << " discovery mismatch: sessionGeneration=" << generationText
				<< " sessionRunning=" << runningText
				<< " sessionState=" << stateText
				<< "; expected sessionGeneration=" << (expectRunning ? "nonzero" : "0")
				<< " sessionRunning=" << (expectRunning ? "true" : "false")
				<< " and sessionState present\n";
			return false;
		}
		return true;
	};

	OpenXR client;
	if (!client.Load(runtimeManifest) || !client.InitializeGraphics() || !client.Begin())
	{
		std::cerr << "session-restart: initial OpenXR setup failed\n";
		return 1;
	}
	RuntimeEndpoint discoveredEndpoint;
	if (!FindEndpoint(discoveredEndpoint))
	{
		std::cerr << "session-restart: initial active endpoint discovery failed\n";
		return 1;
	}
	RuntimeEndpoint endpoint;
	if (!Handshake(discoveredEndpoint.pipe, endpoint))
	{
		std::cerr << "session-restart: initial direct handshake failed for known pipe\n";
		return 1;
	}
	if (!expectDiscovery(endpoint, "initial active session", true))
	{
		return 1;
	}
	Control initialSessionControl;
	if (!initialSessionControl.Connect(endpoint))
	{
		std::cerr << "session-restart: initial endpoint control connection failed\n";
		return 1;
	}

	uint32_t frames = 0;
	if (!client.Frame(true, frames) || !expectWindowCount(1, "first begin"))
	{
		return 1;
	}
	if (!client.End())
	{
		std::cerr << "session-restart: first client.End failed\n";
		return 1;
	}
	if (!expectWindowCount(0, "first end"))
	{
		return 1;
	}
	RuntimeEndpoint stoppedEndpoint;
	if (!Handshake(endpoint.pipe, stoppedEndpoint))
	{
		std::cerr << "session-restart: direct handshake after client.End failed for known pipe\n";
		return 1;
	}
	if (!expectDiscovery(stoppedEndpoint, "after client.End", false))
	{
		return 1;
	}

	if (!client.Begin())
	{
		std::cerr << "session-restart: same-session Begin failed\n";
		return 1;
	}
	RuntimeEndpoint restartedEndpoint;
	if (!Handshake(endpoint.pipe, restartedEndpoint))
	{
		std::cerr << "session-restart: direct handshake after same-session Begin failed for known pipe\n";
		return 1;
	}
	if (!expectDiscovery(restartedEndpoint, "same-session Begin", true))
	{
		return 1;
	}
	if (restartedEndpoint.sessionGeneration == endpoint.sessionGeneration)
	{
		std::cerr << "session-restart: same-session Begin did not change discovery generation: previous="
			<< endpoint.sessionGeneration << " current=" << restartedEndpoint.sessionGeneration << '\n';
		return 1;
	}
	if (!initialSessionControl.StaleSnapshot())
	{
		std::cerr << "session-restart: same-handle snapshot from previous generation was not rejected after Begin\n";
		return 1;
	}

	endpoint = std::move(restartedEndpoint);
	if (!client.Frame(true, frames) || !expectWindowCount(1, "same-session begin"))
	{
		return 1;
	}
	if (!client.StaleProjectionFrames(frames))
	{
		return 1;
	}
	const ULONGLONG waitStart = GetTickCount64();
	while (CountAgentXRSessionWindows() != 0 && GetTickCount64() - waitStart < 2500)
	{
		Sleep(10);
	}
	if (!expectWindowCount(0, "frame inactivity"))
	{
		return 1;
	}
	if (!client.HoldSwapchainImage() || !client.RequestActiveSessionExit() || !client.DestroyHeldSwapchain())
	{
		return 1;
	}
	if (!client.RestartAfterForcedReady() || !client.EndPriorEpochFrame() || !client.RecreateSwapchain() || !client.Frame(true, frames) || !expectWindowCount(1, "frame after session restart"))
	{
		return 1;
	}
	if (!client.StuckFrameAfterIdle() || !client.Frame(true, frames) || !expectWindowCount(1, "frame after stuck frame"))
	{
		return 1;
	}

	std::unique_ptr<Control> staleControl;
	for (uint32_t cycle = 0; cycle < 10; ++cycle)
	{
		RuntimeEndpoint currentEndpoint;
		if (!FindEndpoint(currentEndpoint))
		{
			std::cerr << "session-restart: endpoint discovery failed in cycle " << cycle << '\n';
			return 1;
		}
		std::unique_ptr<Control> current = std::make_unique<Control>();
		if (!current->Connect(currentEndpoint))
		{
			std::cerr << "session-restart: endpoint connect failed in cycle " << cycle << '\n';
			return 1;
		}
		if (staleControl != nullptr && !staleControl->StaleSnapshot())
		{
			std::cerr << "session-restart: stale snapshot was accepted in cycle " << cycle << '\n';
			return 1;
		}
		if (!client.Frame(true, frames) || (cycle == 0 && !expectWindowCount(1, "frame after inactivity")) || !client.End() || !expectWindowCount(0, cycle + 1 == 10 ? "final end" : "cycle end"))
		{
			return 1;
		}
		staleControl = std::move(current);
		if (cycle + 1 < 10)
		{
			if (!client.DestroyCurrentSession() || !client.RecreateSession() || !client.Begin())
			{
				std::cerr << "session-restart: destroy/recreate failed after cycle " << cycle << '\n';
				return 1;
			}
		}
	}
	if (!client.DestroyCurrentSession())
	{
		std::cerr << "session-restart: final DestroyCurrentSession failed\n";
		return 1;
	}
	RuntimeEndpoint absentEndpoint;
	if (!Handshake(endpoint.pipe, absentEndpoint))
	{
		std::cerr << "session-restart: direct handshake after final DestroyCurrentSession failed for known pipe\n";
		return 1;
	}
	if (!expectDiscovery(absentEndpoint, "after final DestroyCurrentSession", false))
	{
		return 1;
	}
	return 0;
}

int RunIdleClientShutdown(const std::wstring& runtimeManifest)
{
	OpenXR client;
	if (!client.Load(runtimeManifest) || !client.InitializeGraphics(false) || !client.Begin())
	{
		std::cerr << "idle-client-shutdown: OpenXR setup failed\n";
		return 1;
	}
	RuntimeEndpoint endpoint;
	if (!FindEndpoint(endpoint))
	{
		std::cerr << "idle-client-shutdown: endpoint discovery failed\n";
		return 1;
	}
	Control control;
	if (!control.Connect(endpoint))
	{
		std::cerr << "idle-client-shutdown: Control connection failed\n";
		return 1;
	}
	Json handshakeRequest = {{"op", "handshake"}};
	agentxr::protocol::PipeFrame handshakeResponse;
	if (!control.Request(handshakeRequest, handshakeResponse) ||
		!handshakeResponse.message.value("ok", false) ||
		handshakeResponse.message.value("sessionGeneration", 0ull) != endpoint.sessionGeneration)
	{
		std::cerr << "idle-client-shutdown: connected Control handshake failed for sessionGeneration=" << endpoint.sessionGeneration << '\n';
		return 1;
	}
	if (!client.End() || !client.DestroyCurrentSession())
	{
		std::cerr << "idle-client-shutdown: normal session teardown failed\n";
		return 1;
	}

	XrInstance instance = XR_NULL_HANDLE;
	PFN_xrDestroyInstance destroyInstance = nullptr;
	if (!client.ReleaseInstance(instance, destroyInstance))
	{
		std::cerr << "idle-client-shutdown: could not transfer instance destruction ownership\n";
		return 1;
	}

	std::future<XrResult> destroyFuture;
	try
	{
		destroyFuture = std::async(std::launch::async, [instance, destroyInstance]()
		{
			return destroyInstance(instance);
		});
	}
	catch (const std::exception& error)
	{
		std::cerr << "idle-client-shutdown: starting xrDestroyInstance asynchronously failed: " << error.what() << '\n';
		control.Disconnect();
		if (destroyInstance(instance) != XR_SUCCESS)
		{
			std::cerr << "idle-client-shutdown: fallback xrDestroyInstance failed\n";
		}
		return 1;
	}

	const bool completedWhileConnected = destroyFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
	if (!completedWhileConnected)
	{
		std::cerr << "idle-client-shutdown: xrDestroyInstance still pending after 3 seconds with idle Control client connected; disconnecting for cleanup\n";
	}
	control.Disconnect();
	if (!completedWhileConnected)
	{
		destroyFuture.wait();
	}

	XrResult destroyResult = XR_ERROR_RUNTIME_FAILURE;
	try
	{
		destroyResult = destroyFuture.get();
	}
	catch (const std::exception& error)
	{
		std::cerr << "idle-client-shutdown: xrDestroyInstance future threw: " << error.what() << '\n';
		if (!completedWhileConnected)
		{
			std::cerr << "idle-client-shutdown: failure; external Control client closure was required for xrDestroyInstance\n";
		}
		return 1;
	}
	catch (...)
	{
		std::cerr << "idle-client-shutdown: xrDestroyInstance future threw an unknown exception\n";
		if (!completedWhileConnected)
		{
			std::cerr << "idle-client-shutdown: failure; external Control client closure was required for xrDestroyInstance\n";
		}
		return 1;
	}
	const bool destroySucceeded = Check(destroyResult, "idle-client-shutdown: xrDestroyInstance");
	if (!completedWhileConnected)
	{
		std::cerr << "idle-client-shutdown: failure; external Control client closure was required for xrDestroyInstance\n";
	}
	if (!destroySucceeded || !completedWhileConnected)
	{
		return 1;
	}
	std::cerr << "idle-client-shutdown: xrDestroyInstance completed while idle Control client remained connected\n";
	return 0;
}

int RunLifecycle(const std::wstring& runtimeManifest)
{
	OpenXR client;
	if (!client.Load(runtimeManifest))
	{
		std::cerr << "lifecycle: runtime load failed\n";
		return 1;
	}
	if (!client.InitializeGraphics())
	{
		std::cerr << "lifecycle: graphics initialization failed\n";
		return 1;
	}
	if (!client.Begin())
	{
		std::cerr << "lifecycle: session begin failed\n";
		return 1;
	}
	if (!client.HasApiFallback())
	{
		std::cerr << "lifecycle: API 1.1 fallback was not observed\n";
		return 1;
	}
	uint32_t frames = 0;
	for (uint32_t index = 0; index < 10; ++index)
	{
		if (!client.Frame(false, frames))
		{
			std::cerr << "lifecycle: frame " << index << " failed\n";
			return 1;
		}
	}
	if (!client.End())
	{
		std::cerr << "lifecycle: session end failed\n";
		return 1;
	}
	if (frames != 10)
	{
		std::cerr << "lifecycle: expected 10 frames, got " << frames << '\n';
		return 1;
	}
	return 0;
}

void PrintHelp()
{
	std::cerr << "usage: agent-xr-smoke.exe --runtime <manifest> [--scenario <name>]\n";
	std::cerr << "scenarios: lifecycle session-restart stereo-composition capture-wait idle-client-shutdown timeline-actions report-lifetime tracking-recovery invalid-input pipelined-swapchain pipelined-frame unreal-action-setup\n";
}

} // namespace


int wmain(int argc, wchar_t** argv)
{
	SetUnhandledExceptionFilter(&SmokeUnhandledExceptionFilter);
	std::set_terminate(&SmokeTerminateHandler);
	std::wstring runtimeManifest;
	std::string scenarioName = "lifecycle";
	for (int index = 1; index < argc; ++index)
	{
		const std::wstring_view argument(argv[index]);
		if (argument == L"--help" || argument == L"-h")
		{
			PrintHelp();
			return 0;
		}
		if (argument == L"--runtime" && index + 1 < argc)
		{
			runtimeManifest = argv[++index];
		}
		else if (argument == L"--scenario" && index + 1 < argc)
		{
			scenarioName = agentxr::protocol::Utf8FromWide(argv[++index]);
		}
	}

	if (runtimeManifest.empty())
	{
		std::wcerr << L"--runtime required\n";
		return 2;
	}
	if (scenarioName == "lifecycle")
	{
		return RunLifecycle(runtimeManifest);
	}
	if (scenarioName == "session-restart")
	{
		return RunSessionRestart(runtimeManifest);
	}
	if (scenarioName == "idle-client-shutdown")
	{
		return RunIdleClientShutdown(runtimeManifest);
	}
	if (scenarioName == "unreal-action-setup")
	{
		return RunUnrealActionSetup(runtimeManifest);
	}
	if (scenarioName == "stereo-composition" || scenarioName == "capture-wait" || scenarioName == "timeline-actions" || scenarioName == "report-lifetime" || scenarioName == "tracking-recovery" || scenarioName == "invalid-input" || scenarioName == "pipelined-swapchain" || scenarioName == "pipelined-frame")
	{
		return RunScenario(runtimeManifest, std::filesystem::path(argv[0]), scenarioName);
	}
	std::cerr << "unknown scenario: " << scenarioName << '\n';
	return 2;
}
