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

    void NodeGraphEditorPanel::DrawStateMachineInspector(GraphNode& machine)
    {
        bool edited = false;
        ImGui::Separator();

        auto stateName = [&](uint32_t id) -> std::string
        {
            for (const NodeSubGraph& state : machine.SubGraphs)
            {
                if (state.Id == id)
                    return state.Name.empty() ? "State" : state.Name;
            }
            return "?";
        };

        if (m_SelectedTransition >= 0 && m_SelectedTransition < static_cast<int>(machine.Transitions.size()))
        {
            NodeTransition& transition = machine.Transitions[m_SelectedTransition];
            ImGui::Text("Transition: %s -> %s", stateName(transition.FromState).c_str(), stateName(transition.ToState).c_str());

            ImGui::SetNextItemWidth(120.0f);
            edited |= ImGui::DragFloat("Blend Time (s)", &transition.Duration, 0.01f, 0.0f, 10.0f, "%.2f");
            ImGui::SameLine();
            edited |= ImGui::Checkbox("Ease In/Out", &transition.EaseInOut);

            ImGui::TextUnformatted("Fires when all of these hold:");
            int removeRule = -1;
            static constexpr const char* kCompareNames[] = { "==", "!=", ">", ">=", "<", "<=", "is true", "is false" };
            for (int i = 0; i < static_cast<int>(transition.Rules.size()); ++i)
            {
                TransitionRule& rule = transition.Rules[i];
                ImGui::PushID(i);

                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::BeginCombo("##param", rule.Parameter.empty() ? "(parameter)" : rule.Parameter.c_str()))
                {
                    for (const GraphParameter& parameter : m_Asset->Graph.Parameters)
                    {
                        if (ImGui::Selectable(parameter.Name.c_str(), parameter.Name == rule.Parameter))
                        {
                            rule.Parameter = parameter.Name;
                            edited = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                int compare = static_cast<int>(rule.Compare);
                if (ImGui::Combo("##compare", &compare, kCompareNames, IM_ARRAYSIZE(kCompareNames)))
                {
                    rule.Compare = static_cast<TransitionCompare>(compare);
                    edited = true;
                }

                if (rule.Compare != TransitionCompare::IsTrue && rule.Compare != TransitionCompare::IsFalse)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    edited |= DrawNodeGraphProperty("##value", rule.Value);
                }

                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                    removeRule = i;
                ImGui::PopID();
            }
            if (removeRule >= 0)
            {
                transition.Rules.erase(transition.Rules.begin() + removeRule);
                edited = true;
            }
            if (ImGui::SmallButton("+ Add Rule"))
            {
                transition.Rules.push_back({ m_Asset->Graph.Parameters.empty() ? std::string() : m_Asset->Graph.Parameters.front().Name,
                                             TransitionCompare::Greater, 0.0f });
                edited = true;
            }
        }
        else if (m_SelectedState != 0)
        {
            NodeSubGraph* selected = nullptr;
            for (NodeSubGraph& state : machine.SubGraphs)
            {
                if (state.Id == m_SelectedState)
                    selected = &state;
            }
            if (selected)
            {
                char buffer[64];
                strncpy_s(buffer, selected->Name.c_str(), sizeof(buffer) - 1);
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::InputText("State Name", buffer, sizeof(buffer)))
                {
                    selected->Name = buffer;
                    edited = true;
                }

                auto entry = machine.Properties.find("EntryState");
                const int32_t* entryId = entry != machine.Properties.end() ? std::get_if<int32_t>(&entry->second) : nullptr;
                if (entryId && static_cast<uint32_t>(*entryId) == selected->Id)
                {
                    ImGui::TextDisabled("This is the entry state.");
                }
                else if (ImGui::Button("Set as Entry State"))
                {
                    machine.Properties["EntryState"] = static_cast<int32_t>(selected->Id);
                    edited = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Open State Graph"))
                    m_Path.push_back(selected->Id);
            }
        }
        else
        {
            ImGui::TextDisabled("Right-click the canvas to add a state. Drag from a state's right pin to another state to add a\n"
                                "transition, then select the arrow to edit its rules. Double-click a state to edit what it plays.");
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

                auto onEdited = [this]()
                {
                    m_Dirty = true;
                    m_Asset->Recompile();
                };

                // Resolve which graph is on the canvas from m_Path (dropping any part of it that no longer exists).
                NodeGraph* graph = &m_Asset->Graph;
                GraphNode* machine = nullptr;
                std::vector<std::string> labels{ "Graph" };
                for (size_t i = 0; i < m_Path.size(); ++i)
                {
                    if (i % 2 == 0)
                    {
                        auto found = std::find_if(graph->Nodes.begin(), graph->Nodes.end(), [&](const GraphNode& n) { return n.Id == m_Path[i]; });
                        if (found == graph->Nodes.end())
                        {
                            m_Path.resize(i);
                            break;
                        }
                        machine = &*found;
                        labels.push_back(machine->TypeName);
                    }
                    else
                    {
                        auto found = std::find_if(machine->SubGraphs.begin(), machine->SubGraphs.end(), [&](const NodeSubGraph& s) { return s.Id == m_Path[i]; });
                        if (found == machine->SubGraphs.end())
                        {
                            m_Path.resize(i);
                            break;
                        }
                        graph = &found->Graph;
                        machine = nullptr;
                        labels.push_back(found->Name.empty() ? "State" : found->Name);
                    }
                }
                if (m_Path.size() % 2 == 0)
                    machine = nullptr; // only an odd path ends on a state machine's own view

                if (m_Path != m_ShownPath)
                {
                    m_Backend->ResetView();
                    m_ShownPath = m_Path;
                    m_SelectedTransition = -1;
                    m_SelectedState = 0;
                }

                // Breadcrumb: click a name to go back up to it.
                if (!m_Path.empty())
                {
                    for (size_t k = 0; k < labels.size(); ++k)
                    {
                        if (k > 0)
                            ImGui::SameLine(0.0f, 4.0f), ImGui::TextUnformatted(">"), ImGui::SameLine(0.0f, 4.0f);
                        ImGui::PushID(static_cast<int>(k));
                        if (k + 1 == labels.size())
                            ImGui::TextUnformatted(labels[k].c_str());
                        else if (ImGui::SmallButton(labels[k].c_str()))
                            m_Path.resize(k);
                        ImGui::PopID();
                    }
                }

                if (machine)
                {
                    constexpr float inspectorHeight = 150.0f;
                    uint32_t openState = 0;
                    INodeGraphCanvasBackend::StateMachineSelection selection;
                    ImGui::BeginChild("##StateView", ImVec2(0.0f, -inspectorHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    m_Backend->DrawStateMachine(*machine, m_Asset->Graph, onEdited, openState, selection);
                    ImGui::EndChild();
                    m_SelectedTransition = selection.Transition;
                    m_SelectedState = selection.State;

                    DrawStateMachineInspector(*machine);
                    if (openState != 0)
                    {
                        m_Path.push_back(openState);
                    }
                }
                else
                {
                    uint32_t openNode = 0;
                    m_Backend->Draw(*graph, onEdited, &openNode);
                    if (openNode != 0)
                        m_Path.push_back(openNode);
                }
            }
        }
        ImGui::End();
    }
}
