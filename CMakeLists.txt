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

add_subdirectory(NoxCore)
add_subdirectory(NoxEditor)

# The standalone player (docs/Runtime_And_Packaging_Plan_2026-10.md, R1). Off by default until it has been built and tried.
option(NOX_BUILD_RUNTIME "Build NoxRuntime, the standalone player (no editor)" OFF)
if(NOX_BUILD_RUNTIME)
    add_subdirectory(NoxRuntime)
endif()
