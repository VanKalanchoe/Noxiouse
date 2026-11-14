#include "McpProjectFiles.h"

#include <fstream>

#include "McpProtocol.h"
#include "NoxCore/Project/Project.h"

namespace Nox::Mcp
{
    std::filesystem::path NoxFolder()
    {
        return Project::GetActive() ? Project::GetActiveProjectDirectory() / ".nox" : std::filesystem::path();
    }

    bool EnsureNoxFolder(const std::filesystem::path& folder)
    {
        if (folder.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        // The folder holds the token: never committed, whatever the project's own .gitignore says.
        if (!std::filesystem::exists(folder / ".gitignore", error))
            std::ofstream(folder / ".gitignore") << "# Nox: per-machine editor files (the MCP token among them) -- never committed\n*\n";
        return std::filesystem::is_directory(folder, error);
    }

    bool LoadOrCreateToken(const std::filesystem::path& folder, bool rotate, std::string& token)
    {
        if (!EnsureNoxFolder(folder))
            return false;
        const std::filesystem::path path = folder / "mcp-token";
        if (!rotate)
        {
            std::ifstream file(path);
            std::string existing;
            if (std::getline(file, existing) && existing.size() == 64)
            {
                token = existing;
                return true;
            }
        }
        const std::string made = GenerateToken();
        if (made.empty())
            return false;
        {
            std::ofstream file(path, std::ios::trunc);
            if (!(file << made << "\n"))
                return false;
        }
#ifndef _WIN32
        // Readable by this user only (on Windows the project folder's own permissions apply).
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace,
                                     error);
#endif
        token = made;
        return true;
    }
}
