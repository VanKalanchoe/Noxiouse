#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "NoxCore/Core/WorldUnits.h"

#include "NoxCore/Asset/Asset.h"
#include <NoxTasks/JobSystem.h>

namespace Nox
{
    class Scene;

    class StreamingTestWorldGenerator
    {
    public:
        struct Settings
        {
            float TargetGigabytes = 20.0f;
            float CellSizeMeters = 64.0f;
            float LoadingRangeMeters = 256.0f;
            uint32_t TileQuads = 128;
            uint32_t TextureSize = 2048;
            uint32_t PropsPerCell = 250;
            float WorldOffsetKm = 0.0f;
            std::string Folder = "StreamingTest";
            std::string CameraScript;
        };

        explicit StreamingTestWorldGenerator(const Settings& settings);
        ~StreamingTestWorldGenerator();

        StreamingTestWorldGenerator(const StreamingTestWorldGenerator&) = delete;
        StreamingTestWorldGenerator& operator=(const StreamingTestWorldGenerator&) = delete;

        bool Step();
        bool Failed() const { return m_Failed; }
        float GetProgress() const;
        std::string GetStage() const;
        AssetHandle GetScene() const { return m_Scene; }
        const glm::dvec3& GetMiddle() const { return m_Middle; }

    private:
        struct Tile
        {
            float MinHeight = 0.0f;
            float MaxHeight = 0.0f;
            bool Written = false;
        };

        void BeginCells();
        glm::dvec3 WorldOffset() const
        {
            const double offset = static_cast<double>(m_Settings.WorldOffsetKm) * WorldUnits::FromMeters(1000.0f);
            return glm::dvec3(offset, 0.0, offset);
        }
        bool WriteCell(uint32_t x, uint32_t z);
        void FinishScene();

        Settings m_Settings;
        std::filesystem::path m_AssetDirectory;
        NoxTasks::TaskFuture<void> m_Job;
        std::atomic<uint32_t> m_Grid{ 0 };
        std::atomic<uint32_t> m_Done{ 0 };
        std::atomic<uint64_t> m_Bytes{ 0 };
        std::vector<Tile> m_Tiles;
        std::shared_ptr<std::string> m_Error;
        bool m_Failed = false;
        bool m_Finished = false;
        AssetHandle m_Scene = 0;
        Ref<Scene> m_World;
        Ref<Scene> m_CellScene;
        uint32_t m_CellsWritten = 0;
        uint64_t m_CellBytes = 0;
        AssetHandle m_PropMeshes[3] = {};
        std::vector<std::filesystem::path> m_CellFiles;
        bool m_Cooking = false;
        NoxTasks::TaskFuture<void> m_CookJob;
        std::atomic<uint32_t> m_CellsCooked{ 0 };
        std::atomic<uint64_t> m_CookedBytes{ 0 }, m_BinaryEntities{ 0 }, m_CookedEntities{ 0 };
        glm::dvec3 m_Middle{ 0.0 };
    };
}
