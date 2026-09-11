#include "runtime.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace agentxr
{
namespace
{
struct ShaderConstants
{
	float uvScaleX = 1.0f;
	float uvScaleY = 1.0f;
	float uvOffsetX = 0.0f;
	float uvOffsetY = 0.0f;
	float ndcLeft = -1.0f;
	float ndcTop = 1.0f;
	float ndcRight = 1.0f;
	float ndcBottom = -1.0f;
	uint32_t arraySlice = 0;
	uint32_t layerFlags = 0;
	float alpha = 1.0f;
	float padding = 0.0f;
};

constexpr char kVertexShader[] = R"shader(
cbuffer Constants : register(b0)
{
    float4 uvTransform;
    float4 ndcRect;
    uint arraySlice;
    uint layerFlags;
    float alpha;
    float padding;
};
struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VertexOutput main(uint vertexId : SV_VertexID)
{
    float2 positions[4] = { float2(ndcRect.x, ndcRect.y), float2(ndcRect.z, ndcRect.y), float2(ndcRect.x, ndcRect.w), float2(ndcRect.z, ndcRect.w) };
    float2 uvs[4] = { float2(uvTransform.z, uvTransform.w), float2(uvTransform.z + uvTransform.x, uvTransform.w), float2(uvTransform.z, uvTransform.w + uvTransform.y), float2(uvTransform.z + uvTransform.x, uvTransform.w + uvTransform.y) };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)shader";

constexpr char kPixelShader[] = R"shader(
Texture2DArray<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer Constants : register(b0)
{
    float4 uvTransform;
    float4 ndcRect;
    uint arraySlice;
    uint layerFlags;
    float alpha;
    float padding;
};
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 color = sourceTexture.Sample(sourceSampler, float3(uv, arraySlice));
    const float sourceAlpha = color.a;
    if ((layerFlags & 0x00000002u) == 0u)
    {
        color.a = 1.0f;
    }
    else
    {
        if ((layerFlags & 0x00000004u) != 0u)
        {
            color.rgb *= sourceAlpha;
        }
        color.rgb *= alpha;
        color.a = sourceAlpha * alpha;
    }
    return color;
}
)shader";

constexpr UINT_PTR kPresentationTimerId = 0x41585201u;
constexpr UINT kPresentationTimerPeriodMs = 1000;

