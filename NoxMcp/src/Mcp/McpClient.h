#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "McpProtocol.h"

// A client for another Nox MCP server on this machine -- the editor forwarding a call to a NoxRuntime it started with --mcp
// (runtime_call). Plain JSON-RPC over HTTP to http://127.0.0.1:<port>/mcp with the project's token; blocking, so run it on a thread
// of its own (never the main thread: the other process may be waiting for this one).
namespace Nox::Mcp
{
    struct ClientReply
    {
        std::string Error; // empty: `Result` holds the JSON-RPC result (a transport or protocol failure otherwise)
        Json Result;
    };

    // One request ("tools/list", "tools/call" ...); `timeoutSeconds` for the whole answer.
    ClientReply CallServer(uint16_t port, const std::string& token, const std::string& method, const Json& params, int timeoutSeconds);

    // A tools/call result (content, structuredContent, isError) as this server's own answer: forwarded as it came.
    ToolResult ToolResultFromJson(const Json& result);

    // Whether a server another process was asked to open (NoxRuntime --mcp) really answers: a thread of its own asks it for its
    // tools every half second until it answers (Up) or `timeoutSeconds` pass (Absent: an old NoxRuntime that ignores --mcp, or a game
    // whose server could not start -- its log says why), and logs which, naming `name`. Cancel (Play stopped) ends it quietly.
    class ServerProbe
    {
    public:
        enum class State : int { Starting = 0, Up, Absent, Cancelled };
        static std::shared_ptr<ServerProbe> Start(std::string name, uint16_t port, std::string token, double timeoutSeconds);
        State Get() const { return static_cast<State>(m_State.load()); }
        void Cancel() { m_Cancelled = true; }

    private:
        std::atomic<int> m_State{ 0 };
        std::atomic<bool> m_Cancelled{ false };
    };
    const char* ToString(ServerProbe::State state); // "starting", "ready", "not listening", "stopped"
}
