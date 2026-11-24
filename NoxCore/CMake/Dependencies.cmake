include(FetchContent)

# Coral is intentionally fetched into the build tree rather than checked into
# NoxCore/vendors. Pinning the commit makes fresh clones reproducible while
# FETCHCONTENT_SOURCE_DIR_CORAL still permits an offline/local override.
FetchContent_Declare(
    Coral
    GIT_REPOSITORY https://github.com/StudioCherno/Coral.git
    GIT_TAG 320c90d
    GIT_SHALLOW FALSE
    # Coral's CMakeLists.txt lives in cmake/, not the repo root.
    SOURCE_SUBDIR _nox_no_cmake_project
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(Coral)

# Build Coral.Managed via `dotnet build` rather than `include_external_msproject` so the C++ Visual Studio solution
# does not require the VS .NET workload (and Rider's VS MSBuild does not fail on net10.0 workload imports).
set(_coral_cmake_file "${coral_SOURCE_DIR}/cmake/CMakeLists.txt")
file(READ "${_coral_cmake_file}" _coral_cmake_contents)
string(REPLACE
    "set(CORAL_MANAGED_IN_SOLUTION ON)"
    "set(CORAL_MANAGED_IN_SOLUTION OFF)"
    _coral_cmake_contents
    "${_coral_cmake_contents}"
)
file(WRITE "${_coral_cmake_file}" "${_coral_cmake_contents}")

set(CORAL_EXAMPLE OFF CACHE BOOL "Build Coral examples" FORCE)
set(CORAL_TESTING OFF CACHE BOOL "Build Coral tests" FORCE)
add_subdirectory("${coral_SOURCE_DIR}/cmake" "${coral_BINARY_DIR}")

set(NOX_CORAL_RUNTIME_DIR "${coral_SOURCE_DIR}/Build/$<CONFIG>" CACHE INTERNAL
    "Directory containing Coral.Managed runtime artifacts")


# D3D12MemoryAllocator (GPUOpen) for Direct3D 12 suballocation and memory budgeting
if(WIN32)
    FetchContent_Declare(
        D3D12MemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git
        GIT_TAG v3.3.0
        GIT_SHALLOW TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(D3D12MemoryAllocator)

    if(NOT TARGET D3D12MemoryAllocator AND EXISTS "${d3d12memoryallocator_SOURCE_DIR}/src/D3D12MemAlloc.cpp")
        add_library(D3D12MemoryAllocator STATIC
            "${d3d12memoryallocator_SOURCE_DIR}/src/D3D12MemAlloc.cpp"
        )
        target_include_directories(D3D12MemoryAllocator SYSTEM PUBLIC
            "${d3d12memoryallocator_SOURCE_DIR}/include"
        )
        target_link_libraries(D3D12MemoryAllocator PUBLIC d3d12 dxgi)
    endif()
endif()
