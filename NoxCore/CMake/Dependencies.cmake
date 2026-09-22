include(FetchContent)

# Coral is intentionally fetched into the build tree rather than checked into
# NoxCore/vendors. Pinning the commit makes fresh clones reproducible while
# FETCHCONTENT_SOURCE_DIR_CORAL still permits an offline/local override.
FetchContent_Declare(
    Coral
    GIT_REPOSITORY https://github.com/StudioCherno/Coral.git
    GIT_TAG d53b2685725f7535bc4d1deaa8a22bf16d112fe2
    GIT_SHALLOW FALSE
    # Populate first; Nox applies a small compatibility fix before adding
    # Coral's CMake project (see below).
    SOURCE_SUBDIR _nox_no_cmake_project
    SYSTEM
    EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(Coral)

# Coral d53b268 requires .NET 9+, but its CMake test accidentally excludes
# exactly 9.0.0 by using VERSION_GREATER. Remove this once the pinned revision
# contains the upstream fix.
set(_coral_cmake_file "${coral_SOURCE_DIR}/cmake/CMakeLists.txt")
file(READ "${_coral_cmake_file}" _coral_cmake_contents)
string(REPLACE
    "ver VERSION_GREATER \"9.0.0\""
    "ver VERSION_GREATER_EQUAL \"9.0.0\""
    _coral_cmake_contents
    "${_coral_cmake_contents}"
)
file(WRITE "${_coral_cmake_file}" "${_coral_cmake_contents}")

# MSVC 19.51 deletes narrow-stream insertion for wchar_t pointers. Coral's
# Windows UCChar is wchar_t, so use the matching diagnostic stream.
if(WIN32)
    set(_coral_host_file
        "${coral_SOURCE_DIR}/Coral.Native/Source/Coral/HostInstance.cpp")
    file(READ "${_coral_host_file}" _coral_host_contents)
    string(REPLACE
        "std::cerr << \"Failed to retrieve managed function pointer"
        "std::wcerr << L\"Failed to retrieve managed function pointer"
        _coral_host_contents
        "${_coral_host_contents}"
    )
    file(WRITE "${_coral_host_file}" "${_coral_host_contents}")
endif()

set(CORAL_EXAMPLE OFF CACHE BOOL "Build Coral examples" FORCE)
set(CORAL_TESTING OFF CACHE BOOL "Build Coral tests" FORCE)
add_subdirectory("${coral_SOURCE_DIR}/cmake" "${coral_BINARY_DIR}")

set(NOX_CORAL_RUNTIME_DIR "${coral_BINARY_DIR}" CACHE INTERNAL
    "Directory containing Coral.Managed runtime artifacts")
