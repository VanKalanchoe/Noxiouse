#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/PostProcessSettings.h"
#include "NoxCore/Scene/Entity.h"

namespace NoxUI
{
        class UIContext;
        class UIElement;
        class UIPropertyRows;
    }

namespace Nox
{
    class EditorCamera;
    class Renderer;

    // The post-process rows (PostProcessSettings): Renderer Settings shows the editor's copy, a Post Process Volume's Inspector card
    // its own. `changed` after each edit.
    void AddPostProcessSettingsRows(NoxUI::UIPropertyRows& rows, PostProcessSettings& post, std::function<void()> changed = {});

    // Window > Renderer Settings: how this session renders (debug views, exposure, anti-aliasing, DLSS, shadows, ray tracing, GI,
    // denoisers, streaming pools, the editor's post effects). Not saved: what a project keeps is in Project Settings (the render
    // quality preset and its overrides) and in the scene (a Post Process Volume). Every row reads the renderer each frame, so a
    // preset, the console or MCP changing a setting shows here at once.
    class RendererSettingsPanel
    {
    public:
        struct Hooks
        {
            Renderer* Render = nullptr;
            EditorCamera* Camera = nullptr;
            std::function<Entity()> Hovered;          // the entity under the pointer (the old Stats window)
            std::function<bool()> DumpFrameGraph;     // writes SceneUpdate.dot; false: nothing to dump
        };

        void SetHooks(Hooks hooks) { m_Hooks = std::move(hooks); }
        const Ref<NoxUI::UIElement>& BuildUI(NoxUI::UIContext* context);
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }
        // Once a frame while it is shown: the rows follow the renderer.
        void Update();

    private:
        Hooks m_Hooks;
        Ref<NoxUI::UIElement> m_Root;
        std::unordered_map<std::string, bool> m_Expanded;
        glm::vec3 m_FogAlbedo{ 1.0f };
    };
}
