#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Panels/AssetEditor.h"

namespace NoxUI
{
        class UIStackBox;
    }

namespace Nox
{

    // A material (.nmat) in its own window: a live preview sphere, Save, and its properties as the Inspector's material card shows
    // them. Edits change the loaded material at once and mark it unsaved (MaterialEdits); Save writes the file; closing with unsaved
    // edits asks (Save / Don't Save / Cancel).
    class MaterialEditor final : public AssetEditor
    {
    public:
        explicit MaterialEditor(AssetHandle material);

        AssetHandle GetHandle() const override { return m_Handle; }
        const std::string& GetName() const override { return m_Name; }
        const char* GetKind() const override { return "material"; }
        std::string GetWindowTitle() const override { return "Material: " + m_Name; }
        NoxUI::UIVec2 GetDefaultSize() const override { return { 430.0f, 580.0f }; }

        Ref<NoxUI::UIElement> BuildUI() override;
        void Update() override;
        bool IsDirty() const override;
        void Save() override;
        bool RevertToDisk() override;

    private:
        AssetHandle m_Handle = 0;
        std::string m_Name;
        Ref<NoxUI::UIStackBox> m_Body;
        Ref<NoxUI::UIElement> m_SaveButton;
        Ref<NoxUI::UIElement> m_Status;
        Ref<NoxUI::UIElement> m_Preview;
        std::unordered_map<std::string, bool> m_ExpandedSections;
        bool m_Rebuild = true;
    };
}
