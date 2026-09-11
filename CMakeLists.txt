cmake_minimum_required(VERSION 3.24)
project(AgentXR VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
	message(FATAL_ERROR "AgentXR supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(OPENXR_LOADER_ROOT
	${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/loader/OpenXR-SDK-f2448a8797c85814aa892efc1ab8707900fbcc78/src)
set(OPENXR_LOADER_DIR ${OPENXR_LOADER_ROOT}/loader)
set(OPENXR_LOADER_COMMON_DIR ${OPENXR_LOADER_ROOT}/common)
set(OPENXR_LOADER_JSONCPP_DIR ${OPENXR_LOADER_ROOT}/external/jsoncpp)
set(OPENXR_LOADER_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/openxr-loader)
file(MAKE_DIRECTORY ${OPENXR_LOADER_GENERATED_DIR})
configure_file(${OPENXR_LOADER_ROOT}/common_config.h.in ${OPENXR_LOADER_GENERATED_DIR}/common_config.h)

add_library(AgentXROpenXRLoader STATIC
	${OPENXR_LOADER_DIR}/android_utilities.cpp
	${OPENXR_LOADER_DIR}/api_layer_interface.cpp
	${OPENXR_LOADER_DIR}/loader_core.cpp
	${OPENXR_LOADER_DIR}/loader_init_data.cpp
	${OPENXR_LOADER_DIR}/loader_instance.cpp
	${OPENXR_LOADER_DIR}/loader_logger.cpp
	${OPENXR_LOADER_DIR}/loader_logger_recorders.cpp
	${OPENXR_LOADER_DIR}/loader_properties.cpp
	${OPENXR_LOADER_DIR}/manifest_file.cpp
	${OPENXR_LOADER_DIR}/runtime_interface.cpp
	${OPENXR_LOADER_DIR}/xr_generated_loader.cpp
	${OPENXR_LOADER_COMMON_DIR}/filesystem_utils.cpp
	${OPENXR_LOADER_COMMON_DIR}/object_info.cpp
	${OPENXR_LOADER_ROOT}/xr_generated_dispatch_table.c
	${OPENXR_LOADER_ROOT}/xr_generated_dispatch_table_core.c
	${OPENXR_LOADER_JSONCPP_DIR}/src/lib_json/json_reader.cpp
	${OPENXR_LOADER_JSONCPP_DIR}/src/lib_json/json_value.cpp
	${OPENXR_LOADER_JSONCPP_DIR}/src/lib_json/json_writer.cpp)
target_include_directories(AgentXROpenXRLoader
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include
	PRIVATE ${OPENXR_LOADER_ROOT}
	${OPENXR_LOADER_DIR}
	${OPENXR_LOADER_COMMON_DIR}
	${OPENXR_LOADER_GENERATED_DIR}
	${OPENXR_LOADER_JSONCPP_DIR}/include
	${OPENXR_LOADER_JSONCPP_DIR}/src/lib_json)
target_compile_definitions(AgentXROpenXRLoader PRIVATE
	WIN32_LEAN_AND_MEAN
	NOMINMAX
	UNICODE
	_UNICODE
	WINVER=0x0601
	_WIN32_WINNT=0x0601
	XR_OS_WINDOWS
	XR_USE_PLATFORM_WIN32
	XR_USE_GRAPHICS_API_D3D12
	OPENXR_HAVE_COMMON_CONFIG)
target_link_libraries(AgentXROpenXRLoader PUBLIC advapi32)
set_target_properties(AgentXROpenXRLoader PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

set(AGENTXR_INCLUDE_DIRS
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include
	${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(AgentXRRuntime SHARED
	src/runtime.cpp
	src/actions.cpp
	src/spaces.cpp
	src/timeline.cpp
	src/compositor.cpp
	src/control.cpp)
target_include_directories(AgentXRRuntime PRIVATE ${AGENTXR_INCLUDE_DIRS})
target_compile_definitions(AgentXRRuntime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D12 XR_NO_PROTOTYPES UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(AgentXRRuntime PRIVATE d3d12 dxgi dxguid d3dcompiler windowscodecs ole32 user32 advapi32 bcrypt)
set_target_properties(AgentXRRuntime PROPERTIES OUTPUT_NAME AgentXRRuntime RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_executable(agent-xr-mcp src/mcp_main.cpp)
target_include_directories(agent-xr-mcp PRIVATE ${AGENTXR_INCLUDE_DIRS})
target_compile_definitions(agent-xr-mcp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(agent-xr-mcp PRIVATE ole32 shell32 advapi32 user32)
set_target_properties(agent-xr-mcp PROPERTIES OUTPUT_NAME agent-xr-mcp RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(agent-xr-smoke tests/openxr_smoke.cpp)
target_include_directories(agent-xr-smoke PRIVATE ${AGENTXR_INCLUDE_DIRS})
target_compile_definitions(agent-xr-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX XR_USE_GRAPHICS_API_D3D12 XR_NO_PROTOTYPES UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(agent-xr-smoke PRIVATE AgentXROpenXRLoader d3d12 dxgi dxguid windowscodecs ole32 user32 dbghelp)
set_target_properties(agent-xr-smoke PROPERTIES OUTPUT_NAME agent-xr-smoke RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

install(TARGETS AgentXRRuntime agent-xr-mcp agent-xr-smoke RUNTIME DESTINATION . LIBRARY DESTINATION . ARCHIVE DESTINATION .)
install(FILES agent-xr.json DESTINATION .)
install(FILES launch-agentxr-editor.ps1 DESTINATION .)
install(FILES src/compositor.hlsl DESTINATION .)
install(FILES third_party/THIRD_PARTY_NOTICES.txt DESTINATION third_party)
install(FILES third_party/openxr/LICENSE.txt third_party/nlohmann/LICENSE.MIT DESTINATION third_party)
install(DIRECTORY scenarios/ DESTINATION scenarios)
