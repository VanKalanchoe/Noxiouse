cmake_minimum_required(VERSION 3.31)

project(Noxiouse VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compile the source files in parallel. With the Visual Studio generator and the ClangCL toolset MSBuild compiles a project's
# files one after another unless its multi-tool task is on (clang-cl has no /MP); with MSVC's own compiler /MP does it. Every
# target created below gets these (CMAKE_VS_GLOBALS sets their VS_GLOBAL_* properties).
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_VS_GLOBALS "UseMultiToolTask=true" "EnforceProcessCountAcrossBuilds=true")
endif()
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP>)
endif()

include(NoxCore/CMake/Dependencies.cmake)

set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "Use dynamic runtime for msdf-atlas-gen" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "Use dynamic runtime for msdfgen" FORCE)
add_subdirectory(NoxCore/vendors/msdf-atlas-gen EXCLUDE_FROM_ALL)

# Jolt Physics
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(USE_AVX2 ON CACHE BOOL "" FORCE)
# Large world coordinates (docs/Engine_Architecture_Plan_2026.md §5.14a): body positions in double precision (JPH_DOUBLE_PRECISION,
# public on the Jolt target, so the engine's Jolt code sees the same RVec3 = DVec3).
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
add_subdirectory(NoxCore/vendors/JoltPhysics/Build NoxCore/vendors/JoltPhysics/Build EXCLUDE_FROM_ALL)

# The shipping build of a game (docs/Runtime_And_Packaging_Plan_2026-10.md R7): build the Release configuration with it on.
# NoxRuntime then has no console window, no stats overlay / profiler, and logs into the user's folder (SDL_GetPrefPath)
# instead of the working directory; asserts are already off outside Debug. It builds NoxRuntime.
option(NOX_DIST "Shipping build of the runtime: no console, no profiler, log in the user's folder" OFF)
if(NOX_DIST)
    set(NOX_PROFILE_STATS OFF CACHE BOOL "" FORCE)
    set(NOX_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
    set(NOX_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
endif()

# The MCP server (docs/MCP_Plan_2026-11.md): AI tools read and use the running editor -- and the games it starts (NoxRuntime --mcp,
# not in NOX_DIST builds) -- over the Model Context Protocol. The shared part is the NoxMcp library, the editor's own tools are
# NoxEditor/src/Mcp; OFF leaves both out of the build (and the few NOX_WITH_MCP lines in the editor and the runtime).
option(NOX_WITH_MCP "Build the MCP server (editor: Tools > MCP Server, off at run time until enabled; NoxRuntime: --mcp)" ON)

# Standalone NoxTasks library (NoxCore/vendors/NoxTasks), reusing NoxCore/vendors/taskflow.
set(NOXTASKS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NOXTASKS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TASKFLOW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/taskflow" CACHE PATH "" FORCE)
add_subdirectory(NoxCore/vendors/NoxTasks)

# Standalone NoxNet library (NoxCore/vendors/NoxNet).
set(NOXNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(NoxCore/vendors/NoxNet)

# Standalone NoxAudio library (NoxCore/vendors/NoxAudio), reusing NoxCore/vendors/{miniaudio,steamaudio}.
set(NOXAUDIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NOXAUDIO_MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/miniaudio" CACHE PATH "" FORCE)
set(NOXAUDIO_STEAMAUDIO_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/steamaudio" CACHE PATH "" FORCE)
add_subdirectory(NoxCore/vendors/NoxAudio)

# Standalone NoxRHI library (NoxCore/vendors/NoxRHI), reusing centralized NoxCore/vendors/* SDKs.
set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRI_BUILD_HPP ON CACHE BOOL "" FORCE)
set(NRI_BUILD_RENDERGRAPH ON CACHE BOOL "" FORCE)
set(NRI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_SLANG ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_STREAMLINE ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_NRD ON CACHE BOOL "" FORCE)
set(NOXRHI_VMA_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/VulkanMemoryAllocator-Hpp" CACHE PATH "" FORCE)
set(NOXRHI_SLANG_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/slang" CACHE PATH "" FORCE)
set(NOXRHI_STREAMLINE_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/Streamline" CACHE PATH "" FORCE)
set(NOXRHI_NRD_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/NRD" CACHE PATH "" FORCE)
add_subdirectory(NoxCore/vendors/NoxRHI)

# Standalone NoxUI library (NoxCore/vendors/NoxUI), reusing root msdf-atlas-gen and NoxRHI.
set(NOXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NOXUI_BUILD_SDL3_BACKEND ON CACHE BOOL "" FORCE)
set(NOXUI_BUILD_NOXRHI_BACKEND ON CACHE BOOL "" FORCE)
set(NOXRHI_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/NoxRHI" CACHE PATH "" FORCE)
add_subdirectory(NoxCore/vendors/NoxUI)

add_subdirectory(NoxCore)
if(NOX_WITH_MCP)
    find_package(yaml-cpp REQUIRED)
    set(NOXMCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NOXMCP_HTTPLIB_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/cpp-httplib" CACHE PATH "" FORCE)
    set(NOXMCP_JSON_DIR "${CMAKE_SOURCE_DIR}/NoxCore/vendors/vcpkg/installed/x64-windows/include/nlohmann" CACHE PATH "" FORCE)
    add_subdirectory(NoxCore/vendors/NoxMCP)
    target_compile_definitions(NoxMcp PUBLIC NOXMCP_HAS_YAML_CPP=1)
    target_link_libraries(NoxMcp PUBLIC yaml-cpp::yaml-cpp)
    add_subdirectory(NoxMcp)
endif()
add_subdirectory(NoxEditor)

# The standalone player (docs/Runtime_And_Packaging_Plan_2026-10.md, R1). Off by default until it has been built and tried.
option(NOX_BUILD_RUNTIME "Build NoxRuntime, the standalone player (no editor)" ON)
if(NOX_BUILD_RUNTIME)
    add_subdirectory(NoxRuntime)
endif()

# Engine tests that run without a window or GPU (Tests/): NoxNetTest (servers and clients over loopback and UDP, NET1).
option(NOX_BUILD_TESTS "Build the engine's command-line tests (NoxNetTest, NoxToolsTest)" OFF)
if(NOX_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

