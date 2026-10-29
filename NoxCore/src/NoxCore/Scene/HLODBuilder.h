#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

#include "WorldPartition.h"
#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    class Scene;

    // Build HLODs (docs/Engine_Architecture_Plan_2026.md §5.13.2, World Partition panel): for every world cell, its static meshes
    // merged, simplified, laid out and baked (HLODBake) into a glTF in the cells' HLOD folder, imported like any model, and its HLOD
    // entity written into the HLOD grid's cell files and the world's index (the scene is saved afterwards by the editor).
    //
    // Each cell is read from its file into a scene of its own (the edited scene is untouched; save it first, a cell's unsaved changes
    // are not in its file), its model and prefab instances spawned there (they need their assets: the builder is stepped every frame
    // on the main thread), the cooked meshes, materials and textures read and the bake run on workers.
    class HLODBuilder
    {
    public:
        explicit HLODBuilder(Ref<Scene> world);
        ~HLODBuilder();

        HLODBuilder(const HLODBuilder&) = delete;
        HLODBuilder& operator=(const HLODBuilder&) = delete;

        // Main thread, every frame. True once every cell is done (or the build failed).
        bool Step();
        bool Failed() const { return m_Failed; }
        float GetProgress() const;
        const std::string& GetStage() const { return m_Stage; }
        size_t GetBuiltCount() const { return m_Entries.size(); }
        // What the cell being built holds (its models and prefabs, loading for its instances to spawn): kept by the editor's sweep of
        // unused assets.
        void CollectAssetReferences(std::unordered_set<AssetHandle>& outHandles) const;

    private:
        struct CellWork;
        struct Entry
        {
            WorldCellCoord Source;
            AssetHandle Mesh = 0;
            glm::dvec3 Origin{ 0.0 };
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
        };

        void StartCell();
        bool StepCell(); // true when the current cell is done (built or skipped)
        void WriteHLODCells();

        Ref<Scene> m_World;
        std::vector<WorldCellCoord> m_Cells;
        size_t m_Next = 0;
        std::unique_ptr<CellWork> m_Work;
        std::vector<Entry> m_Entries;
        bool m_ShowHLODsBefore = true;
        bool m_Failed = false;
        bool m_Done = false;
        std::string m_Stage;
    };
}
