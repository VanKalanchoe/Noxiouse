#include "McpProjectFiles.h"

#include <NoxMcp/McpTokenFiles.h>

#include "NoxCore/Project/Project.h"

namespace Nox::Mcp
{
    using namespace ::NoxMcp;
    std::filesystem::path NoxFolder()
    {
        return Project::GetActive() ? Project::GetActiveProjectDirectory() / ".nox" : std::filesystem::path();
    }

    bool EnsureNoxFolder(const std::filesystem::path& folder)
    {
        return ::NoxMcp::EnsureNoxFolder(folder);
    }

    bool LoadOrCreateToken(const std::filesystem::path& folder, bool rotate, std::string& token)
    {
        return ::NoxMcp::LoadOrCreateToken(folder, rotate, token);
    }
}
