#include "NodeGraphEditorV2.h"

#include <algorithm>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "NodeGraphCanvasV2.h"
#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

namespace Nox
{
    namespace
    {
        // A parameter's readers (nodes naming it) and the transition rules testing it, in this graph and every state under it.
        void RenameParameterReferences(NodeGraph& graph, const std::string& oldName, const std::string& newName)
        {
            for (GraphNode& node : graph.Nodes)
            {
                if (auto reference = node.Properties.find(kParameterReferenceProperty); reference != node.Properties.end())
                    if (auto* name = std::get_if<std::string>(&reference->second); name && *name == oldName)
                        *name = newName;
                for (NodeTransition& transition : node.Transitions)
                    for (TransitionRule& rule : transition.Rules)
                        if (rule.Parameter == oldName)
                            rule.Parameter = newName;
                for (NodeSubGraph& state : node.SubGraphs)
                    RenameParameterReferences(state.Graph, oldName, newName);
            }
        }

        // The window's root: keys typed in it that its children leave (Ctrl+S, Ctrl+Z ...) come here.
        class GraphWindowRoot final : public UI::UIStackBox
        {
        public:
            explicit GraphWindowRoot(std::function<bool(int)> onKey)
                : UI::UIStackBox(UI::UIOrientation::Vertical, 0.0f), m_OnKey(std::move(onKey)) {}
            bool OnKeyDown(const UI::UIKeyEvent& e) override { return m_OnKey && m_OnKey(e.KeyCode); }

        private:
            std::function<bool(int)> m_OnKey;
        };

        Ref<UI::UILabel> MutedLabel(const std::string& text, float size = 13.0f)
        {
            Ref<UI::UILabel> label(new UI::UILabel(text, size));
            label->UseColorOverride = true;
            label->ColorOverride = { 0.60f, 0.64f, 0.72f, 1.0f };
            label->WordWrap = true;
            label->WidthMode = UI::UISizeMode::Fill;
            return label;
        }
    }

    NodeGraphEditorV2::NodeGraphEditorV2(AssetHandle graph)
        : m_Handle(graph)
    {
        m_Asset = AssetManager::GetAsset<NodeGraphAsset>(graph);
        const std::string domain = m_Asset ? m_Asset->Graph.Domain : std::string();
        m_Kind = domain == kAudioGraphDomain ? "Audio Graph" : domain == kMaterialGraphDomain ? "Material Graph" : "Animation Graph";
        m_Title = m_Kind;
        if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
        {
            if (const auto found = assets->GetAssetRegistry().find(graph); found != assets->GetAssetRegistry().end())
                m_Title = found->second.FilePath.stem().string();
        }
        if (m_Asset)
        {
            m_Committed = m_Asset->Graph;
            m_DiskVersion = m_Asset->DiskVersion;
        }
    }

    NodeGraphEditorV2::~NodeGraphEditorV2()
    {
        // Closed: the asset matches the file again. The editor asked about unsaved edits before closing the window (Save / Don't
        // Save); edits still here (the editor shutting down) are dropped, never kept in memory as if saved. A version from disk it
        // had not taken is what the file holds now.
        Notify = nullptr; // (whoever listened may be going away too)
        if (m_Asset && (m_Dirty || m_Asset->DiskGraph))
            RevertToDisk();
        if (m_Asset)
            m_Asset->HasUnsavedEdits = false;
    }

