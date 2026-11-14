# The engine's command-line tests (-DNOX_BUILD_TESTS=ON): no window, no GPU; exit code 0 when every check passed.

# NoxNetTest: servers and clients over the loopback and GameNetworkingSockets transports (docs/Networking_Plan_2026-11.md, NET1).
add_executable(NoxNetTest NetTest.cpp)
target_link_libraries(NoxNetTest PRIVATE NoxCore)
target_precompile_headers(NoxNetTest PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")
# Next to NoxEditor.exe, where the DLLs it loads already are (GameNetworkingSockets and its dependencies, copied by vcpkg).
set_target_properties(NoxNetTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:NoxEditor>")
add_dependencies(NoxNetTest NoxEditor)

# NoxToolsTest: the engine console's registry (docs/MCP_Plan_2026-11.md §4) and the MCP protocol layer, without the editor.
add_executable(NoxToolsTest ToolsTest.cpp)
target_link_libraries(NoxToolsTest PRIVATE NoxCore)
target_precompile_headers(NoxToolsTest PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")
set_target_properties(NoxToolsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:NoxEditor>")
add_dependencies(NoxToolsTest NoxEditor)
# (with the editor's MCP protocol layer when it is built: NOX_WITH_MCP)
if(NOX_WITH_MCP)
    target_sources(NoxToolsTest PRIVATE "${CMAKE_SOURCE_DIR}/NoxEditor/src/Mcp/McpProtocol.cpp" "${CMAKE_SOURCE_DIR}/NoxEditor/src/Mcp/McpServer.cpp"
                                        "${CMAKE_SOURCE_DIR}/NoxEditor/src/Mcp/McpValidation.cpp")
    target_include_directories(NoxToolsTest PRIVATE "${CMAKE_SOURCE_DIR}/NoxEditor/src/Mcp" "${CMAKE_SOURCE_DIR}/Tests"
                               "${CMAKE_SOURCE_DIR}/NoxCore/vendors/cpp-httplib")
    target_compile_definitions(NoxToolsTest PRIVATE NOX_TOOLS_TEST_MCP=1 NOX_TOOLS_TEST_MCP_SERVER=1)
endif()
