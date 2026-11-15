#include "ProjectManagerV2Layer.h"

#include <algorithm>
#include <cctype>

#include "EditorV2Layer.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Widgets/UIBadge.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UISearchBar.h"
#include "NoxCore/UI/Widgets/UISegmentedControl.h"
#include "NoxCore/Utils/Utils.h"
#include "ProjectManager/ProjectLibrary.h"

namespace Nox
{
    namespace
    {
        std::string ToLower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        const char* const kQualityNames[] = { "Auto", "Low", "Medium", "High", "Ultra" };
    }

    ProjectManagerV2Layer::ProjectManagerV2Layer() : Layer("ProjectManagerV2Layer")
    {
        m_OpenLast = ProjectLibrary::GetOpenLastProjectAtStartup();
        m_NewLocation = ProjectLibrary::GetDefaultProjectLocation().generic_string();

        for (int i = 0; i < 2; ++i)
        {
            const auto tmpl = i == 0 ? ProjectLibrary::ProjectTemplate::Basic : ProjectLibrary::ProjectTemplate::Blank;
            const std::filesystem::path pic = ProjectLibrary::TemplatePicturePath(tmpl);
            if (!pic.empty())
                m_TemplatePictures[i] = TextureImporter::LoadTexture2D(pic, { .flip = false, .generateMips = false });
        }

        RefreshEntries();
        BuildUI();
    }

    ProjectManagerV2Layer::~ProjectManagerV2Layer() = default;

    void ProjectManagerV2Layer::OnUpdate(Timestep /*ts*/)
    {
        if (m_PendingOpen.empty())
            return;

        std::filesystem::path file = std::move(m_PendingOpen);
        m_PendingOpen.clear();

        Application& app = Application::Get();
        app.PopLayer<ProjectManagerV2Layer>();
        app.PushLayer<EditorV2Layer>(file);
    }

    void ProjectManagerV2Layer::OpenProjectFile(const std::filesystem::path& projectFile)
    {
        if (projectFile.empty())
            return;
        m_PendingOpen = projectFile;
    }

    void ProjectManagerV2Layer::RefreshEntries()
    {
        m_Projects.clear();

        auto pushEntry = [this](const std::filesystem::path& file, bool isSample)
        {
            std::error_code ec;
            Entry e;
            e.File = file;
            e.Exists = std::filesystem::exists(file, ec);
            e.Sample = isSample;
            e.Name = ProjectLibrary::ReadProjectName(file);
            const std::filesystem::path pic = ProjectLibrary::FindProjectPicture(file);
            if (!pic.empty())
                e.Thumbnail = TextureImporter::LoadTexture2D(pic, { .flip = false, .generateMips = false });
            m_Projects.push_back(std::move(e));
        };

        const std::filesystem::path sample = ProjectLibrary::FindSampleProject();
        for (const std::filesystem::path& file : ProjectLibrary::LoadRecentProjects())
        {
            const bool isSample = !sample.empty() && file == sample;
            pushEntry(file, isSample);
        }

        if (!sample.empty())
        {
            const bool alreadyListed = std::any_of(m_Projects.begin(), m_Projects.end(),
                [&sample](const Entry& e) { return e.File == sample; });
            if (!alreadyListed)
                pushEntry(sample, true);
        }
    }

