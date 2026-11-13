# The engine's command-line tests (-DNOX_BUILD_TESTS=ON): no window, no GPU; exit code 0 when every check passed.

# NoxNetTest: servers and clients over the loopback and GameNetworkingSockets transports (docs/Networking_Plan_2026-11.md, NET1).
add_executable(NoxNetTest NetTest.cpp)
target_link_libraries(NoxNetTest PRIVATE NoxCore)
target_precompile_headers(NoxNetTest PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")
# Next to NoxEditor.exe, where the DLLs it loads already are (GameNetworkingSockets and its dependencies, copied by vcpkg).
set_target_properties(NoxNetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:NoxEditor>")
add_dependencies(NoxNetTest NoxEditor)
