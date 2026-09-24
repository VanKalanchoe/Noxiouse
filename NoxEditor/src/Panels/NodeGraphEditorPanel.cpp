#include "NodeGraphEditorPanel.h"

#include <imgui.h>

#include <algorithm>

#include "NodeGraph/INodeGraphCanvasBackend.h"
#include "NodeGraph/NodeGraphPropertyEditor.h"
#include "NodeGraph/ThedmdCanvasBackend.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"

namespace Nox
{
    NodeGraphEditorPanel::NodeGraphEditorPanel(AssetHandle graphAsset)
        : m_GraphHandle(graphAsset)
    {
        m_Asset = AssetManager::GetAsset<NodeGraphAsset>(graphAsset);
        m_Title = "Animation Graph";
        if (auto assetManager = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
        {
            const auto& registry = assetManager->GetAssetRegistry();
            auto it = registry.find(graphAsset);
            if (it != registry.end())
                m_Title = it->second.FilePath.stem().string();
        }

        // The only place a canvas library gets picked; swapping later means changing this one line (plus
        // adding the new backend's own files under NodeGraph/) -- see
        // docs/Animation_Graph_Architecture_Plan_2026.md Step 3.
        m_Backend = CreateScope<ThedmdCanvasBackend>();
    }

    NodeGraphEditorPanel::~NodeGraphEditorPanel() = default;

    void NodeGraphEditorPanel::Save()
    {
        if (!m_Asset || !Project::GetActive())
            return;

        auto assetManager = Project::GetActive()->GetEditorAssetManager();
        const auto& registry = assetManager->GetAssetRegistry();
        auto it = registry.find(m_GraphHandle);
        if (it == registry.end())
        {
            NOX_CORE_ERROR("NodeGraphEditorPanel::Save - handle {} is not in the asset registry", (uint64_t)m_GraphHandle);
            return;
        }

        std::filesystem::path path = Project::GetActiveAssetDirectory() / it->second.FilePath;
        if (NodeGraphSerializer::Serialize(path, m_Asset->Graph))
            m_Dirty = false;
    }

    void NodeGraphEditorPanel::DrawParametersPanel()
    {
        if (!ImGui::CollapsingHeader("Parameters"))
            return;

        NodeGraph& graph = m_Asset->Graph;
        bool edited = false;
        int removeIndex = -1;

        for (int i = 0; i < (int)graph.Parameters.size(); i++)
        {
            GraphParameter& parameter = graph.Parameters[i];
            ImGui::PushID(i);

            // Drag handle: dropping it on the canvas creates a node reading this parameter (Unreal's
            // drag-a-variable-into-the-graph). The payload is just the name; the backend does the placing.
            ImGui::SmallButton("::");
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                ImGui::SetDragDropPayload(kNodeGraphParameterPayload, parameter.Name.c_str(), parameter.Name.size() + 1);
                ImGui::Text("Get %s", parameter.Name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drag onto the graph to create a node reading this parameter");
            ImGui::SameLine();

            // Applied on every edit rather than on commit: ImGui keeps the text being typed while the field is
            // active, so a name that's momentarily invalid (empty, or a duplicate) simply isn't applied yet.
            char buffer[64];
            strncpy_s(buffer, parameter.Name.c_str(), sizeof(buffer) - 1);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::InputText("##name", buffer, sizeof(buffer)))
            {
                const std::string newName = buffer;
                const bool duplicate = std::any_of(graph.Parameters.begin(), graph.Parameters.end(),
                    [&](const GraphParameter& other) { return &other != &parameter && other.Name == newName; });
                if (!newName.empty() && !duplicate)
                {
                    // Nodes that read this parameter (see kParameterReferenceProperty) follow the rename.
                    for (GraphNode& node : graph.Nodes)
                    {
                        auto reference = node.Properties.find(kParameterReferenceProperty);
                        if (reference != node.Properties.end())
                            if (auto* name = std::get_if<std::string>(&reference->second); name && *name == parameter.Name)
                                *name = newName;
                    }
                    parameter.Name = newName;
                    edited = true;
                }
            }

            ImGui::SameLine();
            int type = std::holds_alternative<bool>(parameter.DefaultValue) ? 1 : (std::holds_alternative<int32_t>(parameter.DefaultValue) ? 2 : 0);
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::Combo("##type", &type, "Float\0Bool\0Int\0"))
            {
                parameter.DefaultValue = type == 1 ? NodeGraphValue(false) : (type == 2 ? NodeGraphValue(int32_t(0)) : NodeGraphValue(0.0f));
                edited = true;
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (DrawNodeGraphProperty("##default", parameter.DefaultValue))
                edited = true;

            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
                removeIndex = i;

            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
            graph.Parameters.erase(graph.Parameters.begin() + removeIndex);
            edited = true;
        }

        if (ImGui::Button("+ Add Parameter"))
        {
            std::string name = "Param";
            for (int suffix = 1; std::any_of(graph.Parameters.begin(), graph.Parameters.end(),
                                             [&](const GraphParameter& p) { return p.Name == name; }); suffix++)
                name = "Param" + std::to_string(suffix);
            graph.Parameters.push_back({ name, 0.0f });
            edited = true;
        }

        if (edited)
        {
            m_Dirty = true;
            m_Asset->Recompile();
        }
    }

    void NodeGraphEditorPanel::OnImGuiRender()
    {
        m_WantsInput = false; // both stay false if the window is closed or collapsed this frame
        m_Hovered = false;
        if (!m_Open)
            return;

        // "###" gives the window a stable ID independent of the visible label, so the "*" dirty marker changing
        // doesn't make ImGui think this is a different window.
        std::string windowTitle = m_Title + (m_Dirty ? " *" : "") + "###NodeGraphEditor" + std::to_string((uint64_t)m_GraphHandle);

        ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(windowTitle.c_str(), &m_Open, ImGuiWindowFlags_MenuBar))
        {
            // Includes child windows: the canvas is one, and a popup opened from it counts as this window's.
            m_Hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup);
            m_WantsInput = m_Hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("Save", "Ctrl+S", false, m_Asset != nullptr))
                        Save();
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            if (m_Asset && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
            {
                Save();
            }

            if (!m_Asset)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed to load graph asset %llu", (unsigned long long)m_GraphHandle);
            }
            else
            {
                DrawParametersPanel();
                m_Backend->Draw(m_Asset->Graph, [this]()
                {
                    m_Dirty = true;
                    m_Asset->Recompile();
                });
            }
        }
        ImGui::End();
    }
}
