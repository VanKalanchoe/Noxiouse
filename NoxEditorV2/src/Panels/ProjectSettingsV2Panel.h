#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Core/Ref.h"

namespace Nox
{
    namespace UI
    {
        class UIContext;
        class UIElement;
        class UIStackBox;
        class UITextInput;
    }
    class Renderer;

    // Window > Project Settings: what the project keeps (the .nproj, saved at each change): the start scene, the render quality
    // preset and the renderer settings saved on top of it, the packaged game's window, the physical surfaces. Split from the old
    // editor's Settings window, which mixed these with session-only renderer switches (those are in Renderer Settings).
    class ProjectSettingsV2Panel
    {
    public:
        struct Hooks
        {
            Renderer* Render = nullptr;
            // Applies a quality preset (the editor keeps an orthographic view raster only).
            std::function<void(int quality)> ApplyQuality;
            // A physical surface was renamed: what uses the old name in the open scene follows (materials are done here).
            std::function<void(const std::string& from, const std::string& to)> SurfaceRenamed;
        };

        void SetHooks(Hooks hooks) { m_Hooks = std::move(hooks); }
        const Ref<UI::UIElement>& BuildUI(UI::UIContext* context);
        const Ref<UI::UIElement>& GetRoot() const { return m_Root; }
        // Once a frame while shown.
        void Update();
        // Another project opened: the rows read its settings (built again).
        void MarkDirty() { m_Rebuild = true; }

    private:
        void RebuildSurfaces();
        void CommitSurface(size_t index, const std::string& name);

        Hooks m_Hooks;
        UI::UIContext* m_Context = nullptr;
        Ref<UI::UIElement> m_Root;
        Ref<UI::UIStackBox> m_Body;
        Ref<UI::UIStackBox> m_Surfaces;
        std::vector<Ref<UI::UITextInput>> m_SurfaceFields; // index i: surface i (none for "Default")
        std::vector<std::function<void()>> m_Syncs;
        std::unordered_map<std::string, bool> m_Expanded;
        bool m_Rebuild = false;
        bool m_SurfacesDirty = false;
    };
}
