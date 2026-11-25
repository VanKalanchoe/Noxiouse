#pragma once

#include <functional>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Core/Ref.h"

namespace NoxUI
{
        class UIContext;
        class UIElement;
        class UIStackBox;
        class UITextInput;
    }

namespace Nox
{
    class Renderer;

    // Window > Project Settings: what the project keeps (the .nproj, saved at each change): the start scene, the render quality
    // preset and the renderer settings saved on top of it, the packaged game's window, the physical surfaces. Split from the old
    // editor's Settings window, which mixed these with session-only renderer switches (those are in Renderer Settings).
    class ProjectSettingsPanel
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
        const Ref<NoxUI::UIElement>& BuildUI(NoxUI::UIContext* context);
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }
        // Once a frame while shown.
        void Update();
        // Another project opened: the rows read its settings (built again).
        void MarkDirty() { m_Rebuild = true; m_AuditCompleted = false; }

    private:
        struct MaterialAuditResult
        {
            std::vector<std::string> NonPhysical;
            size_t TotalMaterials = 0;
        };

        void StartMaterialAudit();
        void RebuildSurfaces();
        void CommitSurface(size_t index, const std::string& name);

        Hooks m_Hooks;
        NoxUI::UIContext* m_Context = nullptr;
        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UIStackBox> m_Body;
        Ref<NoxUI::UIStackBox> m_Surfaces;
        std::vector<Ref<NoxUI::UITextInput>> m_SurfaceFields; // index i: surface i (none for "Default")
        std::unordered_map<std::string, bool> m_Expanded;
        std::future<MaterialAuditResult> m_AuditFuture;
        MaterialAuditResult m_AuditResult;
        bool m_AuditCompleted = false;
        bool m_AuditRunning = false;
        bool m_Rebuild = false;
        bool m_SurfacesDirty = false;
    };
}
