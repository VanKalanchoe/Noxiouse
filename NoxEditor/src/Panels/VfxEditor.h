#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "Panels/AssetEditor.h"
#include "NoxCore/Asset/VfxAsset.h"

namespace NoxUI
{
    class UIStackBox;
    class UITimeline;
    class UILabel;
}

namespace Nox
{
    class Scene;

    // Dedicated Niagara-style Multi-Layer VFX Editor window (.nvfx).
    // Double-clicking any .nvfx asset in the Content Browser (or clicking "Open VFX Editor" in the Inspector)
    // opens this dockable/floating window with:
    //  - Top toolbar: Save / Revert, Preset loader (Teleport / Fireball / Lightning), Playback & Duration controls
    //  - Left column: Multi-layer Emitter/Module Stack (add, reorder, duplicate, toggle, delete) + User Parameters Blackboard
    //  - Right column: Full Inspector for the selected layer + interactive UICurveEditor (Size Over Life)
    //  - Bottom bar: Scrubbable UITimeline synced live to every VfxComponent in the active scene using this asset.
    class VfxEditor final : public AssetEditor
    {
    public:
        struct Hooks
        {
            std::function<Ref<Scene>()> ActiveScene;
        };

        VfxEditor(AssetHandle vfxHandle, Hooks hooks);
        ~VfxEditor() override;

        AssetHandle GetHandle() const override { return m_Handle; }
        const std::string& GetName() const override { return m_Name; }
        const char* GetKind() const override { return "VFX system"; }
        std::string GetWindowTitle() const override { return "VFX Editor: " + m_Name; }
        NoxUI::UIVec2 GetDefaultSize() const override { return { 1080.0f, 660.0f }; }

        Ref<NoxUI::UIElement> BuildUI() override;
        void Update() override;
        bool IsDirty() const override { return m_Dirty; }
        void Save() override;
        bool RevertToDisk() override;

    private:
        VfxAsset* GetAsset() const;
        void MarkDirty(bool restartPreview = false);
        void SyncScenePreview(bool restart, float* scrubTime = nullptr);
        void RebuildLeftPane();
        void RebuildDetailsPane();
        void SyncTimelineMarkers();

        AssetHandle m_Handle = 0;
        std::string m_Name;
        Hooks m_Hooks;
        bool m_Dirty = false;
        bool m_Playing = true;
        int m_SelectedLayer = 0;
        float m_Playhead = 0.0f;
        std::chrono::steady_clock::time_point m_LastTick{};

        Ref<NoxUI::UIStackBox> m_LayersBox;
        Ref<NoxUI::UIStackBox> m_ParamsBox;
        Ref<NoxUI::UIStackBox> m_DetailsBox;
        Ref<NoxUI::UITimeline> m_Timeline;
        Ref<NoxUI::UILabel> m_TimeLabel;
        Ref<NoxUI::UILabel> m_StatusLabel;
    };
}
