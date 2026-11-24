#include "MaterialEdits.h"

#include <algorithm>
#include <unordered_set>

#include "Panels/AssetPreviewRenderer.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"

namespace Nox::MaterialEdits
{
    namespace
    {
        std::unordered_set<AssetHandle> s_Unsaved;

        std::filesystem::path FileOf(AssetHandle material)
        {
            auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
            if (!assets)
                return {};
            const AssetMetadata& metadata = assets->GetMetadata(material);
            return metadata.FilePath.empty() ? std::filesystem::path() : Project::GetActiveAssetDirectory() / metadata.FilePath;
        }
    }

    void MarkEdited(AssetHandle material)
    {
        if (material == 0)
            return;
        Renderer::MarkMaterialChanged(material);          // drawn with its new values from this frame
        AssetPreviewRenderer::Get().Invalidate(material); // its Content Browser sphere is drawn again
        s_Unsaved.insert(material);
    }

    bool IsUnsaved(AssetHandle material) { return s_Unsaved.contains(material); }
    bool Any() { return !s_Unsaved.empty(); }

    std::vector<AssetHandle> Unsaved()
    {
        std::vector<AssetHandle> list(s_Unsaved.begin(), s_Unsaved.end());
        std::sort(list.begin(), list.end());
        return list;
    }

    bool Save(AssetHandle material)
    {
        Ref<Material> loaded = AssetManager::GetAsset<Material>(material);
        const std::filesystem::path path = FileOf(material);
        if (!loaded || path.empty() || !MaterialSerializer::Serialize(path, loaded->GetData()))
        {
            NOX_CORE_ERROR("Material {}: not saved ({} could not be written)", static_cast<uint64_t>(material), path.generic_string());
            return false;
        }
        if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
            assets->NoteSourceWritten(material); // (its own save is no change on disk to reload)
        s_Unsaved.erase(material);
        return true;
    }

    void SaveAll()
    {
        for (AssetHandle material : Unsaved())
            Save(material);
    }

    bool Revert(AssetHandle material)
    {
        Ref<Material> loaded = AssetManager::GetAsset<Material>(material);
        const std::filesystem::path path = FileOf(material);
        MaterialData fromFile;
        if (!loaded || path.empty() || !MaterialSerializer::Deserialize(path, fromFile))
        {
            NOX_CORE_ERROR("Material {}: the file could not be read back ({}); the edits stay", static_cast<uint64_t>(material), path.generic_string());
            return false;
        }
        loaded->GetData() = std::move(fromFile);
        Renderer::MarkMaterialChanged(material);
        AssetPreviewRenderer::Get().Invalidate(material);
        s_Unsaved.erase(material);
        return true;
    }

    void RevertAll()
    {
        for (AssetHandle material : Unsaved())
            Revert(material);
    }

    void Clear() { s_Unsaved.clear(); }
}
