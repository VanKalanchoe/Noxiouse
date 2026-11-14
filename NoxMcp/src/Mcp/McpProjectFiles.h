#pragma once
#include <filesystem>
#include <string>

// The MCP server's files in a project, shared by the editor's server and NoxRuntime's (--mcp): <Project>/.nox -- a per-machine
// folder with its own .gitignore, so the token never goes into version control whatever the project's own .gitignore says -- and
// the token in it (.nox/mcp-token, 32 random bytes as 64 hex digits; readable by this user only on Linux / macOS). One token per
// project: a client connected to the editor and the runtimes it starts use the same one.
namespace Nox::Mcp
{
    std::filesystem::path NoxFolder(); // <active project>/.nox (empty: no project)
    // Creates the folder and its .gitignore when missing; false when it cannot be written.
    bool EnsureNoxFolder(const std::filesystem::path& folder);
    // The token in <folder>/mcp-token; a new one when there is none (or `rotate`). False when it could not be made or written.
    bool LoadOrCreateToken(const std::filesystem::path& folder, bool rotate, std::string& token);
}