    void ProjectManagerV2Layer::RebuildProjectList()
    {
        if (!m_ProjectList)
            return;

        m_ProjectList->ClearChildren();
        const std::string needle = ToLower(m_SearchFilter);
        const UI::UIColor muted = { 0.58f, 0.62f, 0.69f, 1.0f };
        const UI::UIColor accent = { 0.26f, 0.53f, 0.96f, 1.0f };

        int shown = 0;
        for (const Entry& entry : m_Projects)
        {
            if (!needle.empty())
            {
                const std::string hayName = ToLower(entry.Name);
                const std::string hayPath = ToLower(entry.File.generic_string());
                if (hayName.find(needle) == std::string::npos && hayPath.find(needle) == std::string::npos)
                    continue;
            }
            ++shown;

            const uint32_t thumbSlot = entry.Thumbnail ? entry.Thumbnail->GetDescriptorIndexSlot() : ~0u;
            auto thumb = UI::MakeUI<UI::UIImage>(thumbSlot, UI::UIVec2{ 128.0f, 72.0f });
            thumb->CornerRadius = UI::UICornerRadius::All(4.0f);

            auto titleRow = UI::HStack(8.0f, {
                UI::Label(entry.Name).VAlign(UI::UIAlignment::Center)
            });
            if (entry.Sample)
            {
                auto badge = UI::MakeUI<UI::UIBadge>("Sample", accent);
                badge->VAlign = UI::UIAlignment::Center;
                titleRow->AddChild(badge);
            }
            if (!entry.Exists)
            {
                auto missing = UI::MakeUI<UI::UIBadge>("Missing", UI::UIColor{ 0.84f, 0.31f, 0.33f, 1.0f });
                missing->VAlign = UI::UIAlignment::Center;
                titleRow->AddChild(missing);
            }

            auto pathLabel = UI::Label(entry.File.parent_path().generic_string());
            pathLabel->UseColorOverride = true;
            pathLabel->ColorOverride = muted;

            auto infoCol = UI::VStack(4.0f, { titleRow, pathLabel }).FillWidth().VAlign(UI::UIAlignment::Center);

            auto openBtn = UI::Button("Open", [this, file = entry.File]()
            {
                OpenProjectFile(file);
            }).Primary().Enabled(entry.Exists).VAlign(UI::UIAlignment::Center);

            auto actions = UI::HStack(6.0f, { openBtn }).VAlign(UI::UIAlignment::Center);
            if (!entry.Sample)
            {
                auto removeBtn = UI::Button("X", [this, file = entry.File]()
                {
                    ProjectLibrary::ForgetRecentProject(file);
                    RefreshEntries();
                    RebuildProjectList();
                }).Tooltip("Remove from recent list").VAlign(UI::UIAlignment::Center);
                actions->AddChild(removeBtn);
            }

            auto card = UI::HStack(12.0f, { thumb, infoCol, actions })
                .Style("Panel")
                .Background(true)
                .Padding(10.0f)
                .FillWidth();
            card->HeightMode = UI::UISizeMode::Auto;

            m_ProjectList->AddChild(card);
        }

        if (shown == 0)
        {
            auto empty = UI::Label("No matching projects. Create one on the right or Browse for a .nproj file.");
            empty->UseColorOverride = true;
            empty->ColorOverride = muted;
            m_ProjectList->AddChild(empty);
        }
    }

    void ProjectManagerV2Layer::UpdateNewProjectPreview()
    {
        if (m_TemplatePreview)
        {
            const auto& tex = m_TemplatePictures[m_TemplateIndex];
            m_TemplatePreview->TextureDescriptorSlot = tex ? tex->GetDescriptorIndexSlot() : ~0u;
        }
        if (m_TemplateDesc)
        {
            m_TemplateDesc->Text = (m_TemplateIndex == 0)
                ? "Basic: starts with a sky, sun, floor, and camera."
                : "Blank: starts with an empty scene.";
        }

        ProjectLibrary::NewProjectSettings settings;
        settings.Template = (m_TemplateIndex == 0)
            ? ProjectLibrary::ProjectTemplate::Basic
            : ProjectLibrary::ProjectTemplate::Blank;
        settings.Name = m_NewName;
        settings.Location = m_NewLocation;
        settings.WorldUnitsPerMeter = (m_UnitsIndex == 0) ? 100.0f : 1.0f;
        settings.RenderQuality = kQualityNames[m_QualityIndex];
        settings.CreateScripts = m_CreateScripts;

        const std::string err = ProjectLibrary::ValidateNewProject(settings);
        if (m_ErrorLabel)
        {
            m_ErrorLabel->Text = err;
            m_ErrorLabel->Visible = !err.empty();
        }
        if (m_CreateButton)
            m_CreateButton->SetEnabled(err.empty());
    }

