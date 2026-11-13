#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "McpEditorTools.h"
#include "McpServer.h"
#include "NoxCore/Console/Console.h"

namespace Nox::Mcp
{
    // The MCP server in NoxEditor (docs/MCP_Plan_2026-11.md): everything the editor does with MCP, behind four calls from EditorLayer
    // (construct, Update, DrawMenu, destroy) -- leaving it out of the build (CMake NOX_WITH_MCP=OFF) removes the Mcp folder and those.
    //
    // Off by default. Tools > MCP Server (or the console variables mcp.Enabled / mcp.Port / mcp.Access, which only the person at this
    // machine may set) turns it on; the settings are kept per project in <Project>/.nox/mcp.ini, the token in .nox/mcp-token (32 random
    // bytes; that folder carries its own .gitignore). The menu copies a ready connect command (Claude Code) or JSON config (other
    // clients) to the clipboard -- the token is never written to the log.
    class McpEditor
    {
    public:
        explicit McpEditor(EditorContext context);
        ~McpEditor();
        McpEditor(const McpEditor&) = delete;
        McpEditor& operator=(const McpEditor&) = delete;

        // Main thread, at the start of each frame: the tool calls that came in run here.
        void Update();
        // Inside a menu (Tools): the MCP Server sub-menu.
        void DrawMenu();

    private:
        void Initialize(); // once a project is open
        void Apply();      // starts, stops or restarts the server as the variables say
        void LoadSettings();
        void SaveSettings() const;
        bool EnsureToken(bool rotate);
        std::filesystem::path Folder() const; // <Project>/.nox
        std::string Url() const;
        ConsoleAccess Granted() const;

        EditorContext m_Context;
        McpServer m_Server;
        TConsoleVariable<bool>* m_Enabled = nullptr;
        TConsoleVariable<int32_t>* m_Port = nullptr;
        TConsoleVariable<int32_t>* m_Access = nullptr;
        std::vector<Console::Registration> m_Commands;
        std::string m_Token;
        std::string m_Problem;       // why it is not running when enabled (shown in the menu)
        uint16_t m_RunningPort = 0;
        bool m_Initialized = false;
        bool m_Loading = false;      // (settings being read: no saving back, no restarts per variable)
    };
}
