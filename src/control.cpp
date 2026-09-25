#include "runtime.h"

#include <sddl.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#pragma comment(lib, "advapi32.lib")

namespace agentxr
{
namespace
{
struct ClientContext
{
	ControlServer* server = nullptr;
	HANDLE pipe = INVALID_HANDLE_VALUE;
	uint64_t connectionId = 0;
};

std::wstring CurrentModulePath()
{
	std::array<wchar_t, 32768> buffer{};
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	return length == 0 ? std::wstring{} : std::wstring(buffer.data(), buffer.data() + length);
}
protocol::Json SessionError(std::string_view code, std::string_view message)
{
	return protocol::Error(code, message);
}

bool ReadUnsigned(const protocol::Json& object, std::string_view name, uint64_t& value, bool required)
{
	if (!object.is_object() || !object.contains(name))
	{
		return !required;
	}
	const protocol::Json& field = object.at(name);
	if (!field.is_number_unsigned())
	{
		return false;
	}
	value = field.get<uint64_t>();
	return true;
}

bool SameIdentity(const Instance& instance, const protocol::Json& request)
{
	uint64_t processId = 0;
	if (!ReadUnsigned(request, "processId", processId, true) || processId != GetCurrentProcessId())
	{
		return false;
	}
	if (!request.contains("instanceId") || !request.at("instanceId").is_string() || request.at("instanceId").get<std::string>() != instance.instanceId)
	{
		return false;
	}
	return true;
}

Session* FindSessionLocked(Instance& instance, uint64_t generation)
{
	for (XrSession handle : instance.sessions)
	{
		if (IsValidSession(handle) && handle->object->sessionGeneration == generation)
		{
			return handle->object;
		}
	}
	return nullptr;
}

bool LeaseRequired(std::string_view operation)
{
	return operation == "submit_timeline" || operation == "cancel_timeline";
}

bool KnownOperation(std::string_view operation)
{
	return operation == "snapshot" || operation == "submit_timeline" || operation == "get_report" || operation == "cancel_timeline" || operation == "capture";
}

} // namespace

ControlServer::ControlServer(Instance& instanceValue)
	: instance(instanceValue)
{
	pipePath = protocol::PipeName(GetCurrentProcessId(), instance.instanceId);
}

ControlServer::~ControlServer()
{
	Stop();
}

std::wstring ControlServer::PipePath() const
{
	return pipePath;
}

bool ControlServer::Start()
{
	if (listenThread != nullptr) return true;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
	{
		return false;
	}
	LocalFree(descriptor);
	stopping.store(false, std::memory_order_release);
	listenThread = CreateThread(nullptr, 0, &ControlServer::ListenThunk, this, 0, nullptr);
	return listenThread != nullptr;
}

void ControlServer::Stop()
{
	const bool wasStopping = stopping.exchange(true, std::memory_order_acq_rel);
	if (!wasStopping)
	{
		if (WaitNamedPipeW(pipePath.c_str(), 1000))
		{
			HANDLE wake = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
		}
	}
	{
		std::lock_guard lock(listenerMutex);
		if (listenPipe != nullptr)
		{
			DisconnectNamedPipe(listenPipe);
		}
	}
	if (listenThread != nullptr)
	{
		CancelSynchronousIo(listenThread);
		WaitForSingleObject(listenThread, INFINITE);
		CloseHandle(listenThread);
		listenThread = nullptr;
	}
	std::vector<HANDLE> copy;
	{
		std::lock_guard lock(workersMutex);
		copy = workers;
		workers.clear();
	}
	for (HANDLE worker : copy)
	{
		if (worker == nullptr) continue;
		CancelSynchronousIo(worker);
		WaitForSingleObject(worker, INFINITE);
		CloseHandle(worker);
	}
}

void ControlServer::ListenLoop()
{
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	SECURITY_ATTRIBUTES attributes{};
	attributes.nLength = sizeof(attributes);
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
	{
		return;
	}
	attributes.lpSecurityDescriptor = descriptor;
	while (!stopping.load(std::memory_order_acquire))
	{
		HANDLE pipe = CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 8, static_cast<DWORD>(protocol::kMaxMessageBytes), static_cast<DWORD>(protocol::kMaxMessageBytes), 1000, &attributes);
		if (pipe == INVALID_HANDLE_VALUE) break;
		{
			std::lock_guard lock(listenerMutex);
			listenPipe = pipe;
		}
		const BOOL connected = ConnectNamedPipe(pipe, nullptr);
		const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
		{
			std::lock_guard lock(listenerMutex);
			listenPipe = nullptr;
		}
		if ((connected || connectError == ERROR_PIPE_CONNECTED) && !stopping.load(std::memory_order_acquire))
		{
			auto* context = new ClientContext;
			context->server = this;
			context->pipe = pipe;
			context->connectionId = instance.nextConnection.fetch_add(1, std::memory_order_acq_rel);
			HANDLE worker = CreateThread(nullptr, 0, &ControlServer::ClientThunk, context, 0, nullptr);
			if (worker != nullptr)
			{
				std::lock_guard lock(workersMutex);
				workers.push_back(worker);
				continue;
			}
			delete context;
		}
		CloseHandle(pipe);
	}
	LocalFree(descriptor);
}


