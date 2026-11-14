// CustomLitShaders::GatherProjectSources: the active project's Material Shaders (apart from CustomLitShaders.cpp, which needs no project).
#include "NoxCore/Renderer/CustomLitShaders.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Project/Project.h"

namespace Nox::CustomLitShaders
{
    std::vector<CustomLitSource> GatherProjectSources(bool& complete)
    {
        complete = true;
        std::vector<CustomLitSource> sources;
        const Ref<Project> project = Project::GetActive();
        if (!project)
            return sources;
        const std::shared_ptr<EditorAssetManager> assets = project->GetEditorAssetManager();
        if (!assets)
            return sources;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::MaterialShader || metadata.FilePath.empty())
                continue;
            // The file on disk (not through the virtual file system: a packaged game's pack holds no Material Shader to compile).
            std::ifstream file(assetDirectory / metadata.FilePath, std::ios::binary);
            if (!file)
            {
                complete = false;
                continue;
            }
            std::stringstream code;
            code << file.rdbuf();
            sources.push_back({ metadata.FilePath.generic_string(), code.str() });
        }
        std::sort(sources.begin(), sources.end(), [](const CustomLitSource& a, const CustomLitSource& b) { return a.Path < b.Path; });
        return sources;
    }
}
