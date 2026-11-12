#include "ProjectManagerLayer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

#include <imgui.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_video.h>

#include "EditorLayer.h"
#include "ProjectLibrary.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    namespace
    {
        const char* s_IniFilename = nullptr;

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string Age(const std::filesystem::path& file)
        {
            std::error_code error;
            const auto written = std::filesystem::last_write_time(file, error);
            if (error)
                return {};
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - written).count();
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

    ProjectManagerLayer::ProjectManagerLayer() : Layer("ProjectManagerLayer")
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.IniFilename)
        {
            s_IniFilename = io.IniFilename;
            io.IniFilename = nullptr;
        }
        SDL_SetWindowTitle(Application::Get().getWindow()->getHandle(), "Nox Project Manager");
        std::snprintf(m_Location, sizeof(m_Location), "%s", ProjectLibrary::GetDefaultProjectLocation().generic_string().c_str());
        m_OpenLast = ProjectLibrary::GetOpenLastProjectAtStartup();
        Refresh();
    }

    const char* ProjectManagerLayer::TakeIniFilename()
    {
        const char* name = s_IniFilename;
        s_IniFilename = nullptr;
        return name;
    }

    void ProjectManagerLayer::Refresh()
    {
        // Pictures the last frames may still draw are released once those frames are done.
        for (Entry& entry : m_Projects)
            if (entry.Thumbnail)
                Renderer::DeferAssetRelease(entry.Thumbnail);
        m_Projects.clear();
        for (const std::filesystem::path& file : ProjectLibrary::LoadRecentProjects())
        {
            std::error_code error;
            Entry entry;
            entry.File = file;
            entry.Exists = std::filesystem::exists(file, error);
            entry.Name = entry.Exists ? ProjectLibrary::ReadProjectName(file) : file.stem().string();
            entry.Modified = entry.Exists ? Age(file) : std::string();
            m_Projects.push_back(std::move(entry));
        }
        // The repository's sample project, listed until it has been opened once (then it is a recent one).
        if (const std::filesystem::path sample = ProjectLibrary::FindSampleProject(); !sample.empty() &&
            std::none_of(m_Projects.begin(), m_Projects.end(), [&](const Entry& e) { return e.File == sample; }))
        {
            Entry entry;
            entry.File = sample;
            entry.Exists = true;
            entry.Sample = true;
            entry.Name = ProjectLibrary::ReadProjectName(sample);
            entry.Modified = Age(sample);
            m_Projects.push_back(std::move(entry));
        }
        for (Entry& entry : m_Projects)
        {
            std::error_code error;
            const std::filesystem::path picture = ProjectLibrary::ThumbnailPath(entry.File.parent_path());
            if (entry.Exists && std::filesystem::exists(picture, error))
                entry.Thumbnail = TextureImporter::LoadTexture2D(picture, { .flip = false, .generateMips = false });
        }
        m_Selected = m_Projects.empty() ? -1 : std::clamp(m_Selected, 0, static_cast<int>(m_Projects.size()) - 1);
    }

    ProjectManagerLayer::~ProjectManagerLayer()
    {
        for (Entry& entry : m_Projects)
            if (entry.Thumbnail)
                Renderer::DeferAssetRelease(entry.Thumbnail);
    }

    void ProjectManagerLayer::Open(const std::filesystem::path& projectFile, bool createBasicScene)
    {
        std::error_code error;
        if (m_Opening)
            return;
        if (!std::filesystem::exists(projectFile, error))
        {
            m_Error = "The project file " + projectFile.generic_string() + " does not exist (moved or deleted?).";
            return;
        }
        ProjectLibrary::RememberRecentProject(projectFile);
        m_Opening = true;
        // After this frame: this layer goes, the editor comes with the project (the layers are not walked then).
        const std::filesystem::path file = projectFile;
        Application::Get().QueueAfterFrame([file, createBasicScene]()
        {
            Application& app = Application::Get();
            app.PopLayer<ProjectManagerLayer>();
            app.PushLayer<EditorLayer>(file, createBasicScene);
        });
    }

    void ProjectManagerLayer::OnImGuiRender()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
        ImGui::Begin("Nox Project Manager", nullptr, flags);
        ImGui::PopStyleVar();

        if (m_Opening)
        {
            ImGui::TextUnformatted("Opening the project...");
            ImGui::End();
            return;
        }

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted("Nox Project Manager");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("Open a project or make a new one. Every game is its own project: its own folder, assets, scenes and C# scripts.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f;
        const float height = ImGui::GetContentRegionAvail().y - footer;
        const float listWidth = ImGui::GetContentRegionAvail().x * 0.58f;

        ImGui::BeginChild("Projects", ImVec2(listWidth, height), ImGuiChildFlags_Borders);
        DrawProjectList(height);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("NewProject", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        DrawNewProject();
        ImGui::EndChild();

        if (!m_Error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_Error.c_str());
        else
            ImGui::NewLine();
        if (ImGui::Checkbox("Always open the last project at startup", &m_OpenLast))
            ProjectLibrary::SetOpenLastProjectAtStartup(m_OpenLast);
        ImGui::SetItemTooltip("Skip this window and open the most recent project when the editor starts.\n"
                              "In the editor, File > Project Manager... comes back here.");
        ImGui::End();
    }

    void ProjectManagerLayer::DrawProjectList(float height)
    {
        ImGui::TextUnformatted("Projects");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##search", "Search projects...", m_Search, sizeof(m_Search));
        ImGui::Spacing();

        // What the list does this frame happens after it is drawn (taking a project off the list rebuilds it, which must not happen
        // halfway through drawing it or between a BeginDisabled and its EndDisabled).
        int open = -1;
        std::filesystem::path forget;

        const std::string filter = Lower(m_Search);
        const float buttonsHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("List", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - buttonsHeight));
        if (m_Projects.empty())
            ImGui::TextDisabled("No projects yet: make one on the right, or Browse... for a .nproj file.");
        constexpr ImVec2 kThumbnail(128.0f, 72.0f); // 16:9, as the pictures are taken
        for (int i = 0; i < static_cast<int>(m_Projects.size()); ++i)
        {
            const Entry& entry = m_Projects[i];
            if (!filter.empty() && Lower(entry.Name + " " + entry.File.generic_string()).find(filter) == std::string::npos)
                continue;
            ImGui::PushID(i);
            const ImVec2 rowSize(0.0f, kThumbnail.y + 8.0f);
            if (ImGui::Selectable("##row", m_Selected == i, ImGuiSelectableFlags_AllowDoubleClick, rowSize))
            {
                m_Selected = i;
                m_Error.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    open = i;
            }
            if (ImGui::BeginPopupContextItem())
            {
                m_Selected = i;
                if (ImGui::MenuItem("Open", nullptr, false, entry.Exists))
                    open = i;
                if (ImGui::MenuItem("Show in Folder", nullptr, false, entry.Exists))
                    ShowInFolder(entry.File.parent_path());
                if (ImGui::MenuItem("Remove from List", nullptr, false, !entry.Sample))
                    forget = entry.File;
                ImGui::EndPopup();
            }

            // The project's picture (or a tile with its initial until it has one), then its name and path (red when the file is gone).
            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 imageMin(rowMin.x + 4.0f, rowMin.y + 4.0f);
            const ImVec2 imageMax(imageMin.x + kThumbnail.x, imageMin.y + kThumbnail.y);
            if (entry.Thumbnail && entry.Thumbnail->getImTextureID())
                drawList->AddImageRounded(entry.Thumbnail->getImTextureID(), imageMin, imageMax, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE, 4.0f);
            else
            {
                drawList->AddRectFilled(imageMin, imageMax, IM_COL32(45, 55, 75, 255), 4.0f);
                const std::string initial(1, entry.Name.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(entry.Name.front()))));
                const float fontSize = ImGui::GetFontSize() * 2.2f;
                const ImVec2 textSize = ImGui::CalcTextSize(initial.c_str());
                const float scale = fontSize / ImGui::GetFontSize();
                drawList->AddText(ImGui::GetFont(), fontSize,
                                  ImVec2(imageMin.x + (kThumbnail.x - textSize.x * scale) * 0.5f, imageMin.y + (kThumbnail.y - textSize.y * scale) * 0.5f),
                                  IM_COL32(200, 210, 230, 255), initial.c_str());
            }

            const ImVec2 namePos(imageMax.x + 12.0f, rowMin.y + 10.0f);
            const std::string title = entry.Name + (entry.Sample ? "  (sample project)" : "");
            drawList->AddText(namePos, ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
            if (!entry.Modified.empty())
            {
                const float width = ImGui::CalcTextSize(entry.Modified.c_str()).x;
                drawList->AddText(ImVec2(rowMax.x - width - 6.0f, namePos.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), entry.Modified.c_str());
            }
            const std::string path = entry.File.generic_string() + (entry.Exists ? "" : "   (missing)");
            drawList->AddText(ImVec2(namePos.x, namePos.y + ImGui::GetTextLineHeightWithSpacing() * 1.4f),
                              entry.Exists ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : IM_COL32(230, 110, 100, 255), path.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        (void)height;

        const bool hasSelection = m_Selected >= 0 && m_Selected < static_cast<int>(m_Projects.size());
        if (hasSelection && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                open = m_Selected;
            else if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_Projects[m_Selected].Sample)
                forget = m_Projects[m_Selected].File;
        }

        ImGui::BeginDisabled(!hasSelection || !m_Projects[m_Selected].Exists);
        if (ImGui::Button("Open"))
            open = m_Selected;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            const std::string file = Utility::OpenFile("Nox Project\0nproj\0");
            if (!file.empty())
                Open(file);
        }
        ImGui::SetItemTooltip("Open a project that is not in the list (pick its .nproj file).");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection || m_Projects[m_Selected].Sample);
        if (ImGui::Button("Remove from List") && hasSelection)
            forget = m_Projects[m_Selected].File;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Only takes it off this list: no file is deleted.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection || !m_Projects[m_Selected].Exists);
        if (ImGui::Button("Show in Folder") && hasSelection)
            ShowInFolder(m_Projects[m_Selected].File.parent_path());
        ImGui::EndDisabled();

        if (!forget.empty())
        {
            ProjectLibrary::ForgetRecentProject(forget);
            Refresh();
        }
        else if (open >= 0 && open < static_cast<int>(m_Projects.size()))
            Open(m_Projects[open].File);
    }

    void ProjectManagerLayer::DrawNewProject()
    {
        ImGui::TextUnformatted("New Project");
        ImGui::Separator();
        ImGui::Spacing();

        const float labelWidth = 150.0f;
        auto label = [&](const char* text)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelWidth);
        };

        label("Template");
        ImGui::RadioButton("Basic", &m_Template, 0);
        ImGui::SetItemTooltip("Starts on a scene with a procedural sky (clouds, sun, moon at night), a sun light, a 50 m floor\n"
                              "with collision and a camera: Unreal's default level. Saved as Scenes/Main, the project's Start Scene.");
        ImGui::SameLine();
        ImGui::RadioButton("Blank", &m_Template, 1);
        ImGui::SetItemTooltip("Starts on an empty scene: nothing in it, so the view is black until you add a sky or lights.");
        ImGui::SameLine();
        ImGui::TextDisabled(m_Template == 0 ? "(sky, sun, floor, camera)" : "(empty, black)");

        label("Project Name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##name", m_Name, sizeof(m_Name));
        ImGui::SetItemTooltip("Also the C# namespace of its scripts: letters, digits and _.");

        label("Location");
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##location", m_Location, sizeof(m_Location));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##location"))
        {
            const std::string folder = Utility::OpenFolder(m_Location[0] ? m_Location : nullptr);
            if (!folder.empty())
                std::snprintf(m_Location, sizeof(m_Location), "%s", std::filesystem::path(folder).generic_string().c_str());
        }

        label("World Units");
        ImGui::SetNextItemWidth(-1.0f);
        const char* units[] = { "Centimeters (1 unit = 1 cm, like Unreal)", "Meters (1 unit = 1 m, like Godot / Unity)" };
        ImGui::Combo("##units", &m_Units, units, IM_ARRAYSIZE(units));
        ImGui::SetItemTooltip("What one unit of a position is. Imported models are scaled to it; it cannot be changed later\nwithout re-placing what is built.");

        label("Render Quality");
        ImGui::SetNextItemWidth(-1.0f);
        const std::vector<std::string> qualities = QualityChoices();
        m_Quality = std::clamp(m_Quality, 0, static_cast<int>(qualities.size()) - 1);
        if (ImGui::BeginCombo("##quality", qualities[m_Quality].c_str()))
        {
            for (int i = 0; i < static_cast<int>(qualities.size()); ++i)
                if (ImGui::Selectable(qualities[i].c_str(), i == m_Quality))
                    m_Quality = i;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The renderer preset the project opens with (Auto: the one this GPU should run). Changeable later in Settings.");

        label("C# Scripts");
        ImGui::Checkbox("Create the scripts project (with an example Rotator behaviour)", &m_CreateScripts);

        ProjectLibrary::NewProjectSettings settings;
        settings.Name = m_Name;
        settings.Location = m_Location;
        settings.WorldUnitsPerMeter = m_Units == 0 ? 100.0f : 1.0f;
        settings.RenderQuality = qualities[m_Quality];
        settings.CreateScripts = m_CreateScripts;
        settings.Template = m_Template == 0 ? ProjectLibrary::ProjectTemplate::Basic : ProjectLibrary::ProjectTemplate::Blank;
        const std::string problem = ProjectLibrary::ValidateNewProject(settings);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (problem.empty())
        {
            ImGui::TextDisabled("Creates:");
            ImGui::TextWrapped("%s", (settings.Location / settings.Name / (settings.Name + ".nproj")).generic_string().c_str());
            ImGui::TextDisabled(settings.Template == ProjectLibrary::ProjectTemplate::Basic
                                    ? "with Assets/ (Scenes with the Main scene, Scripts) and a .gitignore."
                                    : "with Assets/ (Scenes, Scripts) and a .gitignore.");
        }
        else
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", problem.c_str());
        ImGui::Spacing();

        ImGui::BeginDisabled(!problem.empty());
        if (ImGui::Button("Create & Open", ImVec2(-1.0f, ImGui::GetFrameHeight() * 1.5f)))
        {
            std::string error;
            const std::filesystem::path projectFile = ProjectLibrary::CreateProject(settings, error);
            if (projectFile.empty())
                m_Error = "New Project: " + error;
            else
                Open(projectFile, settings.Template == ProjectLibrary::ProjectTemplate::Basic);
        }
        ImGui::EndDisabled();
    }
}
