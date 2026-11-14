// The MCP protocol layer (NoxEditor/src/Mcp/McpProtocol): HTTP checks, JSON-RPC, version negotiation, tools, validation, access.
// Included by ToolsTest.cpp when NOX_TOOLS_TEST_MCP is defined (the editor's MCP sources are part of the test then).
#include "McpProtocol.h"

#include <thread>

namespace McpTest
{
    using namespace Nox::Mcp;
    constexpr const char* Token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    HttpRequest Post(const Json& body, const char* token = Token, const char* host = "127.0.0.1:8765")
    {
        HttpRequest request;
        request.Method = "POST";
        request.Headers["host"] = host;
        if (token)
            request.Headers["authorization"] = std::string("Bearer ") + token;
        request.Headers["content-type"] = "application/json";
        request.Body = body.dump();
        return request;
    }

    Json Rpc(const std::string& method, const Json& params = Json::object(), int id = 1)
    {
        return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } };
    }

    Json BodyOf(const HttpReply& reply)
    {
        try { return Json::parse(reply.Body); } catch (...) { return Json(); }
    }
}

static void TestMcp()
{
    using namespace McpTest;
    std::printf("\n== MCP protocol\n");

    Settings settings;
    settings.Token = Token;
    settings.Port = 8765;
    settings.Access = Nox::ConsoleAccess::Write;
    settings.Instructions = "Test server";
    Protocol protocol(settings);

    Tool echo;
    echo.Name = "echo";
    echo.Description = "Repeats a text";
    echo.InputSchema = Schema().String("text", "What to repeat", true).Integer("times", "How often", false, 1, 5);
    echo.Handler = [](const Json& arguments, ToolDone done)
    {
        std::string text;
        for (int index = 0; index < arguments.value("times", 1); ++index)
            text += arguments["text"].get<std::string>();
        done(ToolResult::Data(Json{ { "text", text } }));
    };
    protocol.AddTool(echo);
    Tool later;
    later.Name = "later";
    later.Description = "Answers from another thread a little later (like a screenshot a frame after)";
    later.Handler = [](const Json&, ToolDone done)
    {
        std::thread([done]() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); done(ToolResult::Text("late")); }).detach();
    };
    protocol.AddTool(later);
    Tool quit;
    quit.Name = "quit";
    quit.Description = "Needs execute access";
    quit.Access = Nox::ConsoleAccess::Execute;
    quit.Destructive = true;
    quit.Handler = [](const Json&, ToolDone done) { done(ToolResult::Text("quit")); };
    protocol.AddTool(quit);

    // HTTP checks: token, Host, Origin, method, version header.
    HttpReply reply = protocol.Handle(Post(Rpc("ping"), nullptr));
    CHECK(reply.Status == 401 && reply.Headers.count("WWW-Authenticate"), "no token: 401 ({})", reply.Status);
    reply = protocol.Handle(Post(Rpc("ping"), "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeX"));
    CHECK(reply.Status == 401, "a wrong token: 401 ({})", reply.Status);
    reply = protocol.Handle(Post(Rpc("ping"), Token, "evil.example:8765"));
    CHECK(reply.Status == 403, "another Host (DNS rebinding): 403 ({})", reply.Status);
    HttpRequest fromPage = Post(Rpc("ping"));
    fromPage.Headers["origin"] = "https://evil.example";
    CHECK(protocol.Handle(fromPage).Status == 403, "a web page's Origin: 403");
    HttpRequest ownOrigin = Post(Rpc("ping"));
    ownOrigin.Headers["origin"] = "http://localhost:8765";
    ownOrigin.Headers["host"] = "localhost:8765";
    CHECK(protocol.Handle(ownOrigin).Status == 200, "its own origin and localhost: accepted");
    HttpRequest get = Post(Rpc("ping"));
    get.Method = "GET";
    reply = protocol.Handle(get);
    CHECK(reply.Status == 405 && reply.Headers["Allow"] == "POST", "GET (no event stream): 405, Allow: POST");
    HttpRequest oldVersion = Post(Rpc("ping"));
    oldVersion.Headers["mcp-protocol-version"] = "2024-11-05";
    CHECK(protocol.Handle(oldVersion).Status == 400, "an unsupported MCP-Protocol-Version header: 400");

    // JSON-RPC: parse errors, batches, notifications.
    HttpRequest garbage = Post(Json::object());
    garbage.Body = "{not json";
    reply = protocol.Handle(garbage);
    CHECK(reply.Status == 400 && BodyOf(reply)["error"]["code"] == RpcError::ParseError, "not JSON: parse error -32700");
    reply = protocol.Handle(Post(Json::array({ Rpc("ping") })));
    CHECK(BodyOf(reply)["error"]["code"] == RpcError::InvalidRequest, "a batch: invalid request (none in 2025-06-18)");
    reply = protocol.Handle(Post(Json{ { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }));
    CHECK(reply.Status == 202 && reply.Body.empty(), "a notification: 202 without a body ({})", reply.Status);
    reply = protocol.Handle(Post(Rpc("nonsense")));
    CHECK(BodyOf(reply)["error"]["code"] == RpcError::MethodNotFound, "an unknown method: -32601");

    // initialize: the client's revision when supported, else the newest.
    reply = protocol.Handle(Post(Rpc("initialize", Json{ { "protocolVersion", "2025-03-26" }, { "capabilities", Json::object() },
                                                         { "clientInfo", Json{ { "name", "test-client" }, { "version", "1" } } } })));
    Json body = BodyOf(reply);
    CHECK(body["result"]["protocolVersion"] == "2025-03-26" && body["result"]["capabilities"].contains("tools") &&
          body["result"]["serverInfo"]["name"] == "nox-editor" && body["result"]["instructions"] == "Test server",
          "initialize with 2025-03-26: answered with it, tools capability, server info, instructions");
    reply = protocol.Handle(Post(Rpc("initialize", Json{ { "protocolVersion", "2099-01-01" } })));
    CHECK(BodyOf(reply)["result"]["protocolVersion"] == LatestProtocolVersion, "an unknown revision: the server's newest ({})",
          BodyOf(reply)["result"]["protocolVersion"].dump());
    CHECK(BodyOf(protocol.Handle(Post(Rpc("ping", Json::object(), 7))))["id"] == 7, "ping answers with the request's id");

    // tools/list: only what the access grants, with annotations.
    body = BodyOf(protocol.Handle(Post(Rpc("tools/list"))));
    const Json& tools = body["result"]["tools"];
    bool hasQuit = false;
    for (const Json& tool : tools)
        hasQuit |= tool["name"] == "quit";
    CHECK(tools.size() == 2 && !hasQuit && tools[0]["inputSchema"]["required"][0] == "text" && tools[0]["annotations"]["readOnlyHint"] == true,
          "tools/list: {} tools for write access (the execute one hidden), schema and annotations", tools.size());

    // tools/call: result, structured content, validation, unknown tool, access, late answers.
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "echo" }, { "arguments", Json{ { "text", "ab" }, { "times", 3 } } } }))));
    CHECK(body["result"]["isError"] == false && body["result"]["structuredContent"]["text"] == "ababab" &&
          body["result"]["content"][0]["type"] == "text", "a call: structured and text content ({})", body["result"]["structuredContent"].dump());
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "echo" }, { "arguments", Json{ { "times", 2 } } } }))));
    CHECK(body["result"]["isError"] == true && body["result"]["content"][0]["text"].get<std::string>().find("'text' is required") != std::string::npos,
          "a missing required argument: an error result the model can read");
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "echo" }, { "arguments", Json{ { "text", "a" }, { "times", 9 } } } }))));
    CHECK(body["result"]["isError"] == true, "out of the schema's range: refused ({})", body["result"]["content"][0]["text"].dump());
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "echo" }, { "arguments", Json{ { "text", 5 } } } }))));
    CHECK(body["result"]["isError"] == true, "a wrong type: refused ({})", body["result"]["content"][0]["text"].dump());
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "echo" }, { "arguments", Json{ { "text", "a" }, { "extra", 1 } } } }))));
    CHECK(body["result"]["isError"] == true, "an unknown argument: refused ({})", body["result"]["content"][0]["text"].dump());
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "missing" } }))));
    CHECK(body["error"]["code"] == RpcError::InvalidParams, "an unknown tool: -32602");
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "quit" } }))));
    CHECK(body["result"]["isError"] == true && body["result"]["content"][0]["text"].get<std::string>().find("execute") != std::string::npos,
          "a tool above the granted access: refused");
    protocol.SetAccess(Nox::ConsoleAccess::Execute);
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "quit" } }))));
    CHECK(body["result"]["isError"] == false, "granted execute: it runs");
    body = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "later" } }))));
    CHECK(body["result"]["content"][0]["text"] == "late", "a tool that answers later from another thread");

    // Several clients at once.
    std::vector<std::thread> clients;
    std::atomic<int> good{ 0 };
    for (int client = 0; client < 8; ++client)
        clients.emplace_back([&, client]()
        {
            for (int call = 0; call < 25; ++call)
            {
                Json answer = BodyOf(protocol.Handle(Post(Rpc("tools/call", Json{ { "name", "echo" }, { "arguments", Json{ { "text", std::to_string(client) } } } },
                                                              call))));
                good += answer["id"] == call && answer["result"]["structuredContent"]["text"] == std::to_string(client);
            }
        });
    for (std::thread& thread : clients)
        thread.join();
    CHECK(good == 200, "8 clients x 25 calls at once: {} of 200 right", good.load());

    // Helpers.
    const std::string token = GenerateToken();
    CHECK(token.size() == 64 && token != GenerateToken(), "a token: 64 hex characters, different each time");
    const uint8_t bytes[] = { 'M', 'a', 'n' , 'y' };
    CHECK(Base64(bytes, 3) == "TWFu" && Base64(bytes, 4) == "TWFueQ==" && Base64(bytes, 2) == "TWE=", "base64 ({} {} {})", Base64(bytes, 3),
          Base64(bytes, 4), Base64(bytes, 2));
    CHECK(ConstantTimeEquals("abc", "abc") && !ConstantTimeEquals("abc", "abd") && !ConstantTimeEquals("abc", "ab"), "constant-time compare");
}

