#include "AudioWidgets.h"

#include <algorithm>

#include <imgui.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Audio/AudioClip.h"
#include "NoxCore/Audio/AudioEngine.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    bool SoundAssetCombo(const char* label, AssetHandle& sound)
    {
        const AssetHandle before = sound;
        const auto& registry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
        const std::string current = sound != 0 && registry.contains(sound) ? registry.at(sound).FilePath.filename().string() : std::string("None");
        if (ImGui::BeginCombo(label, current.c_str()))
        {
            if (ImGui::Selectable("None", sound == 0))
                sound = 0;
            for (const auto& [handle, metadata] : registry)
            {
                if (metadata.Type != AssetType::AudioClip && metadata.Type != AssetType::AudioGraph)
                    continue;
                ImGui::PushID(static_cast<int>(static_cast<uint64_t>(handle) & 0x7fffffff));
                if (ImGui::Selectable(metadata.FilePath.filename().string().c_str(), sound == handle))
                    sound = handle;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s", metadata.FilePath.generic_string().c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("An audio clip (.wav / .ogg / .flac / .mp3) or an audio graph (.naudiograph). Drop one from the Content Browser.");
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
            {
                const AssetHandle handle = *static_cast<const AssetHandle*>(payload->Data);
                const AssetType type = AssetManager::GetAssetType(handle);
                if (type == AssetType::AudioClip || type == AssetType::AudioGraph)
                    sound = handle;
            }
            ImGui::EndDragDropTarget();
        }
        return sound != before;
    }

    bool PhysicalSurfaceCombo(const char* label, std::string& surface, const char* emptyLabel)
    {
        const std::string before = surface;
        const std::vector<std::string>& surfaces = Project::GetActive()->GetConfig().PhysicalSurfaces;
        const bool known = surface.empty() || std::find(surfaces.begin(), surfaces.end(), surface) != surfaces.end();
        const std::string preview = surface.empty() ? std::string(emptyLabel) : known ? surface : surface + " (not in the project's list)";
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            if (ImGui::Selectable(emptyLabel, surface.empty()))
                surface.clear();
            for (const std::string& name : surfaces)
            {
                if (name == "Default" && std::string_view(emptyLabel) == "Default")
                    continue; // (the empty name is Default)
                if (ImGui::Selectable(name.c_str(), surface == name))
                    surface = name;
            }
            ImGui::EndCombo();
        }
        return surface != before;
    }

    void PlayPreviewSound(AssetHandle sound, float volume)
    {
        if (sound == 0)
            return;
        AudioPlayDesc desc;
        desc.Volume = volume;
        desc.Bus = kAudioBusMaster;
        desc.Group = AudioVoiceGroup::Editor;
        if (AssetManager::GetAssetType(sound) == AssetType::AudioGraph)
        {
            desc.Graph = GetAudioGraphProgram(sound);
            if (!desc.Graph)
                return;
        }
        else
        {
            if (AssetManager::RequestAsset(sound) != AssetState::Ready)
                return;
            desc.Clip = AssetManager::GetAsset<AudioClip>(sound);
        }
        AudioEngine::Play(desc);
    }
}
