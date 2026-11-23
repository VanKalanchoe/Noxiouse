#pragma once

#include <memory>

#include <filesystem>
#include <string>
#include <vector>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/Texture2D.h"

namespace NoxUI
{
        class UIButton;
        class UILabel;
        class UIScrollView;
        class UIStackBox;
        class UITextInput;
    }

namespace Nox
{

    class ProjectManagerV2Layer : public Layer
    {
    public:
        ProjectManagerV2Layer();
        ~ProjectManagerV2Layer() override;

        void OnUpdate(Timestep ts) override;

    private:
        // Dialogs answer later (E7): their callbacks check this (false once the layer is gone).
        std::shared_ptr<bool> m_Alive = std::make_shared<bool>(true);
        struct Entry
        {
            std::filesystem::path File;
            std::string Name;
            std::string Modified;
            bool Exists = false;
            bool Sample = false;
            Ref<Texture2D> Thumbnail;
        };

        void RefreshEntries();
        void RebuildProjectList();
        void BuildUI();
        void OpenProjectFile(const std::filesystem::path& projectFile, bool createBasicScene = false);
        void UpdateNewProjectPreview();

        std::vector<Entry> m_Projects;
        std::filesystem::path m_PendingOpen;
        bool m_Opening = false;

        std::string m_SearchFilter;
        std::string m_NewName = "MyGame";
        std::string m_NewLocation;
        int m_TemplateIndex = 0; // 0: Basic, 1: Blank
        int m_UnitsIndex = 0;    // 0: cm (100), 1: m (1)
        int m_QualityIndex = 0;
        bool m_CreateScripts = true;
        bool m_OpenLast = false;

        Ref<Texture2D> m_TemplatePictures[2];
        Ref<NoxUI::UIScrollView> m_ProjectList;
        Ref<NoxUI::UIStackBox> m_TemplateTiles[2];
        Ref<NoxUI::UILabel> m_OpenErrorLabel;
        Ref<NoxUI::UITextInput> m_NameInput;
        Ref<NoxUI::UITextInput> m_LocationInput;
        Ref<NoxUI::UILabel> m_CreatesLabel;
        Ref<NoxUI::UILabel> m_CreatesDetailsLabel;
        Ref<NoxUI::UILabel> m_ErrorLabel;
        Ref<NoxUI::UIButton> m_CreateButton;
    };
}