#ifdef NOX_TOOLS_TEST_MCP_SERVER
// The HTTP transport (McpServer) end to end: real sockets on 127.0.0.1, a thread standing in for the editor's main thread.
#include "McpServer.h"
#include "httplib.h"

static void TestMcpServer()
{
    using namespace McpTest;
    std::printf("\n== MCP server over HTTP\n");
    constexpr uint16_t port = 18765;

    std::atomic<bool> running{ true };
    std::atomic<int> frames{ 0 };
    std::vector<ToolDone> deferred; // answered a "frame" later, like a screenshot
    std::mutex deferredMutex;
    std::thread::id mainThread;

    Tool where;
    where.Name = "where";
    where.Description = "Says whether it ran on the main thread";
    where.Handler = [&](const Json&, ToolDone done) { done(ToolResult::Data(Json{ { "main", std::this_thread::get_id() == mainThread } })); };
    Tool nextFrame;
    nextFrame.Name = "next_frame";
    nextFrame.Description = "Answers on a later frame";
    nextFrame.Handler = [&](const Json&, ToolDone done)
    {
        std::lock_guard lock(deferredMutex);
        deferred.push_back(done);
    };
    Tool never;
    never.Name = "never";
    never.Description = "Never answers (Stop must still end the wait)";
    never.Handler = [](const Json&, ToolDone) {};

    Settings settings;
    settings.Token = Token;
    settings.Port = port;
    Nox::Mcp::McpServer server;
    std::string error;
    const bool started = server.Start(settings, { where, nextFrame, never }, error);
    CHECK(started, "listens on 127.0.0.1:{} {}", port, error);
    if (!started)
        return;
    Nox::Mcp::McpServer second;
    std::string secondError;
    CHECK(!second.Start(settings, {}, secondError) && !secondError.empty(), "a second server on the same port is refused: '{}'", secondError);

    // The "editor": pumps once a frame, answers deferred calls one frame later.
    std::thread editor([&]()
    {
        mainThread = std::this_thread::get_id();
        while (running)
        {
            std::vector<ToolDone> due;
            {
                std::lock_guard lock(deferredMutex);
                due.swap(deferred);
            }
            server.Pump();
            for (ToolDone& done : due)
                done(ToolResult::Text(std::format("answered at frame {}", frames.load())));
            ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });
    while (mainThread == std::thread::id())
        std::this_thread::yield();

    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10, 0);
    const httplib::Headers headers{ { "Authorization", std::string("Bearer ") + Token } };
    auto post = [&](const Json& body, const httplib::Headers& with) { return client.Post("/mcp", with, body.dump(), "application/json"); };

    auto noToken = post(Rpc("ping"), {});
    CHECK(noToken && noToken->status == 401, "over HTTP without the token: 401");
    auto init = post(Rpc("initialize", Json{ { "protocolVersion", "2025-06-18" } }), headers);
    CHECK(init && init->status == 200 && BodyOf(HttpReply{ 200, "", {}, init->body })["result"]["protocolVersion"] == "2025-06-18",
          "initialize over HTTP");
    auto notification = post(Json{ { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }, headers);
    CHECK(notification && notification->status == 202, "the initialized notification: 202");
    auto call = post(Rpc("tools/call", Json{ { "name", "where" } }), headers);
    Json answer = call ? Json::parse(call->body) : Json();
    CHECK(answer["result"]["structuredContent"]["main"] == true, "a tool runs on the editor's thread, not the HTTP one");
    const int before = frames;
    auto later = post(Rpc("tools/call", Json{ { "name", "next_frame" } }), headers);
    answer = later ? Json::parse(later->body) : Json();
    CHECK(answer["result"]["content"][0]["text"].get<std::string>().rfind("answered at frame", 0) == 0 && frames > before,
          "a tool answering on a later frame: '{}'", answer["result"]["content"][0]["text"].get<std::string>());

    // Concurrent clients: every answer back to its own request.
    std::vector<std::thread> clients;
    std::atomic<int> good{ 0 };
    for (int index = 0; index < 6; ++index)
        clients.emplace_back([&, index]()
        {
            httplib::Client own("127.0.0.1", port);
            for (int request = 0; request < 10; ++request)
            {
                auto reply = own.Post("/mcp", headers, Rpc("tools/call", Json{ { "name", "where" } }, index * 100 + request).dump(), "application/json");
                if (reply && reply->status == 200 && Json::parse(reply->body)["id"] == index * 100 + request)
                    ++good;
            }
        });
    for (std::thread& thread : clients)
        thread.join();
    CHECK(good == 60, "6 HTTP clients x 10 calls: {} of 60 answered", good.load());

    // A call that never answers: Stop ends its wait (the HTTP thread is not stuck for the 60 s time-out).
    std::thread stuck([&]() { post(Rpc("tools/call", Json{ { "name", "never" } }), headers); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto stopStart = std::chrono::steady_clock::now();
    server.Stop();
    stuck.join();
    const double stopSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - stopStart).count();
    CHECK(stopSeconds < 2.0 && !server.IsRunning(), "Stop with a call waiting returns in {:.2f} s", stopSeconds);
    running = false;
    editor.join();
}
#endif

#ifdef NOX_TOOLS_TEST_MCP
// Component edits' JSON side (McpValidation): patches, checks against a component's example, what the engine stored differently.
#include "McpValidation.h"

static void TestMcpValidation()
{
    using namespace Nox::Mcp;
    std::printf("\n== MCP component edits (JSON)\n");
    const Json light = Json::parse(R"({ "Color": [1, 1, 1], "Intensity": 10.0, "CastShadows": true, "Samples": 4, "Mode": "Realtime",
                                        "Mesh": "16531713622870338582", "Texture": "0", "Curve": [{ "Time": 0.0, "Value": 1.0 }],
                                        "Inner": { "Radius": 5.0 } })");

    CHECK(CheckAgainstExample(light, Json::parse(R"({ "Intensity": 3, "Color": [0.5, 0.2, 0.1] })")).empty(), "a whole number for a number, a vector of 3: fits");
    std::string problem = CheckAgainstExample(light, Json::parse(R"({ "Intensty": 3 })"));
    CHECK(problem.find("unknown field 'Intensty'") != std::string::npos && problem.find("Intensity") != std::string::npos,
          "a misspelled field: refused, the valid ones listed ({})", problem);
    CHECK(!CheckAgainstExample(light, Json::parse(R"({ "Color": [1, 1] })")).empty(), "a vector of another length: refused");
    CHECK(!CheckAgainstExample(light, Json::parse(R"({ "Color": [1, "a", 1] })")).empty(), "a vector with a word in it: refused");
    CHECK(!CheckAgainstExample(light, Json::parse(R"({ "CastShadows": 1 })")).empty(), "1 for a bool: refused");
    CHECK(!CheckAgainstExample(light, Json::parse(R"({ "Samples": 2.5 })")).empty() && CheckAgainstExample(light, Json::parse(R"({ "Samples": 2.0 })")).empty(),
          "2.5 for a whole number refused, 2.0 fits");
    problem = CheckAgainstExample(light, Json::parse(R"({ "Mesh": 16531713622870338582 })"));
    CHECK(problem.find("write it as a string") != std::string::npos, "an id as a JSON number: refused with how to write it ({})", problem);
    CHECK(CheckAgainstExample(light, Json::parse(R"({ "Mesh": "10922256994577822127", "Texture": "6087766411216149246" })")).empty(),
          "ids as digit strings, also into a field that was 0: fit");
    CHECK(CheckAgainstExample(light, Json::parse(R"({ "Mesh": "0" })")).empty(), "\"0\" clears an id");
    CHECK(CheckAgainstExample(Json::parse(R"({ "MeshHandle": 0, "Count": 3 })"), Json::parse(R"({ "MeshHandle": "16531713622870338582" })")).empty() &&
          !CheckAgainstExample(Json::parse(R"({ "MeshHandle": 0, "Count": 3 })"), Json::parse(R"({ "Count": "16531713622870338582" })")).empty(),
          "an unset reference (0) takes an id string; a field holding another number does not");
    CHECK(!CheckAgainstExample(light, Json::parse(R"({ "Inner": { "Radius": "big" } })")).empty(), "nested objects are checked");
    CHECK(!CheckAgainstExample(light, Json::parse(R"({ "Curve": [{ "Time": "x", "Value": 1 }] })")).empty() &&
          CheckAgainstExample(light, Json::parse(R"({ "Curve": [{ "Time": 0.5, "Value": 1 }, { "Time": 1, "Value": 0 }] })")).empty(),
          "list items are checked against the list's first item; a longer list fits");

    Json patched;
    problem = ApplyPatch(light, Json::parse(R"([{ "op": "replace", "path": "/Intensity", "value": 2.5 }, { "op": "replace", "path": "/Color/0", "value": 0.25 }])"), patched);
    CHECK(problem.empty() && patched["Intensity"] == 2.5 && patched["Color"][0] == 0.25 && patched["Samples"] == 4,
          "a patch: two fields changed, the rest as they were");
    CHECK(!ApplyPatch(light, Json::parse(R"([{ "op": "move", "from": "/A", "path": "/B" }])"), patched).empty(), "'move' is not allowed");
    CHECK(!ApplyPatch(light, Json::parse(R"([{ "op": "replace", "path": "/Missing", "value": 1 }])"), patched).empty(), "replacing a field that is not there fails");
    CHECK(!ApplyPatch(light, Json::parse(R"([{ "op": "test", "path": "/Samples", "value": 5 }])"), patched).empty(), "a failed 'test' stops the patch");
    CHECK(!ApplyPatch(light, Json::parse(R"([])"), patched).empty() && !ApplyPatch(light, Json::parse(R"([{ "op": "replace", "path": "Intensity", "value": 1 }])"), patched).empty(),
          "an empty patch and a path without '/' are refused");

    const Json stored = Json::parse(R"({ "Intensity": 100.0, "Color": [0.25, 1.0000001, 1], "Mode": "Realtime" })");
    const Json differences = Differences(Json::parse(R"({ "Intensity": 500, "Color": [0.25, 1, 1], "Mode": "Realtime" })"), stored);
    CHECK(differences.size() == 1 && differences[0]["path"] == "/Intensity" && differences[0]["stored"] == 100.0,
          "what the engine stored differently: only the clamped field ({})", differences.dump());

    const Json merged = Merge(Json::parse(R"({ "A": 1, "B": { "C": 2, "D": 3 } })"), Json::parse(R"({ "B": { "C": 9 }, "E": 4 })"));
    CHECK(merged == Json::parse(R"({ "A": 1, "B": { "C": 9, "D": 3 }, "E": 4 })"), "defaults merged under current values");
    // The serializer's YAML and back: kinds kept, ids as text, doubles with every digit.
    const char* yamlText = R"(TransformComponent:
  Translation: [123456.789012345, -10, 0.1]
  Rotation: [0, 1.5707964, 0]
  Scale: [1, 1, 1]
MeshComponent:
  MeshHandle: 16531713622870338582
  SubmeshIndex: 0
  CastShadows: true
ScriptComponent:
  Classes: [Facerun.NetPlayer]
  FieldOverrides:
    - Class: Facerun.NetPlayer
      Field: Speed
      Value: 200
)";
    const Json fromYaml = NodeToJson(YAML::Load(yamlText));
    CHECK(fromYaml["MeshComponent"]["MeshHandle"] == "16531713622870338582" && fromYaml["MeshComponent"]["CastShadows"] == true &&
          fromYaml["MeshComponent"]["SubmeshIndex"] == 0 && fromYaml["ScriptComponent"]["Classes"][0] == "Facerun.NetPlayer" &&
          fromYaml["ScriptComponent"]["FieldOverrides"][0]["Value"] == 200, "YAML to JSON: an id as text, a bool, numbers, lists of maps");
    YAML::Emitter emitter;
    emitter << JsonToYaml(fromYaml);
    const Json roundTrip = NodeToJson(YAML::Load(emitter.c_str()));
    CHECK(roundTrip == fromYaml, "JSON back to YAML and again: the same values");
    CHECK(YAML::Load(emitter.c_str())["TransformComponent"]["Translation"][0].as<double>() == 123456.789012345,
          "a double world position keeps every digit through the round trip");
    CHECK(ScalarToJson("Dynamic") == "Dynamic" && ScalarToJson("2.5") == 2.5 && ScalarToJson("false") == false && ScalarToJson("0") == 0,
          "scalars: an enum name stays text, numbers and booleans read as such");

    const Json schema = SchemaFromExample(light);
    CHECK(schema["Color"]["kind"] == "vector3" && schema["Mesh"]["kind"] == "id" && schema["Samples"]["kind"] == "integer" &&
          schema["Intensity"]["kind"] == "number" && schema["Inner"]["kind"] == "object", "a schema from an example: kinds ({})", schema["Color"].dump());
}
#endif
