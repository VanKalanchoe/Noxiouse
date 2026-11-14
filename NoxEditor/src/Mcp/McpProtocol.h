#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp" // nlohmann/json (tinygltf's copy, NoxCore/vendors/tinygltf)

#include "NoxCore/Console/Console.h"

// MCP for NoxEditor (docs/MCP_Plan_2026-11.md): the protocol layer -- JSON-RPC 2.0 over Streamable HTTP, revision 2025-06-18 (and
// 2025-03-26 accepted), plain JSON responses. It checks a request (token, Host, Origin, protocol version), answers initialize /
// ping / tools/list / tools/call and hands a tool call to a dispatcher (the editor runs it on its main thread between frames). It
// knows nothing of the editor: the tools come in from outside (McpEditorTools), the transport is McpServer. Testable alone
// (NoxToolsTest).
namespace Nox::Mcp
{
    using Json = nlohmann::json;

    constexpr const char* LatestProtocolVersion = "2025-06-18";
    // The revisions this server speaks, newest first.
    const std::vector<std::string>& SupportedProtocolVersions();

    // A tool's answer: content blocks for the model (text, images), optionally the same as structured JSON, and whether it failed
    // (a failed tool is an answer the model reads and corrects itself from, not a protocol error).
    struct ToolResult
    {
        Json Content = Json::array();
        Json Structured;      // null: none
        bool IsError = false;

        static ToolResult Text(const std::string& text);
        static ToolResult Error(const std::string& text);
        // The value as structured content and as indented text (clients that only read text get it too).
        static ToolResult Data(const Json& value);
        ToolResult& AddImage(const std::string& base64, const std::string& mimeType);
        Json ToJson() const;
    };

    using ToolDone = std::function<void(ToolResult)>;

    struct Tool
    {
        std::string Name;
        std::string Title;
        std::string Description;
        Json InputSchema = Json{ { "type", "object" }, { "properties", Json::object() } };
        ConsoleAccess Access = ConsoleAccess::Read; // what the client must be granted to see and call it
        bool Destructive = false;                   // MCP's destructiveHint
        // Runs the call; `done` is called once with the result, now or later (a screenshot answers a frame or two after).
        std::function<void(const Json& arguments, ToolDone done)> Handler;
    };

    // A builder for a tool's input schema (JSON Schema, the subset the validator checks: top-level property types and required).
    class Schema
    {
    public:
        Schema& String(const std::string& name, const std::string& description, bool required = false);
        Schema& Integer(const std::string& name, const std::string& description, bool required = false, std::optional<int64_t> minimum = {},
                        std::optional<int64_t> maximum = {});
        Schema& Number(const std::string& name, const std::string& description, bool required = false, std::optional<double> minimum = {},
                       std::optional<double> maximum = {});
        Schema& Boolean(const std::string& name, const std::string& description, bool required = false);
        // A list of `itemType` ("string", "number", "integer"); the validator checks the list, the handler its items.
        Schema& Array(const std::string& name, const std::string& description, const std::string& itemType, bool required = false);
        Schema& Enum(const std::string& name, const std::string& description, const std::vector<std::string>& values, bool required = false);
        Json Build() const { return m_Schema; }
        operator Json() const { return m_Schema; }

    private:
        void Add(const std::string& name, Json property, bool required);
        Json m_Schema = Json{ { "type", "object" }, { "properties", Json::object() }, { "additionalProperties", false } };
    };

    struct HttpRequest
    {
        std::string Method;                         // "POST", "GET", ...
        std::map<std::string, std::string> Headers; // lower-case names
        std::string Body;
    };

    struct HttpReply
    {
        int Status = 200;
        std::string ContentType = "application/json";
        std::map<std::string, std::string> Headers;
        std::string Body;
    };

    struct Settings
    {
        std::string Token;                         // the bearer token clients must send
        uint16_t Port = 8765;                      // (for the Host / Origin checks)
        ConsoleAccess Access = ConsoleAccess::Write; // granted to clients
        std::string ServerName = "nox-editor";
        std::string ServerTitle = "NoxEditor";
        std::string ServerVersion = "0.1";
        std::string Instructions;                  // initialize's instructions (how to use the tools)
    };

    class Protocol
    {
    public:
        explicit Protocol(Settings settings);

        void AddTool(Tool tool);
        const std::vector<Tool>& GetTools() const { return m_Tools; }
        void SetAccess(ConsoleAccess access);
        ConsoleAccess GetAccess() const;

        // Runs a tool and returns its result (blocking the calling HTTP thread, never the editor). The default calls the handler on
        // the calling thread; the editor queues it to its main thread.
        using Dispatcher = std::function<ToolResult(const Tool& tool, const Json& arguments)>;
        void SetDispatcher(Dispatcher dispatcher) { m_Dispatcher = std::move(dispatcher); }

        // One HTTP request to the MCP endpoint (thread-safe: several clients' requests may arrive at once).
        HttpReply Handle(const HttpRequest& request);

        size_t GetCallCount() const { return m_Calls; }

        // A tool's arguments against its schema: the first problem, or empty when they fit.
        static std::string Validate(const Json& schema, const Json& arguments);

    private:
        std::optional<HttpReply> CheckRequest(const HttpRequest& request) const;
        Json HandleRequest(const Json& message);
        Json CallTool(const Json& params, const Json& id);

        Settings m_Settings;
        mutable std::mutex m_SettingsMutex;
        std::vector<Tool> m_Tools;
        Dispatcher m_Dispatcher;
        std::atomic<size_t> m_Calls{ 0 };
    };

    // JSON-RPC 2.0 error codes.
    namespace RpcError
    {
        constexpr int ParseError = -32700;
        constexpr int InvalidRequest = -32600;
        constexpr int MethodNotFound = -32601;
        constexpr int InvalidParams = -32602;
        constexpr int InternalError = -32603;
    }

    // Compares two secrets in time that does not depend on where they differ.
    bool ConstantTimeEquals(const std::string& a, const std::string& b);
    // 32 random bytes from the operating system's cryptographic generator, as 64 hex characters (empty when it is not available).
    std::string GenerateToken();
    std::string Base64(const uint8_t* data, size_t size);
}
