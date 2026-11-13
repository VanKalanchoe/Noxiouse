#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "McpProtocol.h"

namespace httplib
{
    class Server;
}

namespace Nox::Mcp
{
    // The MCP endpoint over HTTP (cpp-httplib, MIT, NoxCore/vendors/cpp-httplib): http://127.0.0.1:<port>/mcp, loopback only. Requests
    // are answered on the HTTP library's threads; a tool call is queued to the editor's main thread (Pump, once a frame) and its
    // HTTP thread waits for the answer -- the editor's frame never waits for a client.
    class McpServer
    {
    public:
        McpServer(); // (out of line: httplib::Server is only known in the .cpp)
        ~McpServer(); // Stop
        McpServer(const McpServer&) = delete;
        McpServer& operator=(const McpServer&) = delete;

        // Listens on 127.0.0.1:settings.Port; false (and `error`) when the port cannot be opened.
        bool Start(const Settings& settings, std::vector<Tool> tools, std::string& error);
        // Stops listening; calls still queued are answered "the server stopped".
        void Stop();
        bool IsRunning() const { return m_Http != nullptr; }

        void SetAccess(ConsoleAccess access);
        size_t GetCallCount() const { return m_Protocol ? m_Protocol->GetCallCount() : 0; }

        // Main thread, once a frame: runs the tool calls that came in (each answers now or a few frames later).
        void Pump();

        static constexpr size_t MaxQueued = 64;        // more waiting calls are refused ("busy")
        static constexpr int CallTimeoutSeconds = 60;  // a call not answered by then gets an error

    private:
        struct Pending
        {
            std::function<void()> Run;
            std::function<void(const std::string&)> Fail; // answers with this error, unless it answered already
        };
        ToolResult Dispatch(const Tool& tool, const Json& arguments);

        std::unique_ptr<Protocol> m_Protocol;
        std::unique_ptr<httplib::Server> m_Http;
        std::thread m_Thread;
        std::mutex m_QueueMutex;
        std::deque<Pending> m_Queue;
        std::atomic<bool> m_Stopping{ false };
    };
}
