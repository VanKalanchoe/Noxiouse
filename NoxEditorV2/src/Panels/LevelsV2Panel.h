#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/UI/Core/UITypes.h"

namespace Nox
{
    namespace UI
    {
        class UIContext;
        class UIElement;
        class UIStackBox;
        class UILabel;
    }
    class Scene;

    // Window > Levels (Unreal's Levels window): the persistent level (the open scene) and its sub-levels -- other scenes edited
    // together with it. Per level: current (where new entities go), visible, locked, Always Loaded or Streamed, its placement
    // (offset and yaw). Add an existing scene, make a new one, remove one, move the selection into the current level. In Play and
    // Simulate: what is loaded, read-only.
    class LevelsV2Panel
    {
    public:
        struct Hooks
        {
            std::function<Ref<Scene>()> ActiveScene;
            std::function<bool()> Editing;                       // false in Play / Simulate
            std::function<AssetHandle()> OpenScene;              // the open scene's own asset (0: never saved)
            std::function<std::vector<Entity>()> Selection;
            std::function<void()> Save;                          // the scene and its sub-levels
            std::function<void()> SceneChanged;                  // a level setting changed (an undo step; the scene is unsaved)
            std::function<void(const std::string& text, std::function<void()> save, std::function<void()> dontSave)> AskSave;
        };

        void SetHooks(Hooks hooks) { m_Hooks = std::move(hooks); }
        const Ref<UI::UIElement>& BuildUI(UI::UIContext* context);
        const Ref<UI::UIElement>& GetRoot() const { return m_Root; }
        void Update();

    private:
        uint64_t Signature() const;
        void Rebuild();
        void ShowNewLevelDialog();
        void ShowPlacement(AssetHandle level, const UI::UIRect& anchor);
        bool CreateLevel(Scene& scene, const std::string& name, std::string& message);
        void Changed() const;

        Hooks m_Hooks;
        UI::UIContext* m_Context = nullptr;
        Ref<UI::UIElement> m_Root;
        Ref<UI::UIStackBox> m_Body;
        Ref<UI::UILabel> m_Message;
        std::vector<std::function<void()>> m_Syncs;
        uint64_t m_Signature = 0;
        bool m_Rebuild = true;
    };
}
