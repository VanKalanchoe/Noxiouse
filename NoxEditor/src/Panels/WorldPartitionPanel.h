#pragma once
#include <functional>
#include <set>
#include <vector>
#include <glm/glm.hpp>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/WorldPartition.h"

namespace Nox
{
    class Scene;

    // World partition (docs/Engine_Architecture_Plan_2026.md §5.13.1), like Unreal's World Partition window: the world's cells on a
    // top-down map -- their state (unloaded, loading, loaded), their content bounds, the editor camera and its loading range -- to
    // load and unload cells or regions by hand while editing; in Play and Simulate the streaming's state, read-only. Also converts a
    // scene into a world and changes its settings. Window > World Partition.
    class WorldPartitionPanel
    {
    public:
        // Saves the scene (unloading a changed cell asks to save first).
        void SetSaveCallback(std::function<void()> save) { m_Save = std::move(save); }

        // `scene`: the scene shown (the editor's while editing, the running copy in Play / Simulate); `editing`: cells can be changed.
        void OnImGuiRender(const Ref<Scene>& scene, const glm::vec3& cameraPosition, bool editing);

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }
        // The loaded cells' content bounds in the viewports.
        bool ShowCellsInViewport() const { return m_Open && m_ShowInViewport; }

    private:
        void DrawSettings(Scene& scene, bool editing);
        void DrawToolbar(Scene& scene, const glm::vec3& cameraPosition, bool editing);
        void DrawMap(Scene& scene, const glm::vec3& cameraPosition, bool editing);
        void DrawUnloadPrompt(Scene& scene);
        // Unloads the cells, asking first when one has unsaved changes.
        void RequestUnload(Scene& scene, std::vector<WorldCellCoord> cells);

    private:
        bool m_Open = false;
        bool m_ShowInViewport = true;
        bool m_FollowCamera = true;
        glm::vec2 m_ViewCenter{ 0.0f };   // world x, z at the map's center
        float m_UnitsPerPixel = 0.0f;     // 0: fitted to the world on the next draw
        std::set<WorldCellCoord> m_Selection;
        bool m_BoxSelecting = false;
        glm::vec2 m_BoxStart{ 0.0f };
        std::vector<WorldCellCoord> m_PendingUnload; // waiting for the save prompt's answer
        bool m_OpenUnloadPrompt = false;
        WorldPartitionSettings m_NewSettings;  // the convert / cell size fields
        const Scene* m_SettingsScene = nullptr; // the scene they were taken from
        std::function<void()> m_Save;
    };
}
