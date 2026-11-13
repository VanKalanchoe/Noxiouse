#include "McpProtocol.h"

#include <algorithm>
#include <format>
#include <future>

#include "NoxCore/Core/Log.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fstream>
#endif

namespace Nox::Mcp
{
    const std::vector<std::string>& SupportedProtocolVersions()
    {
        static const std::vector<std::string> versions = { "2025-06-18", "2025-03-26" };
        return versions;
    }

    // ------------------------------------------------------------------------------------------------------------------ result

    ToolResult ToolResult::Text(const std::string& text)
    {
        ToolResult result;
        result.Content.push_back(Json{ { "type", "text" }, { "text", text } });
        return result;
    }

    ToolResult ToolResult::Error(const std::string& text)
    {
        ToolResult result = Text(text);
        result.IsError = true;
        return result;
    }

    ToolResult ToolResult::Data(const Json& value)
    {
        ToolResult result = Text(value.dump(1));
        result.Structured = value.is_object() ? value : Json{ { "value", value } };
        return result;
    }

    ToolResult& ToolResult::AddImage(const std::string& base64, const std::string& mimeType)
    {
        Content.push_back(Json{ { "type", "image" }, { "data", base64 }, { "mimeType", mimeType } });
        return *this;
    }

    Json ToolResult::ToJson() const
    {
        Json result{ { "content", Content }, { "isError", IsError } };
        if (!Structured.is_null())
            result["structuredContent"] = Structured;
        return result;
    }

    // ------------------------------------------------------------------------------------------------------------------ schema

    void Schema::Add(const std::string& name, Json property, bool required)
    {
        m_Schema["properties"][name] = std::move(property);
        if (required)
            m_Schema["required"].push_back(name);
    }

    Schema& Schema::String(const std::string& name, const std::string& description, bool required)
    {
        Add(name, Json{ { "type", "string" }, { "description", description } }, required);
        return *this;
    }

    Schema& Schema::Integer(const std::string& name, const std::string& description, bool required, std::optional<int64_t> minimum,
                            std::optional<int64_t> maximum)
    {
        Json property{ { "type", "integer" }, { "description", description } };
        if (minimum)
            property["minimum"] = *minimum;
        if (maximum)
            property["maximum"] = *maximum;
        Add(name, std::move(property), required);
        return *this;
    }

    Schema& Schema::Boolean(const std::string& name, const std::string& description, bool required)
    {
        Add(name, Json{ { "type", "boolean" }, { "description", description } }, required);
        return *this;
    }

    Schema& Schema::Enum(const std::string& name, const std::string& description, const std::vector<std::string>& values, bool required)
    {
        Add(name, Json{ { "type", "string" }, { "description", description }, { "enum", values } }, required);
        return *this;
    }

    std::string Protocol::Validate(const Json& schema, const Json& arguments)
    {
        if (!arguments.is_object())
            return "the arguments must be an object";
        const Json properties = schema.value("properties", Json::object());
        if (schema.contains("required"))
            for (const Json& name : schema["required"])
                if (!arguments.contains(name.get<std::string>()))
                    return std::format("'{}' is required", name.get<std::string>());
        for (auto it = arguments.begin(); it != arguments.end(); ++it)
        {
            if (!properties.contains(it.key()))
            {
                if (schema.value("additionalProperties", true) == false)
                    return std::format("unknown argument '{}'", it.key());
                continue;
            }
            const Json& property = properties[it.key()];
            const std::string type = property.value("type", "");
            const Json& value = it.value();
            const bool fits = type.empty() || (type == "string" && value.is_string()) ||
                              (type == "integer" && value.is_number_integer()) || (type == "number" && value.is_number()) ||
                              (type == "boolean" && value.is_boolean()) || (type == "object" && value.is_object()) ||
                              (type == "array" && value.is_array());
            if (!fits)
                return std::format("'{}' must be {} {}", it.key(), type == "integer" || type == "object" || type == "array" ? "an" : "a", type);
            if (property.contains("enum") && std::find(property["enum"].begin(), property["enum"].end(), value) == property["enum"].end())
                return std::format("'{}' must be one of {}", it.key(), property["enum"].dump());
            if (value.is_number())
            {
                if (property.contains("minimum") && value.get<double>() < property["minimum"].get<double>())
                    return std::format("'{}' must be at least {}", it.key(), property["minimum"].dump());
                if (property.contains("maximum") && value.get<double>() > property["maximum"].get<double>())
                    return std::format("'{}' must be at most {}", it.key(), property["maximum"].dump());
            }
        }
        return {};
    }

