#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentxr::protocol
{
using Json = nlohmann::json;

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr char kMcpProtocolVersion[] = "2025-11-25";
inline constexpr size_t kMaxMessageBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxPngBytes = 16u * 1024u * 1024u;
inline constexpr size_t kMaxRecordsPerRun = 100000u;

struct PipeFrame
{
	Json message;
	std::vector<uint8_t> binary;
};

inline bool ReadExact(HANDLE pipe, void* destination, size_t bytes, uint32_t timeoutMs)
{
	if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || (bytes != 0 && destination == nullptr))
	{
		return false;
	}
	auto* output = static_cast<uint8_t*>(destination);
	size_t offset = 0;
	const ULONGLONG start = GetTickCount64();
	while (offset < bytes)
	{
		if (timeoutMs != UINT32_MAX && GetTickCount64() - start >= timeoutMs)
		{
			return false;
		}
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
		{
			return false;
		}
		if (available == 0)
		{
			Sleep(1);
			continue;
		}
		const DWORD requested = static_cast<DWORD>(std::min<size_t>({bytes - offset, available, 1u << 20}));
		DWORD read = 0;
		if (!ReadFile(pipe, output + offset, requested, &read, nullptr) || read == 0)
		{
			return false;
		}
		offset += read;
	}
	return true;
}

inline bool WriteExact(HANDLE pipe, const void* source, size_t bytes, uint32_t timeoutMs)
{
	if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || (bytes != 0 && source == nullptr))
	{
		return false;
	}
	const auto* input = static_cast<const uint8_t*>(source);
	size_t offset = 0;
	const ULONGLONG start = GetTickCount64();
	while (offset < bytes)
	{
		if (timeoutMs != UINT32_MAX && GetTickCount64() - start >= timeoutMs)
		{
			return false;
		}
		DWORD written = 0;
		if (!WriteFile(pipe, input + offset, static_cast<DWORD>(std::min<size_t>(bytes - offset, 1u << 16)), &written, nullptr) || written == 0)
		{
			return false;
		}
		offset += written;
	}
	return true;
}

inline bool ReadFrame(HANDLE pipe, PipeFrame& frame, uint32_t timeoutMs)
{
	frame.message = Json::object();
	frame.binary.clear();
	uint32_t length = 0;
	if (!ReadExact(pipe, &length, sizeof(length), timeoutMs) || length == 0 || length > kMaxMessageBytes)
	{
		return false;
	}
	std::string payload(length, '\0');
	if (!ReadExact(pipe, payload.data(), payload.size(), timeoutMs))
	{
		return false;
	}
	try
	{
		frame.message = Json::parse(payload);
		if (!frame.message.is_object())
		{
			return false;
		}
		uint64_t binaryLength = 0;
		if (frame.message.contains("binaryLength"))
		{
			const Json& field = frame.message.at("binaryLength");
			if (!field.is_number_unsigned())
			{
				return false;
			}
			binaryLength = field.get<uint64_t>();
		}
		if (binaryLength > kMaxPngBytes)
		{
			return false;
		}
		if (binaryLength != 0)
		{
			frame.binary.resize(static_cast<size_t>(binaryLength));
			if (!ReadExact(pipe, frame.binary.data(), frame.binary.size(), timeoutMs))
			{
				return false;
			}
		}
	}
	catch (...)
	{
		frame.message = Json::object();
		frame.binary.clear();
		return false;
	}
	return true;
}

inline bool WriteFrame(HANDLE pipe, const Json& message, const std::vector<uint8_t>& binary, uint32_t timeoutMs)
{
	if (binary.size() > kMaxPngBytes || !message.is_object())
	{
		return false;
	}
	uint64_t declaredBinaryLength = 0;
	if (message.contains("binaryLength"))
	{
		const Json& field = message.at("binaryLength");
		if (!field.is_number_unsigned())
		{
			return false;
		}
		declaredBinaryLength = field.get<uint64_t>();
	}
	if (declaredBinaryLength != binary.size())
	{
		return false;
	}
	std::string payload;
	try
	{
		payload = message.dump();
	}
	catch (...)
	{
		return false;
	}
	if (payload.empty() || payload.size() > kMaxMessageBytes)
	{
		return false;
	}
	const uint32_t length = static_cast<uint32_t>(payload.size());
	return WriteExact(pipe, &length, sizeof(length), timeoutMs) && WriteExact(pipe, payload.data(), payload.size(), timeoutMs) && (binary.empty() || WriteExact(pipe, binary.data(), binary.size(), timeoutMs));
}


inline std::wstring PipeName(uint32_t processId, std::string_view instanceId)
{
	std::wstring result = L"\\\\.\\pipe\\AgentXR.";
	result += std::to_wstring(processId);
	result.push_back(L'.');
	result.append(instanceId.begin(), instanceId.end());
	return result;
}

inline std::string Base64Encode(const std::vector<uint8_t>& bytes)
{
	static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve(((bytes.size() + 2) / 3) * 4);
	for (size_t index = 0; index < bytes.size(); index += 3)
	{
		const uint32_t value = (static_cast<uint32_t>(bytes[index]) << 16) | (index + 1 < bytes.size() ? static_cast<uint32_t>(bytes[index + 1]) << 8 : 0u) | (index + 2 < bytes.size() ? bytes[index + 2] : 0u);
		result.push_back(alphabet[(value >> 18) & 63]);
		result.push_back(alphabet[(value >> 12) & 63]);
		result.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
		result.push_back(index + 2 < bytes.size() ? alphabet[value & 63] : '=');
	}
	return result;
}

inline std::string Utf8FromWide(std::wstring_view value)
{
	if (value.empty()) return {};
	const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (length <= 0) return {};
	std::string result(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
	return result;
}

inline std::wstring WideFromUtf8(std::string_view value)
{
	if (value.empty()) return {};
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
	return result;
}

inline Json Error(std::string_view code, std::string_view message)
{
	return Json{{"ok", false}, {"error", Json{{"code", code}, {"message", message}}}};
}

} // namespace agentxr::protocol
