#include "MaterialVariants.h"

#include <algorithm>
#include <functional>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Renderer/Mesh.h"
#include "Entity.h"
#include "Scene.h"

namespace Nox::MaterialVariants
{
    namespace
    {
        // A loaded mesh's variant data: per submesh its mappings and their .nmat (empty when the mesh has none or is not loaded).
        struct MeshVariants
        {
            size_t SubmeshCount = 0;
            const std::vector<std::vector<MaterialVariantMapping>>* Mappings = nullptr;
            const std::vector<std::vector<AssetHandle>>* Assets = nullptr;

            bool Any() const { return Mappings && !Mappings->empty(); }
            // The .nmat a slot draws with under `variant`: 0 for the mesh's own (no mapping, or none linked).
            AssetHandle MaterialOf(size_t slot, std::string_view variant) const
            {
                if (variant.empty() || !Any() || slot >= Mappings->size())
                    return 0;
                const std::vector<MaterialVariantMapping>& mappings = (*Mappings)[slot];
                for (size_t i = 0; i < mappings.size(); ++i)
                {
                    if (mappings[i].Variant == variant)
                        return slot < Assets->size() && i < (*Assets)[slot].size() ? (*Assets)[slot][i] : AssetHandle(0);
                }
                return 0;
            }
        };

        MeshVariants VariantsOf(AssetHandle mesh)
        {
            MeshVariants result;
            auto take = [&](const auto* asset)
            {
                if (!asset)
                    return;
                result.SubmeshCount = asset->GetSubMeshCount();
                result.Mappings = &asset->GetMaterialVariants();
                result.Assets = &asset->GetVariantMaterialAssets();
            };
            const AssetType type = AssetManager::GetAssetType(mesh);
            if (type == AssetType::StaticMesh)
                take(AssetManager::FindLoadedAsset<StaticMesh>(mesh));
            else if (type == AssetType::Mesh || type == AssetType::MeshSource)
                take(AssetManager::FindLoadedAsset<Mesh>(mesh));
            return result;
        }

        // The entity and its descendants that draw a mesh with variants, with that mesh's data and the slots they draw (the
        // renderer's range: Renderer::addSubmeshInstances).
        void ForEachVariantMesh(Scene& scene, Entity root, const std::function<void(Entity, const MeshVariants&, size_t, size_t)>& visit)
        {
            std::vector<Entity> pending{ root };
            while (!pending.empty())
            {
                Entity entity = pending.back();
                pending.pop_back();
                if (!entity)
                    continue;
                if (entity.HasComponent<RelationshipComponent>())
                {
                    for (UUID child : entity.GetComponent<RelationshipComponent>().Children)
                        pending.push_back(scene.GetEntityByUUID(child));
                }
                if (!entity.HasComponent<MeshComponent>())
                    continue;
                const MeshComponent& mesh = entity.GetComponent<MeshComponent>();
                const MeshVariants variants = VariantsOf(mesh.Mesh);
                if (!variants.Any())
                    continue;
                const size_t first = std::min<size_t>(mesh.SubmeshIndex, variants.SubmeshCount);
                const size_t count = mesh.SubmeshCount == UINT32_MAX ? variants.SubmeshCount : std::max(mesh.SubmeshCount, 1u);
                visit(entity, variants, first, std::min(first + count, variants.SubmeshCount));
            }
        }
    }

    std::vector<std::string> List(Scene& scene, Entity root)
    {
        std::vector<std::pair<uint32_t, std::string>> found; // file order, name
        ForEachVariantMesh(scene, root, [&](Entity, const MeshVariants& variants, size_t first, size_t last)
        {
            for (size_t slot = first; slot < last && slot < variants.Mappings->size(); ++slot)
            {
                for (const MaterialVariantMapping& mapping : (*variants.Mappings)[slot])
                {
                    if (std::none_of(found.begin(), found.end(), [&](const auto& known) { return known.second == mapping.Variant; }))
                        found.emplace_back(mapping.VariantIndex, mapping.Variant);
                }
            }
        });
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> names;
        for (auto& [index, name] : found)
            names.push_back(std::move(name));
        return names;
    }

    bool Select(Scene& scene, Entity root, std::string_view variant)
    {
        bool known = variant.empty();
        ForEachVariantMesh(scene, root, [&](Entity entity, const MeshVariants& variants, size_t first, size_t last)
        {
            for (size_t slot = first; slot < last && !known; ++slot)
                known = variants.MaterialOf(slot, variant) != 0;
        });
        if (!known)
            return false;

        ForEachVariantMesh(scene, root, [&](Entity entity, const MeshVariants& variants, size_t first, size_t last)
        {
            const bool had = entity.HasComponent<MaterialComponent>();
            std::vector<AssetHandle> materials = had ? entity.GetComponent<MaterialComponent>().MaterialAssets : std::vector<AssetHandle>{};
            if (materials.size() < last)
                materials.resize(last, AssetHandle(0));
            for (size_t slot = first; slot < last; ++slot)
                materials[slot] = variants.MaterialOf(slot, variant);
            while (!materials.empty() && materials.back() == 0)
                materials.pop_back();
            if (!had && materials.empty())
                return;
            if (!had)
            {
                entity.AddOrReplaceComponent<MaterialComponent>().MaterialAssets = std::move(materials);
                entity.PatchComponent<MaterialComponent>();
                return;
            }
            MaterialComponent& component = entity.GetComponent<MaterialComponent>();
            if (component.MaterialAssets == materials)
                return;
            component.MaterialAssets = std::move(materials);
            entity.PatchComponent<MaterialComponent>();
        });
        return true;
    }

    std::optional<std::string> Current(Scene& scene, Entity root)
    {
        // The slots as they are, per mesh entity.
        struct Slots
        {
            MeshVariants Variants;
            size_t First = 0, Last = 0;
            std::vector<AssetHandle> Materials;
        };
        std::vector<Slots> meshes;
        ForEachVariantMesh(scene, root, [&](Entity entity, const MeshVariants& variants, size_t first, size_t last)
        {
            Slots& slots = meshes.emplace_back(Slots{ variants, first, last, {} });
            if (entity.HasComponent<MaterialComponent>())
                slots.Materials = entity.GetComponent<MaterialComponent>().MaterialAssets;
        });
        auto shows = [&](std::string_view variant)
        {
            for (const Slots& slots : meshes)
            {
                for (size_t slot = slots.First; slot < slots.Last; ++slot)
                {
                    const AssetHandle current = slot < slots.Materials.size() ? slots.Materials[slot] : AssetHandle(0);
                    if (current != slots.Variants.MaterialOf(slot, variant))
                        return false;
                }
            }
            return true;
        };
        if (shows(""))
            return std::string();
        for (const std::string& name : List(scene, root))
        {
            if (shows(name))
                return name;
        }
        return std::nullopt;
    }
}
