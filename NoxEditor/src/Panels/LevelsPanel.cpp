#include "LevelsPanel.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <cmath>
#include <string>

#include <imgui.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"

namespace Nox
{
    namespace
    {
        std::string LevelName(AssetHandle level)
        {
            return AssetManager::IsAssetHandleValid(level) ? AssetManager::GetMetadata(level).FilePath.stem().string()
                                                           : std::string("(missing scene)");
        }

        const char* StateText(Scene::LevelState state)
        {
            return state == Scene::LevelState::Loaded ? "loaded" : state == Scene::LevelState::Loading ? "loading" : "unloaded";
        }
    }

    void LevelsPanel::OnImGuiRender(const Ref<Scene>& scene, bool editing, const std::vector<Entity>& selection)
    {
        if (!m_Open)
            return;
        ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Levels", &m_Open))
        {
            ImGui::End();
            return;
        }
        if (!scene)
        {
            ImGui::TextDisabled("No scene");
            ImGui::End();
            return;
        }
        if (scene->IsWorldPartitioned())
        {
            ImGui::TextWrapped("The scene is a world: it has no sub-levels (its cells are loaded in Window > World Partition).");
            ImGui::End();
            return;
        }

        DrawLevels(*scene, editing);
        if (editing)
        {
            ImGui::Separator();
            DrawActions(*scene, selection);
            DrawRemovePrompt(*scene);
        }
        else
        {
            ImGui::TextDisabled("Play / Simulate: Always Loaded levels are in from the start, the others follow their volumes and scripts.");
        }
        ImGui::End();
    }

