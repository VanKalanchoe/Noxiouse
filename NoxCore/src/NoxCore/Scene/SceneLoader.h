#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    class Scene;

    // Opens a scene file without stopping the program. The file is read and parsed (YAML: most of a large scene's cost) as a JobSystem
    // job (Normal priority, a background worker); the main thread then creates the entities in slices of a few milliseconds a frame (Step), into a new scene nothing
    // draws or updates yet. The caller swaps it in once Step says it is complete, and shows Progress meanwhile.
    //
    // A scene in its runtime form (a packaged game's) is read whole in the last Step: it is not text to parse.
    //
    // Or a level, streamed into a scene that runs (Scene::LoadLevel): its entities are created in that scene, each marked with a
    // LevelComponent of `level`, and CreatedEntities lists them; it fails, creating nothing, when one of its entity IDs is in the scene
    // already. A runtime-form level is parsed and created in slices like a text one.
    //
    // Or a world partition's cell (Scene::LoadWorldCells, the streaming): like a level, from its file or from text in memory (a Play
    // session's copy of the edited cells), except that an entity already in the scene is skipped with a warning instead of refusing
    // the cell (the editor's own copy of an entity being saved into a cell it then loads).
    class SceneLoader
    {
    public:
        explicit SceneLoader(std::filesystem::path path);
        SceneLoader(std::filesystem::path path, Scene& into, AssetHandle level, bool cell = false);
        SceneLoader(std::string name, std::string text, Scene& into, AssetHandle level, bool cell);
        // A world cell into a scene of its own (§5.13.3's built scene): read, parsed and built on the worker, so the main thread only
        // takes its entities (Step, once). Nothing may use the scene before Step returns true.
        SceneLoader(std::filesystem::path path, const Ref<Scene>& cellScene, AssetHandle cell);
        SceneLoader(std::string name, std::string text, const Ref<Scene>& cellScene, AssetHandle cell);
        ~SceneLoader(); // a read still going on finishes on its own (it shares only the parse result with the loader)

        SceneLoader(const SceneLoader&) = delete;
        SceneLoader& operator=(const SceneLoader&) = delete;

        // Main thread. Creates entities for about `budgetMs`; true once the scene is complete or the load failed (Failed()).
        bool Step(double budgetMs);
        bool Failed() const { return m_Failed; }
        // Blocks until the file is read and parsed (Play's first cells, which must be in before the first frame).
        void WaitForRead() const { m_Read.Wait(); }
        // 0..1 over the entities (0 while the file is still being read), and what is going on, for a progress display.
        float GetProgress() const;
        std::string GetStage() const;
        const std::filesystem::path& GetPath() const { return m_Path; }
        Ref<Scene> GetScene() const { return m_Scene; }
        const std::vector<UUID>& GetCreatedEntities() const { return m_Created; }
        // Where the time went (world streaming's stats, §5.13.4): reading and parsing the file on a worker (valid once Step has
        // started creating), and creating the entities on the main thread, in how many Step calls.
        double GetReadMs() const;
        double GetWorkerBuildMs() const; // a cell built on the worker
        bool WasCooked() const;          // a cell read from its cooked file (§5.13.4)
        double GetCreateMs() const { return m_CreateMs; }
        uint32_t GetCreateSteps() const { return m_CreateSteps; }

        struct Parsed; // the YAML document (kept out of this header)

    private:
        void StartReading(std::string text, Ref<Scene> buildInto = nullptr);

        std::filesystem::path m_Path;
        std::shared_ptr<Parsed> m_Parsed; // shared with the reading job
        TaskFuture<void> m_Read;
        Ref<Scene> m_Scene;
        Scene* m_Into = nullptr; // a level's scene (additive), else m_Scene
        AssetHandle m_Level = 0;
        bool m_Cell = false;
        std::vector<UUID> m_Created;
        bool m_Started = false;
        bool m_Failed = false;
        size_t m_Next = 0;
        size_t m_Total = 0;
        double m_CreateMs = 0.0;
        uint32_t m_CreateSteps = 0;
    };
}
