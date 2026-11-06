#include "LevelsPanel.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <imgui.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Utils/Utils.h"

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
        if (!ImGui::BeginTable("##Levels", 6, flags))
            return;
        ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Locked", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Loading", ImGuiTableColumnFlags_WidthFixed, 130.0f);
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
                    scene.AddSubLevel(handle);
                ImGui::SetItemTooltip("%s", metadata.FilePath.generic_string().c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("A scene made on its own, edited here together with this one (it keeps its own file)");
        ImGui::SameLine();
        if (ImGui::Button("New Level..."))
            CreateLevel(scene);
        ImGui::SetItemTooltip("Makes an empty scene file and adds it as a sub-level");

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
    }

    void LevelsPanel::CreateLevel(Scene& scene)
    {
        std::filesystem::path path = Utility::SaveFile("Nox Scene *.nox\0nox\0");
        if (path.empty())
            return;
        if (path.extension() != ".nox")
            path += ".nox";
        const std::filesystem::path assetRoot = std::filesystem::weakly_canonical(Project::GetActiveAssetDirectory());
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::weakly_canonical(path, error).lexically_relative(assetRoot);
        if (error || relative.empty() || relative.generic_string().rfind("..", 0) == 0)
        {
            NOX_CORE_ERROR("New Level: '{}' is not inside the project's assets folder", path.string());
            return;
        }
        if (std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("New Level: '{}' exists already (use Add Existing Level)", path.string());
            return;
        }
        {
            std::ofstream out(path);
            out << "Scene: " << path.stem().string() << "\nEntities: []\n";
            if (!out)
            {
                NOX_CORE_ERROR("New Level: '{}' could not be written", path.string());
                return;
            }
        }
        const AssetHandle handle = Project::GetActive()->GetEditorAssetManager()->RegisterExistingFile(relative, AssetType::Scene);
        if (handle != 0 && scene.AddSubLevel(handle))
            scene.SetCurrentLevel(handle);
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
