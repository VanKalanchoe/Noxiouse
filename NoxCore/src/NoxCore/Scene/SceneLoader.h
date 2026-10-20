#pragma once
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Scene;

    // Opens a scene file without stopping the program. The file is read and parsed (YAML: most of a large scene's cost) on a thread of
    // its own; the main thread then creates the entities in slices of a few milliseconds a frame (Step), into a new scene nothing
    // draws or updates yet. The caller swaps it in once Step says it is complete, and shows Progress meanwhile.
    //
    // A scene in its runtime form (a packaged game's) is read whole in the last Step: it is not text to parse.
    class SceneLoader
    {
    public:
        explicit SceneLoader(std::filesystem::path path);
        ~SceneLoader(); // waits for the reading thread (a loader dropped before it finished)

        SceneLoader(const SceneLoader&) = delete;
        SceneLoader& operator=(const SceneLoader&) = delete;

        // Main thread. Creates entities for about `budgetMs`; true once the scene is complete or the load failed (Failed()).
        bool Step(double budgetMs);
        bool Failed() const { return m_Failed; }
        // 0..1 over the entities (0 while the file is still being read), and what is going on, for a progress display.
        float GetProgress() const;
        std::string GetStage() const;
        const std::filesystem::path& GetPath() const { return m_Path; }
        Ref<Scene> GetScene() const { return m_Scene; }

    private:
        struct Parsed; // the YAML document (kept out of this header)

        std::filesystem::path m_Path;
        std::thread m_Thread;
        std::atomic<bool> m_ReadDone{ false };
        std::unique_ptr<Parsed> m_Parsed;
        Ref<Scene> m_Scene;
        bool m_Started = false;
        bool m_Failed = false;
        size_t m_Next = 0;
        size_t m_Total = 0;
    };
}
