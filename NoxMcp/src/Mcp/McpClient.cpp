#include "McpClient.h"

#include <atomic>
#include <format>

#include "httplib.h"

namespace Nox::Mcp
{
    ClientReply CallServer(uint16_t port, const std::string& token, const std::string& method, const Json& params, int timeoutSeconds)
    {
        static std::atomic<uint64_t> nextId{ 1 };
        ClientReply reply;
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(2, 0);
        client.set_read_timeout(timeoutSeconds, 0);
        client.set_write_timeout(5, 0);
        const httplib::Headers headers{ { "Authorization", "Bearer " + token }, { "Accept", "application/json, text/event-stream" },
                                        { "MCP-Protocol-Version", LatestProtocolVersion } };
        const Json request{ { "jsonrpc", "2.0" }, { "id", static_cast<int64_t>(nextId++) }, { "method", method }, { "params", params } };
        const httplib::Result response = client.Post("/mcp", headers, request.dump(), "application/json");
        if (!response)
        {
            reply.Error = std::format("no answer from 127.0.0.1:{} ({}) -- is that game running with --mcp?", port, httplib::to_string(response.error()));
            return reply;
        }
        if (response->status != 200)
        {
            reply.Error = std::format("127.0.0.1:{} answered HTTP {}: {}", port, response->status, response->body.substr(0, 300));
            return reply;
        }
        Json message;
        try
        {
            message = Json::parse(response->body);
        }
        catch (const std::exception&)
        {
            reply.Error = std::format("127.0.0.1:{} did not answer JSON", port);
            return reply;
        }
        if (message.contains("error"))
        {
            const Json& error = message["error"];
            reply.Error = std::format("127.0.0.1:{}: {}", port, error.is_object() ? error.value("message", error.dump()) : error.dump());
            return reply;
        }
        if (!message.contains("result"))
        {
            reply.Error = std::format("127.0.0.1:{} answered without a result", port);
            return reply;
        }
        reply.Result = message["result"];
        return reply;
    }

    ToolResult ToolResultFromJson(const Json& result)
    {
        ToolResult tool;
        if (result.is_object())
        {
            if (result.contains("content") && result["content"].is_array())
                tool.Content = result["content"];
            if (result.contains("structuredContent"))
                tool.Structured = result["structuredContent"];
            tool.IsError = result.value("isError", false);
        }
        return tool;
    }
}
