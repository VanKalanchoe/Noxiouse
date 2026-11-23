# NoxEngineMcp: engine-specific MCP tools and host helpers shared by NoxEditor (Tools > MCP Server) and NoxRuntime (--mcp).
# Core MCP protocol, HTTP server, client, validation, token files, and InputPlayer come from external/NoxMCP (NoxMcp::Core).
add_library(NoxEngineMcp STATIC)
file(GLOB_RECURSE NOXENGINEMCP_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)
target_sources(NoxEngineMcp PRIVATE ${NOXENGINEMCP_FILES})
target_link_libraries(NoxEngineMcp PUBLIC NoxMcp::Core NoxCore)
target_include_directories(NoxEngineMcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/Mcp")
# NoxCore's precompiled header (NoxCore/src/NoxPCH.h), built for this target's own compile flags.
target_precompile_headers(NoxEngineMcp PRIVATE "${CMAKE_SOURCE_DIR}/NoxCore/src/NoxPCH.h")
