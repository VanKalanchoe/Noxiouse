#pragma once

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/WorldPartition.h"

namespace NoxUI
{
        class UIContext;
        class UIElement;
        class UIStackBox;
    }

namespace Nox
{
    class HLODBuilder;
    class Scene;
    class WorldPartitionMap;

    // Window > World Partition (Unreal's World Partition window): converts a scene into a world and changes its settings; the
    // world's cells on a top-down map (unloaded, loading, loaded, activated; their content bounds; the editor camera and its
    // loading range) to load and unload cells by hand while editing; the HLOD settings and Build HLODs. In Play and Simulate the
    // streaming's state, read-only.
    class WorldPartitionV2Panel
    {
    public:
        struct Hooks
        {
            std::function<Ref<Scene>()> ActiveScene;
            std::function<bool()> Editing;
            std::function<glm::dvec3()> StreamingSource;   // the camera the cells stream around
            std::function<void()> Save;
            std::function<void()> SceneChanged;            // a world setting changed (an undo step; unsaved)
            std::function<void()> BuildHLODs;
            std::function<const HLODBuilder*()> RunningBuild;
            std::function<void(const std::string& text, std::function<void()> save, std::function<void()> dontSave)> AskSave;
        };

        void SetHooks(Hooks hooks) { m_Hooks = std::move(hooks); }
        const Ref<NoxUI::UIElement>& BuildUI(NoxUI::UIContext* context);
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }
        void Update();
        // The loaded cells' content bounds drawn in the viewports (while the window is open and Show in Viewport is on).
        bool ShowCellsInViewport() const { return m_ShowInViewport; }

    private:
        uint64_t Signature() const;
        void Rebuild();
        // Unloads the cells, asking first (Save / Don't Save / Cancel) when one has unsaved changes.
        void RequestUnload(std::vector<WorldCellCoord> cells);
        void Changed() const;

        Hooks m_Hooks;
        NoxUI::UIContext* m_Context = nullptr;
        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UIStackBox> m_Body;
        Ref<NoxUI::UIElement> m_Map; // (a WorldPartitionMap, defined in the .cpp)
        std::unordered_map<std::string, bool> m_Expanded;
        uint64_t m_Signature = 0;
        bool m_Rebuild = true;
        bool m_ShowInViewport = true;
        bool m_FollowCamera = true;
        WorldPartitionSettings m_NewSettings;  // the convert / cell size fields
        const Scene* m_SettingsScene = nullptr; // the scene they were taken from
        std::set<WorldCellCoord> m_Selection;   // cells picked on the map
    };
}