    // ---------------------------------------------------------------------------------------------------------------- protocol

    Protocol::Protocol(Settings settings)
        : m_Settings(std::move(settings))
    {
        // By default a tool runs on the calling thread and its result is awaited (tests; the editor replaces this).
        m_Dispatcher = [](const Tool& tool, const Json& arguments)
        {
            auto promise = std::make_shared<std::promise<ToolResult>>();
            std::future<ToolResult> future = promise->get_future();
            auto once = std::make_shared<std::once_flag>();
            tool.Handler(arguments, [promise, once](ToolResult result) { std::call_once(*once, [&] { promise->set_value(std::move(result)); }); });
            return future.get();
        };
    }

    void Protocol::AddTool(Tool tool)
    {
        m_Tools.push_back(std::move(tool));
    }

    void Protocol::SetAccess(ConsoleAccess access)
    {
        std::lock_guard lock(m_SettingsMutex);
        m_Settings.Access = access;
    }

    ConsoleAccess Protocol::GetAccess() const
    {
        std::lock_guard lock(m_SettingsMutex);
        return m_Settings.Access;
    }

    namespace
    {
        Json RpcErrorReply(const Json& id, int code, const std::string& message)
        {
            return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", Json{ { "code", code }, { "message", message } } } };
        }

        HttpReply JsonReply(int status, const Json& body)
        {
            HttpReply reply;
            reply.Status = status;
            reply.Body = body.dump();
            return reply;
        }

        HttpReply PlainReply(int status, const std::string& text)
        {
            HttpReply reply;
            reply.Status = status;
            reply.ContentType = "text/plain";
            reply.Body = text;
            return reply;
        }

        std::string HeaderOf(const HttpRequest& request, const char* name)
        {
            auto found = request.Headers.find(name);
            return found != request.Headers.end() ? found->second : std::string();
        }
    }

    std::optional<HttpReply> Protocol::CheckRequest(const HttpRequest& request) const
    {
        std::string token;
        uint16_t port = 0;
        {
            std::lock_guard lock(m_SettingsMutex);
            token = m_Settings.Token;
            port = m_Settings.Port;
        }
        // Host: this machine's loopback name and port only (a web page that rebinds a domain to 127.0.0.1 sends its own host).
        const std::string host = HeaderOf(request, "host");
        const std::string portText = std::to_string(port);
        if (host != "127.0.0.1:" + portText && host != "localhost:" + portText && host != "[::1]:" + portText)
            return PlainReply(403, "Forbidden: the Host header must be 127.0.0.1 or localhost with this port");
        // Origin: none (a command-line client) or this server's own; any web page's origin is refused.
        const std::string origin = HeaderOf(request, "origin");
        if (!origin.empty() && origin != "http://127.0.0.1:" + portText && origin != "http://localhost:" + portText)
            return PlainReply(403, "Forbidden: requests from web pages are not accepted");
        // The bearer token (compared in constant time; never logged).
        const std::string authorization = HeaderOf(request, "authorization");
        constexpr std::string_view bearer = "Bearer ";
        if (token.empty() || authorization.size() <= bearer.size() || authorization.compare(0, bearer.size(), bearer) != 0 ||
            !ConstantTimeEquals(authorization.substr(bearer.size()), token))
        {
            HttpReply reply = PlainReply(401, "Unauthorized: send 'Authorization: Bearer <token>' (the token is in the project's .nox/mcp-token)");
            reply.Headers["WWW-Authenticate"] = "Bearer";
            return reply;
        }
        // A protocol revision named in the header must be one this server speaks.
        const std::string version = HeaderOf(request, "mcp-protocol-version");
        if (!version.empty())
        {
            const auto& supported = SupportedProtocolVersions();
            if (std::find(supported.begin(), supported.end(), version) == supported.end())
                return PlainReply(400, std::format("Bad Request: unsupported MCP-Protocol-Version '{}'", version));
        }
        return std::nullopt;
    }