    void ProjectManagerV2Layer::BuildUI()
    {
        UILayer* uiLayer = Application::Get().GetUILayer();
        if (!uiLayer)
            return;

        const UI::UIColor muted = { 0.58f, 0.62f, 0.69f, 1.0f };

        auto subtitle = UI::Label("Select an existing project or create a new one");
        subtitle->UseColorOverride = true;
        subtitle->ColorOverride = muted;

        auto topBar = UI::HStack(12.0f, {
            UI::VStack(2.0f, {
                UI::Label("Nox Engine — Project Browser"),
                subtitle
            }).FillWidth(),
            UI::CheckBox("Always open last project", m_OpenLast, [this](bool v)
            {
                m_OpenLast = v;
                ProjectLibrary::SetOpenLastProjectAtStartup(v);
            }).VAlign(UI::UIAlignment::Center),
            UI::Button("Browse...", [this]()
            {
                const std::string file = Utility::OpenFile("Nox Project\0nproj\0");
                if (!file.empty())
                    OpenProjectFile(file);
            }).VAlign(UI::UIAlignment::Center)
        })
        .Style("TopBar")
        .Background(true)
        .Padding(16.0f, 12.0f)
        .FillWidth();
        topBar->HeightMode = UI::UISizeMode::Auto;

        auto searchBar = UI::MakeUI<UI::UISearchBar>("Search recent projects...");
        searchBar->OnSearchChanged = [this](const std::string& q)
        {
            m_SearchFilter = q;
            RebuildProjectList();
        };

        m_ProjectList = UI::Scroll({}, 10.0f);
        RebuildProjectList();

        auto leftColumn = UI::Card("Recent Projects", {
            searchBar,
            m_ProjectList
        }).Fill(1.45f);

        auto tmplControl = UI::MakeUI<UI::UISegmentedControl>(
            std::vector<std::string>{ "Basic", "Blank" },
            m_TemplateIndex,
            [this](int idx)
            {
                m_TemplateIndex = idx;
                UpdateNewProjectPreview();
            }
        );

        m_TemplatePreview = UI::MakeUI<UI::UIImage>(~0u, UI::UIVec2{ 320.0f, 160.0f });
        m_TemplatePreview->WidthMode = UI::UISizeMode::Fill;
        m_TemplatePreview->CornerRadius = UI::UICornerRadius::All(6.0f);

        m_TemplateDesc = UI::Label();
        m_TemplateDesc->UseColorOverride = true;
        m_TemplateDesc->ColorOverride = muted;

        m_NameInput = UI::TextInput(m_NewName, "Project name").FillWidth();
        m_NameInput->OnChanged = [this](const std::string& s)
        {
            m_NewName = s;
            UpdateNewProjectPreview();
        };

        m_LocationInput = UI::TextInput(m_NewLocation, "Folder path").FillWidth();
        m_LocationInput->OnChanged = [this](const std::string& s)
        {
            m_NewLocation = s;
            UpdateNewProjectPreview();
        };

        auto browseFolderBtn = UI::Button("...", [this]()
        {
            const std::string folder = Utility::OpenFolder(m_NewLocation.empty() ? nullptr : m_NewLocation.c_str());
            if (!folder.empty())
            {
                m_NewLocation = folder;
                if (m_LocationInput)
                    m_LocationInput->Text = folder;
                UpdateNewProjectPreview();
            }
        });

        auto unitsControl = UI::MakeUI<UI::UISegmentedControl>(
            std::vector<std::string>{ "cm (UE)", "m (1.0)" },
            m_UnitsIndex,
            [this](int idx) { m_UnitsIndex = idx; }
        );

        auto qualityDropdown = UI::MakeUI<UI::UIDropdown>(
            &uiLayer->GetContext(),
            std::vector<std::string>{ "Auto", "Low", "Medium", "High", "Ultra" },
            m_QualityIndex,
            [this](int idx) { m_QualityIndex = idx; }
        );

        m_ErrorLabel = UI::Label();
        m_ErrorLabel->UseColorOverride = true;
        m_ErrorLabel->ColorOverride = { 0.92f, 0.36f, 0.38f, 1.0f };
        m_ErrorLabel->Visible = false;

        m_CreateButton = UI::Button("Create & Open", [this]()
        {
            ProjectLibrary::NewProjectSettings settings;
            settings.Template = (m_TemplateIndex == 0)
                ? ProjectLibrary::ProjectTemplate::Basic
                : ProjectLibrary::ProjectTemplate::Blank;
            settings.Name = m_NewName;
            settings.Location = m_NewLocation;
            settings.WorldUnitsPerMeter = (m_UnitsIndex == 0) ? 100.0f : 1.0f;
            settings.RenderQuality = kQualityNames[m_QualityIndex];
            settings.CreateScripts = m_CreateScripts;

            std::string error;
            const std::filesystem::path created = ProjectLibrary::CreateProject(settings, error);
            if (created.empty())
            {
                if (m_ErrorLabel)
                {
                    m_ErrorLabel->Text = error;
                    m_ErrorLabel->Visible = true;
                }
                return;
            }
            ProjectLibrary::SetDefaultProjectLocation(settings.Location);
            OpenProjectFile(created);
        }).Primary().HAlign(UI::UIAlignment::End);

        auto rightColumn = UI::Card("New Project", {
            tmplControl,
            m_TemplatePreview,
            m_TemplateDesc,
            UI::Label("Name"),
            m_NameInput,
            UI::Label("Location"),
            UI::HStack(6.0f, { m_LocationInput, browseFolderBtn }).FillWidth(),
            UI::HStack(10.0f, {
                UI::VStack(4.0f, { UI::Label("Units"), unitsControl }).FillWidth(),
                UI::VStack(4.0f, { UI::Label("Quality"), qualityDropdown }).FillWidth()
            }).FillWidth(),
            UI::CheckBox("Create C# Scripts Project", m_CreateScripts, [this](bool v) { m_CreateScripts = v; }),
            m_ErrorLabel,
            m_CreateButton
        }).Fill(1.0f);

        UpdateNewProjectPreview();

        uiLayer->SetRoot(
            UI::VStack(14.0f, {
                topBar,
                UI::HStack(14.0f, { leftColumn, rightColumn }).Fill()
            })
            .Padding(16.0f)
            .Style("Background")
            .Background(true)
        );
    }
}
