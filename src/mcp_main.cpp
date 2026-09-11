#include "protocol.h"

#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
using Json = agentxr::protocol::Json;
using agentxr::protocol::PipeFrame;

struct Endpoint
{
	uint32_t processId = 0;
	uint64_t processCreationTime = 0;
	std::string image;
	std::string runtimeVersion;
	std::string instanceId;
	std::wstring pipe;
	Json handshake;
};

std::filesystem::path ExecutableDirectory()
{
	std::vector<wchar_t> buffer(32768);
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size()) return {};
	return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

bool MakeAbsolute(const std::filesystem::path& input, std::filesystem::path& output)
{
	if (input.empty()) return false;
	std::error_code error;
	output = std::filesystem::absolute(input, error);
	return !error && !output.empty();
}

bool ResolvePath(const Json& arguments, std::string_view name, bool required, std::filesystem::path& output)
{
	if (!arguments.contains(name)) return !required;
	const Json& value = arguments.at(name);
	if (!value.is_string()) return false;
	const std::wstring wideValue = agentxr::protocol::WideFromUtf8(value.get<std::string>());
	return !wideValue.empty() && MakeAbsolute(std::filesystem::path(wideValue), output);
}

std::wstring NormalizePath(std::wstring value)
{
	for (wchar_t& character : value)
	{
		character = character == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(character));
	}
	return value;
}

bool ShortcutContainsPath(const std::wstring& arguments, const std::filesystem::path& expected)
{
	const std::wstring normalizedArguments = NormalizePath(arguments);
	const std::wstring normalizedExpected = NormalizePath(expected.wstring());
	return !normalizedExpected.empty() && normalizedArguments.find(normalizedExpected) != std::wstring::npos;
}

uint64_t ProcessCreationTime(HANDLE process)
{
	FILETIME creation{}, exitTime{}, kernel{}, user{};
	if (!GetProcessTimes(process, &creation, &exitTime, &kernel, &user)) return 0;
	ULARGE_INTEGER value{};
	value.LowPart = creation.dwLowDateTime;
	value.HighPart = creation.dwHighDateTime;
	return value.QuadPart;
}

std::wstring ProcessImagePath(uint32_t processId)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (process == nullptr) return {};
	std::vector<wchar_t> buffer(32768);
	DWORD length = static_cast<DWORD>(buffer.size());
	const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &length);
	CloseHandle(process);
	return ok ? std::wstring(buffer.data(), buffer.data() + length) : std::wstring{};
}

std::vector<uint32_t> ProcessIdsByImage(const std::wstring& expected)
{
	std::vector<uint32_t> result;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return result;
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry))
	{
		do
		{
			const std::wstring image = ProcessImagePath(entry.th32ProcessID);
			if (!image.empty() && _wcsicmp(image.c_str(), expected.c_str()) == 0) result.push_back(entry.th32ProcessID);
		}
		while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return result;
}

bool OpenEndpoint(const std::wstring& pipeName, Endpoint& endpoint)
{
	HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (pipe == INVALID_HANDLE_VALUE) return false;
	DWORD serverProcessId = 0;
	if (!GetNamedPipeServerProcessId(pipe, &serverProcessId))
	{
		CloseHandle(pipe);
		return false;
	}
	const Json request = {{"op", "handshake"}};
	if (!agentxr::protocol::WriteFrame(pipe, request, {}, 500))
	{
		CloseHandle(pipe);
		return false;
	}
	PipeFrame response;
	if (!agentxr::protocol::ReadFrame(pipe, response, 1000) || !response.message.value("ok", false) || response.message.value("processId", 0u) != serverProcessId)
	{
		CloseHandle(pipe);
		return false;
	}
	endpoint.processId = response.message.value("processId", 0u);
	endpoint.processCreationTime = response.message.value("processCreationTime", 0ull);
	endpoint.image = response.message.value("image", std::string{});
	endpoint.runtimeVersion = response.message.value("runtimeVersion", std::string{});
	endpoint.instanceId = response.message.value("instanceId", std::string{});
	endpoint.pipe = pipeName;
	endpoint.handshake = response.message;
	CloseHandle(pipe);
	return endpoint.processId != 0 && !endpoint.instanceId.empty();
}

