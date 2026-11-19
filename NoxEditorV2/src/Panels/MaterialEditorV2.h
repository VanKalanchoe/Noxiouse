#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Panels/AssetEditorV2.h"

namespace Nox
{
    namespace UI
    {
        class UIStackBox;
    }

    // A material (.nmat) in its own window: a live preview sphere, Save, and its properties as the Inspector's material card shows
    // them. Edits change the loaded material at once and mark it unsaved (MaterialEdits); Save writes the file; closing with unsaved
    // edits asks (Save / Don't Save / Cancel).
    class MaterialEditorV2 final : public AssetEditorV2
    {
    public:
        explicit MaterialEditorV2(AssetHandle material);

        AssetHandle GetHandle() const override { return m_Handle; }
        const std::string& GetName() const override { return m_Name; }
        const char* GetKind() const override { return "material"; }
        std::string GetWindowTitle() const override { return "Material: " + m_Name; }
        UI::UIVec2 GetDefaultSize() const override { return { 430.0f, 580.0f }; }

        Ref<UI::UIElement> BuildUI() override;
        void Update() override;
        bool IsDirty() const override;
        void Save() override;
        bool RevertToDisk() override;

    private:
        AssetHandle m_Handle = 0;
        std::string m_Name;
        Ref<UI::UIStackBox> m_Body;
        Ref<UI::UIElement> m_SaveButton;
        Ref<UI::UIElement> m_Status;
        Ref<UI::UIElement> m_Preview;
        std::unordered_map<std::string, bool> m_ExpandedSections;
        std::vector<std::function<void()>> m_Syncs;
        bool m_Rebuild = true;
    };
}
