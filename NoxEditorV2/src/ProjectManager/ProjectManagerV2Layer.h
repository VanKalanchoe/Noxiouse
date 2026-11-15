#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"

namespace Nox
{
    namespace UI
    {
        class UIButton;
        class UIImage;
        class UILabel;
        class UIScrollView;
        class UITextInput;
    }

    class ProjectManagerV2Layer : public Layer
    {
    public:
        ProjectManagerV2Layer();
        ~ProjectManagerV2Layer() override;

        void OnUpdate(Timestep ts) override;

    private:
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
        Ref<UI::UIScrollView> m_ProjectList;
        Ref<UI::UIImage> m_TemplatePreview;
        Ref<UI::UILabel> m_TemplateDesc;
        Ref<UI::UILabel> m_OpenErrorLabel;
        Ref<UI::UITextInput> m_NameInput;
        Ref<UI::UITextInput> m_LocationInput;
        Ref<UI::UILabel> m_CreatesLabel;
        Ref<UI::UILabel> m_ErrorLabel;
        Ref<UI::UIButton> m_CreateButton;
    };
}