std::vector<Endpoint> DiscoverEndpoints()
{
	std::vector<Endpoint> result;
	WIN32_FIND_DATAW data{};
	HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\AgentXR.*", &data);
	if (find == INVALID_HANDLE_VALUE) return result;
	do
	{
		const std::wstring pipe = std::wstring(L"\\\\.\\pipe\\") + data.cFileName;
		Endpoint endpoint;
		if (OpenEndpoint(pipe, endpoint)) result.push_back(std::move(endpoint));
	}
	while (FindNextFileW(find, &data));
	FindClose(find);
	return result;
}

std::wstring MakeEnvironment(const std::wstring& runtimeManifest)
{
	std::wstring environment;
	static constexpr const wchar_t* excludedVariables[] = {
		L"XR_RUNTIME_JSON",
		L"XR_ENABLE_API_LAYERS",
		L"XR_API_LAYER_PATH",
		L"DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY"};
	LPWCH block = GetEnvironmentStringsW();
	if (block != nullptr)
	{
		for (LPWCH entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1)
		{
			bool excluded = false;
			for (const wchar_t* name : excludedVariables)
			{
				const size_t nameLength = std::wcslen(name);
				if (_wcsnicmp(entry, name, nameLength) == 0 && entry[nameLength] == L'=')
				{
					excluded = true;
					break;
				}
			}
			if (!excluded)
			{
				environment.append(entry);
				environment.push_back(L'\0');
			}
		}
		FreeEnvironmentStringsW(block);
	}
	const auto appendVariable = [&environment](const wchar_t* name, const wchar_t* value)
	{
		environment.append(name);
		environment.push_back(L'=');
		environment.append(value);
		environment.push_back(L'\0');
	};
	appendVariable(L"XR_RUNTIME_JSON", runtimeManifest.c_str());
	appendVariable(L"XR_ENABLE_API_LAYERS", L"");
	appendVariable(L"XR_API_LAYER_PATH", L"");
	appendVariable(L"DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY", L"1");
	environment.push_back(L'\0');
	return environment;
}

Json LaunchResolvedEditor(const std::filesystem::path& shortcut, const std::filesystem::path& manifest, const std::filesystem::path& expectedProject, bool hasExpectedProject)
{
	IShellLinkW* shellLinkRaw = nullptr;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLinkRaw))))
	{
		return agentxr::protocol::Error("shortcut_failed", "cannot create shortcut handler");
	}
	Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
	shellLink.Attach(shellLinkRaw);
	Microsoft::WRL::ComPtr<IPersistFile> persist;
	if (FAILED(shellLink.As(&persist)) || FAILED(persist->Load(shortcut.c_str(), STGM_READ)))
	{
		return agentxr::protocol::Error("shortcut_failed", "cannot load shortcut");
	}
	WCHAR targetBuffer[32768]{};
	WIN32_FIND_DATAW findData{};
	WCHAR argumentsBuffer[32768]{};
	WCHAR workingDirectoryBuffer[32768]{};
	if (FAILED(shellLink->GetPath(targetBuffer, ARRAYSIZE(targetBuffer), &findData, SLGP_RAWPATH)) || FAILED(shellLink->GetArguments(argumentsBuffer, ARRAYSIZE(argumentsBuffer))) || FAILED(shellLink->GetWorkingDirectory(workingDirectoryBuffer, ARRAYSIZE(workingDirectoryBuffer))))
	{
		return agentxr::protocol::Error("shortcut_failed", "cannot resolve shortcut");
	}
	if (targetBuffer[0] == L'\0')
	{
		return agentxr::protocol::Error("shortcut_failed", "shortcut target is empty");
	}
	std::filesystem::path target;
	if (!MakeAbsolute(std::filesystem::path(targetBuffer), target))
	{
		return agentxr::protocol::Error("shortcut_failed", "cannot resolve shortcut target");
	}
	const std::wstring shortcutArguments(argumentsBuffer);
	if (hasExpectedProject && !ShortcutContainsPath(shortcutArguments, expectedProject))
	{
		return agentxr::protocol::Error("project_mismatch", "shortcut arguments do not match expected project");
	}
	if (!ProcessIdsByImage(target.wstring()).empty())
	{
		return agentxr::protocol::Error("editor_already_running", "matching editor already running");
	}
	std::filesystem::path workingDirectory;
	if (workingDirectoryBuffer[0] != L'\0')
	{
		if (!MakeAbsolute(std::filesystem::path(workingDirectoryBuffer), workingDirectory))
		{
			return agentxr::protocol::Error("shortcut_failed", "cannot resolve shortcut working directory");
		}
	}
	else
	{
		workingDirectory = target.parent_path();
	}
	if (workingDirectory.empty())
	{
		return agentxr::protocol::Error("shortcut_failed", "shortcut working directory is empty");
	}
	const std::wstring targetString = target.wstring();
	const std::wstring workingDirectoryString = workingDirectory.wstring();
	std::wstring command = L"\"" + targetString + L"\"";
	if (!shortcutArguments.empty())
	{
		command.push_back(L' ');
		command.append(shortcutArguments);
	}
	std::vector<wchar_t> commandLine(command.begin(), command.end());
	commandLine.push_back(L'\0');
	const std::wstring environment = MakeEnvironment(manifest.wstring());
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION processInfo{};
	const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, const_cast<wchar_t*>(environment.c_str()), workingDirectoryString.c_str(), &startup, &processInfo);
	if (!created) return agentxr::protocol::Error("launch_failed", "cannot launch editor");
	const uint32_t processId = processInfo.dwProcessId;
	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	return {{"ok", true}, {"result", {{"processId", processId}, {"manifest", agentxr::protocol::Utf8FromWide(manifest.wstring())}}}};
}