    Ref<UI::UIElement> NodeGraphEditorV2::BuildUI()
    {
        Ref<GraphWindowRoot> root(new GraphWindowRoot([this](int key) { return HandleKey(key); }));
        root->WidthMode = UI::UISizeMode::Fill;
        root->HeightMode = UI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;

        auto button = [](const std::string& text, std::function<void()> onClick, const std::string& tooltip)
        {
            Ref<UI::UIButton> b(new UI::UIButton(text, std::move(onClick)));
            b->Padding = UI::UIThickness::Symmetric(10.0f, 2.0f);
            b->HeightMode = UI::UISizeMode::Fixed;
            b->FixedSize = { 0.0f, 26.0f };
            b->Tooltip = tooltip;
            b->VAlign = UI::UIAlignment::Center;
            return b;
        };
        Ref<UI::UIStackBox> toolbar(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
        toolbar->WidthMode = UI::UISizeMode::Fill;
        toolbar->Padding = UI::UIThickness{ 6.0f, 5.0f, 6.0f, 4.0f };
        toolbar->VAlign = UI::UIAlignment::Center;
        m_SaveButton = button("Save", [this]() { Save(); }, "Write the graph to its file (Ctrl+S)");
        m_UndoButton = button("Undo", [this]() { Undo(); }, "Ctrl+Z");
        m_RedoButton = button("Redo", [this]() { Redo(); }, "Ctrl+Y");
        toolbar->AddChild(m_SaveButton);
        toolbar->AddChild(m_UndoButton);
        toolbar->AddChild(m_RedoButton);
        toolbar->AddChild(button("Frame", [this]() { if (m_Canvas) m_Canvas->FrameSelection(); }, "The selection, or the whole graph, in view (F)"));
        m_Status = MutedLabel("", 13.0f);
        m_Status->WordWrap = false;
        m_Status->VAlign = UI::UIAlignment::Center;
        m_Status->Margin = UI::UIThickness{ 8.0f, 0.0f, 0.0f, 0.0f };
        toolbar->AddChild(m_Status);
        root->AddChild(toolbar);

        // Parameters | canvas | Details.
        auto column = [](float width)
        {
            Ref<UI::UIScrollView> scroll(new UI::UIScrollView());
            scroll->WidthMode = UI::UISizeMode::Fixed;
            scroll->FixedSize = { width, 0.0f };
            scroll->HeightMode = UI::UISizeMode::Fill;
            scroll->ChildSpacing = 6.0f;
            return scroll;
        };
        Ref<UI::UIScrollView> left = column(230.0f);
        m_Parameters = Ref<UI::UIStackBox>(new UI::UIStackBox(UI::UIOrientation::Vertical, 6.0f));
        m_Parameters->WidthMode = UI::UISizeMode::Fill;
        m_Parameters->Padding = UI::UIThickness::All(8.0f);
        left->AddChild(m_Parameters);

        m_Canvas = Ref<NodeGraphCanvasV2>(new NodeGraphCanvasV2());
        m_Canvas->Parameters = m_Asset ? &m_Asset->Graph.Parameters : nullptr;
        m_Canvas->OnEdited = [this](bool structural) { OnCanvasEdited(structural); };
        m_Canvas->OnSelectionChanged = [this]() { m_DetailsDirty = true; };
        m_Canvas->OnOpenNode = [this](uint32_t)
        {
            if (Notify)
                Notify({ UI::UINotificationSeverity::Info, "State machines are not in NoxEditorV2 yet",
                         "Their states and transitions open in the next step of the port; NoxEditor edits them meanwhile." });
        };
        m_Canvas->SetGraph(m_Asset ? &m_Asset->Graph : nullptr);

        Ref<UI::UIScrollView> right = column(300.0f);
        m_Details = Ref<UI::UIStackBox>(new UI::UIStackBox(UI::UIOrientation::Vertical, 6.0f));
        m_Details->WidthMode = UI::UISizeMode::Fill;
        m_Details->Padding = UI::UIThickness::All(8.0f);
        right->AddChild(m_Details);

        Ref<UI::UIStackBox> body(new UI::UIStackBox(UI::UIOrientation::Horizontal, 0.0f));
        body->WidthMode = UI::UISizeMode::Fill;
        body->HeightMode = UI::UISizeMode::Fill;
        body->AddChild(left);
        body->AddChild(m_Canvas);
        body->AddChild(right);
        root->AddChild(body);

        m_Root = root;
        m_DetailsDirty = m_ParametersDirty = true;
        Update();
        return m_Root;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphEditorV2::Update()
    {
        if (!m_Asset)
            return;
        FollowDiskChanges();
        // A finished edit is one history step: a value dragged in Details is finished when the mouse button comes up.
        if (m_EditPending && SDL_GetMouseState(nullptr, nullptr) == 0)
            CommitHistory();
        if (m_ParametersDirty)
            RebuildParameters();
        if (m_DetailsDirty)
            RebuildDetails();
        for (const auto& sync : m_DetailSyncs)
            sync();
        RefreshToolbar();
    }

    void NodeGraphEditorV2::FollowDiskChanges()
    {
        // The file changed on disk (a pull, another tool, MCP): the asset manager reloaded it. Without edits of this window the
        // graph was replaced in place -- the window takes it (an undo step: Ctrl+Z goes back to what it showed) and builds its
        // rows again, which pointed into the old graph. With unsaved edits the new version waits (DiskGraph) and the window says so.
        if (m_Asset->DiskVersion == m_DiskVersion)
            return;
        m_DiskVersion = m_Asset->DiskVersion;
        if (m_Asset->DiskGraph)
        {
            if (Notify)
                Notify({ UI::UINotificationSeverity::Warning, m_Title + " changed on disk",
                         "Your unsaved edits are kept: Save writes them over the file; closing the window and choosing Don't Save takes the file's version." });
            return;
        }
        m_UndoStack.push_back(std::move(m_Committed));
        m_RedoStack.clear();
        m_Committed = m_Asset->Graph;
        m_EditPending = false;
        if (m_Canvas)
            m_Canvas->ReloadGraph();
        RebuildParameters();
        RebuildDetails();
        if (Notify)
            Notify({ UI::UINotificationSeverity::Info, m_Title + " reloaded", "The file changed on disk (Ctrl+Z goes back to what was shown)." });
    }

    void NodeGraphEditorV2::RefreshToolbar()
    {
        if (m_SaveButton)
        {
            // Only something to save makes it a button to press (Unreal greys it out too).
            m_SaveButton->StyleVariant = m_Dirty ? "primary" : "";
            m_SaveButton->SetEnabled(m_Dirty);
            m_SaveButton->Tooltip = m_Dirty ? "Write the graph to its file (Ctrl+S)" : "Saved: nothing changed since";
        }
        if (m_UndoButton)
            m_UndoButton->SetEnabled(!m_UndoStack.empty() || m_EditPending);
        if (m_RedoButton)
            m_RedoButton->SetEnabled(!m_RedoStack.empty() && !m_EditPending);
        if (m_Status)
        {
            const size_t nodes = m_Asset ? m_Asset->Graph.Nodes.size() : 0;
            m_Status->Text = m_Kind + "   |   " + std::to_string(nodes) + (nodes == 1 ? " node" : " nodes") + (m_Dirty ? "   |   unsaved" : "");
        }
    }

    bool NodeGraphEditorV2::HandleKey(int keyCode)
    {
        const SDL_Keymod mods = SDL_GetModState();
        if ((mods & SDL_KMOD_CTRL) == 0)
            return false;
        switch (keyCode)
        {
        case SDL_SCANCODE_S: Save(); return true;
        case SDL_SCANCODE_Z: if (mods & SDL_KMOD_SHIFT) Redo(); else Undo(); return true;
        case SDL_SCANCODE_Y: Redo(); return true;
        default: return false;
        }
    }

    void NodeGraphEditorV2::OnCanvasEdited(bool structural)
    {
        MarkEdited(structural);
        m_DetailsDirty = true; // (a wire into an input: its value row shows "connected")
    }

    void NodeGraphEditorV2::MarkEdited(bool structural)
    {
        if (!m_Asset)
            return;
        m_Dirty = true;
        m_EditPending = true;
        m_Asset->HasUnsavedEdits = true;
        // What the graph computes changed: compiled again, so a playing scene uses it now (moving a node changes nothing to run).
        if (structural)
            m_Asset->Recompile();
    }

    void NodeGraphEditorV2::CommitHistory()
    {
        constexpr size_t kMaxSteps = 200;
        m_UndoStack.push_back(std::move(m_Committed));
        if (m_UndoStack.size() > kMaxSteps)
            m_UndoStack.erase(m_UndoStack.begin());
        m_Committed = m_Asset->Graph;
        m_RedoStack.clear();
        m_EditPending = false;
    }

    void NodeGraphEditorV2::RestoreGraph(NodeGraph graph)
    {
        m_Asset->Graph = std::move(graph);
        m_Committed = m_Asset->Graph;
        m_EditPending = false;
        m_Dirty = true;
        m_Asset->HasUnsavedEdits = true;
        m_Asset->Recompile();
        if (m_Canvas)
            m_Canvas->ReloadGraph();
        // The Details and Parameters rows pointed into the graph that was replaced: built again now, before anything uses them.
        RebuildParameters();
        RebuildDetails();
    }

    void NodeGraphEditorV2::Undo()
    {
        if (!m_Asset)
            return;
        if (m_EditPending)
            CommitHistory(); // an edit not finished yet is undone as a step of its own
        if (m_UndoStack.empty())
            return;
        m_RedoStack.push_back(m_Asset->Graph);
        NodeGraph previous = std::move(m_UndoStack.back());
        m_UndoStack.pop_back();
        RestoreGraph(std::move(previous));
    }

    void NodeGraphEditorV2::Redo()
    {
        if (!m_Asset || m_RedoStack.empty() || m_EditPending)
            return; // (something edited since the undo: redoing would throw it away)
        m_UndoStack.push_back(m_Asset->Graph);
        NodeGraph next = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        RestoreGraph(std::move(next));
    }

    void NodeGraphEditorV2::Save()
    {
        auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
        if (!m_Asset || !assets || !m_Dirty)
            return; // (Ctrl+S with nothing changed: nothing to write, no toast)
        const auto found = assets->GetAssetRegistry().find(m_Handle);
        if (found == assets->GetAssetRegistry().end())
        {
            NOX_CORE_ERROR("Graph save: handle {} is not in the asset registry", static_cast<uint64_t>(m_Handle));
            return;
        }
        if (m_EditPending)
            CommitHistory();
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / found->second.FilePath;
        if (!NodeGraphSerializer::Serialize(path, m_Asset->Graph))
        {
            if (Notify)
                Notify({ UI::UINotificationSeverity::Error, "Graph not saved", path.filename().string() + " could not be written -- see the Output Log." });
            return;
        }
        m_Dirty = false;
        m_Asset->HasUnsavedEdits = false;
        m_Asset->DiskGraph.reset(); // (a version from disk that waited is overwritten now: closing must not bring it back)
        assets->NoteSourceWritten(m_Handle); // (its own save is no change on disk to reload)
        if (Notify)
            Notify({ UI::UINotificationSeverity::Success, "Graph saved", found->second.FilePath.generic_string() });
    }

    bool NodeGraphEditorV2::RevertToDisk()
    {
        if (!m_Asset)
            return false;
        if (!m_Dirty && !m_Asset->DiskGraph)
            return true;
        NodeGraph fromDisk;
        if (m_Asset->DiskGraph)
        {
            // The file changed while this window had edits: that version is what the file holds.
            fromDisk = std::move(*m_Asset->DiskGraph);
            m_Asset->DiskGraph.reset();
        }
        else
        {
            bool read = false;
            if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
            {
                const auto& registry = assets->GetAssetRegistry();
                if (const auto found = registry.find(m_Handle); found != registry.end())
                    read = NodeGraphSerializer::Deserialize(Project::GetActiveAssetDirectory() / found->second.FilePath, fromDisk);
            }
            if (!read)
            {
                if (Notify)
                    Notify({ UI::UINotificationSeverity::Error, "Changes not discarded",
                             m_Title + ": the file could not be read back -- see the Output Log. The edits stay." });
                return false;
            }
        }
        m_Asset->Graph = std::move(fromDisk);
        m_Asset->HasUnsavedEdits = false;
        m_Asset->Recompile();
        m_Committed = m_Asset->Graph;
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_EditPending = false;
        m_Dirty = false;
        if (m_Canvas)
            m_Canvas->ReloadGraph();
        m_ParametersDirty = true;
        m_DetailsDirty = true;
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphEditorV2::RebuildParameters()
    {
        m_ParametersDirty = false;
        m_ParameterSyncs.clear();
        if (!m_Parameters)
            return;
        m_Parameters->ClearChildren();
        if (!m_Asset)
            return;

        Ref<UI::UIStackBox> header(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
        header->WidthMode = UI::UISizeMode::Fill;
        header->VAlign = UI::UIAlignment::Center;
        Ref<UI::UILabel> title(new UI::UILabel("Parameters", 15.0f));
        title->WidthMode = UI::UISizeMode::Fill;
        title->VAlign = UI::UIAlignment::Center;
        header->AddChild(title);
        Ref<UI::UIButton> add(new UI::UIButton("+ Add", {}));
        add->Padding = UI::UIThickness::Symmetric(10.0f, 2.0f);
        add->HeightMode = UI::UISizeMode::Fixed;
        add->FixedSize = { 0.0f, 26.0f };
        add->StyleVariant = "primary";
        add->Tooltip = "A value the game sets from scripts (Animator.SetFloat ...) and the graph reads";
        UI::UIButton* addRaw = add.get();
        add->OnClick = [this, addRaw]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx)
                return;
            auto addParameter = [this](NodeGraphValue value, bool trigger)
            {
                std::vector<GraphParameter>& parameters = m_Asset->Graph.Parameters;
                std::string name = "NewParameter";
                for (int n = 2; std::any_of(parameters.begin(), parameters.end(), [&](const GraphParameter& p) { return p.Name == name; }); ++n)
                    name = "NewParameter" + std::to_string(n);
                parameters.push_back({ name, value, trigger });
                MarkEdited(true);
                m_ParametersDirty = true;
            };
            std::vector<UI::UIMenuItem> items;
            items.push_back({ "Float", "", [addParameter]() { addParameter(0.0f, false); } });
            items.push_back({ "Int", "", [addParameter]() { addParameter(int32_t(0), false); } });
            items.push_back({ "Bool", "", [addParameter]() { addParameter(false, false); } });
            items.push_back({ "Trigger", "", [addParameter]() { addParameter(false, true); }, false, true, false, false, "Fired once (a jump, a hit): true for one update." });
            UI::UIMenuPopup::ShowPopupMenu(*ctx, addRaw->GetBounds(), std::move(items));
        };
        header->AddChild(add);
        m_Parameters->AddChild(header);

        std::vector<GraphParameter>& parameters = m_Asset->Graph.Parameters;
        if (parameters.empty())
            m_Parameters->AddChild(MutedLabel("None yet. A parameter is set by the game and read in the graph with a Get node (right-click the canvas)."));
        for (size_t index = 0; index < parameters.size(); ++index)
        {
            GraphParameter& parameter = parameters[index];
            Ref<UI::UIStackBox> card(new UI::UIStackBox(UI::UIOrientation::Vertical, 4.0f));
            card->WidthMode = UI::UISizeMode::Fill;
            card->StyleClass = "Card";
            card->DrawBackground = true;
            card->Padding = UI::UIThickness::All(6.0f);
            InspectorCardBuilder builder{ card, m_ParameterSyncs, [this](bool) { MarkEdited(true); }, UI::UIContext::GetCurrent() };
            auto lastName = std::make_shared<std::string>(parameter.Name);
            builder.AddString("Name", &parameter.Name, "Name", [this, index, lastName]()
            {
                // Every node and transition rule reading it follows the new name.
                GraphParameter& renamed = m_Asset->Graph.Parameters[index];
                RenameParameterReferences(m_Asset->Graph, *lastName, renamed.Name);
                *lastName = renamed.Name;
                m_DetailsDirty = true;
            });
            builder.AddReadonly("Type", GraphParameterTypeName(parameter));
            if (!parameter.IsTrigger)
            {
                if (auto* value = std::get_if<float>(&parameter.DefaultValue))
                    builder.AddFloat("Default", value, 0.01f);
                else if (auto* number = std::get_if<int32_t>(&parameter.DefaultValue))
                    builder.AddInt32("Default", number);
                else if (auto* flag = std::get_if<bool>(&parameter.DefaultValue))
                    builder.AddBool("Default", flag);
            }
            Ref<UI::UIButton> remove(new UI::UIButton("Delete", [this, index]()
            {
                std::vector<GraphParameter>& list = m_Asset->Graph.Parameters;
                if (index < list.size())
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                MarkEdited(true);
                m_ParametersDirty = true;
                m_DetailsDirty = true;
            }));
            remove->Padding = UI::UIThickness::Symmetric(10.0f, 2.0f);
            remove->HAlign = UI::UIAlignment::End;
            remove->Tooltip = "Nodes reading it then read nothing (their value is the type's default)";
            card->AddChild(remove);
            m_Parameters->AddChild(card);
        }
    }

    void NodeGraphEditorV2::RebuildDetails()
    {
        m_DetailsDirty = false;
        m_DetailSyncs.clear();
        if (!m_Details)
            return;
        m_Details->ClearChildren();
        Ref<UI::UILabel> title(new UI::UILabel("Details", 15.0f));
        m_Details->AddChild(title);
        if (!m_Asset || !m_Canvas)
            return;
        const std::vector<uint32_t>& selection = m_Canvas->GetSelectedNodes();
        if (selection.empty())
        {
            m_Details->AddChild(MutedLabel("Select a node to edit its properties.\n\nRight-click the canvas to add a node; drag from a pin to "
                                           "connect; Alt+click a pin to cut its wires; double-click a wire for a reroute; F frames the graph."));
            return;
        }
        if (selection.size() > 1)
        {
            m_Details->AddChild(MutedLabel(std::to_string(selection.size()) + " nodes selected."));
            return;
        }
        NodeGraph& graph = m_Asset->Graph;
        GraphNode* node = nullptr;
        for (GraphNode& candidate : graph.Nodes)
            if (candidate.Id == selection.front())
                node = &candidate;
        if (!node)
            return;
        const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node->TypeName);
        Ref<UI::UILabel> name(new UI::UILabel(node->TypeName, 14.5f));
        m_Details->AddChild(name);
        if (!type)
        {
            m_Details->AddChild(MutedLabel("This node's type is not registered in this build: it is kept as it is."));
            return;
        }
        if (!type->Category.empty())
            m_Details->AddChild(MutedLabel(type->Category + (node->Id == graph.OutputNode ? "   |   the graph's output" : std::string())));

        Ref<UI::UIStackBox> body(new UI::UIStackBox(UI::UIOrientation::Vertical, 4.0f));
        body->WidthMode = UI::UISizeMode::Fill;
        m_Details->AddChild(body);
        InspectorCardBuilder builder{ body, m_DetailSyncs, [this](bool) { MarkEdited(true); }, UI::UIContext::GetCurrent() };
        auto edited = [this]() { MarkEdited(true); };

        // One property's editor, by what it holds and what its type declares (a range, choices, an asset kind, a parameter).
        auto addProperty = [&](const std::string& key, const std::string& label)
        {
            NodeGraphValue& value = node->Properties[key];
            if (key == kParameterReferenceProperty)
            {
                std::vector<std::string> names;
                int selected = -1;
                for (const GraphParameter& parameter : graph.Parameters)
                {
                    if (!type->ParameterType.empty() && type->ParameterType != GraphParameterTypeName(parameter))
                        continue;
                    if (const auto* current = std::get_if<std::string>(&value); current && *current == parameter.Name)
                        selected = static_cast<int>(names.size());
                    names.push_back(parameter.Name);
                }
                if (names.empty())
                {
                    builder.AddReadonly(label, "No parameter of this type (+ Add on the left)");
                    return;
                }
                builder.AddEnum(label, names, selected, [this, nodeId = node->Id, names](int index)
                {
                    for (GraphNode& n : m_Asset->Graph.Nodes)
                        if (n.Id == nodeId && index >= 0 && index < static_cast<int>(names.size()))
                            n.Properties[kParameterReferenceProperty] = names[static_cast<size_t>(index)];
                    MarkEdited(true);
                });
                return;
            }
            const auto hint = type->PropertyAssetTypeHints.find(key);
            const auto range = type->PropertyRanges.find(key);
            const auto choices = type->PropertyEnums.find(key);
            if (auto* flag = std::get_if<bool>(&value))
                builder.AddBool(label, flag, edited);
            else if (auto* number = std::get_if<int32_t>(&value))
            {
                if (choices != type->PropertyEnums.end())
                    builder.AddEnum(label, choices->second, *number, [this, number](int index) { *number = index; MarkEdited(true); });
                else
                    builder.AddInt32(label, number, 0, 0, edited);
            }
            else if (auto* real = std::get_if<float>(&value))
            {
                if (range != type->PropertyRanges.end())
                    builder.AddSlider(label, real, range->second.first, range->second.second, edited);
                else
                    builder.AddFloat(label, real, 0.01f, 0.0f, 0.0f, "{:.3f}", edited);
            }
            else if (auto* pair = std::get_if<glm::vec2>(&value))
                builder.AddVec2(label, pair, 0.01f, edited);
            else if (auto* color = std::get_if<glm::vec4>(&value))
                builder.AddColor4(label, color, edited);
            else if (auto* handle = std::get_if<uint64_t>(&value))
            {
                std::vector<AssetType> accepted;
                if (hint != type->PropertyAssetTypeHints.end())
                    if (const AssetType assetType = AssetTypeFromString(hint->second); assetType != AssetType::None)
                        accepted.push_back(assetType);
                builder.AddAssetSlot(label, AssetHandle(*handle), accepted, [this, handle](AssetHandle picked)
                {
                    *handle = static_cast<uint64_t>(picked);
                    MarkEdited(true);
                });
            }
            else if (auto* text = std::get_if<std::string>(&value))
                builder.AddString(label, text, hint != type->PropertyAssetTypeHints.end() && hint->second == "Bone" ? "Bone name" : "", edited,
                                  hint != type->PropertyAssetTypeHints.end() && hint->second == "Bone" ? "The bone's name in the skeleton (the bone picker comes with the preview step)" : "");
            else if (const auto* curve = std::get_if<NodeGraphCurve>(&value))
                builder.AddReadonly(label, std::to_string(curve->Keys.size()) + " keys (curve editing comes in a later step)");
        };

        // Properties the type declares, in its order (the variable input count is the + / - Pin buttons').
        std::unordered_set<std::string> shown{ kVariableInputCountProperty };
        bool anyProperty = false;
        for (const auto& [key, defaultValue] : type->DefaultProperties)
        {
            if (shown.contains(key))
                continue;
            if (!anyProperty)
                builder.AddSubheader("Properties");
            anyProperty = true;
            node->Properties.try_emplace(key, defaultValue);
            addProperty(key, key == kParameterReferenceProperty ? "Parameter" : key);
            shown.insert(key);
        }

        // The value inputs: the value each uses while no wire comes in.
        const NodePinList pins = ResolveNodePins(*type, node->Properties);
        bool anyInput = false;
        for (uint32_t pin = 0; pin < pins.Inputs.size(); ++pin)
        {
            const std::string& pinName = pins.Inputs[pin].Name;
            if (shown.contains(pinName) || !node->Properties.contains(pinName))
                continue;
            if (!anyInput)
                builder.AddSubheader("Inputs", "The value an input uses while no wire comes into it");
            anyInput = true;
            shown.insert(pinName);
            const bool connected = std::any_of(graph.Links.begin(), graph.Links.end(),
                                               [&](const GraphLink& l) { return l.ToNode == node->Id && l.ToPin == pin; });
            if (connected)
                builder.AddReadonly(pinName, "connected");
            else
                addProperty(pinName, pinName);
        }

        // Unreal's Add Pin: one more of the type's variable input group, or the last one away (its wires with it).
        if (!type->VariableInputs.empty())
        {
            const uint32_t variable = VariableInputCount(*type, node->Properties);
            Ref<UI::UIStackBox> row(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
            const uint32_t nodeId = node->Id;
            Ref<UI::UIButton> addPin(new UI::UIButton("+ Pin", [this, nodeId, type, variable]()
            {
                for (GraphNode& n : m_Asset->Graph.Nodes)
                {
                    if (n.Id != nodeId)
                        continue;
                    for (const NodePinDesc& pin : type->VariableInputs)
                        if (const auto literal = type->VariableInputLiterals.find(pin.Name); literal != type->VariableInputLiterals.end())
                            n.Properties.try_emplace(pin.Name + " " + std::to_string(variable), literal->second);
                    n.Properties[kVariableInputCountProperty] = static_cast<int32_t>(variable + 1);
                }
                MarkEdited(true);
                m_DetailsDirty = true;
            }));
            row->AddChild(addPin);
            if (variable > type->MinVariableInputs)
            {
                Ref<UI::UIButton> removePin(new UI::UIButton("- Pin", [this, nodeId, type, variable, inputCount = static_cast<uint32_t>(pins.Inputs.size())]()
                {
                    const uint32_t firstOfLast = inputCount - static_cast<uint32_t>(type->VariableInputs.size());
                    std::erase_if(m_Asset->Graph.Links, [&](const GraphLink& l) { return l.ToNode == nodeId && l.ToPin >= firstOfLast; });
                    for (GraphNode& n : m_Asset->Graph.Nodes)
                    {
                        if (n.Id != nodeId)
                            continue;
                        for (const NodePinDesc& pin : type->VariableInputs)
                            n.Properties.erase(pin.Name + " " + std::to_string(variable - 1));
                        n.Properties[kVariableInputCountProperty] = static_cast<int32_t>(variable - 1);
                    }
                    MarkEdited(true);
                    m_DetailsDirty = true;
                }));
                row->AddChild(removePin);
            }
            builder.AddRow("Inputs", row, "Add or remove one of the node's repeating inputs");
        }

        // Anything else the node carries (a property its type no longer declares is kept, shown here).
        bool anyOther = false;
        for (auto& [key, value] : node->Properties)
        {
            if (shown.contains(key))
                continue;
            if (!anyOther)
                builder.AddSubheader("Other");
            anyOther = true;
            shown.insert(key);
            builder.AddReadonly(key, "kept from an older version of the node");
        }
        if (!anyProperty && !anyInput && type->VariableInputs.empty())
            m_Details->AddChild(MutedLabel("This node has nothing to set."));
    }
}
