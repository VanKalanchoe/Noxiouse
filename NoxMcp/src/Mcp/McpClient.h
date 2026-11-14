#pragma once
#include <cstdint>
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
}