Json LaunchEditor(const Json& arguments)
{
	if (!arguments.is_object()) return agentxr::protocol::Error("invalid_arguments", "launch_editor arguments must be an object");
	std::filesystem::path shortcut;
	if (!ResolvePath(arguments, "shortcutPath", true, shortcut)) return agentxr::protocol::Error("invalid_arguments", "shortcutPath must be a non-empty string");
	const bool hasExpectedProject = arguments.contains("expectedProjectPath");
	std::filesystem::path expectedProject;
	if (hasExpectedProject && !ResolvePath(arguments, "expectedProjectPath", false, expectedProject)) return agentxr::protocol::Error("invalid_arguments", "expectedProjectPath must be a non-empty string");
	std::filesystem::path sourceManifest;
	if (arguments.contains("runtimeManifestPath"))
	{
		if (!ResolvePath(arguments, "runtimeManifestPath", false, sourceManifest)) return agentxr::protocol::Error("invalid_arguments", "runtimeManifestPath must be a non-empty string");
	}
	else
	{
		const std::filesystem::path executableDirectory = ExecutableDirectory();
		if (executableDirectory.empty() || !MakeAbsolute(executableDirectory / L"agent-xr.json", sourceManifest)) return agentxr::protocol::Error("runtime_not_installed", "runtime manifest is unavailable");
	}
	std::error_code manifestError;
	if (!std::filesystem::is_regular_file(sourceManifest, manifestError)) return agentxr::protocol::Error("runtime_not_installed", "runtime manifest is unavailable");
	std::ifstream sourceFile(sourceManifest, std::ios::binary);
	if (!sourceFile) return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest cannot be read");
	Json manifestDocument;
	try
	{
		manifestDocument = Json::parse(sourceFile);
	}
	catch (...)
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest is invalid");
	}
	if (!manifestDocument.is_object() || !manifestDocument.contains("runtime") || !manifestDocument.at("runtime").is_object() || !manifestDocument.at("runtime").contains("library_path") || !manifestDocument.at("runtime").at("library_path").is_string())
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest is invalid");
	}
	const std::wstring libraryValue = agentxr::protocol::WideFromUtf8(manifestDocument.at("runtime").at("library_path").get<std::string>());
	if (libraryValue.empty()) return agentxr::protocol::Error("runtime_manifest_invalid", "runtime library path is invalid");
	std::filesystem::path libraryPath;
	const std::filesystem::path libraryInput(libraryValue);
	if (!libraryInput.is_absolute())
	{
		if (!MakeAbsolute(sourceManifest.parent_path() / libraryInput, libraryPath)) return agentxr::protocol::Error("runtime_manifest_invalid", "runtime library path is invalid");
	}
	else if (!MakeAbsolute(libraryInput, libraryPath))
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime library path is invalid");
	}
	std::error_code libraryError;
	if (!std::filesystem::is_regular_file(libraryPath, libraryError)) return agentxr::protocol::Error("runtime_manifest_invalid", "runtime library is unavailable");
	const std::string resolvedLibrary = agentxr::protocol::Utf8FromWide(libraryPath.wstring());
	if (resolvedLibrary.empty()) return agentxr::protocol::Error("runtime_manifest_invalid", "runtime library path is invalid");
	manifestDocument["runtime"]["library_path"] = resolvedLibrary;
	std::error_code temporaryError;
	std::filesystem::path temporaryDirectory = std::filesystem::temp_directory_path(temporaryError);
	if (temporaryError || temporaryDirectory.empty())
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest directory is unavailable");
	}
	temporaryDirectory /= L"AgentXR";
	if (!std::filesystem::create_directories(temporaryDirectory, temporaryError) && temporaryError)
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest directory is unavailable");
	}
	std::filesystem::path resolvedManifest;
	const std::wstring filename = L"agent-xr-" + std::to_wstring(GetCurrentProcessId()) + L".json";
	if (!MakeAbsolute(temporaryDirectory / filename, resolvedManifest)) return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest path is invalid");
	std::ofstream resolvedFile(resolvedManifest, std::ios::binary | std::ios::trunc);
	if (!resolvedFile)
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest cannot be written");
	}
	resolvedFile << manifestDocument.dump() << '\n';
	if (!resolvedFile)
	{
		return agentxr::protocol::Error("runtime_manifest_invalid", "runtime manifest cannot be written");
	}
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool uninitialize = SUCCEEDED(init);
	Json result = LaunchResolvedEditor(shortcut, resolvedManifest, expectedProject, hasExpectedProject);
	if (uninitialize) CoUninitialize();
	if (result.value("ok", false))
	{
		result["result"]["manifest"] = agentxr::protocol::Utf8FromWide(resolvedManifest.wstring());
		result["result"]["sourceManifest"] = agentxr::protocol::Utf8FromWide(sourceManifest.wstring());
	}
	return result;
}

