#include "Project.h"
#include "NoxCore/Core/VirtualFileSystem.h"

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
            // A packaged game's assets are in pack files next to its project (GamePackager): mounted over the asset directory
            // before the registry (which is in them too) is read.
            if (runtime)
                VFS::MountPacks(project->m_ProjectDirectory, project->m_ProjectDirectory / project->GetConfig().AssetDirectory);
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
