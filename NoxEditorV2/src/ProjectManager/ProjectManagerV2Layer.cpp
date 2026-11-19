#include "ProjectManagerV2Layer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_video.h>

#include "EditorV2Layer.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Renderer/Renderer.h"
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

        std::string Age(const std::filesystem::path& file)
        {
            std::error_code error;
            const auto written = std::filesystem::last_write_time(file, error);
            if (error)
                return {};
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                std::filesystem::file_time_type::clock::now() - written).count();
            char text[64];
            if (seconds < 60)
                return "just now";
            if (seconds < 3600)
                std::snprintf(text, sizeof(text), "%lld minutes ago", static_cast<long long>(seconds / 60));
            else if (seconds < 86400)
                std::snprintf(text, sizeof(text), "%lld hours ago", static_cast<long long>(seconds / 3600));
            else
                std::snprintf(text, sizeof(text), "%lld days ago", static_cast<long long>(seconds / 86400));
            return text;
        }

        // The project's Render Quality choices: "Auto" (the preset this GPU should run) and the presets by their names.
        std::vector<std::string> QualityChoices()
        {
            std::vector<std::string> choices{ "Auto" };
            for (Renderer::RenderQuality quality : { Renderer::RenderQuality::Low, Renderer::RenderQuality::Medium, Renderer::RenderQuality::High,
                                                     Renderer::RenderQuality::RayTracedAll, Renderer::RenderQuality::RasterAll })
                choices.push_back(Renderer::GetQualityName(quality));
            return choices;
        }

        void ShowInFolder(const std::filesystem::path& folder)
        {
            SDL_OpenURL(("file:///" + folder.generic_string()).c_str());
        }
    }

    ProjectManagerV2Layer::ProjectManagerV2Layer() : Layer("ProjectManagerV2Layer")
    {
        if (auto win = Application::Get().getWindow(); win && win->getHandle())
            SDL_SetWindowTitle(win->getHandle(), "Nox Project Manager");

        m_OpenLast = ProjectLibrary::GetOpenLastProjectAtStartup();
        m_NewLocation = ProjectLibrary::GetDefaultProjectLocation().generic_string();

        for (int i = 0; i < 2; ++i)
        {
            std::error_code error;
            const auto tmpl = i == 0 ? ProjectLibrary::ProjectTemplate::Basic : ProjectLibrary::ProjectTemplate::Blank;
            const std::filesystem::path pic = ProjectLibrary::TemplatePicturePath(tmpl);
            if (!pic.empty() && std::filesystem::exists(pic, error))
                m_TemplatePictures[i] = TextureImporter::LoadTexture2D(pic, { .flip = false, .generateMips = false });
        }

        RefreshEntries();
        BuildUI();
    }

    ProjectManagerV2Layer::~ProjectManagerV2Layer()
    {
        *m_Alive = false;
        for (Entry& entry : m_Projects)
            if (entry.Thumbnail)
                Renderer::DeferAssetRelease(entry.Thumbnail);
        for (Ref<Texture2D>& picture : m_TemplatePictures)
            if (picture)
                Renderer::DeferAssetRelease(picture);
    }

    void ProjectManagerV2Layer::OnUpdate(Timestep /*ts*/)
    {
    }

    void ProjectManagerV2Layer::OpenProjectFile(const std::filesystem::path& projectFile, bool createBasicScene)
    {
        if (m_Opening || projectFile.empty())
            return;

        std::error_code error;
        if (!std::filesystem::exists(projectFile, error))
        {
            if (m_OpenErrorLabel)
            {
                m_OpenErrorLabel->Text = "The project file " + projectFile.generic_string() + " does not exist (moved or deleted?).";
                m_OpenErrorLabel->Visible = true;
            }
            return;
        }

        if (m_OpenErrorLabel)
            m_OpenErrorLabel->Visible = false;

        ProjectLibrary::RememberRecentProject(projectFile);
        m_Opening = true;

        const std::filesystem::path file = projectFile;
        Application::Get().QueueAfterFrame([file, createBasicScene]()
        {
            Application& app = Application::Get();
            app.PopLayer<ProjectManagerV2Layer>();
            app.PushLayer<EditorV2Layer>(file, createBasicScene);
        });
    }

    void ProjectManagerV2Layer::RefreshEntries()
    {
        for (Entry& entry : m_Projects)
            if (entry.Thumbnail)
                Renderer::DeferAssetRelease(entry.Thumbnail);
        m_Projects.clear();

        auto pushEntry = [this](const std::filesystem::path& file, bool isSample)
        {
            std::error_code error;
            Entry e;
            e.File = file;
            e.Exists = std::filesystem::exists(file, error);
            e.Sample = isSample;
            e.Name = e.Exists ? ProjectLibrary::ReadProjectName(file) : file.stem().string();
            e.Modified = e.Exists ? Age(file) : std::string();
            const std::filesystem::path pic = e.Exists ? ProjectLibrary::FindProjectPicture(file) : std::filesystem::path();
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
        const UI::UIColor missingRed = { 0.84f, 0.31f, 0.33f, 1.0f };

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

            Ref<UI::UIElement> thumbWidget;
            if (entry.Thumbnail)
            {
                auto thumb = UI::MakeUI<UI::UIImage>(entry.Thumbnail->GetDescriptorIndexSlot(), UI::UIVec2{ 128.0f, 72.0f });
                thumb->CornerRadius = UI::UICornerRadius::All(4.0f);
                thumbWidget = thumb;
            }
            else
            {
                const std::string initial(1, entry.Name.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(entry.Name.front()))));
                auto initialLabel = UI::Label(initial, 38.0f)
                    .Fill()
                    .Align(UI::UIAlignment::Center, UI::UIAlignment::Center);
                initialLabel->UseColorOverride = true;
                initialLabel->ColorOverride = { 0.78f, 0.84f, 0.94f, 1.0f };
                auto placeholder = UI::VStack(0.0f, { initialLabel })
                    .FixedSize(128.0f, 72.0f)
                    .Style("Card")
                    .Background(true);
                thumbWidget = placeholder;
            }

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
                auto missing = UI::MakeUI<UI::UIBadge>("Missing", missingRed);
                missing->VAlign = UI::UIAlignment::Center;
                titleRow->AddChild(missing);
            }
            if (!entry.Modified.empty())
            {
                auto modLabel = UI::Label(entry.Modified).VAlign(UI::UIAlignment::Center);
                modLabel->UseColorOverride = true;
                modLabel->ColorOverride = muted;
                titleRow->AddChild(modLabel);
            }

            auto pathLabel = UI::Label(entry.File.parent_path().generic_string()).FillWidth();
            pathLabel->WordWrap = true;
            pathLabel->UseColorOverride = true;
            pathLabel->ColorOverride = entry.Exists ? muted : missingRed;

            auto infoCol = UI::VStack(4.0f, { titleRow, pathLabel }).FillWidth().VAlign(UI::UIAlignment::Center);

            auto openBtn = UI::Button("Open", [this, file = entry.File]()
            {
                OpenProjectFile(file);
            }).Primary().Enabled(entry.Exists).VAlign(UI::UIAlignment::Center);

            auto folderBtn = UI::Button("Folder", [folder = entry.File.parent_path()]()
            {
                ShowInFolder(folder);
            }).Tooltip("Show in Folder").Enabled(entry.Exists).VAlign(UI::UIAlignment::Center);

            auto actions = UI::HStack(6.0f, { openBtn, folderBtn }).VAlign(UI::UIAlignment::Center);
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

            auto card = UI::HStack(12.0f, { thumbWidget, infoCol, actions })
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
        for (int i = 0; i < 2; ++i)
        {
            if (m_TemplateTiles[i])
                m_TemplateTiles[i]->StyleVariant = (m_TemplateIndex == i) ? "selected" : "";
        }

        const std::vector<std::string> qualities = QualityChoices();
        m_QualityIndex = std::clamp(m_QualityIndex, 0, static_cast<int>(qualities.size()) - 1);

        ProjectLibrary::NewProjectSettings settings;
        settings.Template = (m_TemplateIndex == 0)
            ? ProjectLibrary::ProjectTemplate::Basic
            : ProjectLibrary::ProjectTemplate::Blank;
        settings.Name = m_NewName;
        settings.Location = m_NewLocation;
        settings.WorldUnitsPerMeter = (m_UnitsIndex == 0) ? 100.0f : 1.0f;
        settings.RenderQuality = qualities[m_QualityIndex];
        settings.CreateScripts = m_CreateScripts;

        const std::string err = ProjectLibrary::ValidateNewProject(settings);
        if (m_CreatesLabel)
        {
            if (err.empty())
            {
                const std::filesystem::path nproj = settings.Location / settings.Name / (settings.Name + ".nproj");
                m_CreatesLabel->Text = "Creates: " + nproj.generic_string();
                m_CreatesLabel->Visible = true;
            }
            else
                m_CreatesLabel->Visible = false;
        }
        if (m_CreatesDetailsLabel)
        {
            if (err.empty())
            {
                m_CreatesDetailsLabel->Text = settings.Template == ProjectLibrary::ProjectTemplate::Basic
                    ? "with Assets/ (Scenes with the Main scene, Scripts) and a .gitignore."
                    : "with Assets/ (Scenes, Scripts) and a .gitignore.";
                m_CreatesDetailsLabel->Visible = true;
            }
            else
                m_CreatesDetailsLabel->Visible = false;
        }
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
        const UI::UIColor errorRed = { 0.92f, 0.36f, 0.38f, 1.0f };

        auto subtitle = UI::Label("Open a project or make a new one. Every game is its own project: its own folder, assets, scenes and C# scripts.")
            .FillWidth();
        subtitle->WordWrap = true;
        subtitle->UseColorOverride = true;
        subtitle->ColorOverride = muted;

        auto topBar = UI::HStack(12.0f, {
            UI::VStack(2.0f, {
                UI::Label("Nox Project Manager"),
                subtitle
            }).FillWidth(),
            UI::CheckBox("Open the last project when the editor starts", m_OpenLast, [this](bool v)
            {
                m_OpenLast = v;
                ProjectLibrary::SetOpenLastProjectAtStartup(v);
            }).VAlign(UI::UIAlignment::Center),
            UI::Button("Browse...", [this]()
            {
                Utility::OpenFileAsync("Nox Project\0nproj\0", [this, alive = m_Alive](std::string file)
                {
                    if (*alive && !file.empty())
                        OpenProjectFile(file);
                });
            }).VAlign(UI::UIAlignment::Center)
        })
        .Style("TopBar")
        .Background(true)
        .Padding(16.0f, 12.0f)
        .FillWidth();
        topBar->HeightMode = UI::UISizeMode::Auto;

        m_OpenErrorLabel = UI::Label().FillWidth();
        m_OpenErrorLabel->WordWrap = true;
        m_OpenErrorLabel->UseColorOverride = true;
        m_OpenErrorLabel->ColorOverride = errorRed;
        m_OpenErrorLabel->Visible = false;

        auto searchBar = UI::MakeUI<UI::UISearchBar>("Search recent projects...");
        searchBar->OnSearchChanged = [this](const std::string& q)
        {
            m_SearchFilter = q;
            RebuildProjectList();
        };

        m_ProjectList = UI::Scroll({}, 10.0f);
        RebuildProjectList();

        auto leftColumn = UI::Card("Projects", {
            m_OpenErrorLabel,
            searchBar,
            m_ProjectList
        }).Fill(1.45f);

        struct TemplateCardInfo
        {
            const char* Name;
            const char* Summary;
            const char* Tooltip;
        };
        static constexpr TemplateCardInfo kTemplates[2] = {
            {
                "Basic",
                "Sky, sun, floor, camera",
                "Starts on a scene with a procedural sky (clouds, sun, moon at night), a sun light, a 50 m floor\n"
                "with collision and a camera: Unreal's default level. Saved as Scenes/Main, the project's Start Scene."
            },
            {
                "Blank",
                "Empty scene",
                "Starts on an empty scene: nothing in it, so the view is black until you add a sky or lights."
            }
        };

        auto templateRow = UI::HStack(10.0f, {}).FillWidth();
        templateRow->HeightMode = UI::UISizeMode::Auto;
        for (int i = 0; i < 2; ++i)
        {
            const uint32_t picSlot = m_TemplatePictures[i] ? m_TemplatePictures[i]->GetDescriptorIndexSlot() : ~0u;
            Ref<UI::UIElement> pic;
            if (picSlot != ~0u)
            {
                auto img = UI::MakeUI<UI::UIImage>(picSlot, UI::UIVec2{ 160.0f, 90.0f });
                img->WidthMode = UI::UISizeMode::Fill;
                img->HeightMode = UI::UISizeMode::Auto;
                img->AspectRatio = 16.0f / 9.0f;
                img->CornerRadius = UI::UICornerRadius::All(4.0f);
                pic = img;
            }
            else
            {
                auto fallbackLabel = UI::Label(kTemplates[i].Name, 22.0f)
                    .Fill()
                    .Align(UI::UIAlignment::Center, UI::UIAlignment::Center);
                fallbackLabel->UseColorOverride = true;
                fallbackLabel->ColorOverride = { 0.78f, 0.84f, 0.94f, 1.0f };
                auto fallbackBox = UI::VStack(0.0f, { fallbackLabel })
                    .Style("Card")
                    .Background(true)
                    .FillWidth();
                fallbackBox->HeightMode = UI::UISizeMode::Fixed;
                fallbackBox->FixedSize.y = 90.0f;
                pic = fallbackBox;
            }

            auto nameLabel = UI::Label(kTemplates[i].Name);
            auto descLabel = UI::Label(kTemplates[i].Summary).FillWidth();
            descLabel->WordWrap = true;
            descLabel->UseColorOverride = true;
            descLabel->ColorOverride = muted;

            auto tile = UI::VStack(6.0f, { pic, nameLabel, descLabel })
                .Style("TemplateCard", m_TemplateIndex == i ? "selected" : "")
                .Background(true)
                .Padding(8.0f)
                .Tooltip(kTemplates[i].Tooltip)
                .FillWidth();
            tile->HeightMode = UI::UISizeMode::Auto;
            tile->VAlign = UI::UIAlignment::Start;
            tile->HitTestMode = UI::UIHitTestMode::Self;
            tile->Focusable = true;
            tile->OnMouseUp = [this, i](const UI::UIPointerEvent& e)
            {
                if (e.Button == 1 && m_TemplateTiles[i] && m_TemplateTiles[i]->GetBounds().Contains(e.Position))
                {
                    m_TemplateIndex = i;
                    UpdateNewProjectPreview();
                    return true;
                }
                return false;
            };

            m_TemplateTiles[i] = tile.Get();
            templateRow->AddChild(tile);
        }

        m_NameInput = UI::TextInput(m_NewName, "Project name").FillWidth();
        m_NameInput->Tooltip = "Also the C# namespace of its scripts: letters, digits and _.";
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

        auto browseFolderBtn = UI::Button("Browse...", [this]()
        {
            Utility::OpenFolderAsync(m_NewLocation.empty() ? nullptr : m_NewLocation.c_str(), [this, alive = m_Alive](std::string folder)
            {
                if (!*alive || folder.empty())
                    return;
                m_NewLocation = std::filesystem::path(folder).generic_string();
                if (m_LocationInput)
                    m_LocationInput->Text = m_NewLocation;
                UpdateNewProjectPreview();
            });
        });

        auto unitsDropdown = UI::MakeUI<UI::UIDropdown>(
            &uiLayer->GetContext(),
            std::vector<std::string>{
                "Centimeters (1 unit = 1 cm, like Unreal)",
                "Meters (1 unit = 1 m, like Godot / Unity)"
            },
            m_UnitsIndex,
            [this](int idx) { m_UnitsIndex = idx; }
        );
        unitsDropdown->WidthMode = UI::UISizeMode::Fill;
        unitsDropdown->Tooltip = "What one unit of a position is. Imported models are scaled to it; it cannot be changed later\nwithout re-placing what is built.";

        const std::vector<std::string> qualities = QualityChoices();
        auto qualityDropdown = UI::MakeUI<UI::UIDropdown>(
            &uiLayer->GetContext(),
            qualities,
            m_QualityIndex,
            [this](int idx) { m_QualityIndex = idx; }
        );
        qualityDropdown->WidthMode = UI::UISizeMode::Fill;
        qualityDropdown->Tooltip = "The renderer preset the project opens with (Auto: the one this GPU should run). Changeable later in Settings.";

        m_CreatesLabel = UI::Label().FillWidth();
        m_CreatesLabel->WordWrap = true;
        m_CreatesLabel->UseColorOverride = true;
        m_CreatesLabel->ColorOverride = muted;
        m_CreatesLabel->Visible = false;

        m_CreatesDetailsLabel = UI::Label().FillWidth();
        m_CreatesDetailsLabel->WordWrap = true;
        m_CreatesDetailsLabel->UseColorOverride = true;
        m_CreatesDetailsLabel->ColorOverride = muted;
        m_CreatesDetailsLabel->Visible = false;

        m_ErrorLabel = UI::Label().FillWidth();
        m_ErrorLabel->WordWrap = true;
        m_ErrorLabel->UseColorOverride = true;
        m_ErrorLabel->ColorOverride = errorRed;
        m_ErrorLabel->Visible = false;

        m_CreateButton = UI::Button("Create & Open", [this]()
        {
            const std::vector<std::string> qualityList = QualityChoices();
            const int qIdx = std::clamp(m_QualityIndex, 0, static_cast<int>(qualityList.size()) - 1);

            ProjectLibrary::NewProjectSettings settings;
            settings.Template = (m_TemplateIndex == 0)
                ? ProjectLibrary::ProjectTemplate::Basic
                : ProjectLibrary::ProjectTemplate::Blank;
            settings.Name = m_NewName;
            settings.Location = m_NewLocation;
            settings.WorldUnitsPerMeter = (m_UnitsIndex == 0) ? 100.0f : 1.0f;
            settings.RenderQuality = qualityList[qIdx];
            settings.CreateScripts = m_CreateScripts;

            std::string error;
            const std::filesystem::path created = ProjectLibrary::CreateProject(settings, error);
            if (created.empty())
            {
                if (m_ErrorLabel)
                {
                    m_ErrorLabel->Text = "New Project: " + error;
                    m_ErrorLabel->Visible = true;
                }
                return;
            }
            ProjectLibrary::SetDefaultProjectLocation(settings.Location);
            OpenProjectFile(created, settings.Template == ProjectLibrary::ProjectTemplate::Basic);
        }).Primary().HAlign(UI::UIAlignment::End);

        auto rightFormScroll = UI::Scroll({
            templateRow,
            UI::Label("Project Name"),
            m_NameInput,
            UI::Label("Location"),
            UI::HStack(6.0f, { m_LocationInput, browseFolderBtn }).FillWidth(),
            UI::Label("World Units"),
            unitsDropdown,
            UI::Label("Render Quality"),
            qualityDropdown,
            UI::CheckBox("Create the scripts project (with an example Rotator behaviour)", m_CreateScripts, [this](bool v) { m_CreateScripts = v; }),
            UI::Separator(),
            m_CreatesLabel,
            m_CreatesDetailsLabel,
            m_ErrorLabel
        }, 8.0f);

        auto rightColumn = UI::Card("New Project", {
            rightFormScroll,
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
