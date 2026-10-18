#include "Project.h"

#include "ProjectSerializer.h"
#include "NoxCore/Core/WorldUnits.h"

namespace Nox
{
    std::filesystem::path Project::GetAssetAbsolutePath(const std::filesystem::path& path)
    {
        return GetAssetDirectory() / path;
    }
    
    Ref<Project> Project::New()
    {
        s_ActiveProject = CreateRef<Project>();
        return s_ActiveProject;
    }

    Ref<Project> Project::Load(const std::filesystem::path& path, bool runtime)
    {
        Ref<Project> project = CreateRef<Project>();
        
        ProjectSerializer serializer(project);
        if (serializer.Deserialize(path))
        {
            project->m_ProjectDirectory = path.parent_path();
            project->m_ProjectFile = path;
            s_ActiveProject = project;
            WorldUnits::SetPerMeter(project->GetConfig().WorldUnitsPerMeter);
            // RuntimeAssetManager is the editor's pipeline in read-only mode, so both are reached through GetEditorAssetManager().
            std::shared_ptr<EditorAssetManager> assetManager = runtime ? std::make_shared<RuntimeAssetManager>() : std::make_shared<EditorAssetManager>();
            s_ActiveProject->m_AssetManager = assetManager;
            assetManager->DeserializeAssetRegistry();
            return s_ActiveProject;
        }
        
        return {};
    }

    bool Project::SaveActive()
    {
        if (!s_ActiveProject || s_ActiveProject->m_ProjectFile.empty())
            return false;
        return SaveActive(s_ActiveProject->m_ProjectFile);
    }

    bool Project::SaveActive(const std::filesystem::path& path)
    {
        ProjectSerializer serializer(s_ActiveProject);
        if (serializer.Serialize(path))
        {
            s_ActiveProject->m_ProjectDirectory = path.parent_path();
            s_ActiveProject->m_ProjectFile = path;
            return true;
        }
        return false;
    }
}