Compositor* WindowCompositor(HWND window)
{
	return reinterpret_cast<Compositor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE)
	{
		const auto* createInfo = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createInfo->lpCreateParams));
		return DefWindowProcW(window, message, wParam, lParam);
	}
	Compositor* compositor = WindowCompositor(window);
	if (message == WM_TIMER && wParam == kPresentationTimerId)
	{
		KillTimer(window, kPresentationTimerId);
		DestroyWindow(window);
		if (compositor != nullptr)
		{
			compositor->MarkPresentationWindowClosed();
		}
		return 0;
	}
	if (message == WM_CLOSE)
	{
		ShowWindow(window, SW_HIDE);
		return 0;
	}
	if (message == WM_NCDESTROY)
	{
		SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

void PumpWindowMessages(HWND window)
{
	MSG message{};
	while (PeekMessageW(&message, window, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

bool SupportedFormat(int64_t value)
{
	const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(value);
	return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

DXGI_FORMAT TypelessFormat(DXGI_FORMAT format)
{
	if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	return format;
}

bool ValidRect(const XrRect2Di& rect, uint32_t width, uint32_t height)
{
	return rect.offset.x >= 0 && rect.offset.y >= 0 && rect.extent.width > 0 && rect.extent.height > 0 && static_cast<uint64_t>(rect.offset.x) + static_cast<uint64_t>(rect.extent.width) <= width && static_cast<uint64_t>(rect.offset.y) + static_cast<uint64_t>(rect.extent.height) <= height;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (before == after) return;
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 width)
{
	D3D12_RESOURCE_DESC description{};
	description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	description.Width = width;
	description.Height = 1;
	description.DepthOrArraySize = 1;
	description.MipLevels = 1;
	description.SampleDesc.Count = 1;
	description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return description;
}

D3D12_RESOURCE_DESC TextureDesc(DXGI_FORMAT format, UINT width, UINT height, UINT16 arraySize, D3D12_RESOURCE_FLAGS flags)
{
	D3D12_RESOURCE_DESC description{};
	description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	description.Width = width;
	description.Height = height;
	description.DepthOrArraySize = arraySize;
	description.MipLevels = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	description.Flags = flags;
	return description;
}

} // namespace

Compositor::Compositor(Session& sessionValue)
	: session(sessionValue)
{
}

Compositor::~Compositor()
{
	Shutdown();
}

bool Compositor::Initialize(ID3D12Device* deviceValue, ID3D12CommandQueue* queueValue)
{
	std::lock_guard lock(mutex);
	if (initialized)
	{
		return device.Get() == deviceValue && queue.Get() == queueValue;
	}
	if (deviceValue == nullptr || queueValue == nullptr || queueValue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
	{
		return false;
	}
	Microsoft::WRL::ComPtr<ID3D12Device> queueDevice;
	if (FAILED(queueValue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != deviceValue)
	{
		return false;
	}
	device = deviceValue;
	queue = queueValue;
	deviceLost = false;
	presentationWindowClosed.store(false, std::memory_order_release);
	completedFrame = 0;
	completedFence = 0;
	presentedFrame = 0;
	presentOccluded = false;
	presentResult = 0;
	nextFence = 1;
	outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
	{
		return false;
	}
	if (FAILED(commandList->Close()))
	{
		return false;
	}
	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (fenceEvent == nullptr || !CreatePipeline())
	{
		if (fenceEvent != nullptr)
		{
			CloseHandle(fenceEvent);
			fenceEvent = nullptr;
		}
		return false;
	}
	initialized = true;
	return true;
}
bool Compositor::StartPresentation()
{
	std::lock_guard lock(mutex);
	if (!initialized || deviceLost)
	{
		return false;
	}
	if (ConsumePresentationWindowClosed())
	{
		DestroyPresentationLocked(true);
	}
	if (window != nullptr && windowSwapchain != nullptr && output != nullptr && rtvHeap != nullptr && windowBufferCount != 0)
	{
		ShowWindow(window, SW_SHOWNOACTIVATE);
		UpdateWindow(window);
		return true;
	}
	if (window != nullptr || windowSwapchain != nullptr || output != nullptr || rtvHeap != nullptr || windowBufferCount != 0)
	{
		DestroyPresentationLocked();
	}
	if (!CreateWindowResources())
	{
		DestroyPresentationLocked();
		return false;
	}
	return true;
}

void Compositor::StopPresentation()
{
	std::lock_guard lock(mutex);
	DestroyPresentationLocked();
}

void Compositor::DestroyPresentationLocked(bool windowAlreadyClosed)
{
	const bool wasClosed = windowAlreadyClosed || ConsumePresentationWindowClosed();
	if (fence != nullptr && nextFence > 1)
	{
		WaitFence(nextFence - 1, 2000);
	}
	if (window != nullptr)
	{
		if (!wasClosed)
		{
			KillTimer(window, kPresentationTimerId);
			PumpWindowMessages(window);
			if (!presentationWindowClosed.load(std::memory_order_acquire))
			{
				DestroyWindow(window);
			}
		}
		window = nullptr;
	}
	if (!windowClassName.empty())
	{
		const std::wstring className(windowClassName.begin(), windowClassName.end());
		UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));
		windowClassName.clear();
	}
	windowSwapchain.Reset();
	windowBuffers[0].Reset();
	windowBuffers[1].Reset();
	output.Reset();
	readback.Reset();
	rtvHeap.Reset();
	windowRtvHeap.Reset();
	windowBufferCount = 0;
	readbackRowPitch = 0;
	readbackHeight = 0;
	rtvStride = 0;
	outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	completedFrame = 0;
	completedFence = 0;
	presentedFrame = 0;
	presentOccluded = false;
	presentResult = 0;
	presentationWindowClosed.store(false, std::memory_order_release);
	captureCv.notify_all();
}
void Compositor::Shutdown()
{
	std::lock_guard lock(mutex);
	DestroyPresentationLocked();
	if (constantData != nullptr && constantBuffer != nullptr)
	{
		constantBuffer->Unmap(0, nullptr);
		constantData = nullptr;
	}
	if (fenceEvent != nullptr)
	{
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	pipeline.Reset();
	rootSignature.Reset();
	constantBuffer.Reset();
	commandList.Reset();
	allocator.Reset();
	fence.Reset();
	srvHeap.Reset();
	device.Reset();
	queue.Reset();
	nextFence = 1;
	initialized = false;
	deviceLost = false;
	presentationWindowClosed.store(false, std::memory_order_release);
}


bool Compositor::CreatePipeline()
{
	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER parameters[2]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[0].Descriptor.ShaderRegister = 0;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable.NumDescriptorRanges = 1;
	parameters[1].DescriptorTable.pDescriptorRanges = &range;
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rootDescription{};
	rootDescription.NumParameters = 2;
	rootDescription.pParameters = parameters;
	rootDescription.NumStaticSamplers = 1;
	rootDescription.pStaticSamplers = &sampler;
	rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	Microsoft::WRL::ComPtr<ID3DBlob> serialized;
	Microsoft::WRL::ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature)))) return false;
	Microsoft::WRL::ComPtr<ID3DBlob> vertex;
	Microsoft::WRL::ComPtr<ID3DBlob> pixel;
	if (FAILED(D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "agent-xr-vertex", nullptr, nullptr, "main", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors)) || FAILED(D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, "agent-xr-pixel", nullptr, nullptr, "main", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors))) return false;
	D3D12_BLEND_DESC blend{};
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	D3D12_RASTERIZER_DESC rasterizer{};
	rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizer.CullMode = D3D12_CULL_MODE_NONE;
	rasterizer.DepthClipEnable = TRUE;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
	description.pRootSignature = rootSignature.Get();
	description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
	description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
	description.BlendState = blend;
	description.RasterizerState = rasterizer;
	description.SampleMask = UINT_MAX;
	description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	description.NumRenderTargets = 1;
	description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	description.SampleDesc.Count = 1;
	if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)))) return false;
	D3D12_HEAP_PROPERTIES upload{};
	upload.Type = D3D12_HEAP_TYPE_UPLOAD;
	const D3D12_RESOURCE_DESC constants = BufferDesc(65536);
	if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &constants, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constantBuffer))) || FAILED(constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&constantData)))) return false;
	D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
	descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	descriptors.NumDescriptors = 64;
	descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&srvHeap)))) return false;
	srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return true;
}