    void LevelsPanel::DrawLevels(Scene& scene, bool editing)
    {
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
        if (!ImGui::BeginTable("##Levels", 7, flags))
            return;
        ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Locked", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Loading", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Placement", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();

        // The persistent level: the scene itself.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!editing);
        if (ImGui::RadioButton("##CurrentPersistent", scene.GetCurrentLevel() == 0))
            scene.SetCurrentLevel(0);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The current level: new entities go into it");
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        const AssetHandle openScene = m_OpenScene ? m_OpenScene() : AssetHandle(0);
        ImGui::Text("Persistent Level (%s)", openScene != 0 ? LevelName(openScene).c_str() : "unsaved scene");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("always");
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();

        for (const Scene::SubLevel& subLevel : std::vector<Scene::SubLevel>(scene.GetSubLevels()))
        {
            ImGui::PushID(static_cast<int>(static_cast<uint64_t>(subLevel.Level)));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const bool usable = editing && subLevel.Loaded && subLevel.Visible && !subLevel.Locked;
            ImGui::BeginDisabled(!usable);
            if (ImGui::RadioButton("##Current", scene.GetCurrentLevel() == subLevel.Level))
                scene.SetCurrentLevel(subLevel.Level);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The current level: new entities go into it (a level must be loaded, visible and unlocked)");

            ImGui::TableNextColumn();
            bool visible = subLevel.Visible;
            ImGui::BeginDisabled(!editing);
            if (ImGui::Checkbox("##Visible", &visible))
                scene.SetSubLevelVisible(subLevel.Level, visible);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Shown in the editor. Hidden: its entities are kept aside (not drawn, lit, picked or simulated);\n"
                                  "it is still saved. In Play its loading setting decides, not this.");

            ImGui::TableNextColumn();
            bool locked = subLevel.Locked;
            ImGui::BeginDisabled(!editing);
            if (ImGui::Checkbox("##Locked", &locked))
                scene.SetSubLevelLocked(subLevel.Level, locked);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Locked: its entities cannot be selected (so not moved, edited or deleted)");

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(LevelName(subLevel.Level).c_str());
            if (editing && !subLevel.Loaded)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(not read: see the log; its file is not written)");
            }
            if (!editing)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", StateText(scene.GetLevelState(subLevel.Level)));
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            int loading = subLevel.AlwaysLoaded ? 0 : 1;
            ImGui::BeginDisabled(!editing);
            if (ImGui::Combo("##Loading", &loading, "Always Loaded\0Streamed\0"))
                scene.SetSubLevelAlwaysLoaded(subLevel.Level, loading == 0);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Always Loaded: in from the start of Play, with the persistent level.\n"
                                  "Streamed: loaded by Level Streaming Volumes naming it, or a script (Scene.LoadLevel).");

            // Level transform (Unreal's: offset and yaw): where the level's own space sits in this scene.
            ImGui::TableNextColumn();
            const double meter = static_cast<double>(WorldUnits::FromMeters(1.0f));
            const std::string placement = subLevel.HasTransform()
                ? std::format("{:.1f}, {:.1f}, {:.1f} m  {:.0f}\xC2\xB0", subLevel.Offset.x / meter, subLevel.Offset.y / meter, subLevel.Offset.z / meter,
                              subLevel.Yaw * 57.29578f)
                : std::string("as built");
            ImGui::BeginDisabled(!editing || subLevel.Locked);
            if (ImGui::SmallButton(placement.c_str()))
                ImGui::OpenPopup("##Placement");
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The level transform: moves and turns the whole level (all its entities) at once. Its file keeps it\n"
                                  "where it was built; the offset is applied when it is read and taken out when it is saved.");
            if (ImGui::BeginPopup("##Placement"))
            {
                double offset[3] = { subLevel.Offset.x / meter, subLevel.Offset.y / meter, subLevel.Offset.z / meter };
                float yawDegrees = subLevel.Yaw * 57.29578f;
                bool changed = false;
                ImGui::TextUnformatted(LevelName(subLevel.Level).c_str());
                ImGui::SetNextItemWidth(240.0f);
                const double step = 0.05;
                changed |= ImGui::DragScalarN("Offset (m)", ImGuiDataType_Double, offset, 3, static_cast<float>(step), nullptr, nullptr, "%.2f");
                ImGui::SetNextItemWidth(240.0f);
                changed |= ImGui::DragFloat("Yaw (deg)", &yawDegrees, 0.5f, -360.0f, 360.0f, "%.1f");
                if (changed)
                    scene.SetSubLevelTransform(subLevel.Level, glm::dvec3(offset[0], offset[1], offset[2]) * meter, yawDegrees / 57.29578f);
                if (ImGui::Button("Reset"))
                    scene.SetSubLevelTransform(subLevel.Level, glm::dvec3(0.0), 0.0f);
                ImGui::SetItemTooltip("Back to where it was built");
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            if (editing && ImGui::SmallButton("Remove"))
                m_RemoveRequest = subLevel.Level;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void LevelsPanel::DrawActions(Scene& scene, const std::vector<Entity>& selection)
    {
        const AssetHandle openScene = m_OpenScene ? m_OpenScene() : AssetHandle(0);
        const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();

        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##AddExisting", "Add Existing Level..."))
        {
            for (const auto& [handle, metadata] : registry)
            {
                if (metadata.Type != AssetType::Scene || handle == openScene || scene.FindSubLevel(handle))
                    continue;
                if (ImGui::Selectable(metadata.FilePath.stem().string().c_str()))
                    m_Message = scene.AddSubLevel(handle) ? "Added " + metadata.FilePath.stem().string()
                                                          : "Could not add " + metadata.FilePath.stem().string() + " (see the log)";
                ImGui::SetItemTooltip("%s", metadata.FilePath.generic_string().c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("A scene made on its own, edited here together with this one (it keeps its own file)");
        ImGui::SameLine();
        if (ImGui::Button("New Level..."))
            m_NewLevelRequest = true;
        ImGui::SetItemTooltip("Makes an empty scene file next to the open scene, adds it as a sub-level and makes it the current level");

        ImGui::SameLine();
        const AssetHandle current = scene.GetCurrentLevel();
        ImGui::BeginDisabled(selection.empty());
        if (ImGui::Button("Move Selection to Current"))
            scene.MoveToLevel(selection, current);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The selected entities (with their parents and children: a tree stays in one level) into the current level (%s)",
                              current != 0 ? LevelName(current).c_str() : "the persistent level");
        if (openScene == 0 && !scene.GetSubLevels().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "The scene was never saved: save it to keep its sub-level list.");
        if (!m_Message.empty())
            ImGui::TextDisabled("%s", m_Message.c_str());
        DrawNewLevelPrompt(scene);
    }

    void LevelsPanel::DrawNewLevelPrompt(Scene& scene)
    {
        if (m_NewLevelRequest)
        {
            ImGui::OpenPopup("New Level");
            m_NewLevelRequest = false;
        }
        if (!ImGui::BeginPopupModal("New Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        const AssetHandle openScene = m_OpenScene ? m_OpenScene() : AssetHandle(0);
        const std::filesystem::path folder = openScene != 0 ? AssetManager::GetMetadata(openScene).FilePath.parent_path() : std::filesystem::path();
        ImGui::Text("A new empty level in %s", folder.empty() ? "the assets folder" : ("Assets/" + folder.generic_string()).c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("Name", m_NewLevelName, sizeof(m_NewLevelName), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button("Create", ImVec2(100.0f, 0.0f)) || entered)
        {
            if (CreateLevel(scene, m_NewLevelName))
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        if (!m_Message.empty())
            ImGui::TextDisabled("%s", m_Message.c_str());
        ImGui::EndPopup();
    }

    bool LevelsPanel::CreateLevel(Scene& scene, const std::string& name)
    {
        // Paths built from the asset folder itself (no file dialog, no path comparisons): next to the open scene.
        if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos)
        {
            m_Message = "New Level: give it a name without / \\ : * ? \" < > |";
            return false;
        }
        const AssetHandle openScene = m_OpenScene ? m_OpenScene() : AssetHandle(0);
        const std::filesystem::path folder = openScene != 0 ? AssetManager::GetMetadata(openScene).FilePath.parent_path() : std::filesystem::path();
        const std::filesystem::path relative = (folder / (name + ".nox")).lexically_normal();
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / relative;
        if (std::filesystem::exists(path))
        {
            m_Message = "New Level: " + relative.generic_string() + " exists already (use Add Existing Level)";
            return false;
        }
        {
            std::ofstream out(path);
            out << "Scene: " << name << "\nEntities: []\n";
            if (!out)
            {
                m_Message = "New Level: " + path.string() + " could not be written";
                return false;
            }
        }
        EditorAssetManager& assets = *Project::GetActive()->GetEditorAssetManager();
        AssetHandle handle = assets.FindHandleByPath(relative, AssetType::Scene);
        if (handle == 0)
            handle = assets.RegisterExistingFile(relative, AssetType::Scene);
        if (handle == 0 || !scene.AddSubLevel(handle))
        {
            m_Message = "New Level: " + relative.generic_string() + " was written but could not be added (see the log)";
            return false;
        }
        scene.SetCurrentLevel(handle);
        m_Message = "Created " + relative.generic_string() + " (the current level now)";
        NOX_CORE_INFO("New level '{}'", relative.generic_string());
        return true;
    }

    void LevelsPanel::DrawRemovePrompt(Scene& scene)
    {
        if (m_RemoveRequest != 0)
        {
            ImGui::OpenPopup("Remove Level");
        }
        if (!ImGui::BeginPopupModal("Remove Level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::Text("Remove `%s` from this scene?", LevelName(m_RemoveRequest).c_str());
        ImGui::TextDisabled("Its file stays. Changes to it not saved yet are lost unless saved now.");
        if (ImGui::Button("Save and Remove"))
        {
            if (m_Save)
                m_Save();
            scene.RemoveSubLevel(m_RemoveRequest);
            m_RemoveRequest = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove"))
        {
            scene.RemoveSubLevel(m_RemoveRequest);
            m_RemoveRequest = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_RemoveRequest = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