class RuntimeConnection
{
public:
	~RuntimeConnection()
	{
		Close();
	}

	bool Connect(const Endpoint& endpoint)
	{
		Close();
		pipe = CreateFileW(endpoint.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) return false;
		processId = endpoint.processId;
		instanceId = endpoint.instanceId;
		return true;
	}

	void Close()
	{
		if (pipe != INVALID_HANDLE_VALUE)
		{
			CloseHandle(pipe);
			pipe = INVALID_HANDLE_VALUE;
		}
		processId = 0;
		instanceId.clear();
	}

	bool Send(const Json& request, PipeFrame& response)
	{
		return pipe != INVALID_HANDLE_VALUE && agentxr::protocol::WriteFrame(pipe, request, {}, 2000) && agentxr::protocol::ReadFrame(pipe, response, 2500);
	}

	uint32_t processId = 0;
	std::string instanceId;

private:
	HANDLE pipe = INVALID_HANDLE_VALUE;
};

Json TextResult(const Json& value)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}};
}

Json ToolFailure(const Json& value)
{
	Json result = TextResult(value);
	result["isError"] = true;
	return result;
}

Json ListProcesses()
{
	Json processes = Json::array();
	for (const Endpoint& endpoint : DiscoverEndpoints())
	{
		Json item = endpoint.handshake;
		item["pipe"] = agentxr::protocol::Utf8FromWide(endpoint.pipe);
		processes.push_back(std::move(item));
	}
	return {{"content", Json::array({{{"type", "text"}, {"text", processes.dump()}}})}};
}

Json CallXr(const Json& arguments, RuntimeConnection& connection)
{
	if (!arguments.is_object() || !arguments.contains("action") || !arguments.at("action").is_string()) return ToolFailure(agentxr::protocol::Error("invalid_arguments", "xr action is required"));
	const std::string action = arguments.at("action").get<std::string>();
	if (action == "list_processes") return ListProcesses();
	if (action == "launch_editor")
	{
		Json launch = LaunchEditor(arguments);
		return launch.value("ok", false) ? TextResult(launch) : ToolFailure(launch);
	}
	if (!arguments.contains("processId") || !arguments.contains("instanceId") || !arguments.at("processId").is_number_unsigned() || !arguments.at("instanceId").is_string()) return ToolFailure(agentxr::protocol::Error("invalid_arguments", "processId and instanceId required"));
	const uint32_t processId = arguments.at("processId").get<uint32_t>();
	const std::string instanceId = arguments.at("instanceId").get<std::string>();
	if (connection.processId != processId || connection.instanceId != instanceId)
	{
		Endpoint selected;
		for (const Endpoint& endpoint : DiscoverEndpoints())
		{
			if (endpoint.processId == processId && endpoint.instanceId == instanceId)
			{
				selected = endpoint;
				break;
			}
		}
		if (selected.pipe.empty() || !connection.Connect(selected)) return ToolFailure(agentxr::protocol::Error("runtime_unavailable", "runtime endpoint not found"));
	}
	Json request = arguments;
	request["op"] = action;
	PipeFrame response;
	if (!connection.Send(request, response))
	{
		connection.Close();
		return ToolFailure(agentxr::protocol::Error("runtime_closed", "runtime endpoint closed connection"));
	}
	if (!response.message.value("ok", false)) return ToolFailure(response.message);
	if (action == "capture")
	{
		Json result = response.message.value("result", Json::object());
		Json output = TextResult(result);
		if (!response.binary.empty()) output["content"].push_back({{"type", "image"}, {"data", agentxr::protocol::Base64Encode(response.binary)}, {"mimeType", "image/png"}});
		return output;
	}
	return TextResult(response.message.value("result", response.message));
}

