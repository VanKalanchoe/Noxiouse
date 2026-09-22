cmake_minimum_required(VERSION 3.31)

project(Noxiouse VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_subdirectory(NoxCore)
add_subdirectory(NoxEditor)
