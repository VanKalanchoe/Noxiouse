#pragma once
#include <functional>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"

namespace Nox
{
    class Scene;
    class Entity;

    // Unreal's Levels window (Backlog L1): the persistent level (the open scene) and its sub-levels -- other scenes edited together
    // with it. Per level: current (where new entities go), visible, locked, Always Loaded or streamed (by Level Streaming Volumes or
    // a script). Add an existing scene, make a new one, remove one, move the selection into the current level. A save writes every
    // level into its own file. In Play and Simulate: what is loaded, read-only. Window > Levels.
    class LevelsPanel
    {
    public:
        // Saves the scene and its sub-levels (Remove asks to save first).
        void SetSaveCallback(std::function<void()> save) { m_Save = std::move(save); }
        // The open scene's own asset (it cannot be its own sub-level); 0 when it was never saved.
        void SetOpenSceneCallback(std::function<AssetHandle()> openScene) { m_OpenScene = std::move(openScene); }

        // `scene`: the editor's scene while editing, the running copy in Play / Simulate; `selection`: the selected entities.
        void OnImGuiRender(const Ref<Scene>& scene, bool editing, const std::vector<Entity>& selection);

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }

    private:
        void DrawLevels(Scene& scene, bool editing);
        void DrawActions(Scene& scene, const std::vector<Entity>& selection);
        void DrawRemovePrompt(Scene& scene);
        void DrawNewLevelPrompt(Scene& scene);
        // A new empty scene file named `name` next to the open scene, added as a sub-level and made the current level.
        bool CreateLevel(Scene& scene, const std::string& name);

    private:
        bool m_Open = false;
        std::function<void()> m_Save;
        std::function<AssetHandle()> m_OpenScene;
        AssetHandle m_RemoveRequest = 0; // asks: save first, discard, cancel
        bool m_NewLevelRequest = false;
        char m_NewLevelName[128] = "NewLevel";
        std::string m_Message; // the last New Level / Add result, shown in the panel
    };
}
