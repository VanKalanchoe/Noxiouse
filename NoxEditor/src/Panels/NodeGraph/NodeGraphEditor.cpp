#include "NodeGraphEditor.h"

#include <algorithm>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "NodeGraphCanvas.h"
#include "StateMachineView.h"
#include "Panels/AssetPreviewRenderer.h"
#include "Panels/InspectorWidgets.h"
#include "NoxCore/Animation/AnimPose.h"
#include "NoxCore/Animation/AnimationGraphInstance.h"
#include "NoxCore/Audio/AudioEngine.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include <NoxUI/Widgets/UICurveEditor.h>
#include <NoxUI/Widgets/UILevelMeter.h>
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UICheckBox.h>
#include <NoxUI/Widgets/UIDragScalar.h>
#include <NoxUI/Widgets/UIImage.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIPopup.h>
#include <NoxUI/Widgets/UIScrollView.h>
#include <NoxUI/Widgets/UITextInput.h>

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

        Ref<NoxUI::UILabel> MutedLabel(const std::string& text, float size = 13.0f)
        {
            Ref<NoxUI::UILabel> label(new NoxUI::UILabel(text, size));
            label->UseColorOverride = true;
            label->ColorOverride = NoxUI::ThemeColor("TextMuted", { 0.60f, 0.64f, 0.72f, 1.0f });
            label->WordWrap = true;
            label->WidthMode = NoxUI::UISizeMode::Fill;
            return label;
        }
    }

    NodeGraphEditor::NodeGraphEditor(AssetHandle graph)
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

    NodeGraphEditor::~NodeGraphEditor()
    {
        // Closed: the asset matches the file again. The editor asked about unsaved edits before closing the window (Save / Don't
        // Save); edits still here (the editor shutting down) are dropped, never kept in memory as if saved. A version from disk it
        // had not taken is what the file holds now.
        Notify = nullptr; // (whoever listened may be going away too)
        StopAudioPreview(); // (closing the window stops what it plays)
        ClosePopup(); // (its rows call into this editor)
        if (m_Asset && (m_Dirty || m_Asset->DiskGraph))
            RevertToDisk();
        if (m_Asset)
            m_Asset->HasUnsavedEdits = false;
    }

    Ref<NoxUI::UIElement> NodeGraphEditor::BuildUI()
    {
        // The window's root: keys typed in it that its children leave (Ctrl+S, Ctrl+Z ...) come here.
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 0.0f);
        root->OnKey = [this](const NoxUI::UIKeyEvent& e) { return HandleKey(e); };
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;

        auto button = [](const std::string& text, std::function<void()> onClick, const std::string& tooltip)
        {
            Ref<NoxUI::UIButton> b(new NoxUI::UIButton(text, std::move(onClick)));
            b->Padding = NoxUI::UIThickness::Symmetric(10.0f, 2.0f);
            b->HeightMode = NoxUI::UISizeMode::Fixed;
            b->FixedSize = { 0.0f, 26.0f };
            b->Tooltip = tooltip;
            b->VAlign = NoxUI::UIAlignment::Center;
            return b;
        };
        Ref<NoxUI::UIStackBox> toolbar(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        toolbar->WidthMode = NoxUI::UISizeMode::Fill;
        toolbar->Padding = NoxUI::UIThickness{ 6.0f, 5.0f, 6.0f, 4.0f };
        toolbar->VAlign = NoxUI::UIAlignment::Center;
        m_SaveButton = button("Save", [this]() { Save(); }, "Write the graph to its file (Ctrl+S)");
        m_UndoButton = button("Undo", [this]() { Undo(); }, "Ctrl+Z");
        m_RedoButton = button("Redo", [this]() { Redo(); }, "Ctrl+Y");
        toolbar->AddChild(m_SaveButton);
        toolbar->AddChild(m_UndoButton);
        toolbar->AddChild(m_RedoButton);
        toolbar->AddChild(button("Frame", [this]() { if (m_Canvas) m_Canvas->FrameSelection(); }, "The selection, or the whole graph, in view (F)"));
        m_Status = MutedLabel("", 13.0f);
        m_Status->WordWrap = false;
        m_Status->VAlign = NoxUI::UIAlignment::Center;
        m_Status->Margin = NoxUI::UIThickness{ 8.0f, 0.0f, 0.0f, 0.0f };
        toolbar->AddChild(m_Status);
        root->AddChild(toolbar);
        if (m_Asset && m_Asset->Graph.Domain == kMaterialGraphDomain)
        {
            m_CompileStatus = MutedLabel("", 12.5f);
            m_CompileStatus->Margin = NoxUI::UIThickness{ 8.0f, 0.0f, 8.0f, 4.0f };
            root->AddChild(m_CompileStatus);
            m_CompileStatusDirty = true;
        }

        // Parameters | canvas | Details.
        auto column = [](float width)
        {
            Ref<NoxUI::UIScrollView> scroll(new NoxUI::UIScrollView());
            scroll->WidthMode = NoxUI::UISizeMode::Fixed;
            scroll->FixedSize = { width, 0.0f };
            scroll->HeightMode = NoxUI::UISizeMode::Fill;
            scroll->ChildSpacing = 6.0f;
            return scroll;
        };
        Ref<NoxUI::UIScrollView> left = column(230.0f);
        m_Parameters = Ref<NoxUI::UIStackBox>(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 6.0f));
        m_Parameters->WidthMode = NoxUI::UISizeMode::Fill;
        m_Parameters->Padding = NoxUI::UIThickness::All(8.0f);
        left->AddChild(m_Parameters);

        m_Canvas = Ref<NodeGraphCanvas>(new NodeGraphCanvas());
        m_Canvas->Parameters = m_Asset ? &m_Asset->Graph.Parameters : nullptr;
        m_Canvas->OnEdited = [this](bool structural) { OnCanvasEdited(structural); };
        m_Canvas->OnSelectionChanged = [this]() { m_DetailsDirty = true; };
        m_Canvas->OnOpenNode = [this](uint32_t nodeId) { m_Path.push_back(nodeId); }; // (shown by the next update's ResolvePath)
        m_Canvas->OnEditValue = [this](uint32_t nodeId, const std::string& property, const NoxUI::UIRect& at) { ShowValuePopup(nodeId, property, at); };
        m_Canvas->SetGraph(m_Asset ? &m_Asset->Graph : nullptr);

        Ref<NoxUI::UIScrollView> right = column(300.0f);
        if (m_Asset && m_Asset->Graph.Domain != kMaterialGraphDomain)
        {
            m_PreviewBox = Ref<NoxUI::UIStackBox>(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 4.0f));
            m_PreviewBox->WidthMode = NoxUI::UISizeMode::Fill;
            m_PreviewBox->Padding = NoxUI::UIThickness::All(8.0f);
            m_PreviewBox->StyleClass = "Card";
            m_PreviewBox->DrawBackground = true;
            right->AddChild(m_PreviewBox);
            m_PreviewDirty = true;
        }
        m_Details = Ref<NoxUI::UIStackBox>(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 6.0f));
        m_Details->WidthMode = NoxUI::UISizeMode::Fill;
        m_Details->Padding = NoxUI::UIThickness::All(8.0f);
        right->AddChild(m_Details);

        m_StateView = Ref<StateMachineView>(new StateMachineView());
        m_StateView->Visible = false;
        m_StateView->OnEdited = [this](bool structural) { OnCanvasEdited(structural); };
        m_StateView->OnSelectionChanged = [this]() { m_DetailsDirty = true; };
        m_StateView->OnOpenState = [this](uint32_t state) { m_Path.push_back(state); };
        m_StateView->FirstParameter = [this]() { return m_Asset && !m_Asset->Graph.Parameters.empty() ? m_Asset->Graph.Parameters.front().Name : std::string(); };

        // The middle: where the window is in the graph (shown below the top), then the canvas or the states view.
        m_Breadcrumb = Ref<NoxUI::UIStackBox>(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 4.0f));
        m_Breadcrumb->WidthMode = NoxUI::UISizeMode::Fill;
        m_Breadcrumb->Padding = NoxUI::UIThickness{ 6.0f, 3.0f, 6.0f, 3.0f };
        m_Breadcrumb->Visible = false;
        Ref<NoxUI::UIStackBox> middle(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 0.0f));
        middle->WidthMode = NoxUI::UISizeMode::Fill;
        middle->HeightMode = NoxUI::UISizeMode::Fill;
        middle->AddChild(m_Breadcrumb);
        middle->AddChild(m_Canvas);
        middle->AddChild(m_StateView);

        Ref<NoxUI::UIStackBox> body(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 0.0f));
        body->WidthMode = NoxUI::UISizeMode::Fill;
        body->HeightMode = NoxUI::UISizeMode::Fill;
        body->AddChild(left);
        body->AddChild(middle);
        body->AddChild(right);
        root->AddChild(body);

        m_Root = root;
        m_DetailsDirty = m_ParametersDirty = true;
        m_PathLabels.clear();
        ResolvePath();
        Update();
        return m_Root;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphEditor::Update()
    {
        if (!m_Asset)
            return;
        FollowDiskChanges();
        // A finished edit is one history step: a value dragged in Details is finished when the mouse button comes up.
        if (m_EditPending && !NoxUI::UIContext::IsMouseButtonHeld())
            CommitHistory();
        if (m_ParametersDirty)
            RebuildParameters();
        ResolvePath();
        if (!m_PreviewSearched)
            FindPreviewMesh();
        if (m_DetailsDirty)
            RebuildDetails();
        if (m_CompileStatusDirty)
            RefreshCompileStatus();
        if (m_PreviewBox)
        {
            if (m_PreviewDirty)
                RebuildPreview();
            if (m_Asset->Graph.Domain == kAudioGraphDomain)
                UpdateAudioPreview();
            else
                UpdateAnimationPreview();
        }
        RefreshToolbar();
    }

    void NodeGraphEditor::FollowDiskChanges()
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
                Notify({ NoxUI::UINotificationSeverity::Warning, m_Title + " changed on disk",
                         "Your unsaved edits are kept: Save writes them over the file; closing the window and choosing Don't Save takes the file's version." });
            return;
        }
        ClosePopup();
        m_CompileStatusDirty = true;
        m_UndoStack.push_back(std::move(m_Committed));
        m_RedoStack.clear();
        m_Committed = m_Asset->Graph;
        m_EditPending = false;
        ResolvePath();
        if (m_Canvas)
            m_Canvas->ReloadGraph();
        RebuildParameters();
        RebuildDetails();
        if (Notify)
            Notify({ NoxUI::UINotificationSeverity::Info, m_Title + " reloaded", "The file changed on disk (Ctrl+Z goes back to what was shown)." });
    }

    void NodeGraphEditor::RefreshToolbar()
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

    bool NodeGraphEditor::HandleKey(const NoxUI::UIKeyEvent& e)
    {
        if ((e.Modifiers & NoxUI::UIMod::Ctrl) == 0)
            return false;
        switch (e.KeyCode)
        {
        case SDL_SCANCODE_S: Save(); return true;
        case SDL_SCANCODE_Z: if (e.Modifiers & NoxUI::UIMod::Shift) Redo(); else Undo(); return true;
        case SDL_SCANCODE_Y: Redo(); return true;
        default: return false;
        }
    }

    void NodeGraphEditor::OnCanvasEdited(bool structural)
    {
        MarkEdited(structural);
        m_DetailsDirty = true; // (a wire into an input: its value row shows "connected")
    }

    void NodeGraphEditor::MarkEdited(bool structural)
    {
        if (!m_Asset)
            return;
        m_Dirty = true;
        m_EditPending = true;
        m_Asset->HasUnsavedEdits = true;
        // What the graph computes changed: compiled again, so a playing scene uses it now (moving a node changes nothing to run).
        if (structural)
        {
            m_Asset->Recompile();
            m_CompileStatusDirty = true;
        }
    }

    void NodeGraphEditor::CommitHistory()
    {
        constexpr size_t kMaxSteps = 200;
        m_UndoStack.push_back(std::move(m_Committed));
        if (m_UndoStack.size() > kMaxSteps)
            m_UndoStack.erase(m_UndoStack.begin());
        m_Committed = m_Asset->Graph;
        m_RedoStack.clear();
        m_EditPending = false;
    }

    void NodeGraphEditor::RestoreGraph(NodeGraph graph)
    {
        ClosePopup(); // (its rows point into the graph being replaced)
        m_CompileStatusDirty = true;
        m_Asset->Graph = std::move(graph);
        m_Committed = m_Asset->Graph;
        m_EditPending = false;
        m_Dirty = true;
        m_Asset->HasUnsavedEdits = true;
        m_Asset->Recompile();
        ResolvePath(); // (a state's graph shown lives inside the graph that was replaced: the canvas takes its new address)
        if (m_Canvas)
            m_Canvas->ReloadGraph();
        // The Details and Parameters rows pointed into the graph that was replaced: built again now, before anything uses them.
        RebuildParameters();
        RebuildDetails();
    }

    void NodeGraphEditor::Undo()
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

    void NodeGraphEditor::Redo()
    {
        if (!m_Asset || m_RedoStack.empty() || m_EditPending)
            return; // (something edited since the undo: redoing would throw it away)
        m_UndoStack.push_back(m_Asset->Graph);
        NodeGraph next = std::move(m_RedoStack.back());
        m_RedoStack.pop_back();
        RestoreGraph(std::move(next));
    }

    void NodeGraphEditor::Save()
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
                Notify({ NoxUI::UINotificationSeverity::Error, "Graph not saved", path.filename().string() + " could not be written -- see the Output Log." });
            return;
        }
        m_Dirty = false;
        m_Asset->HasUnsavedEdits = false;
        m_Asset->DiskGraph.reset(); // (a version from disk that waited is overwritten now: closing must not bring it back)
        assets->NoteSourceWritten(m_Handle); // (its own save is no change on disk to reload)
        if (Notify)
            Notify({ NoxUI::UINotificationSeverity::Success, "Graph saved", found->second.FilePath.generic_string() });
    }

    bool NodeGraphEditor::ApplyExternalEdit(NodeGraph replacement)
    {
        if (!m_Asset)
            return false;
        if (m_EditPending)
            CommitHistory();
        m_UndoStack.push_back(m_Asset->Graph);
        m_RedoStack.clear();
        RestoreGraph(std::move(replacement)); // (marks it edited, recompiles, rebuilds the rows)
        Save();
        return !m_Dirty;
    }

    bool NodeGraphEditor::RevertToDisk()
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
                    Notify({ NoxUI::UINotificationSeverity::Error, "Changes not discarded",
                             m_Title + ": the file could not be read back -- see the Output Log. The edits stay." });
                return false;
            }
        }
        ClosePopup();
        m_CompileStatusDirty = true;
        m_Asset->Graph = std::move(fromDisk);
        m_Asset->HasUnsavedEdits = false;
        m_Asset->Recompile();
        m_Committed = m_Asset->Graph;
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_EditPending = false;
        m_Dirty = false;
        ResolvePath();
        if (m_Canvas)
            m_Canvas->ReloadGraph();
        m_ParametersDirty = true;
        m_DetailsDirty = true;
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphEditor::RebuildParameters()
    {
        m_ParametersDirty = false;
        m_PreviewDirty = true; // (the preview lists the parameters too)
        if (!m_Parameters)
            return;
        m_Parameters->ClearChildren();
        if (!m_Asset)
            return;

        Ref<NoxUI::UIStackBox> header(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        header->WidthMode = NoxUI::UISizeMode::Fill;
        header->VAlign = NoxUI::UIAlignment::Center;
        Ref<NoxUI::UILabel> title(new NoxUI::UILabel("Parameters", 15.0f));
        title->WidthMode = NoxUI::UISizeMode::Fill;
        title->VAlign = NoxUI::UIAlignment::Center;
        header->AddChild(title);
        Ref<NoxUI::UIButton> add(new NoxUI::UIButton("+ Add", {}));
        add->Padding = NoxUI::UIThickness::Symmetric(10.0f, 2.0f);
        add->HeightMode = NoxUI::UISizeMode::Fixed;
        add->FixedSize = { 0.0f, 26.0f };
        add->StyleVariant = "primary";
        add->Tooltip = "A value the game sets from scripts (Animator.SetFloat ...) and the graph reads";
        NoxUI::UIButton* addRaw = add.get();
        add->OnClick = [this, addRaw]()
        {
            NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
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
            std::vector<NoxUI::UIMenuItem> items;
            items.push_back({ "Float", "", [addParameter]() { addParameter(0.0f, false); } });
            items.push_back({ "Int", "", [addParameter]() { addParameter(int32_t(0), false); } });
            items.push_back({ "Bool", "", [addParameter]() { addParameter(false, false); } });
            items.push_back({ "Trigger", "", [addParameter]() { addParameter(false, true); }, false, true, false, false, "Fired once (a jump, a hit): true for one update." });
            NoxUI::UIMenuPopup::ShowPopupMenu(*ctx, addRaw->GetBounds(), std::move(items));
        };
        header->AddChild(add);
        m_Parameters->AddChild(header);

        std::vector<GraphParameter>& parameters = m_Asset->Graph.Parameters;
        if (parameters.empty())
            m_Parameters->AddChild(MutedLabel("None yet. A parameter is set by the game and read in the graph with a Get node (right-click the canvas)."));
        for (size_t index = 0; index < parameters.size(); ++index)
        {
            GraphParameter& parameter = parameters[index];
            Ref<NoxUI::UIStackBox> card(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 4.0f));
            card->WidthMode = NoxUI::UISizeMode::Fill;
            card->StyleClass = "Card";
            card->DrawBackground = true;
            card->Padding = NoxUI::UIThickness::All(6.0f);
            // Dragged (by its background or labels) onto the canvas: a Get node of it there.
            card->Tooltip = "Drag onto the graph for a node that reads it";
            card->OnDragStart = [this, index]() -> std::optional<NoxUI::UIDragPayload>
            {
                if (!m_Asset || index >= m_Asset->Graph.Parameters.size())
                    return std::nullopt;
                NoxUI::UIDragPayload payload;
                payload.Type = NodeGraphCanvas::kParameterDragType;
                payload.Text = m_Asset->Graph.Parameters[index].Name;
                return payload;
            };
            InspectorCardBuilder builder{ card, [this](bool) { MarkEdited(true); }, NoxUI::UIContext::GetCurrent() };
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
            Ref<NoxUI::UIButton> remove(new NoxUI::UIButton("Delete", [this, index]()
            {
                std::vector<GraphParameter>& list = m_Asset->Graph.Parameters;
                if (index < list.size())
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                MarkEdited(true);
                m_ParametersDirty = true;
                m_DetailsDirty = true;
            }));
            remove->Padding = NoxUI::UIThickness::Symmetric(10.0f, 2.0f);
            remove->HAlign = NoxUI::UIAlignment::End;
            remove->Tooltip = "Nodes reading it then read nothing (their value is the type's default)";
            card->AddChild(remove);
            m_Parameters->AddChild(card);
        }
    }

    void NodeGraphEditor::RebuildDetails()
    {
        m_DetailsDirty = false;
        if (!m_Details)
            return;
        m_Details->ClearChildren();
        Ref<NoxUI::UILabel> title(new NoxUI::UILabel("Details", 15.0f));
        m_Details->AddChild(title);
        if (!m_Asset || !m_Canvas)
            return;
        if (m_ShownMachine)
        {
            BuildMachineDetails(*m_ShownMachine);
            return;
        }
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
        NodeGraph& graph = m_Canvas->GetGraph() ? *m_Canvas->GetGraph() : m_Asset->Graph; // (a state's graph when one is open)
        GraphNode* node = nullptr;
        for (GraphNode& candidate : graph.Nodes)
            if (candidate.Id == selection.front())
                node = &candidate;
        if (!node)
            return;
        const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node->TypeName);
        Ref<NoxUI::UILabel> name(new NoxUI::UILabel(node->TypeName, 14.5f));
        m_Details->AddChild(name);
        if (!type)
        {
            m_Details->AddChild(MutedLabel("This node's type is not registered in this build: it is kept as it is."));
            return;
        }
        if (!type->Category.empty())
            m_Details->AddChild(MutedLabel(type->Category + (node->Id == graph.OutputNode ? "   |   the graph's output" : std::string())));

        Ref<NoxUI::UIStackBox> body(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 4.0f));
        body->WidthMode = NoxUI::UISizeMode::Fill;
        m_Details->AddChild(body);
        InspectorCardBuilder builder{ body, [this](bool) { MarkEdited(true); }, NoxUI::UIContext::GetCurrent() };
        const uint32_t nodeId = node->Id;
        auto addProperty = [&](const std::string& key, const std::string& label)
        {
            AddValueEditor(builder, nodeId, key, label, [this]() { m_DetailsDirty = true; });
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
            Ref<NoxUI::UIStackBox> row(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
            Ref<NoxUI::UIButton> addPin(new NoxUI::UIButton("+ Pin", [this, nodeId, type, variable]()
            {
                for (GraphNode& n : ShownGraph().Nodes)
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
                Ref<NoxUI::UIButton> removePin(new NoxUI::UIButton("- Pin", [this, nodeId, type, variable, inputCount = static_cast<uint32_t>(pins.Inputs.size())]()
                {
                    const uint32_t firstOfLast = inputCount - static_cast<uint32_t>(type->VariableInputs.size());
                    std::erase_if(ShownGraph().Links, [&](const GraphLink& l) { return l.ToNode == nodeId && l.ToPin >= firstOfLast; });
                    for (GraphNode& n : ShownGraph().Nodes)
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

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphEditor::AddValueEditor(InspectorCardBuilder& builder, uint32_t nodeId, const std::string& key, const std::string& label,
                                           std::function<void()> rebuild)
    {
        NodeGraph& graph = ShownGraph();
        GraphNode* node = nullptr;
        for (GraphNode& candidate : graph.Nodes)
            if (candidate.Id == nodeId)
                node = &candidate;
        const NodeTypeDesc* type = node ? NodeTypeRegistry::Find(graph.Domain, node->TypeName) : nullptr;
        if (!node || !type)
            return;
        auto edited = [this]() { MarkEdited(true); };
        NodeGraphValue& value = node->Properties[key];
        if (key == kParameterReferenceProperty)
        {
            std::vector<std::string> names;
            int selected = -1;
            for (const GraphParameter& parameter : m_Asset->Graph.Parameters) // (the asset's: a state's graph reads them too)
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
            builder.AddEnum(label, names, selected, [this, nodeId, names](int index)
            {
                for (GraphNode& n : ShownGraph().Nodes)
                    if (n.Id == nodeId && index >= 0 && index < static_cast<int>(names.size()))
                        n.Properties[kParameterReferenceProperty] = names[static_cast<size_t>(index)];
                MarkEdited(true);
            });
            return;
        }
        const auto hint = type->PropertyAssetTypeHints.find(key);
        const auto range = type->PropertyRanges.find(key);
        const auto choices = type->PropertyEnums.find(key);
        const bool bone = hint != type->PropertyAssetTypeHints.end() && hint->second == "Bone";
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
                m_PreviewSearched = m_PreviewMesh != 0; // (a clip picked: the bone list may now find its skeleton)
            });
        }
        else if (auto* text = bone ? std::get_if<std::string>(&value) : nullptr)
        {
            // A bone: a button reading the bone (marked when the preview skeleton has no bone of that name) that opens the picker.
            const Skeleton* skeleton = m_PreviewSkeleton != 0 ? AssetManager::FindLoadedAsset<Skeleton>(m_PreviewSkeleton) : nullptr;
            const bool known = !skeleton || text->empty() ||
                               std::any_of(skeleton->AllNodes.begin(), skeleton->AllNodes.end(), [&](const Node* n) { return n && n->Name == *text; });
            Ref<NoxUI::UIButton> pick(new NoxUI::UIButton(text->empty() ? std::string("(pick a bone)") : *text + (known ? "" : "  (not in the skeleton)")));
            pick->WidthMode = NoxUI::UISizeMode::Fill;
            pick->Tooltip = skeleton ? "Pick from the preview skeleton's bones, or type a name" : "Type the bone's name, or pick a preview mesh to list its bones";
            NoxUI::UIButton* raw = pick.get();
            pick->OnClick = [this, nodeId, key, raw]() { ShowBonePicker(nodeId, key, raw->GetBounds()); };
            // (the button follows the value: a pick or an undo changes it)
            NoxUI::AppendSync(*raw, [this, nodeId, key, raw]()
            {
                for (const GraphNode& n : ShownGraph().Nodes)
                    if (n.Id == nodeId)
                        if (const auto found = n.Properties.find(key); found != n.Properties.end())
                            if (const auto* name = std::get_if<std::string>(&found->second))
                            {
                                const std::string shown = name->empty() ? std::string("(pick a bone)") : *name;
                                if (raw->GetText().rfind(shown, 0) != 0)
                                    raw->SetText(shown);
                            }
            });
            builder.AddRow(label, pick);
        }
        else if (auto* plain = std::get_if<std::string>(&value))
            builder.AddString(label, plain, "", edited);
        else if (std::holds_alternative<NodeGraphCurve>(value))
        {
            // The curve drawn and shaped by hand, then its keys as numbers, + Key and Smooth.
            const auto curveOf = [this, nodeId, key]() -> NodeGraphCurve*
            {
                for (GraphNode& n : ShownGraph().Nodes)
                    if (n.Id == nodeId)
                        if (auto found = n.Properties.find(key); found != n.Properties.end())
                            return std::get_if<NodeGraphCurve>(&found->second);
                return nullptr;
            };
            builder.AddSubheader(label, "x (what comes in) -> y (what goes out)");
            // The curve to see and shape (UI::UICurveEditor over the node's keys): drag a key, double-click to add one, right-click a key
            // to remove it; the view follows the keys.
            Ref<NoxUI::UICurveEditor> view(new NoxUI::UICurveEditor({}));
            view->RangeMode = NoxUI::UICurveEditor::Range::FitKeys;
            view->AddOnDoubleClick = true;
            view->Keys = [curveOf]() -> std::vector<NoxUI::UIVec2>* { NodeGraphCurve* c = curveOf(); return c ? &c->Keys : nullptr; };
            view->Evaluator = [curveOf](float x) { const NodeGraphCurve* c = curveOf(); return c ? c->Evaluate(x) : 0.0f; };
            view->OnChanging = [this]() { MarkEdited(true); };
            view->OnKeyCountChanged = [this, rebuild]() { MarkEdited(true); if (rebuild) rebuild(); };
            view->HeightMode = NoxUI::UISizeMode::Fixed;
            view->FixedSize = { 0.0f, 120.0f };
            view->Padding = NoxUI::UIThickness::All(4.0f);
            view->Tooltip = "Drag a key; double-click to add one; right-click a key to remove it.";
            builder.Body->AddChild(view);
            NodeGraphCurve* curve = curveOf();
            for (size_t k = 0; curve && k < curve->Keys.size(); ++k)
            {
                // A key's x stays between its neighbours (the keys stay in order: no row jumps to another key).
                builder.AddVec2("Key " + std::to_string(k + 1), &curve->Keys[k], 0.01f, [this, curveOf, k]()
                {
                    if (NodeGraphCurve* c = curveOf(); c && k < c->Keys.size())
                    {
                        if (k > 0)
                            c->Keys[k].x = std::max(c->Keys[k].x, c->Keys[k - 1].x);
                        if (k + 1 < c->Keys.size())
                            c->Keys[k].x = std::min(c->Keys[k].x, c->Keys[k + 1].x);
                    }
                    MarkEdited(true);
                });
            }
            Ref<NoxUI::UIStackBox> row(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
            Ref<NoxUI::UIButton> addKey(new NoxUI::UIButton("+ Key", [this, curveOf, rebuild]()
            {
                if (NodeGraphCurve* c = curveOf())
                    c->Keys.push_back(c->Keys.empty() ? glm::vec2(0.0f) : c->Keys.back() + glm::vec2(1.0f, 0.0f));
                MarkEdited(true);
                if (rebuild)
                    rebuild();
            }));
            addKey->Tooltip = "A key one unit after the last";
            row->AddChild(addKey);
            if (curve && !curve->Keys.empty())
            {
                Ref<NoxUI::UIButton> removeKey(new NoxUI::UIButton("- Key", [this, curveOf, rebuild]()
                {
                    if (NodeGraphCurve* c = curveOf(); c && !c->Keys.empty())
                        c->Keys.pop_back();
                    MarkEdited(true);
                    if (rebuild)
                        rebuild();
                }));
                removeKey->Tooltip = "Removes the last key (right-click a key on the curve to remove that one)";
                row->AddChild(removeKey);
            }
            builder.AddRow("Keys", row);
            if (curve)
                builder.AddBool("Smooth", &curve->Smooth, edited, "Off: straight lines between the keys. On: smooth through them.");
        }
    }

    void NodeGraphEditor::ClosePopup()
    {
        ++*m_PopupGeneration; // (a popup asked to show again is not shown: it belonged to what is closing)
        if (m_PopupOpen && *m_PopupOpen && m_PopupContext) // (still up: its context is alive -- the flag drops with the popup)
            m_PopupContext->ClearOverlay();
        m_PopupOpen.reset();
        m_PopupContext = nullptr;
    }

    void NodeGraphEditor::DeferPopup(std::function<void()> show)
    {
        // Next frame, not from inside the click that changed the popup's shape (that would destroy the button running it); skipped
        // when the popup was closed meanwhile or the editor is gone.
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx)
        {
            show();
            return;
        }
        ctx->Defer([token = std::weak_ptr<uint32_t>(m_PopupGeneration), generation = *m_PopupGeneration, show = std::move(show)]()
        {
            const std::shared_ptr<uint32_t> alive = token.lock();
            if (alive && *alive == generation)
                show();
        });
    }

    void NodeGraphEditor::ShowValuePopup(uint32_t nodeId, const std::string& key, const NoxUI::UIRect& at)
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx || !m_Asset)
            return;
        // A bone opens its picker straight away (the popup would only hold its button).
        for (const GraphNode& n : ShownGraph().Nodes)
            if (n.Id == nodeId)
                if (const NodeTypeDesc* type = NodeTypeRegistry::Find(ShownGraph().Domain, n.TypeName))
                    if (const auto hint = type->PropertyAssetTypeHints.find(key); hint != type->PropertyAssetTypeHints.end() && hint->second == "Bone")
                    {
                        ShowBonePicker(nodeId, key, at);
                        return;
                    }
        Ref<NoxUI::UIStackBox> box(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 4.0f));
        box->Padding = NoxUI::UIThickness::All(10.0f);
        box->FixedSize = { 340.0f, 0.0f };
        InspectorCardBuilder builder{ box, [this](bool) { MarkEdited(true); }, ctx };
        AddValueEditor(builder, nodeId, key, key, [this, nodeId, key, at]() { DeferPopup([this, nodeId, key, at]() { ShowValuePopup(nodeId, key, at); }); });
        m_PopupOpen = NoxUI::UIPopup::Show(*ctx, at, box);
        m_PopupContext = ctx;
        m_DetailsDirty = true; // (Details shows the same value: built again so both point at it)
    }

    void NodeGraphEditor::ShowBonePicker(uint32_t nodeId, const std::string& key, const NoxUI::UIRect& at)
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        if (!ctx || !m_Asset)
            return;
        std::string current;
        for (const GraphNode& n : ShownGraph().Nodes)
            if (n.Id == nodeId)
                if (const auto found = n.Properties.find(key); found != n.Properties.end())
                    if (const auto* name = std::get_if<std::string>(&found->second))
                        current = *name;
        // Choosing: the value set (one step), the popup closed. Copied first: closing destroys the button that called it.
        auto choose = [this, nodeId, key, ctx](const std::string& bone)
        {
            const std::string chosen = bone;
            const uint32_t id = nodeId;
            const std::string property = key;
            NoxUI::UIContext* context = ctx;
            NodeGraphEditor* self = this;
            context->ClearOverlay(); // (nothing of this lambda is used after: it goes with the popup)
            for (GraphNode& n : self->ShownGraph().Nodes)
                if (n.Id == id)
                    n.Properties[property] = chosen;
            self->MarkEdited(true);
            self->m_DetailsDirty = true;
        };

        Ref<NoxUI::UIStackBox> box(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 6.0f));
        box->Padding = NoxUI::UIThickness::All(10.0f);
        box->FixedSize = { 300.0f, 0.0f };
        const Skeleton* skeleton = m_PreviewSkeleton != 0 ? AssetManager::FindLoadedAsset<Skeleton>(m_PreviewSkeleton) : nullptr;
        if (m_PreviewSkeleton != 0 && !skeleton)
            AssetManager::RequestAsset(m_PreviewSkeleton);
        if (!skeleton)
        {
            box->AddChild(MutedLabel(m_PreviewSkeleton != 0 ? "The preview skeleton is loading: open the picker again in a moment."
                                                             : "No preview mesh: pick the skeletal mesh this graph animates to list its bones, or type the bone's name below."));
            InspectorCardBuilder slot{ box, {}, ctx };
            slot.AddAssetSlot("Preview Mesh", m_PreviewMesh, { AssetType::Mesh }, [this, nodeId, key, at](AssetHandle mesh)
            {
                SetPreviewMesh(mesh);
                DeferPopup([this, nodeId, key, at]() { ShowBonePicker(nodeId, key, at); }); // (next frame: this slot goes with the popup)
            });
        }
        else
        {
            const std::string skeletonName = AssetDisplayName(m_PreviewSkeleton);
            box->AddChild(MutedLabel("Bones of " + skeletonName));
            // The skin's joints and the nodes above them (a mesh node under the armature is not a bone), in tree order.
            std::unordered_set<const Node*> joints;
            for (const Skin* skin : skeleton->Skins)
                if (skin)
                    joints.insert(skin->Joints.begin(), skin->Joints.end());
            std::function<bool(const Node*)> holdsJoint = [&](const Node* n)
            {
                if (!n)
                    return false;
                if (joints.empty() || joints.contains(n))
                    return true;
                return std::any_of(n->Children.begin(), n->Children.end(), [&](const Node* child) { return holdsJoint(child); });
            };
            auto bones = std::make_shared<std::vector<std::pair<std::string, int>>>();
            std::function<void(const Node*, int)> walk = [&](const Node* n, int depth)
            {
                if (!n || !holdsJoint(n))
                    return;
                bones->emplace_back(n->Name, depth);
                for (const Node* child : n->Children)
                    walk(child, depth + 1);
            };
            for (const Node* root : skeleton->RootNodes)
                walk(root, 0);

            Ref<NoxUI::UITextInput> search(new NoxUI::UITextInput("", "Search bones..."));
            search->WidthMode = NoxUI::UISizeMode::Fill;
            box->AddChild(search);
            Ref<NoxUI::UIScrollView> scroll(new NoxUI::UIScrollView());
            scroll->WidthMode = NoxUI::UISizeMode::Fill;
            scroll->HeightMode = NoxUI::UISizeMode::Fixed;
            scroll->FixedSize = { 0.0f, 260.0f };
            box->AddChild(scroll);
            Ref<NoxUI::UIStackBox> list(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 1.0f));
            list->WidthMode = NoxUI::UISizeMode::Fill;
            scroll->AddChild(list);
            NoxUI::UIStackBox* listRaw = list.get();
            auto fill = [listRaw, bones, choose, current](const std::string& filterText)
            {
                listRaw->ClearChildren();
                std::string filter = filterText;
                std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                for (const auto& [name, depth] : *bones)
                {
                    std::string lower = name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!filter.empty() && lower.find(filter) == std::string::npos)
                        continue;
                    // Indented by depth while the whole tree shows; a flat list of matches while searching.
                    const std::string shown = (filter.empty() ? std::string(static_cast<size_t>(std::min(depth, 12)) * 2, ' ') : std::string()) + name;
                    Ref<NoxUI::UIButton> row(new NoxUI::UIButton(shown, [choose, name]() { choose(name); }));
                    row->WidthMode = NoxUI::UISizeMode::Fill;
                    if (name == current)
                        row->StyleVariant = "primary";
                    listRaw->AddChild(row);
                }
            };
            search->OnChanged = [fill](const std::string& text) { fill(text); };
            fill("");
        }
        // Or typed (Enter): a bone the preview skeleton does not have (another rig), or clearing it.
        Ref<NoxUI::UITextInput> typed(new NoxUI::UITextInput(current, "Bone name, Enter to set"));
        typed->WidthMode = NoxUI::UISizeMode::Fill;
        typed->OnSubmit = [choose](const std::string& text) { choose(text); };
        box->AddChild(typed);
        m_PopupOpen = NoxUI::UIPopup::Show(*ctx, at, box);
        m_PopupContext = ctx;
    }

    void NodeGraphEditor::FindPreviewMesh()
    {
        m_PreviewSearched = true;
        auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
        if (!assets || !m_Asset || m_Asset->Graph.Domain == kAudioGraphDomain || m_Asset->Graph.Domain == kMaterialGraphDomain || m_PreviewMesh != 0)
            return;
        // The first clip the graph plays (in it or a state under it): a skeletal mesh cooked from the same model, else one in its folder.
        AssetHandle clip = 0;
        std::function<void(const NodeGraph&)> findClip = [&](const NodeGraph& graph)
        {
            for (const GraphNode& node : graph.Nodes)
            {
                for (const auto& [name, value] : node.Properties)
                    if (const auto* handle = std::get_if<uint64_t>(&value); handle && clip == 0 && *handle != 0 &&
                        AssetManager::IsAssetHandleValid(*handle) && AssetManager::GetAssetType(*handle) == AssetType::AnimationSequence)
                        clip = *handle;
                for (const NodeSubGraph& state : node.SubGraphs)
                    findClip(state.Graph);
            }
        };
        findClip(m_Asset->Graph);
        if (clip == 0)
            return;
        const AssetMetadata& clipMetadata = assets->GetMetadata(clip);
        AssetHandle sameSource = 0, sameFolder = 0;
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
        {
            if (metadata.Type != AssetType::Mesh)
                continue;
            if (!clipMetadata.SourceFilePath.empty() && metadata.SourceFilePath == clipMetadata.SourceFilePath && sameSource == 0)
                sameSource = handle;
            if (metadata.FilePath.parent_path() == clipMetadata.FilePath.parent_path() && sameFolder == 0)
                sameFolder = handle;
        }
        SetPreviewMesh(sameSource != 0 ? sameSource : sameFolder);
    }

    void NodeGraphEditor::SetPreviewMesh(AssetHandle mesh)
    {
        m_PreviewMesh = mesh;
        m_PreviewSkeleton = 0;
        if (mesh != 0)
            if (auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr)
                m_PreviewSkeleton = assets->FindSkeletonOf(mesh);
        if (m_PreviewSkeleton != 0)
            AssetManager::RequestAsset(m_PreviewSkeleton); // (loaded for the bone list)
        m_DetailsDirty = true;
    }

    void NodeGraphEditor::RefreshCompileStatus()
    {
        m_CompileStatusDirty = false;
        if (!m_CompileStatus || !m_Asset)
            return;
        // Compiled here from the graph as it is on the canvas, so an error shows while editing.
        const MaterialGraphProgram program = CompileMaterialGraph(m_Asset->Graph);
        m_CompileStatus->UseColorOverride = true;
        if (!program.Valid)
        {
            m_CompileStatus->ColorOverride = NoxUI::ThemeColor("StatusError", { 1.0f, 0.45f, 0.40f, 1.0f });
            m_CompileStatus->Text = "Does not compile: " + program.Error;
            return;
        }
        static constexpr const char* kOutputNames[] = { "Base Color", "Metallic", "Roughness", "Normal", "Emissive", "Ambient Occlusion", "Opacity",
                                                        "Opacity Mask", "World Position Offset", "Custom Light", "Custom Ambient" };
        std::string outputs;
        for (uint32_t bit = 0; bit < std::size(kOutputNames); ++bit)
            if ((program.Outputs & (1u << bit)) != 0)
                outputs += std::string(outputs.empty() ? "" : ", ") + kOutputNames[bit];
        std::string parameters;
        for (const MaterialGraphParameter& parameter : program.Parameters)
            if (!parameter.Name.starts_with('#'))
                parameters += std::string(parameters.empty() ? "" : ", ") + parameter.Name;
        std::string text = "Compiles. Sets: " + (outputs.empty() ? std::string("nothing yet (connect the Material Output)") : outputs) +
                           ". Parameters: " + (parameters.empty() ? std::string("none") : parameters) + ".";
        // World Position Offset without a Max Displacement: culled by the mesh's own bounds while pushed out of them.
        const bool wpoWithoutBounds = (program.Outputs & MaterialGraphOutputWorldPositionOffset) != 0 && program.MaxDisplacement <= 0.0f;
        if (wpoWithoutBounds)
            text += "\nWorld Position Offset: set Max World Position Offset Displacement on the Material Output (how far it moves the surface, "
                    "world units), or the surface may be culled while pushed out of its bounds.";
        m_CompileStatus->ColorOverride = wpoWithoutBounds ? NoxUI::ThemeColor("StatusWarning", { 1.0f, 0.80f, 0.30f, 1.0f }) : NoxUI::ThemeColor("StatusGood", { 0.55f, 0.82f, 0.60f, 1.0f });
        m_CompileStatus->Text = text;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    NodeGraph& NodeGraphEditor::ShownGraph()
    {
        return m_Canvas && m_Canvas->GetGraph() ? *m_Canvas->GetGraph() : m_Asset->Graph;
    }

    void NodeGraphEditor::ResolvePath()
    {
        if (!m_Asset || !m_Canvas || !m_StateView)
            return;
        NodeGraph* graph = &m_Asset->Graph;
        GraphNode* machine = nullptr;
        std::vector<std::string> labels{ m_Title };
        for (size_t i = 0; i < m_Path.size(); ++i)
        {
            if (i % 2 == 0)
            {
                const auto found = std::find_if(graph->Nodes.begin(), graph->Nodes.end(), [&](const GraphNode& n) { return n.Id == m_Path[i]; });
                if (found == graph->Nodes.end() || !NodeTypeRegistry::Find(graph->Domain, found->TypeName) ||
                    !NodeTypeRegistry::Find(graph->Domain, found->TypeName)->OwnsSubGraphs)
                {
                    m_Path.resize(i);
                    break;
                }
                machine = &*found;
                labels.push_back(machine->TypeName);
            }
            else
            {
                const auto found = std::find_if(machine->SubGraphs.begin(), machine->SubGraphs.end(), [&](const NodeSubGraph& st) { return st.Id == m_Path[i]; });
                if (found == machine->SubGraphs.end())
                {
                    m_Path.resize(i);
                    break;
                }
                graph = &found->Graph;
                machine = nullptr;
                labels.push_back(found->Name.empty() ? std::string("State") : found->Name);
            }
        }
        if (m_Path.size() % 2 == 0)
            machine = nullptr;

        const bool moved = m_Path != m_ShownPath;
        m_ShownPath = m_Path;
        if (machine)
        {
            m_StateView->SetMachine(machine, m_Asset->Graph.Domain);
            m_StateView->Visible = true;
            m_Canvas->Visible = false;
        }
        else
        {
            m_StateView->SetMachine(nullptr, m_Asset->Graph.Domain);
            // Another place: shown fresh (framed); the same graph at a new address (undo): the view stays.
            if (moved)
                m_Canvas->SetGraph(graph);
            else if (m_Canvas->GetGraph() != graph)
                m_Canvas->RebindGraph(graph);
            m_StateView->Visible = false;
            m_Canvas->Visible = true;
        }
        if (moved || machine != m_ShownMachine)
            m_DetailsDirty = true;
        m_ShownMachine = machine;
        if (moved)
            ClosePopup(); // (a value popup belongs to the place it was opened in)
        if (labels != m_PathLabels)
        {
            m_PathLabels = std::move(labels);
            RebuildBreadcrumb();
        }
    }

    void NodeGraphEditor::RebuildBreadcrumb()
    {
        if (!m_Breadcrumb)
            return;
        m_Breadcrumb->ClearChildren();
        m_Breadcrumb->Visible = m_PathLabels.size() > 1;
        if (!m_Breadcrumb->Visible)
            return;
        // Up one level, then every level: a click goes back to it.
        // (every click here only changes the path: the next update shows it and builds this bar again, never mid-click)
        Ref<NoxUI::UIButton> up(new NoxUI::UIButton("Up", [this]() { if (!m_Path.empty()) m_Path.pop_back(); }));
        up->Tooltip = "Back to where this was opened from";
        up->Padding = NoxUI::UIThickness::Symmetric(8.0f, 2.0f);
        m_Breadcrumb->AddChild(up);
        for (size_t k = 0; k < m_PathLabels.size(); ++k)
        {
            if (k > 0)
            {
                Ref<NoxUI::UILabel> separator = MutedLabel(">", 13.0f);
                separator->WordWrap = false;
                separator->WidthMode = NoxUI::UISizeMode::Auto;
                separator->VAlign = NoxUI::UIAlignment::Center;
                m_Breadcrumb->AddChild(separator);
            }
            if (k + 1 == m_PathLabels.size())
            {
                Ref<NoxUI::UILabel> here(new NoxUI::UILabel(m_PathLabels[k], 13.5f));
                here->VAlign = NoxUI::UIAlignment::Center;
                m_Breadcrumb->AddChild(here);
                continue;
            }
            Ref<NoxUI::UIButton> level(new NoxUI::UIButton(m_PathLabels[k], [this, k]() { m_Path.resize(k); }));
            level->Padding = NoxUI::UIThickness::Symmetric(8.0f, 2.0f);
            m_Breadcrumb->AddChild(level);
        }
    }

    void NodeGraphEditor::BuildMachineDetails(GraphNode& machine)
    {
        Ref<NoxUI::UIStackBox> body(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 4.0f));
        body->WidthMode = NoxUI::UISizeMode::Fill;
        m_Details->AddChild(body);
        InspectorCardBuilder builder{ body, [this](bool) { MarkEdited(true); }, NoxUI::UIContext::GetCurrent() };
        auto edited = [this]() { MarkEdited(true); };
        const auto stateName = [&machine](uint32_t id) -> std::string
        {
            for (const NodeSubGraph& state : machine.SubGraphs)
                if (state.Id == id)
                    return state.Name.empty() ? std::string("State") : state.Name;
            return "?";
        };
        const int transitionIndex = m_StateView->SelectedTransition;
        const uint32_t stateId = m_StateView->SelectedState;

        if (transitionIndex >= 0 && transitionIndex < static_cast<int>(machine.Transitions.size()))
        {
            NodeTransition& transition = machine.Transitions[static_cast<size_t>(transitionIndex)];
            m_Details->AddChild(Ref<NoxUI::UILabel>(new NoxUI::UILabel("Transition: " + stateName(transition.FromState) + " -> " + stateName(transition.ToState), 14.5f)));
            builder.AddFloat("Blend Time (s)", &transition.Duration, 0.01f, 0.0f, 10.0f, "{:.2f}", edited, "How long the two states blend");
            builder.AddBool("Ease In/Out", &transition.EaseInOut, edited, "Smooth start and end of the blend (off: linear)");
            builder.AddBool("At End of State", &transition.AtStateEnd, edited,
                            "Also waits for the state's animation to end (Unity's Has Exit Time): the blend starts Blend Time before its clips end. "
                            "With no rules below it fires at the end alone (Jump Start -> Jump Loop). Uncheck Loop on those clips.");
            builder.AddSubheader(transition.AtStateEnd ? "Rules (all must hold; none: at the end alone)" : "Rules (all must hold)",
                                 "Parameter, comparison, value: set by the game (Animator.SetFloat ...)");
            static const std::vector<std::string> kCompareNames{ "==", "!=", ">", ">=", "<", "<=", "is true", "is false" };
            std::vector<std::string> parameterNames;
            for (const GraphParameter& parameter : m_Asset->Graph.Parameters)
                parameterNames.push_back(parameter.Name);
            const size_t machineIndex = static_cast<size_t>(transitionIndex);
            for (size_t r = 0; r < transition.Rules.size(); ++r)
            {
                TransitionRule& rule = transition.Rules[r];
                const auto selected = std::find(parameterNames.begin(), parameterNames.end(), rule.Parameter);
                builder.AddEnum("Rule " + std::to_string(r + 1), parameterNames.empty() ? std::vector<std::string>{ "(no parameters)" } : parameterNames,
                                selected == parameterNames.end() ? -1 : static_cast<int>(selected - parameterNames.begin()),
                                [this, &machine, machineIndex, r, parameterNames](int index)
                {
                    if (index < 0 || index >= static_cast<int>(parameterNames.size()) || machineIndex >= machine.Transitions.size() ||
                        r >= machine.Transitions[machineIndex].Rules.size())
                        return;
                    TransitionRule& changed = machine.Transitions[machineIndex].Rules[r];
                    changed.Parameter = parameterNames[static_cast<size_t>(index)];
                    // The value takes the parameter's kind (a bool / trigger is tested with is true / is false).
                    for (const GraphParameter& parameter : m_Asset->Graph.Parameters)
                        if (parameter.Name == changed.Parameter)
                        {
                            if (parameter.IsTrigger || std::holds_alternative<bool>(parameter.DefaultValue))
                            {
                                if (changed.Compare != TransitionCompare::IsTrue && changed.Compare != TransitionCompare::IsFalse)
                                    changed.Compare = TransitionCompare::IsTrue;
                            }
                            else if (changed.Value.index() != parameter.DefaultValue.index())
                                changed.Value = std::holds_alternative<int32_t>(parameter.DefaultValue) ? NodeGraphValue(int32_t(0)) : NodeGraphValue(0.0f);
                        }
                    MarkEdited(true);
                    m_DetailsDirty = true;
                });
                builder.AddEnum("  Compare", kCompareNames, static_cast<int>(rule.Compare), [this, &rule](int index)
                {
                    rule.Compare = static_cast<TransitionCompare>(index);
                    MarkEdited(true);
                    m_DetailsDirty = true; // (is true / is false take no value)
                });
                if (rule.Compare != TransitionCompare::IsTrue && rule.Compare != TransitionCompare::IsFalse)
                {
                    if (auto* real = std::get_if<float>(&rule.Value))
                        builder.AddFloat("  Value", real, 0.01f, 0.0f, 0.0f, "{:.3f}", edited);
                    else if (auto* number = std::get_if<int32_t>(&rule.Value))
                        builder.AddInt32("  Value", number, 0, 0, edited);
                    else if (auto* flag = std::get_if<bool>(&rule.Value))
                        builder.AddBool("  Value", flag, edited);
                }
                Ref<NoxUI::UIButton> remove(new NoxUI::UIButton("Remove Rule", [this, &machine, machineIndex, r]()
                {
                    if (machineIndex < machine.Transitions.size() && r < machine.Transitions[machineIndex].Rules.size())
                        machine.Transitions[machineIndex].Rules.erase(machine.Transitions[machineIndex].Rules.begin() + static_cast<std::ptrdiff_t>(r));
                    MarkEdited(true);
                    m_DetailsDirty = true;
                }));
                remove->Padding = NoxUI::UIThickness::Symmetric(8.0f, 2.0f);
                builder.AddRow("", remove);
            }
            Ref<NoxUI::UIStackBox> row(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
            Ref<NoxUI::UIButton> addRule(new NoxUI::UIButton("+ Add Rule", [this, &machine, machineIndex]()
            {
                if (machineIndex < machine.Transitions.size())
                    machine.Transitions[machineIndex].Rules.push_back({ m_Asset->Graph.Parameters.empty() ? std::string() : m_Asset->Graph.Parameters.front().Name,
                                                                       TransitionCompare::Greater, 0.0f });
                MarkEdited(true);
                m_DetailsDirty = true;
            }));
            row->AddChild(addRule);
            Ref<NoxUI::UIButton> remove(new NoxUI::UIButton("Delete Transition", [this, &machine, machineIndex]()
            {
                if (machineIndex < machine.Transitions.size())
                    machine.Transitions.erase(machine.Transitions.begin() + static_cast<std::ptrdiff_t>(machineIndex));
                m_StateView->Select(0, -1);
                MarkEdited(true);
                m_DetailsDirty = true;
            }));
            row->AddChild(remove);
            builder.AddRow("", row);
            if (m_Asset->Graph.Parameters.empty())
                m_Details->AddChild(MutedLabel("No parameters yet: + Add on the left (a Float Speed, a Trigger Jump), then pick it in a rule."));
            return;
        }

        NodeSubGraph* state = nullptr;
        for (NodeSubGraph& candidate : machine.SubGraphs)
            if (candidate.Id == stateId)
                state = &candidate;
        if (state)
        {
            m_Details->AddChild(Ref<NoxUI::UILabel>(new NoxUI::UILabel("State", 14.5f)));
            builder.AddString("Name", &state->Name, "State", [this]() { MarkEdited(true); m_PathLabels.clear(); });
            const bool entry = StateMachineView::EntryState(machine) == state->Id;
            if (entry)
                builder.AddReadonly("Entry", "The machine starts in this state");
            Ref<NoxUI::UIStackBox> row(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
            const uint32_t id = state->Id;
            Ref<NoxUI::UIButton> open(new NoxUI::UIButton("Open State Graph", [this, id]() { m_Path.push_back(id); }));
            open->Tooltip = "What this state plays (double-click the state too)";
            row->AddChild(open);
            if (!entry)
            {
                Ref<NoxUI::UIButton> setEntry(new NoxUI::UIButton("Set as Entry State", [this, &machine, id]()
                {
                    StateMachineView::SetEntryState(machine, id);
                    MarkEdited(true);
                    m_DetailsDirty = true;
                }));
                row->AddChild(setEntry);
            }
            builder.AddRow("", row);
            Ref<NoxUI::UIButton> remove(new NoxUI::UIButton("Delete State", [this, &machine, id]()
            {
                StateMachineView::DeleteState(machine, id);
                m_StateView->Select(0, -1);
                MarkEdited(true);
                m_DetailsDirty = true;
            }));
            remove->Tooltip = "With its transitions (Ctrl+Z brings it back)";
            builder.AddRow("", remove);
            builder.AddReadonly("Its Graph", std::to_string(state->Graph.Nodes.size()) + " nodes");
            return;
        }
        m_Details->AddChild(MutedLabel(std::to_string(machine.SubGraphs.size()) + " states, " + std::to_string(machine.Transitions.size()) +
                                       " transitions.\n\nRight-click: Add State. Drag from a state's round handle onto another state: a transition "
                                       "(click its middle button to edit its rules). Double-click a state: what it plays. F frames the states."));
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    void NodeGraphEditor::ShowPreviewMeshMenu(const NoxUI::UIRect& anchor)
    {
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
        if (!ctx || !assets)
            return;
        // Skeletal meshes: meshes with a skeleton beside them (the rule the import links them by).
        std::vector<std::pair<std::string, AssetHandle>> meshes;
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
            if (metadata.Type == AssetType::Mesh && assets->FindSkeletonOf(handle) != 0)
                meshes.emplace_back(metadata.FilePath.generic_string(), handle);
        std::sort(meshes.begin(), meshes.end());
        std::vector<NoxUI::UIMenuItem> items;
        for (const auto& [path, handle] : meshes)
            items.push_back({ path, "", [this, handle]()
            {
                SetPreviewMesh(handle);
                m_AnimPreview.reset(); // (playheads from the start)
                m_PreviewDirty = true;
            }, false, true, handle == m_PreviewMesh });
        if (items.empty())
            items.push_back({ "No skeletal mesh in the project (import a skinned model)", "", {}, false, false, false, true });
        NoxUI::UIMenuPopup::ShowPopupMenu(*ctx, anchor, std::move(items));
    }

    void NodeGraphEditor::RebuildPreview()
    {
        m_PreviewDirty = false;
        m_PreviewImage = nullptr;
        if (!m_PreviewBox || !m_Asset)
            return;
        m_PreviewBox->ClearChildren();
        m_PreviewBox->AddChild(Ref<NoxUI::UILabel>(new NoxUI::UILabel("Preview", 15.0f)));
        NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent();
        const auto smallButton = [](const std::string& text, std::function<void()> onClick, const std::string& tooltip)
        {
            Ref<NoxUI::UIButton> b(new NoxUI::UIButton(text, std::move(onClick)));
            b->Padding = NoxUI::UIThickness::Symmetric(10.0f, 2.0f);
            b->Tooltip = tooltip;
            return b;
        };
        const bool audio = m_Asset->Graph.Domain == kAudioGraphDomain;
        Ref<NoxUI::UIStackBox> controls(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        controls->WidthMode = NoxUI::UISizeMode::Fill;
        m_PreviewBox->AddChild(controls);
        if (audio)
        {
            Ref<NoxUI::UIButton> play = smallButton("Play", {}, "Plays the graph as it is now; an edit is heard at once (a value change goes into the playing sound)");
            NoxUI::UIButton* playRaw = play.get();
            play->OnClick = [this]()
            {
                if (m_AudioWanted)
                    StopAudioPreview();
                else
                    m_AudioWanted = true;
            };
            NoxUI::AppendSync(*playRaw, [this, playRaw]()
            {
                const char* text = m_AudioWanted ? "Stop" : "Play";
                if (playRaw->GetText() != text)
                    playRaw->SetText(text);
                playRaw->StyleVariant = m_AudioWanted ? "primary" : "";
            });
            controls->AddChild(play);
            Ref<NoxUI::UILabel> status = MutedLabel("", 12.5f);
            status->VAlign = NoxUI::UIAlignment::Center;
            NoxUI::UILabel* statusRaw = status.get();
            NoxUI::AppendSync(*statusRaw, [this, statusRaw]()
            {
                statusRaw->Text = !m_AudioError.empty() ? "Cannot play: " + m_AudioError : !m_AudioProgram ? std::string("Loading...") : std::string();
            });
            controls->AddChild(status);
            // The voice's output level, left and right: bars over -60 .. 0 dB, red at 0 dB and above, the dB written on them.
            Ref<NoxUI::UILevelMeter> meter(new NoxUI::UILevelMeter(NoxUI::UIOrientation::Horizontal));
            meter->MinDb = -60.0f;
            meter->MaxDb = 0.0f;
            meter->LoudDb = meter->ClipDb = 0.0f;
            meter->HitTestMode = NoxUI::UIHitTestMode::None;
            meter->AutoTooltip = false;
            meter->HeightMode = NoxUI::UISizeMode::Fixed;
            meter->FixedSize = { 0.0f, 32.0f };
            meter->Source = [this]()
            {
                const glm::vec2 peak = m_AudioVoice != 0 ? AudioEngine::GetVoicePeak(m_AudioVoice) : glm::vec2(0.0f);
                return NoxUI::UILevelMeter::Values{ 0.0f, 0.0f, peak.x, peak.y };
            };
            m_PreviewBox->AddChild(meter);
        }
        else
        {
            const std::string meshName = m_PreviewMesh != 0 ? AssetDisplayName(m_PreviewMesh) : std::string("(pick a skeletal mesh)");
            Ref<NoxUI::UIButton> mesh = smallButton(meshName, {}, "The skeletal mesh the graph plays on (its skeleton lists the bones in bone pickers too)");
            NoxUI::UIButton* meshRaw = mesh.get();
            mesh->OnClick = [this, meshRaw]() { ShowPreviewMeshMenu(meshRaw->GetBounds()); };
            controls->AddChild(mesh);
            Ref<NoxUI::UIButton> play = smallButton("Pause", [this]() { m_AnimPlaying = !m_AnimPlaying; }, "Plays / holds the playheads");
            NoxUI::UIButton* playRaw = play.get();
            NoxUI::AppendSync(*playRaw, [this, playRaw]()
            {
                const char* text = m_AnimPlaying ? "Pause" : "Play";
                if (playRaw->GetText() != text)
                    playRaw->SetText(text);
            });
            controls->AddChild(play);
            controls->AddChild(smallButton("Restart", [this]() { m_AnimPreview.reset(); }, "Every playhead from the start, the parameters back to their defaults"));
            if (m_PreviewMesh == 0)
            {
                m_PreviewBox->AddChild(MutedLabel("Pick a skeletal mesh to see the graph play (found from the graph's clips when it plays one)."));
                return;
            }
            Ref<NoxUI::UIImage> image(new NoxUI::UIImage(~0u, { 280.0f, 280.0f }));
            image->WidthMode = NoxUI::UISizeMode::Fixed;
            image->HeightMode = NoxUI::UISizeMode::Fixed;
            image->FixedSize = { 280.0f, 280.0f };
            image->HAlign = NoxUI::UIAlignment::Center;
            image->Tooltip = "The graph's pose on the preview mesh, every frame";
            m_PreviewImage = image;
            m_PreviewBox->AddChild(image);
            Ref<NoxUI::UILabel> notify = MutedLabel("", 12.5f);
            NoxUI::UILabel* notifyRaw = notify.get();
            NoxUI::AppendSync(*notifyRaw, [this, notifyRaw]()
            {
                notifyRaw->Text = m_PreviewNotifyAge < 1.5f ? "Notify: " + m_PreviewNotify : std::string();
                notifyRaw->ColorOverride = NoxUI::ThemeColor("StatusWarning", { 1.0f, 0.75f, 0.25f, 1.0f });
            });
            m_PreviewBox->AddChild(notify);
        }

        // The parameters, as a script would set them while it plays (not saved: the graph's defaults are in Parameters).
        const std::vector<GraphParameter>& parameters = m_Asset->Graph.Parameters;
        if (parameters.empty())
            return;
        Ref<NoxUI::UIStackBox> rowsBox(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 3.0f));
        rowsBox->WidthMode = NoxUI::UISizeMode::Fill;
        m_PreviewBox->AddChild(rowsBox);
        InspectorCardBuilder rows{ rowsBox, {}, ctx };
        for (const GraphParameter& parameter : parameters)
        {
            const std::string name = parameter.Name;
            if (parameter.IsTrigger)
            {
                rows.AddRow(name, smallButton("Fire", [this, name]()
                {
                    if (m_Asset->Graph.Domain == kAudioGraphDomain)
                    {
                        if (m_AudioProgram && m_AudioVoice != 0)
                            AudioEngine::FireGraphParameter(m_AudioVoice, m_AudioProgram->FindParameter(name));
                    }
                    else if (m_AnimPreview)
                        m_AnimPreview->SetBool(name, true); // (true for one update)
                }, "Fires it once, as a script would"));
                continue;
            }
            // The value now: the preview's own, else the graph's default.
            const auto current = [this, name, audio]() -> NodeGraphValue
            {
                const GraphParameter* declared = nullptr;
                for (const GraphParameter& p : m_Asset->Graph.Parameters)
                    if (p.Name == name)
                        declared = &p;
                const NodeGraphValue fallback = declared ? declared->DefaultValue : NodeGraphValue(0.0f);
                if (audio)
                {
                    const auto set = m_AudioValues.find(name);
                    return set != m_AudioValues.end() && set->second.index() == fallback.index() ? set->second : fallback;
                }
                if (!m_AnimPreview)
                    return fallback;
                if (std::holds_alternative<bool>(fallback))
                    return m_AnimPreview->GetBool(name);
                if (std::holds_alternative<int32_t>(fallback))
                    return static_cast<int32_t>(m_AnimPreview->GetFloat(name));
                return m_AnimPreview->GetFloat(name);
            };
            const auto apply = [this, name, audio](const NodeGraphValue& value)
            {
                if (audio)
                {
                    m_AudioValues[name] = value;
                    if (m_AudioProgram && m_AudioVoice != 0)
                        AudioEngine::SetGraphParameter(m_AudioVoice, m_AudioProgram->FindParameter(name), NodeGraphValueToFloat(value));
                }
                else if (m_AnimPreview)
                {
                    if (const bool* flag = std::get_if<bool>(&value))
                        m_AnimPreview->SetBool(name, *flag);
                    else
                        m_AnimPreview->SetFloat(name, NodeGraphValueToFloat(value));
                }
            };
            if (std::holds_alternative<bool>(parameter.DefaultValue))
            {
                Ref<NoxUI::UICheckBox> box(new NoxUI::UICheckBox("", false, [apply](bool on) { apply(on); }));
                NoxUI::UICheckBox* boxRaw = box.get();
                NoxUI::AppendSync(*boxRaw, [boxRaw, current]() { const NodeGraphValue v = current(); boxRaw->Checked = std::holds_alternative<bool>(v) && std::get<bool>(v); });
                rows.AddRow(name, box);
            }
            else
            {
                const bool isInt = std::holds_alternative<int32_t>(parameter.DefaultValue);
                Ref<NoxUI::UIDragScalar> drag(new NoxUI::UIDragScalar(0.0f, isInt ? 0.1f : 0.01f, [apply, isInt](float v)
                {
                    apply(isInt ? NodeGraphValue(static_cast<int32_t>(std::round(v))) : NodeGraphValue(v));
                }));
                drag->Format = isInt ? "{:.0f}" : "{:.3f}";
                NoxUI::UIDragScalar* dragRaw = drag.get();
                NoxUI::AppendSync(*dragRaw, [dragRaw, current]() { if (!dragRaw->IsInteracting()) dragRaw->Value = NodeGraphValueToFloat(current()); });
                rows.AddRow(name, drag);
            }
            if (audio)
            {
                // Changed here: Reset goes back to following the graph's default.
                Ref<NoxUI::UIButton> reset = smallButton("Reset", [this, name]()
                {
                    m_AudioValues.erase(name);
                    if (m_AudioProgram && m_AudioVoice != 0)
                    {
                        const int32_t index = m_AudioProgram->FindParameter(name);
                        for (const GraphParameter& p : m_Asset->Graph.Parameters)
                            if (p.Name == name)
                                AudioEngine::SetGraphParameter(m_AudioVoice, index, NodeGraphValueToFloat(p.DefaultValue));
                    }
                }, "Changed here for the preview only: back to the default set in Parameters");
                NoxUI::UIButton* resetRaw = reset.get();
                NoxUI::AppendSync(*resetRaw, [this, resetRaw, name]() { resetRaw->Visible = m_AudioValues.contains(name); });
                rows.AddRow("", reset);
            }
        }
    }

    void NodeGraphEditor::UpdateAnimationPreview()
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = m_PreviewTick.time_since_epoch().count() == 0 ? 0.0f : std::min(std::chrono::duration<float>(now - m_PreviewTick).count(), 0.1f);
        m_PreviewTick = now;
        m_PreviewNotifyAge += dt;
        auto* image = static_cast<NoxUI::UIImage*>(m_PreviewImage.get());
        if (!image || m_PreviewMesh == 0)
            return;
        if (!m_AnimPreview)
            m_AnimPreview = std::make_unique<AnimationGraphInstance>();
        // One evaluation a frame (the graph as edited: Recompile after each change), its pose on the mesh.
        std::vector<AssetHandle> missing;
        m_AnimPreview->Sync(m_Handle, missing);
        for (AssetHandle asset : missing)
            AssetManager::RequestAsset(asset);
        const Skeleton* skeleton = m_PreviewSkeleton != 0 && AssetManager::RequestAsset(m_PreviewSkeleton) == AssetState::Ready
            ? AssetManager::FindLoadedAsset<Skeleton>(m_PreviewSkeleton) : nullptr;
        m_PreviewBones.clear();
        if (skeleton && !skeleton->AllNodes.empty())
            if (const AnimPose* pose = m_AnimPreview->Evaluate(dt, m_AnimPlaying, *skeleton))
                ApplyPoseToSkeleton(*pose, *skeleton, m_PreviewBones);
        if (!m_AnimPreview->GetFiredNotifies().empty())
        {
            m_PreviewNotify = m_AnimPreview->GetFiredNotifies().back().Name;
            m_PreviewNotifyAge = 0.0f;
        }
        Texture2D* texture = AssetPreviewRenderer::Get().GetPosePreview(m_PreviewMesh, m_PreviewBones, { m_Handle, m_PreviewSkeleton }, 280);
        image->TextureId = texture ? texture->GetDescriptorIndexSlot() : ~0u;
    }

    void NodeGraphEditor::StopAudioPreview()
    {
        if (m_AudioVoice != 0)
            AudioEngine::Stop(m_AudioVoice, 0.05f);
        m_AudioVoice = 0;
        m_AudioWanted = false;
        m_AudioProgram = nullptr;
    }

    void NodeGraphEditor::UpdateAudioPreview()
    {
        // Only while it plays or is asked to: the program is built from the graph as edited (the cache hands out a new one per edit).
        if (!m_AudioWanted && m_AudioVoice == 0)
            return;
        m_AudioError.clear();
        const std::shared_ptr<const AudioGraphProgram> program = GetAudioGraphProgram(m_Handle, &m_AudioError);
        if (m_AudioVoice != 0 && !AudioEngine::IsPlaying(m_AudioVoice))
        {
            // It ended by itself (its Output's On Finished): a one-shot graph played once.
            m_AudioVoice = 0;
            m_AudioWanted = false;
            return;
        }
        // An edit that changed only values (a frequency, a gain) goes into the playing sound, as MetaSounds does; a parameter's default
        // changed in Parameters follows too, unless set here.
        std::vector<std::pair<uint32_t, float>> literalChanges;
        if (m_AudioVoice != 0 && program && m_AudioProgram && program != m_AudioProgram && m_AudioProgram->LiteralChanges(*program, literalChanges))
        {
            for (const auto& [slot, value] : literalChanges)
                AudioEngine::SetGraphConstant(m_AudioVoice, slot, value);
            const auto& before = m_AudioProgram->GetParameters();
            const auto& after = program->GetParameters();
            for (size_t i = 0; i < after.size() && i < before.size(); ++i)
                if (after[i].Default != before[i].Default && !m_AudioValues.contains(after[i].Name))
                    AudioEngine::SetGraphParameter(m_AudioVoice, static_cast<int32_t>(i), after[i].Default);
            m_AudioProgram = program;
        }
        if (m_AudioWanted && program && (m_AudioVoice == 0 || program != m_AudioProgram))
        {
            // Any other edit: the new graph replaces the playing one (crossfaded), with the values set here.
            if (m_AudioVoice != 0)
                AudioEngine::Stop(m_AudioVoice, 0.02f);
            AudioPlayDesc desc;
            desc.Graph = program;
            desc.GraphParameters = m_AudioValues;
            desc.Bus = NoxAudio::kAudioBusMaster;
            desc.Group = NoxAudio::AudioVoiceGroup::Editor;
            desc.FadeIn = m_AudioVoice != 0 ? 0.02f : 0.0f;
            m_AudioVoice = AudioEngine::Play(desc);
            m_AudioProgram = program;
            if (m_AudioVoice == 0)
                m_AudioWanted = false;
        }
    }
}