DWORD WINAPI ControlServer::ListenThunk(void* context)
{
	static_cast<ControlServer*>(context)->ListenLoop();
	return 0;
}

DWORD WINAPI ControlServer::ClientThunk(void* context)
{
	std::unique_ptr<ClientContext> client(static_cast<ClientContext*>(context));
	client->server->ClientLoop(client->pipe, client->connectionId);
	CloseHandle(client->pipe);
	return 0;
}


void ControlServer::ClientLoop(HANDLE pipe, uint64_t connectionId)
{
	while (!stopping.load(std::memory_order_acquire))
	{
		protocol::PipeFrame request;
		if (!protocol::ReadFrame(pipe, request, UINT32_MAX, &stopping)) break;
		protocol::PipeFrame response;
		const XrResult result = instance.HandleControl(connectionId, request.message, response);
		if (result != XR_SUCCESS && response.message.empty()) response.message = SessionError("runtime_error", "control request failed");
		if (!protocol::WriteFrame(pipe, response.message, response.binary, 2000)) break;
	}
	instance.ReleaseLease(connectionId, false);
}

XrResult Instance::HandleControl(uint64_t connectionId, const protocol::Json& request, protocol::PipeFrame& response)
{
	return GuardResult([&]() -> XrResult
	{
		response.message = protocol::Json::object();
		response.binary.clear();
		if (!request.is_object())
		{
			response.message = SessionError("invalid_request", "control request must be an object");
			return XR_ERROR_VALIDATION_FAILURE;
		}
		std::string operation;
		if (request.contains("op"))
		{
			if (!request.at("op").is_string())
			{
				response.message = SessionError("invalid_request", "op must be a string");
				return XR_ERROR_VALIDATION_FAILURE;
			}
			operation = request.at("op").get<std::string>();
		}
		else if (request.contains("action"))
		{
			if (!request.at("action").is_string())
			{
				response.message = SessionError("invalid_request", "action must be a string");
				return XR_ERROR_VALIDATION_FAILURE;
			}
			operation = request.at("action").get<std::string>();
		}
		else
		{
			response.message = SessionError("invalid_request", "control operation is required");
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (operation == "handshake")
		{
			response.message = {{"ok", true}, {"protocolVersion", protocol::kProtocolVersion}, {"mcpProtocolVersion", protocol::kMcpProtocolVersion}, {"processId", GetCurrentProcessId()}, {"processCreationTime", processCreationTime}, {"image", protocol::Utf8FromWide(CurrentModulePath())}, {"runtimeVersion", "AgentXR/1.0"}, {"instanceId", instanceId}, {"pipe", protocol::Utf8FromWide(control->PipePath())}, {"sessionGeneration", uint64_t{0}}, {"sessionRunning", false}, {"sessionState", static_cast<int>(XR_SESSION_STATE_UNKNOWN)}};
			bool foundInactiveSession = false;
			std::lock_guard instanceLock(mutex);
			for (XrSession handle : sessions)
			{
				if (!IsValidSession(handle))
				{
					continue;
				}
				std::lock_guard sessionLock(handle->object->mutex);
				Session* session = handle->object;
				const int sessionState = static_cast<int>(session->state);
				if (session->running)
				{
					response.message["sessionGeneration"] = session->sessionGeneration;
					response.message["sessionState"] = sessionState;
					response.message["sessionRunning"] = true;
					break;
				}
				if (!foundInactiveSession)
				{
					response.message["sessionState"] = sessionState;
					foundInactiveSession = true;
				}
			}
			return XR_SUCCESS;
		}
		if (!KnownOperation(operation))
		{
			response.message = SessionError("unknown_command", "unsupported control operation");
			return XR_ERROR_FUNCTION_UNSUPPORTED;
		}
		if (!SameIdentity(*this, request))
		{
			response.message = SessionError("identity_mismatch", "request identity does not match runtime endpoint");
			return XR_ERROR_HANDLE_INVALID;
		}
		uint64_t requestedGeneration = 0;
		if (!ReadUnsigned(request, "sessionGeneration", requestedGeneration, true) || requestedGeneration == 0)
		{
			response.message = SessionError("invalid_session", "sessionGeneration must be a nonzero unsigned integer");
			return XR_ERROR_VALIDATION_FAILURE;
		}
		if (LeaseRequired(operation) && !AcquireLease(connectionId))
		{
			response.message = SessionError("busy", "controller lease held by another connection");
			return XR_ERROR_SESSION_NOT_READY;
		}
		std::lock_guard instanceLock(mutex);
		Session* session = FindSessionLocked(*this, requestedGeneration);
		if (session == nullptr)
		{
			response.message = SessionError("stale_session", "session generation is not active");
			return XR_ERROR_SESSION_NOT_READY;
		}
		if (operation == "snapshot")
		{
			response.message = {{"ok", true}, {"result", session->Snapshot()}};
			return XR_SUCCESS;
		}
		if (operation == "submit_timeline")
		{
			if (!request.contains("timeline") || !request.at("timeline").is_object())
			{
				response.message = SessionError("invalid_timeline", "timeline object missing");
				return XR_ERROR_VALIDATION_FAILURE;
			}
			uint64_t timelineId = 0;
			std::string error;
			const XrResult result = session->SubmitTimeline(request.at("timeline"), timelineId, error);
			if (result != XR_SUCCESS)
			{
				response.message = SessionError("invalid_timeline", error);
				return result;
			}
			response.message = {{"ok", true}, {"result", {{"timelineId", timelineId}, {"sessionGeneration", session->sessionGeneration}, {"status", "armed"}}}};
			return XR_SUCCESS;
		}
		if (operation == "get_report")
		{
			uint64_t timelineId = 0;
			uint64_t cursorValue = 0;
			uint64_t limitValue = 100;
			if (!ReadUnsigned(request, "timelineId", timelineId, true) || !ReadUnsigned(request, "cursor", cursorValue, false) || !ReadUnsigned(request, "limit", limitValue, false))
			{
				response.message = SessionError("invalid_arguments", "timelineId, cursor and limit must be unsigned integers");
				return XR_ERROR_VALIDATION_FAILURE;
			}
			const size_t cursor = static_cast<size_t>(cursorValue);
			const size_t limit = static_cast<size_t>(std::min<uint64_t>(limitValue, 1000));
			response.message = {{"ok", true}, {"result", session->ReportPage(timelineId, cursor, limit)}};
			return XR_SUCCESS;
		}
		if (operation == "cancel_timeline")
		{
			uint64_t timelineId = 0;
			if (!ReadUnsigned(request, "timelineId", timelineId, true) || timelineId == 0)
			{
				response.message = SessionError("invalid_arguments", "timelineId must be a nonzero unsigned integer");
				return XR_ERROR_VALIDATION_FAILURE;
			}
			std::string error;
			const XrResult result = session->CancelTimeline(timelineId, error);
			if (result != XR_SUCCESS)
			{
				response.message = SessionError("cancel_failed", error);
				return result;
			}
			response.message = {{"ok", true}, {"result", {{"timelineId", timelineId}, {"status", "canceled"}}}};
			return XR_SUCCESS;
		}
		if (operation == "capture")
		{
			uint64_t afterFrameId = 0;
			if (!ReadUnsigned(request, "afterFrameId", afterFrameId, false))
			{
				response.message = SessionError("invalid_arguments", "afterFrameId must be an unsigned integer");
				return XR_ERROR_VALIDATION_FAILURE;
			}
			protocol::Json metadata;
			std::vector<uint8_t> png;
			const XrResult result = session->Capture(afterFrameId, metadata, png, 2000);
			if (result != XR_SUCCESS)
			{
				response.message = SessionError(result == XR_TIMEOUT_EXPIRED ? "timeout" : "capture_failed", result == XR_TIMEOUT_EXPIRED ? "no fresh completed frame" : "capture failed");
				return result;
			}
			metadata["binaryLength"] = png.size();
			response.message = {{"ok", true}, {"result", metadata}, {"binaryLength", png.size()}};
			response.binary = std::move(png);
			return XR_SUCCESS;
		}
		response.message = SessionError("unknown_command", "unsupported control operation");
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	});
}

} // namespace agentxr