bool Compositor::CreateWindowResources()
{
	Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
	windowClassName = "AgentXRPreview." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(session.sessionGeneration);
	const std::wstring className(windowClassName.begin(), windowClassName.end());
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = className.c_str();
	windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassExW(&windowClass);
	const std::wstring title = L"AgentXR PID " + std::to_wstring(GetCurrentProcessId()) + L" Session " + std::to_wstring(session.sessionGeneration);
	window = CreateWindowExW(0, className.c_str(), title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 512, nullptr, nullptr, GetModuleHandleW(nullptr), this);
	if (window == nullptr) return false;
	DXGI_SWAP_CHAIN_DESC1 description{};
	description.Width = 2048;
	description.Height = 1024;
	description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	description.BufferCount = 2;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	description.SampleDesc.Count = 1;
	Microsoft::WRL::ComPtr<IDXGISwapChain1> temporary;
	if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &description, nullptr, nullptr, &temporary)) || FAILED(temporary.As(&windowSwapchain))) return false;
	factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
	D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
	rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDescription.NumDescriptors = 3;
	if (FAILED(device->CreateDescriptorHeap(&rtvDescription, IID_PPV_ARGS(&rtvHeap)))) return false;
	rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	const D3D12_RESOURCE_DESC outputDescription = TextureDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 2048, 1024, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.0f, 0.0f, 0.0f, 1.0f}};
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outputDescription, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&output)))) return false;
	D3D12_CPU_DESCRIPTOR_HANDLE start = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(output.Get(), nullptr, start);
	for (UINT index = 0; index < 2; ++index)
	{
		if (FAILED(windowSwapchain->GetBuffer(index, IID_PPV_ARGS(&windowBuffers[index])))) return false;
		D3D12_CPU_DESCRIPTOR_HANDLE target = start;
		target.ptr += static_cast<SIZE_T>(index + 1) * rtvStride;
		device->CreateRenderTargetView(windowBuffers[index].Get(), nullptr, target);
	}
	windowBufferCount = 2;
	ShowWindow(window, SW_SHOWNOACTIVATE);
	UpdateWindow(window);
	return true;
}

bool Compositor::WaitFence(uint64_t value, uint32_t timeoutMs)
{
	if (value == 0 || fence == nullptr || fence->GetCompletedValue() >= value)
	{
		completedFence = std::max(completedFence, value);
		return true;
	}
	if (fenceEvent == nullptr || FAILED(fence->SetEventOnCompletion(value, fenceEvent))) return false;
	const DWORD result = WaitForSingleObject(fenceEvent, timeoutMs == UINT32_MAX ? INFINITE : timeoutMs);
	if (result != WAIT_OBJECT_0) return false;
	completedFence = std::max(completedFence, value);
	return true;
}

bool Compositor::SubmitAndSignal(uint64_t frameId)
{
	if (FAILED(commandList->Close())) return false;
	ID3D12CommandList* lists[] = {commandList.Get()};
	queue->ExecuteCommandLists(1, lists);
	const uint64_t value = nextFence++;
	if (FAILED(queue->Signal(fence.Get(), value)) || !WaitFence(value, 2000)) return false;
	if (frameId != 0) completedFrame = frameId;
	return true;
}

