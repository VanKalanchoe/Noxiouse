// CustomLitShaders::GatherProjectSources and GatherProjectMaterialGraphs: the active project's Material Shaders and material graphs (apart
// from CustomLitShaders.cpp / MaterialGraph.cpp, which need no project).
#include "NoxCore/Renderer/CustomLitShaders.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/MaterialGraph.h"

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
            const std::filesystem::path onDisk = std::filesystem::absolute(assetDirectory / metadata.FilePath).lexically_normal();
            sources.push_back({ metadata.FilePath.generic_string(), code.str(), onDisk.generic_string() });
        }
        std::sort(sources.begin(), sources.end(), [](const CustomLitSource& a, const CustomLitSource& b) { return a.Path < b.Path; });
        return sources;
    }
}

namespace Nox
{
    std::vector<ProjectMaterialGraph> GatherProjectMaterialGraphs(bool& complete)
    {
        complete = true;
        std::vector<ProjectMaterialGraph> graphs;
        const Ref<Project> project = Project::GetActive();
        if (!project)
            return graphs;
        const std::shared_ptr<EditorAssetManager> assets = project->GetEditorAssetManager();
        if (!assets)
            return graphs;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::MaterialGraph || metadata.FilePath.empty())
                continue;
            // The file on disk, as saved (the node graph editor saves before the renderer builds again).
            const std::filesystem::path file = assetDirectory / metadata.FilePath;
            if (!std::filesystem::exists(file))
            {
                complete = false;
                continue;
            }
            NodeGraph graph;
            ProjectMaterialGraph compiled;
            compiled.Path = metadata.FilePath.generic_string();
            if (!NodeGraphSerializer::Deserialize(file, graph))
            {
                compiled.Program.Error = "the file could not be read";
                NOX_CORE_ERROR("Material graph {}: the file could not be read", compiled.Path);
            }
            else
            {
                compiled.Program = CompileMaterialGraph(graph);
                if (!compiled.Program.Valid)
                    NOX_CORE_ERROR("Material graph {}: {}", compiled.Path, compiled.Program.Error);
            }
            graphs.push_back(std::move(compiled));
        }
        std::sort(graphs.begin(), graphs.end(), [](const ProjectMaterialGraph& a, const ProjectMaterialGraph& b) { return a.Path < b.Path; });
        return graphs;
    }
}
