#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "McpHost.h"
#include "McpServer.h"

namespace Nox
{
    struct ApplicationCommandLineArgs;
}

namespace Nox::Mcp
{
    // NoxRuntime's MCP server (docs/MCP_Plan_2026-11.md, MCP5): an AI tool reads and drives the running game -- a player window or a
    // dedicated server -- the way it drives the editor's Play: the log, the scene's entities, screenshots of the game's view, the
    // console, input, waiting, pause; plus get_runtime_status and quit. Off unless the command line asks for it:
    //
    //   --mcp                 the server on 127.0.0.1 (loopback only), with the project's token (<Project>/.nox/mcp-token, the
    //                         same one the editor's server uses; created when there is none)
    //   --mcp-port=<port>     default 8770 (the editor's Play gives each player its own: its MCP port + 1 for the dedicated
    //                         server, + n for player n)
    //   --mcp-access=<level>  read, write (default) or execute -- what a client may do (execute: run_command's execute
    //                         commands and quit)
    //
    // Not built into shipping runtimes (NOX_DIST) nor without NOX_WITH_MCP (CMake).
    class McpRuntime
    {
    public:
        // What the game tells the server about itself (get_runtime_status).
        struct Status
        {
            std::string Role;         // "standalone", "dedicated server", "listen server", "client"
            std::string Session;      // the network session's line, empty when none
            std::string ScenePath;    // the scene played, under the asset directory
            std::string PlayerName;   // --name
            float FramesPerSecond = 0.0f;
        };

        // The server as the command line says: none without --mcp (or when it cannot start: the reason is logged).
        static std::unique_ptr<McpRuntime> FromCommandLine(const ApplicationCommandLineArgs& args, HostContext context,
                                                           std::function<Status()> status, std::function<void()> quit);
        ~McpRuntime();
        McpRuntime(const McpRuntime&) = delete;
        McpRuntime& operator=(const McpRuntime&) = delete;

        // Main thread, at the start of each frame: the tool calls that came in run here, and the tools answering later.
        void Update();
        uint16_t GetPort() const { return m_Port; }

        // "read" / "write" / "execute" (or 0 / 1 / 2) to an access level; false when it is none of them.
        static bool ParseAccess(const std::string& text, ConsoleAccess& access);

    private:
        McpRuntime() = default;

        McpServer m_Server;
        HostContext m_Context;
        ConsoleAccess m_GrantedAccess = ConsoleAccess::Write;
        std::vector<std::function<bool()>> m_Polls;
        uint16_t m_Port = 0;
    };
}