XrResult Compositor::CreateSwapchain(const XrSwapchainCreateInfo& info, Swapchain& swapchain)
{
	std::lock_guard lock(mutex);
	if (!initialized || deviceLost) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	if (!SupportedFormat(info.format)) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	if (info.sampleCount != 1 || info.faceCount != 1 || info.mipCount != 1 || (info.arraySize != 1 && info.arraySize != 2) || info.width == 0 || info.height == 0 || info.width > 4096 || info.height > 4096) return XR_ERROR_VALIDATION_FAILURE;
	if ((info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0 || (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) return XR_ERROR_FEATURE_UNSUPPORTED;
	swapchain.info = info;
	swapchain.viewFormat = static_cast<DXGI_FORMAT>(info.format);
	swapchain.resourceFormat = (info.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0 ? TypelessFormat(swapchain.viewFormat) : swapchain.viewFormat;
	swapchain.staticImage = (info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
	swapchain.images.resize(swapchain.staticImage ? 1u : 3u);
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	const D3D12_RESOURCE_DESC description = TextureDesc(swapchain.resourceFormat, info.width, info.height, static_cast<UINT16>(info.arraySize), D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	const D3D12_CLEAR_VALUE clear{swapchain.viewFormat, {0.0f, 0.0f, 0.0f, 1.0f}};
	for (SwapchainImage& image : swapchain.images)
	{
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, &clear, IID_PPV_ARGS(&image.resource)))) return XR_ERROR_OUT_OF_MEMORY;
	}
	return XR_SUCCESS;
}

XrResult Compositor::EnumerateSwapchainImages(Swapchain& swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
	std::lock_guard lock(mutex);
	if (count == nullptr) return XR_ERROR_VALIDATION_FAILURE;
	*count = static_cast<uint32_t>(swapchain.images.size());
	if (capacity == 0) return XR_SUCCESS;
	if (images == nullptr) return XR_ERROR_VALIDATION_FAILURE;
	const uint32_t copyCount = std::min(capacity, *count);
	auto* typedImages = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
	for (uint32_t index = 0; index < copyCount; ++index)
	{
		if (typedImages[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;
		typedImages[index].texture = swapchain.images[index].resource.Get();
	}
	return capacity < *count ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
}

XrResult Compositor::AcquireSwapchainImage(Swapchain& swapchain, uint32_t* index)
{
	std::unique_lock lock(mutex);
	if (!initialized || index == nullptr || swapchain.images.empty()) return XR_ERROR_VALIDATION_FAILURE;
	for (;;)
	{
		if (swapchain.staticImage && swapchain.lastReleasedIndex != UINT32_MAX) return XR_ERROR_CALL_ORDER_INVALID;
		const uint32_t imageCount = static_cast<uint32_t>(swapchain.images.size());
		const uint64_t completedValue = fence->GetCompletedValue();
		uint32_t pendingCandidate = UINT32_MAX;
		uint64_t pendingFenceValue = 0;
		for (uint32_t offset = 0; offset < imageCount; ++offset)
		{
			const uint32_t candidate = (swapchain.nextIndex + offset) % imageCount;
			SwapchainImage& image = swapchain.images[candidate];
			if (image.acquired || image.waited)
			{
				continue;
			}
			if (image.fenceValue == 0 || completedValue >= image.fenceValue)
			{
				swapchain.nextIndex = (candidate + 1) % imageCount;
				swapchain.acquiredIndices.push_back(candidate);
				image.acquired = true;
				image.waited = false;
				image.released = false;
				*index = candidate;
				return XR_SUCCESS;
			}
			if (pendingCandidate == UINT32_MAX)
			{
				pendingCandidate = candidate;
				pendingFenceValue = image.fenceValue;
			}
		}
		if (pendingCandidate == UINT32_MAX)
		{
			return XR_ERROR_CALL_ORDER_INVALID;
		}
		lock.unlock();
		if (!WaitFence(pendingFenceValue, 2000))
		{
			return XR_TIMEOUT_EXPIRED;
		}
		lock.lock();
	}
}

XrResult Compositor::WaitSwapchainImage(Swapchain& swapchain, XrDuration timeout)
{
	std::unique_lock lock(mutex);
	if (swapchain.acquiredIndices.empty()) return XR_ERROR_CALL_ORDER_INVALID;
	const uint32_t candidate = swapchain.acquiredIndices.front();
	if (candidate >= swapchain.images.size()) return XR_ERROR_CALL_ORDER_INVALID;
	SwapchainImage& image = swapchain.images[candidate];
	if (!image.acquired || image.waited || image.released) return XR_ERROR_CALL_ORDER_INVALID;
	const uint32_t timeoutMs = timeout == XR_INFINITE_DURATION ? UINT32_MAX : timeout <= 0 ? 0u : static_cast<uint32_t>(std::min<XrDuration>(timeout / 1000000, UINT32_MAX));
	const uint64_t value = image.fenceValue;
	lock.unlock();
	if (value != 0 && !WaitFence(value, timeoutMs)) return XR_TIMEOUT_EXPIRED;
	lock.lock();
	if (swapchain.acquiredIndices.empty() || swapchain.acquiredIndices.front() != candidate || candidate >= swapchain.images.size()) return XR_ERROR_CALL_ORDER_INVALID;
	SwapchainImage& waitedImage = swapchain.images[candidate];
	if (!waitedImage.acquired || waitedImage.waited || waitedImage.released) return XR_ERROR_CALL_ORDER_INVALID;
	if (waitedImage.state != D3D12_RESOURCE_STATE_RENDER_TARGET)
	{
		if (FAILED(allocator->Reset()) || FAILED(commandList->Reset(allocator.Get(), pipeline.Get()))) return XR_ERROR_RUNTIME_FAILURE;
		Transition(commandList.Get(), waitedImage.resource.Get(), waitedImage.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (!SubmitAndSignal(0)) return XR_ERROR_RUNTIME_FAILURE;
		waitedImage.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}
	swapchain.acquiredIndices.pop_front();
	swapchain.waitedIndices.push_back(candidate);
	waitedImage.waited = true;
	return XR_SUCCESS;
}

XrResult Compositor::ReleaseSwapchainImage(Swapchain& swapchain)
{
	std::lock_guard lock(mutex);
	if (swapchain.waitedIndices.empty()) return XR_ERROR_CALL_ORDER_INVALID;
	const uint32_t candidate = swapchain.waitedIndices.front();
	if (candidate >= swapchain.images.size()) return XR_ERROR_CALL_ORDER_INVALID;
	SwapchainImage& image = swapchain.images[candidate];
	if (!image.acquired || !image.waited || image.released) return XR_ERROR_CALL_ORDER_INVALID;
	swapchain.waitedIndices.pop_front();
	image.acquired = false;
	image.waited = false;
	image.released = true;
	swapchain.lastReleasedIndex = candidate;
	return XR_SUCCESS;
}

XrResult Compositor::Compose(const XrFrameEndInfo& endInfo, uint64_t frameId)
{
	std::lock_guard lock(mutex);
	if (!initialized || deviceLost)
	{
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	if (window != nullptr && !presentationWindowClosed.load(std::memory_order_acquire))
	{
		PumpWindowMessages(window);
	}
	if (ConsumePresentationWindowClosed())
	{
		DestroyPresentationLocked(true);
	}
	if (window == nullptr || windowSwapchain == nullptr || output == nullptr || rtvHeap == nullptr || windowBufferCount == 0)
	{
		if (window != nullptr || windowSwapchain != nullptr || output != nullptr || rtvHeap != nullptr || windowBufferCount != 0)
		{
			DestroyPresentationLocked();
		}
		if (!CreateWindowResources())
		{
			DestroyPresentationLocked();
			return XR_ERROR_GRAPHICS_DEVICE_INVALID;
		}
	}
	if (SetTimer(window, kPresentationTimerId, kPresentationTimerPeriodMs, nullptr) == 0)
	{
		DestroyPresentationLocked();
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	if (FAILED(allocator->Reset()) || FAILED(commandList->Reset(allocator.Get(), pipeline.Get())))
	{
		return XR_ERROR_RUNTIME_FAILURE;
	}
	commandList->SetGraphicsRootSignature(rootSignature.Get());
	ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
	commandList->SetDescriptorHeaps(1, heaps);
	D3D12_VIEWPORT viewport{0.0f, 0.0f, 2048.0f, 1024.0f, 0.0f, 1.0f};
	D3D12_RECT scissor{0, 0, 2048, 1024};
	commandList->RSSetViewports(1, &viewport);
	commandList->RSSetScissorRects(1, &scissor);
	D3D12_CPU_DESCRIPTOR_HANDLE outputRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	commandList->OMSetRenderTargets(1, &outputRtv, FALSE, nullptr);
	const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	commandList->ClearRenderTargetView(outputRtv, black, 0, nullptr);
	std::array<SwapchainImage*, 64> touched{};
	uint32_t touchedCount = 0;
	uint32_t srvIndex = 0;
	const auto draw = [&](Swapchain& source, const XrRect2Di& rect, uint32_t arrayIndex, float left, float top, float right, float bottom, float alpha, XrCompositionLayerFlags layerFlags) -> bool
	{
		if (source.lastReleasedIndex == UINT32_MAX || source.lastReleasedIndex >= source.images.size() || arrayIndex >= source.info.arraySize || !ValidRect(rect, source.info.width, source.info.height) || srvIndex >= 64) return false;
		SwapchainImage* image = &source.images[source.lastReleasedIndex];
		if (!image->released) return false;
		if (std::find(touched.begin(), touched.begin() + touchedCount, image) == touched.begin() + touchedCount)
		{
			Transition(commandList.Get(), image->resource.Get(), image->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			image->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			if (touchedCount >= touched.size()) return false;
			touched[touchedCount++] = image;
		}
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = source.viewFormat;
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2DArray.MipLevels = 1;
		view.Texture2DArray.ArraySize = source.info.arraySize;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(srvIndex) * srvStride;
		device->CreateShaderResourceView(image->resource.Get(), &view, cpu);
		D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<UINT64>(srvIndex) * srvStride;
		ShaderConstants constants;
		constants.uvScaleX = static_cast<float>(rect.extent.width) / static_cast<float>(source.info.width);
		constants.uvScaleY = static_cast<float>(rect.extent.height) / static_cast<float>(source.info.height);
		constants.uvOffsetX = static_cast<float>(rect.offset.x) / static_cast<float>(source.info.width);
		constants.uvOffsetY = static_cast<float>(rect.offset.y) / static_cast<float>(source.info.height);
		constants.ndcLeft = left;
		constants.ndcTop = top;
		constants.ndcRight = right;
		constants.ndcBottom = bottom;
		constants.arraySlice = arrayIndex;
		constants.layerFlags = static_cast<uint32_t>(layerFlags);
		constants.alpha = alpha;
		const UINT64 constantOffset = static_cast<UINT64>(srvIndex) * D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
		std::memcpy(constantData + constantOffset, &constants, sizeof(constants));
		commandList->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress() + constantOffset);
		commandList->SetGraphicsRootDescriptorTable(1, gpu);
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		commandList->DrawInstanced(4, 1, 0, 0);
		++srvIndex;
		return true;
	};
	for (uint32_t layerIndex = 0; layerIndex < endInfo.layerCount; ++layerIndex)
	{
		const XrCompositionLayerBaseHeader* base = endInfo.layers[layerIndex];
		if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
		{
			const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
			if (projection->space == XR_NULL_HANDLE || !IsValidSpace(projection->space) || projection->space->object->session != &session || projection->viewCount != 2 || projection->views == nullptr) return XR_ERROR_LAYER_INVALID;
			for (uint32_t viewIndex = 0; viewIndex < 2; ++viewIndex)
			{
				const XrSwapchainSubImage& subImage = projection->views[viewIndex].subImage;
				if (!IsValidSwapchain(subImage.swapchain) || subImage.swapchain->object->session != &session) return XR_ERROR_LAYER_INVALID;
				Swapchain& source = *subImage.swapchain->object;
				if (!draw(source, subImage.imageRect, subImage.imageArrayIndex, viewIndex == 0 ? -1.0f : 0.0f, 1.0f, viewIndex == 0 ? 0.0f : 1.0f, -1.0f, 1.0f, projection->layerFlags)) return XR_ERROR_LAYER_INVALID;
			}
		}
		else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
		{
			const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(base);
			if (quad->space == XR_NULL_HANDLE || !IsValidSpace(quad->space) || quad->space->object->session != &session) return XR_ERROR_LAYER_INVALID;
			const XrSwapchainSubImage& subImage = quad->subImage;
			if (!IsValidSwapchain(subImage.swapchain) || subImage.swapchain->object->session != &session) return XR_ERROR_LAYER_INVALID;
			Swapchain& source = *subImage.swapchain->object;
			const float halfWidth = std::clamp(quad->size.width * 0.5f, 0.02f, 1.0f);
			const float halfHeight = std::clamp(quad->size.height * 0.5f, 0.02f, 1.0f);
			const float centerX = std::clamp(quad->pose.position.x * 0.5f, -1.0f + halfWidth, 1.0f - halfWidth);
			const float centerY = std::clamp(quad->pose.position.y * 0.25f, -1.0f + halfHeight, 1.0f - halfHeight);
			if (quad->eyeVisibility != XR_EYE_VISIBILITY_RIGHT && !draw(source, subImage.imageRect, subImage.imageArrayIndex, centerX - halfWidth, 1.0f - centerY - halfHeight, centerX, 1.0f - centerY, 1.0f, quad->layerFlags)) return XR_ERROR_LAYER_INVALID;
			if (quad->eyeVisibility != XR_EYE_VISIBILITY_LEFT && !draw(source, subImage.imageRect, subImage.imageArrayIndex, centerX, 1.0f - centerY - halfHeight, centerX + halfWidth, 1.0f - centerY, 1.0f, quad->layerFlags)) return XR_ERROR_LAYER_INVALID;
		}
	}
	for (uint32_t touchedIndex = 0; touchedIndex < touchedCount; ++touchedIndex)
	{
		SwapchainImage* image = touched[touchedIndex];
		Transition(commandList.Get(), image->resource.Get(), image->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		image->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}
	const UINT backIndex = windowSwapchain->GetCurrentBackBufferIndex();
	ID3D12Resource* backBuffer = windowBuffers[backIndex].Get();
	Transition(commandList.Get(), backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->CopyResource(backBuffer, output.Get());
	Transition(commandList.Get(), backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	Transition(commandList.Get(), output.Get(), outputState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (!SubmitAndSignal(frameId))
	{
		deviceLost = true;
		return XR_ERROR_RUNTIME_FAILURE;
	}
	const uint64_t signal = nextFence - 1;
	for (uint32_t touchedIndex = 0; touchedIndex < touchedCount; ++touchedIndex)
	{
		touched[touchedIndex]->fenceValue = signal;
	}
	const HRESULT present = windowSwapchain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
	presentResult = static_cast<int32_t>(present);
	presentOccluded = present == DXGI_STATUS_OCCLUDED;
	if (FAILED(present) && present != DXGI_STATUS_OCCLUDED && present != DXGI_ERROR_WAS_STILL_DRAWING)
	{
		deviceLost = true;
		return XR_ERROR_RUNTIME_FAILURE;
	}
	if (present == S_OK) presentedFrame = frameId;
	captureCv.notify_all();
	return XR_SUCCESS;
}

bool Compositor::EncodePng(std::vector<uint8_t>& png)
{
	if (readback == nullptr || constantData == nullptr || readbackRowPitch == 0 || readbackHeight == 0) return false;
	void* mapped = nullptr;
	if (FAILED(readback->Map(0, nullptr, &mapped)) || mapped == nullptr) return false;
	const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool uninitialize = SUCCEEDED(initResult);
	bool success = false;
	{
		Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
		Microsoft::WRL::ComPtr<IStream> stream;
		Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
		Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
		Microsoft::WRL::ComPtr<IPropertyBag2> properties;
		success = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) && SUCCEEDED(frame->Initialize(properties.Get())) && SUCCEEDED(frame->SetSize(2048, 1024));
		if (success)
		{
			WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
			success = SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WritePixels(1024, readbackRowPitch, readbackRowPitch * readbackHeight, reinterpret_cast<BYTE*>(mapped))) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
		}
		if (success)
		{
			LARGE_INTEGER zero{};
			stream->Seek(zero, STREAM_SEEK_SET, nullptr);
			STATSTG statistics{};
			if (SUCCEEDED(stream->Stat(&statistics, STATFLAG_NONAME)) && statistics.cbSize.QuadPart <= static_cast<LONGLONG>(protocol::kMaxPngBytes))
			{
				png.resize(static_cast<size_t>(statistics.cbSize.QuadPart));
				ULONG read = 0;
				success = SUCCEEDED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) && read == png.size();
			}
			else success = false;
		}
	}
	readback->Unmap(0, nullptr);
	if (uninitialize) CoUninitialize();
	return success;
}

XrResult Compositor::Capture(uint64_t afterFrameId, protocol::Json& metadata, std::vector<uint8_t>& png, uint32_t timeoutMs)
{
	std::unique_lock lock(mutex);
	if (!initialized || deviceLost || window == nullptr || windowSwapchain == nullptr || output == nullptr || rtvHeap == nullptr || windowBufferCount == 0) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	if (completedFrame == 0 || completedFrame <= afterFrameId) return XR_TIMEOUT_EXPIRED;
	if (!WaitFence(completedFence, timeoutMs)) return XR_TIMEOUT_EXPIRED;
	const D3D12_RESOURCE_DESC description = output->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 totalSize = 0;
	device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows, &rowSize, &totalSize);
	readbackRowPitch = footprint.Footprint.RowPitch;
	readbackHeight = rows;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	const D3D12_RESOURCE_DESC readbackDescription = BufferDesc(totalSize);
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return XR_ERROR_OUT_OF_MEMORY;
	if (FAILED(allocator->Reset()) || FAILED(commandList->Reset(allocator.Get(), pipeline.Get()))) return XR_ERROR_RUNTIME_FAILURE;
	Transition(commandList.Get(), output.Get(), outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	outputState = D3D12_RESOURCE_STATE_COPY_SOURCE;
	D3D12_TEXTURE_COPY_LOCATION source{};
	source.pResource = output.Get();
	source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION destination{};
	destination.pResource = readback.Get();
	destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	destination.PlacedFootprint = footprint;
	commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
	Transition(commandList.Get(), output.Get(), outputState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (!SubmitAndSignal(0) || !EncodePng(png)) return XR_ERROR_RUNTIME_FAILURE;
	metadata = {{"frameId", completedFrame}, {"sessionGeneration", session.sessionGeneration}, {"displayTime", session.report.actualLastFrame}, {"layerCount", session.frames.empty() ? 0 : session.frames.back().layerCount}, {"captureTimestamp", session.instance->clock.Now()}, {"width", 2048}, {"height", 1024}, {"binaryLength", png.size()}};
	return XR_SUCCESS;
}


void Compositor::MarkPresentationWindowClosed() noexcept
{
	presentationWindowClosed.store(true, std::memory_order_release);
}

bool Compositor::ConsumePresentationWindowClosed() noexcept
{
	return presentationWindowClosed.exchange(false, std::memory_order_acq_rel);
}


uint64_t Compositor::LastCompletedFrame() const
{
	std::lock_guard lock(mutex);
	return completedFrame;
}
uint64_t Compositor::LastPresentedFrame() const
{
	std::lock_guard lock(mutex);
	return presentedFrame;
}

bool Compositor::DeviceLost() const
{
	std::lock_guard lock(mutex);
	return deviceLost;
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t capacity, uint32_t* count, int64_t* formats)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session)) return XR_ERROR_HANDLE_INVALID;
		const std::array<int64_t, 4> supported = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
		return EnumerateArray(capacity, count, formats, sizeof(int64_t), supported.data(), static_cast<uint32_t>(supported.size()));
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSession(session)) return XR_ERROR_HANDLE_INVALID;
		if (CheckType(createInfo, XR_TYPE_SWAPCHAIN_CREATE_INFO) != XR_SUCCESS || swapchain == nullptr) return XR_ERROR_VALIDATION_FAILURE;
		auto state = std::make_unique<Swapchain>();
		state->session = session->object;
		auto handle = std::make_unique<XrSwapchain_T>();
		handle->object = state.get();
		state->handle = handle.get();
		const XrResult result = session->object->compositor->CreateSwapchain(*createInfo, *state);
		if (result != XR_SUCCESS) return result;
		{
			std::lock_guard lock(session->object->mutex);
			session->object->swapchains.push_back(handle.get());
		}
		*swapchain = handle.release();
		state.release();
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSwapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
		Swapchain* state = swapchain->object;
		if (!state->acquiredIndices.empty() || !state->waitedIndices.empty()) return XR_ERROR_CALL_ORDER_INVALID;
		Session* owner = state->session;
		{
			std::lock_guard lock(owner->mutex);
			owner->swapchains.erase(std::remove(owner->swapchains.begin(), owner->swapchains.end(), swapchain), owner->swapchains.end());
		}
		delete state;
		swapchain->object = nullptr;
		swapchain->alive = false;
		return XR_SUCCESS;
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSwapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
		return swapchain->object->session->compositor->EnumerateSwapchainImages(*swapchain->object, capacity, count, images);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSwapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
		if (CheckType(acquireInfo, XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) != XR_SUCCESS || index == nullptr) return XR_ERROR_VALIDATION_FAILURE;
		return swapchain->object->session->compositor->AcquireSwapchainImage(*swapchain->object, index);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSwapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
		if (CheckType(waitInfo, XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) != XR_SUCCESS) return XR_ERROR_VALIDATION_FAILURE;
		return swapchain->object->session->compositor->WaitSwapchainImage(*swapchain->object, waitInfo->timeout);
	});
}

extern "C" AGENTXR_API XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo)
{
	return GuardResult([&]() -> XrResult
	{
		if (!IsValidSwapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
		if (CheckType(releaseInfo, XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) != XR_SUCCESS) return XR_ERROR_VALIDATION_FAILURE;
		return swapchain->object->session->compositor->ReleaseSwapchainImage(*swapchain->object);
	});
}

} // namespace agentxr
