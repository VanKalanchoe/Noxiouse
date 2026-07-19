#include "SceneImporter.h"

#include "NoxCore/Debug/Instrumentor.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/SceneSerializer.h"

namespace Nox
{
    Ref<Scene> SceneImporter::ImportScene(AssetHandle handle, const AssetMetadata& metadata)
    {
        /*VK_PROFILE_FUNCTION();*/

        return LoadScene(Project::GetActiveAssetDirectory() / metadata.FilePath);
    }

    Ref<Scene> SceneImporter::LoadScene(const std::filesystem::path& path)
    {
        /*VK_PROFILE_FUNCTION();*/

        Ref<Scene> scene = CreateRef<Scene>();
        SceneSerializer serializer(scene);
        serializer.Deserialize(path);
        return scene;
    }

    void SceneImporter::SaveScene(Ref<Scene> scene, const std::filesystem::path& path)
    {
        SceneSerializer serializer(scene);
        serializer.Serialize(Project::GetActiveAssetDirectory() / path);
    }
}