    HttpReply Protocol::Handle(const HttpRequest& request)
    {
        if (std::optional<HttpReply> refused = CheckRequest(request))
            return *refused;
        // Only POST: no server-to-client stream (GET) and no sessions to end (DELETE).
        if (request.Method != "POST")
        {
            HttpReply reply = PlainReply(405, "Method Not Allowed: this server answers POST requests with JSON");
            reply.Headers["Allow"] = "POST";
            return reply;
        }
        Json message;
        try
        {
            message = Json::parse(request.Body);
        }
        catch (const std::exception&)
        {
            return JsonReply(400, RpcErrorReply(nullptr, RpcError::ParseError, "Parse error: the body is not JSON"));
        }
        if (message.is_array())
            return JsonReply(400, RpcErrorReply(nullptr, RpcError::InvalidRequest, "Batches are not supported (MCP 2025-06-18)"));
        if (!message.is_object() || message.value("jsonrpc", "") != "2.0")
            return JsonReply(400, RpcErrorReply(nullptr, RpcError::InvalidRequest, "Invalid Request: not a JSON-RPC 2.0 message"));

        // A notification or a response (no method, or no id): accepted, nothing to answer.
        if (!message.contains("id") || !message.contains("method"))
        {
            HttpReply reply;
            reply.Status = 202;
            reply.ContentType.clear();
            return reply;
        }
        return JsonReply(200, HandleRequest(message));
    }

    Json Protocol::HandleRequest(const Json& message)
    {
        const Json& id = message["id"];
        if (!id.is_string() && !id.is_number_integer())
            return RpcErrorReply(nullptr, RpcError::InvalidRequest, "Invalid Request: the id must be a string or an integer");
        if (!message["method"].is_string())
            return RpcErrorReply(id, RpcError::InvalidRequest, "Invalid Request: the method must be a string");
        const std::string method = message["method"].get<std::string>();
        const Json params = message.value("params", Json::object());

        if (method == "initialize")
        {
            // The client's revision when this server speaks it, else the newest this server speaks (the client decides whether it can).
            const std::string requested = params.value("protocolVersion", "");
            const auto& supported = SupportedProtocolVersions();
            const std::string version = std::find(supported.begin(), supported.end(), requested) != supported.end() ? requested : supported.front();
            Settings settings;
            {
                std::lock_guard lock(m_SettingsMutex);
                settings = m_Settings;
            }
            Json result{ { "protocolVersion", version },
                         { "capabilities", Json{ { "tools", Json{ { "listChanged", false } } } } },
                         { "serverInfo", Json{ { "name", settings.ServerName }, { "title", settings.ServerTitle }, { "version", settings.ServerVersion } } } };
            if (!settings.Instructions.empty())
                result["instructions"] = settings.Instructions;
            const std::string client = params.contains("clientInfo") ? params["clientInfo"].value("name", "a client") : std::string("a client");
            NOX_CORE_INFO("MCP: {} connected (protocol {})", client, version);
            return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
        }
        if (method == "ping")
            return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", Json::object() } };
        if (method == "tools/list")
        {
            // Only the tools the client may call (the granted access).
            const ConsoleAccess access = GetAccess();
            Json tools = Json::array();
            for (const Tool& tool : m_Tools)
            {
                if (static_cast<uint8_t>(tool.Access) > static_cast<uint8_t>(access))
                    continue;
                Json entry{ { "name", tool.Name }, { "description", tool.Description }, { "inputSchema", tool.InputSchema } };
                if (!tool.Title.empty())
                    entry["title"] = tool.Title;
                entry["annotations"] = Json{ { "readOnlyHint", tool.Access == ConsoleAccess::Read }, { "destructiveHint", tool.Destructive } };
                tools.push_back(std::move(entry));
            }
            return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", Json{ { "tools", tools } } } };
        }
        if (method == "tools/call")
            return CallTool(params, id);
        return RpcErrorReply(id, RpcError::MethodNotFound, std::format("Method not found: '{}'", method));
    }

