#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    class Scene;

    // The 6c test world (docs/Engine_Architecture_Plan_2026.md §5.13.4): a world partition of N x N cells, each holding a terrain
    // tile of its own -- a unique height field mesh and a unique texture -- so that the world's data is larger than RAM and VRAM
    // and only streaming can show it -- and props by the hundred (entities sharing three meshes), so that its cells are as dense as
    // an open world's. Written straight in cooked form (.nsmesh with its cluster LOD, .ntex BC1: nothing to
    // import) on workers, then registered as assets on the main thread, with the cell files, their index and the scene (a camera,
    // a sun, a procedural sky).
    class StreamingTestWorldGenerator
    {
    public:
        struct Settings
        {
            float TargetGigabytes = 20.0f;     // the world's data on disk: N is chosen for it from the first tile's size
            float CellSizeMeters = 64.0f;      // a tile is one cell
            float LoadingRangeMeters = 256.0f;
            uint32_t TileQuads = 128;          // per side: (n + 1)^2 vertices, 2 n^2 triangles
            uint32_t TextureSize = 2048;
            uint32_t PropsPerCell = 250;       // rocks, pines, stumps: entities of their own, three meshes shared by every cell
            std::string Folder = "StreamingTest";  // under the asset directory
            std::string CameraScript;              // a script class put on the camera (the fly-through test), optional
        };

        explicit StreamingTestWorldGenerator(const Settings& settings);
        ~StreamingTestWorldGenerator(); // a running generation stops (what it wrote stays on disk, unregistered)

        StreamingTestWorldGenerator(const StreamingTestWorldGenerator&) = delete;
        StreamingTestWorldGenerator& operator=(const StreamingTestWorldGenerator&) = delete;

        // Main thread, every frame. True once done (or failed): then GetScene is the generated scene.
        bool Step();
        bool Failed() const { return m_Failed; }
        float GetProgress() const;
        std::string GetStage() const;
        AssetHandle GetScene() const { return m_Scene; }
        // The world's middle, near the ground (world units): where the editor camera looks once it opens.
        const glm::vec3& GetMiddle() const { return m_Middle; }

    private:
        struct Tile
        {
            float MinHeight = 0.0f; // world units
            float MaxHeight = 0.0f;
            bool Written = false;
        };

        void BeginCells();
        bool WriteCell(uint32_t x, uint32_t z);
        void FinishScene();

        Settings m_Settings;
        std::filesystem::path m_AssetDirectory;
        TaskFuture<void> m_Job;
        std::atomic<uint32_t> m_Grid{ 0 };    // N, once the first tile's size is known
        std::atomic<uint32_t> m_Done{ 0 };    // tiles written
        std::atomic<uint64_t> m_Bytes{ 0 };   // written so far
        std::vector<Tile> m_Tiles;            // N x N, z-major; written by the job, read once it is done
        std::shared_ptr<std::string> m_Error;
        bool m_Failed = false;
        bool m_Finished = false;
        AssetHandle m_Scene = 0;
        // Main thread, once the tiles are written: the world scene being made, the scene each cell's entities are made in (emptied
        // after each), and how far the cells are.
        Ref<Scene> m_World;
        Ref<Scene> m_CellScene;
        uint32_t m_CellsWritten = 0;
        uint64_t m_CellBytes = 0;
        AssetHandle m_PropMeshes[3] = {};
        // Then the cells' cooked files, on the workers.
        std::vector<std::filesystem::path> m_CellFiles;
        bool m_Cooking = false;
        TaskFuture<void> m_CookJob;
        std::atomic<uint32_t> m_CellsCooked{ 0 };
        std::atomic<uint64_t> m_CookedBytes{ 0 }, m_BinaryEntities{ 0 }, m_CookedEntities{ 0 };
        glm::vec3 m_Middle{ 0.0f };
    };
}