Json JsonRpcError(const Json& id, int code, std::string_view message)
{
	return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json HandleRpc(const Json& request, RuntimeConnection& connection, bool& respond)
{
	respond = request.contains("id");
	const Json id = request.contains("id") ? request.at("id") : nullptr;
	if (!request.is_object() || request.value("jsonrpc", std::string{}) != "2.0" || !request.contains("method") || !request.at("method").is_string()) return JsonRpcError(id, -32600, "invalid request");
	const std::string method = request.at("method").get<std::string>();
	if (method.rfind("notifications/", 0) == 0)
	{
		respond = false;
		return {};
	}
	if (method == "initialize")
	{
		return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"protocolVersion", agentxr::protocol::kMcpProtocolVersion}, {"capabilities", {{"tools", Json::object()}}}, {"serverInfo", {{"name", "agent-xr-mcp"}, {"version", "1.0.0"}}}}}};
	}
	if (method == "ping") return {{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};
	if (method == "tools/list")
	{
		Json properties = Json::object();
		properties["action"] = Json::object();
		properties["action"]["type"] = "string";
		properties["action"]["enum"] = Json::array({"list_processes", "launch_editor", "snapshot", "submit_timeline", "get_report", "cancel_timeline", "capture"});
		properties["processId"] = Json{{"type", "integer"}};
		properties["instanceId"] = Json{{"type", "string"}};
		properties["sessionGeneration"] = Json{{"type", "integer"}};
		properties["timelineId"] = Json{{"type", "integer"}};
		properties["cursor"] = Json{{"type", "integer"}};
		properties["limit"] = Json{{"type", "integer"}};
		properties["afterFrameId"] = Json{{"type", "integer"}};
		properties["shortcutPath"] = Json{{"type", "string"}};
		properties["expectedProjectPath"] = Json{{"type", "string"}};
		properties["runtimeManifestPath"] = Json{{"type", "string"}};
		properties["timeline"] = Json{{"type", "object"}};
		Json inputSchema = Json::object();
		inputSchema["type"] = "object";
		inputSchema["properties"] = std::move(properties);
		inputSchema["required"] = Json::array({"action"});
		Json tool = Json::object();
		tool["name"] = "xr";
		tool["description"] = "AgentXR runtime control and evidence";
		tool["inputSchema"] = std::move(inputSchema);
		return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", Json::array({tool})}}}};
	}
	if (method == "tools/call")
	{
		if (!request.contains("params") || !request.at("params").is_object() || request.at("params").value("name", std::string{}) != "xr") return JsonRpcError(id, -32602, "unknown tool");
		const Json arguments = request.at("params").value("arguments", Json::object());
		return {{"jsonrpc", "2.0"}, {"id", id}, {"result", CallXr(arguments, connection)}};
	}
	return JsonRpcError(id, -32601, "method not found");
}

} // namespace

int wmain()
{
	std::ios::sync_with_stdio(false);
	RuntimeConnection connection;
	std::string line;
	while (std::getline(std::cin, line))
	{
		if (line.size() > agentxr::protocol::kMaxMessageBytes)
		{
			std::cout << JsonRpcError(nullptr, -32600, "request exceeds 4 MiB limit").dump() << '\n' << std::flush;
			continue;
		}
		Json request;
		try
		{
			request = Json::parse(line);
		}
		catch (...)
		{
			std::cout << JsonRpcError(nullptr, -32700, "parse error").dump() << '\n' << std::flush;
			continue;
		}
		bool respond = true;
		Json response = HandleRpc(request, connection, respond);
		if (respond && !response.is_null()) std::cout << response.dump() << '\n' << std::flush;
	}
	return 0;
}