    Json Protocol::CallTool(const Json& params, const Json& id)
    {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
            return RpcErrorReply(id, RpcError::InvalidParams, "tools/call needs a tool name");
        const std::string name = params["name"].get<std::string>();
        const Json arguments = params.value("arguments", Json::object());
        auto tool = std::find_if(m_Tools.begin(), m_Tools.end(), [&](const Tool& candidate) { return candidate.Name == name; });
        if (tool == m_Tools.end())
            return RpcErrorReply(id, RpcError::InvalidParams, std::format("Unknown tool: '{}'", name));

        ToolResult result;
        const ConsoleAccess access = GetAccess();
        if (static_cast<uint8_t>(tool->Access) > static_cast<uint8_t>(access))
            result = ToolResult::Error(std::format("{} needs {} access; this editor grants {} (Tools > MCP Server)", name, ToString(tool->Access),
                                                   ToString(access)));
        else if (const std::string problem = Validate(tool->InputSchema, arguments); !problem.empty())
            result = ToolResult::Error(std::format("{}: {}", name, problem));
        else
        {
            ++m_Calls;
            // What was called, for the user (the arguments shortened; nothing secret travels in them).
            std::string shown = arguments.dump();
            if (shown.size() > 160)
                shown = shown.substr(0, 157) + "...";
            NOX_CORE_INFO("MCP: {} {}", name, shown == "{}" ? std::string() : shown);
            try
            {
                result = m_Dispatcher(*tool, arguments);
            }
            catch (const std::exception& error)
            {
                result = ToolResult::Error(std::format("{} failed: {}", name, error.what()));
            }
        }
        return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result.ToJson() } };
    }

    // ----------------------------------------------------------------------------------------------------------------- helpers

    bool ConstantTimeEquals(const std::string& a, const std::string& b)
    {
        // (the length is not secret: tokens have a fixed length)
        if (a.size() != b.size())
            return false;
        unsigned char difference = 0;
        for (size_t index = 0; index < a.size(); ++index)
            difference |= static_cast<unsigned char>(a[index] ^ b[index]);
        return difference == 0;
    }

    std::string GenerateToken()
    {
        uint8_t bytes[32] = {};
#ifdef _WIN32
        if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return {};
#else
        std::ifstream random("/dev/urandom", std::ios::binary);
        if (!random.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
            return {};
#endif
        static constexpr char hex[] = "0123456789abcdef";
        std::string token;
        token.reserve(64);
        for (uint8_t byte : bytes)
        {
            token += hex[byte >> 4];
            token += hex[byte & 15];
        }
        return token;
    }

    std::string Base64(const uint8_t* data, size_t size)
    {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((size + 2) / 3 * 4);
        size_t index = 0;
        for (; index + 2 < size; index += 3)
        {
            const uint32_t triple = (uint32_t(data[index]) << 16) | (uint32_t(data[index + 1]) << 8) | data[index + 2];
            out += alphabet[(triple >> 18) & 63];
            out += alphabet[(triple >> 12) & 63];
            out += alphabet[(triple >> 6) & 63];
            out += alphabet[triple & 63];
        }
        if (index < size)
        {
            const uint32_t triple = (uint32_t(data[index]) << 16) | (index + 1 < size ? uint32_t(data[index + 1]) << 8 : 0u);
            out += alphabet[(triple >> 18) & 63];
            out += alphabet[(triple >> 12) & 63];
            out += index + 1 < size ? alphabet[(triple >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }
}
