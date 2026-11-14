# NoxMcp: the MCP server shared by NoxEditor (Tools > MCP Server) and NoxRuntime (--mcp) -- the protocol, the HTTP transport, the
# project's token, the client the editor forwards calls with, and the tools both offer (docs/MCP_Plan_2026-11.md, MCP5). Built when
# NOX_WITH_MCP is ON; the editor's own tools stay in NoxEditor/src/Mcp.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/NoxCore/vendors/cpp-httplib/httplib.h")
    message(FATAL_ERROR "NoxCore/vendors/cpp-httplib is missing (a git submodule, the MCP server's HTTP library): run "
                        "'git submodule update --init NoxCore/vendors/cpp-httplib', "
                        "or configure with -DNOX_WITH_MCP=OFF")
endif()

add_library(NoxMcp STATIC)
file(GLOB_RECURSE NOXMCP_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)
target_sources(NoxMcp PRIVATE ${NOXMCP_FILES})
target_link_libraries(NoxMcp PUBLIC NoxCore)
target_include_directories(NoxMcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/Mcp")
target_include_directories(NoxMcp PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/vendors/cpp-httplib")
# NoxCore's precompiled header (NoxCore/src/NoxPCH.h), built for this target's own compile flags.
target_precompile_headers(NoxMcp PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")
