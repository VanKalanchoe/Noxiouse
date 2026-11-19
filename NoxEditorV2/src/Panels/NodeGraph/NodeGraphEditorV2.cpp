#include "NodeGraphEditorV2.h"

#include <algorithm>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "NodeGraphCanvasV2.h"
#include "StateMachineViewV2.h"
#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Audio/AudioGraph.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIPopup.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

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

        // A curve (x -> y) to see and shape: key dots dragged (kept between their neighbours), a double-click adds a key there, a
        // right click on a key removes it. The range follows the keys (held still while one is dragged).
        class CurveEditView final : public UI::UIElement
        {
        public:
            CurveEditView(std::function<NodeGraphCurve*()> curve, std::function<void()> changing, std::function<void()> reshaped)
                : m_Curve(std::move(curve)), m_Changing(std::move(changing)), m_Reshaped(std::move(reshaped))
            {
                HitTestMode = UI::UIHitTestMode::Self;
                WidthMode = UI::UISizeMode::Fill;
                HeightMode = UI::UISizeMode::Fixed;
                FixedSize = { 0.0f, 120.0f };
                Tooltip = "Drag a key; double-click to add one; right-click a key to remove it.";
            }
            bool OnPointerDown(const UI::UIPointerEvent& e) override
            {
                NodeGraphCurve* curve = m_Curve();
                if (!curve)
                    return true;
                if (UI::UIContext* ctx = UI::UIContext::GetCurrent())
                    ctx->CancelPendingDrag();
                const int key = KeyAt(*curve, e.Position);
                if (e.Button == 3 && key >= 0)
                {
                    curve->Keys.erase(curve->Keys.begin() + key);
                    m_Reshaped();
                    return true;
                }
                if (e.Button != 1)
                    return false;
                const auto now = std::chrono::steady_clock::now();
                const bool quick = now - m_LastClick < std::chrono::milliseconds(350);
                m_LastClick = now;
                if (key >= 0)
                {
                    m_Dragged = key;
                    return true;
                }
                if (quick)
                {
                    const glm::vec2 point = ToCurve(e.Position);
                    const auto at = std::upper_bound(curve->Keys.begin(), curve->Keys.end(), point.x, [](float x, const glm::vec2& k) { return x < k.x; });
                    curve->Keys.insert(at, point);
                    m_Reshaped();
                }
                return true;
            }
            bool OnPointerMove(const UI::UIPointerEvent& e) override
            {
                NodeGraphCurve* curve = m_Curve();
                if (m_Dragged < 0 || !curve || m_Dragged >= static_cast<int>(curve->Keys.size()))
                    return true;
                glm::vec2 point = ToCurve(e.Position);
                const size_t i = static_cast<size_t>(m_Dragged);
                if (i > 0)
                    point.x = std::max(point.x, curve->Keys[i - 1].x);
                if (i + 1 < curve->Keys.size())
                    point.x = std::min(point.x, curve->Keys[i + 1].x);
                curve->Keys[i] = point;
                m_Changing();
                return true;
            }
            bool OnPointerUp(const UI::UIPointerEvent&) override
            {
                m_Dragged = -1;
                return true;
            }

        protected:
            void OnPaint(UI::UIDrawList& d, const UI::UITheme& theme) override
            {
                const UI::UIRect b = GetBounds();
                d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f }), glm::vec4(4.0f), 1.0f,
                                 theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f }));
                NodeGraphCurve* curve = m_Curve();
                if (!curve)
                    return;
                if (m_Dragged < 0)
                {
                    // The keys' range with a margin (one key, or all at one height: a unit around them).
                    glm::vec2 low(0.0f), high(1.0f);
                    if (!curve->Keys.empty())
                    {
                        low = high = curve->Keys.front();
                        for (const glm::vec2& key : curve->Keys)
                        {
                            low = glm::min(low, key);
                            high = glm::max(high, key);
                        }
                    }
                    for (int axis = 0; axis < 2; ++axis)
                        if (high[axis] - low[axis] < 1e-4f)
                        {
                            low[axis] -= 0.5f;
                            high[axis] += 0.5f;
                        }
                    const glm::vec2 margin = (high - low) * 0.12f;
                    m_Low = low - margin;
                    m_High = high + margin;
                }
                d.PushClipRect(b.Min, b.Max);
                const UI::UIColor axisColor{ 1.0f, 1.0f, 1.0f, 0.10f };
                if (m_Low.y < 0.0f && m_High.y > 0.0f)
                {
                    const float y = ToScreen({ 0.0f, 0.0f }).y;
                    d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + 1.0f }, axisColor);
                }
                if (m_Low.x < 0.0f && m_High.x > 0.0f)
                {
                    const float x = ToScreen({ 0.0f, 0.0f }).x;
                    d.AddRoundedRect({ x, b.Min.y }, { x + 1.0f, b.Max.y }, axisColor);
                }
                const UI::UIColor line = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
                constexpr int kSamples = 96;
                UI::UIVec2 previous = ToScreen({ m_Low.x, curve->Evaluate(m_Low.x) });
                for (int s = 1; s <= kSamples; ++s)
                {
                    const float x = m_Low.x + (m_High.x - m_Low.x) * static_cast<float>(s) / kSamples;
                    const UI::UIVec2 point = ToScreen({ x, curve->Evaluate(x) });
                    d.AddBezier(previous, previous, point, point, 2.0f, line);
                    previous = point;
                }
                for (size_t i = 0; i < curve->Keys.size(); ++i)
                {
                    const UI::UIVec2 p = ToScreen(curve->Keys[i]);
                    const float r = static_cast<int>(i) == m_Dragged ? 5.5f : 4.5f;
                    d.AddRoundedRect(p - UI::UIVec2{ r }, p + UI::UIVec2{ r }, { 1.0f, 0.82f, 0.35f, 1.0f }, glm::vec4(r), 1.0f, { 0.05f, 0.05f, 0.05f, 1.0f });
                }
                d.AddText(std::format("x {:.3g} .. {:.3g}   y {:.3g} .. {:.3g}", m_Low.x, m_High.x, m_Low.y, m_High.y), nullptr, { b.Min.x + 6.0f, b.Min.y + 4.0f },
                          11.0f, theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f }));
                d.PopClipRect();
            }

        private:
            UI::UIVec2 ToScreen(const glm::vec2& p) const
            {
                const UI::UIRect b = GetBounds();
                const glm::vec2 t = (p - m_Low) / glm::max(m_High - m_Low, glm::vec2(1e-6f));
                return { b.Min.x + t.x * b.Width(), b.Max.y - t.y * b.Height() };
            }
            glm::vec2 ToCurve(const UI::UIVec2& p) const
            {
                const UI::UIRect b = GetBounds();
                const glm::vec2 t{ (p.x - b.Min.x) / std::max(b.Width(), 1.0f), (b.Max.y - p.y) / std::max(b.Height(), 1.0f) };
                return m_Low + glm::clamp(t, 0.0f, 1.0f) * (m_High - m_Low);
            }
            int KeyAt(const NodeGraphCurve& curve, const UI::UIVec2& p) const
            {
                for (size_t i = 0; i < curve.Keys.size(); ++i)
                    if (glm::length(ToScreen(curve.Keys[i]) - p) <= 7.0f)
                        return static_cast<int>(i);
                return -1;
            }

            std::function<NodeGraphCurve*()> m_Curve;
            std::function<void()> m_Changing, m_Reshaped;
            int m_Dragged = -1;
            glm::vec2 m_Low{ 0.0f }, m_High{ 1.0f };
            std::chrono::steady_clock::time_point m_LastClick{};
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
        if (m_Asset && m_Asset->Graph.Domain == kMaterialGraphDomain)
        {
            m_CompileStatus = MutedLabel("", 12.5f);
            m_CompileStatus->Margin = UI::UIThickness{ 8.0f, 0.0f, 8.0f, 4.0f };
            root->AddChild(m_CompileStatus);
            m_CompileStatusDirty = true;
        }

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
        m_Canvas->OnOpenNode = [this](uint32_t nodeId) { m_Path.push_back(nodeId); }; // (shown by the next update's ResolvePath)
        m_Canvas->OnEditValue = [this](uint32_t nodeId, const std::string& property, const UI::UIRect& at) { ShowValuePopup(nodeId, property, at); };
        m_Canvas->SetGraph(m_Asset ? &m_Asset->Graph : nullptr);

        Ref<UI::UIScrollView> right = column(300.0f);
        m_Details = Ref<UI::UIStackBox>(new UI::UIStackBox(UI::UIOrientation::Vertical, 6.0f));
        m_Details->WidthMode = UI::UISizeMode::Fill;
        m_Details->Padding = UI::UIThickness::All(8.0f);
        right->AddChild(m_Details);

        m_StateView = Ref<StateMachineViewV2>(new StateMachineViewV2());
        m_StateView->Visible = false;
        m_StateView->OnEdited = [this](bool structural) { OnCanvasEdited(structural); };
        m_StateView->OnSelectionChanged = [this]() { m_DetailsDirty = true; };
        m_StateView->OnOpenState = [this](uint32_t state) { m_Path.push_back(state); };
        m_StateView->FirstParameter = [this]() { return m_Asset && !m_Asset->Graph.Parameters.empty() ? m_Asset->Graph.Parameters.front().Name : std::string(); };

        // The middle: where the window is in the graph (shown below the top), then the canvas or the states view.
        m_Breadcrumb = Ref<UI::UIStackBox>(new UI::UIStackBox(UI::UIOrientation::Horizontal, 4.0f));
        m_Breadcrumb->WidthMode = UI::UISizeMode::Fill;
        m_Breadcrumb->Padding = UI::UIThickness{ 6.0f, 3.0f, 6.0f, 3.0f };
        m_Breadcrumb->Visible = false;
        Ref<UI::UIStackBox> middle(new UI::UIStackBox(UI::UIOrientation::Vertical, 0.0f));
        middle->WidthMode = UI::UISizeMode::Fill;
        middle->HeightMode = UI::UISizeMode::Fill;
        middle->AddChild(m_Breadcrumb);
        middle->AddChild(m_Canvas);
        middle->AddChild(m_StateView);

        Ref<UI::UIStackBox> body(new UI::UIStackBox(UI::UIOrientation::Horizontal, 0.0f));
        body->WidthMode = UI::UISizeMode::Fill;
        body->HeightMode = UI::UISizeMode::Fill;
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
        ResolvePath();
        if (!m_PreviewSearched)
            FindPreviewMesh();
        if (m_DetailsDirty)
            RebuildDetails();
        for (const auto& sync : m_DetailSyncs)
            sync();
        if (m_DeferredPopup)
        {
            std::function<void()> show = std::move(m_DeferredPopup);
            m_DeferredPopup = nullptr;
            show();
        }
        if (m_PopupSyncs && m_PopupOpen && *m_PopupOpen)
            for (const auto& sync : *m_PopupSyncs)
                sync();
        else
            m_PopupSyncs.reset();
        if (m_CompileStatusDirty)
            RefreshCompileStatus();
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
        {
            m_Asset->Recompile();
            m_CompileStatusDirty = true;
        }
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

    bool NodeGraphEditorV2::ApplyExternalEdit(NodeGraph replacement)
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
            // Dragged (by its background or labels) onto the canvas: a Get node of it there.
            card->Tooltip = "Drag onto the graph for a node that reads it";
            card->OnDragStart = [this, index]() -> std::optional<UI::UIDragPayload>
            {
                if (!m_Asset || index >= m_Asset->Graph.Parameters.size())
                    return std::nullopt;
                UI::UIDragPayload payload;
                payload.Type = NodeGraphCanvasV2::kParameterDragType;
                payload.Text = m_Asset->Graph.Parameters[index].Name;
                return payload;
            };
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
            Ref<UI::UIStackBox> row(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
            Ref<UI::UIButton> addPin(new UI::UIButton("+ Pin", [this, nodeId, type, variable]()
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
                Ref<UI::UIButton> removePin(new UI::UIButton("- Pin", [this, nodeId, type, variable, inputCount = static_cast<uint32_t>(pins.Inputs.size())]()
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

    void NodeGraphEditorV2::AddValueEditor(InspectorCardBuilder& builder, uint32_t nodeId, const std::string& key, const std::string& label,
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
            Ref<UI::UIButton> pick(new UI::UIButton(text->empty() ? std::string("(pick a bone)") : *text + (known ? "" : "  (not in the skeleton)")));
            pick->WidthMode = UI::UISizeMode::Fill;
            pick->Tooltip = skeleton ? "Pick from the preview skeleton's bones, or type a name" : "Type the bone's name, or pick a preview mesh to list its bones";
            UI::UIButton* raw = pick.get();
            pick->OnClick = [this, nodeId, key, raw]() { ShowBonePicker(nodeId, key, raw->GetBounds()); };
            // (the button follows the value: a pick or an undo changes it)
            builder.Syncs.push_back([this, nodeId, key, raw]()
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
            Ref<CurveEditView> view(new CurveEditView(curveOf, [this]() { MarkEdited(true); }, [this, rebuild]() { MarkEdited(true); if (rebuild) rebuild(); }));
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
            Ref<UI::UIStackBox> row(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
            Ref<UI::UIButton> addKey(new UI::UIButton("+ Key", [this, curveOf, rebuild]()
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
                Ref<UI::UIButton> removeKey(new UI::UIButton("- Key", [this, curveOf, rebuild]()
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

    void NodeGraphEditorV2::ClosePopup()
    {
        m_DeferredPopup = nullptr;
        if (m_PopupOpen && *m_PopupOpen)
            if (UI::UIContext* ctx = UI::UIContext::GetCurrent())
                ctx->ClearOverlay();
        m_PopupSyncs.reset();
        m_PopupOpen.reset();
    }

    void NodeGraphEditorV2::ShowValuePopup(uint32_t nodeId, const std::string& key, const UI::UIRect& at)
    {
        UI::UIContext* ctx = UI::UIContext::GetCurrent();
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
        Ref<UI::UIStackBox> box(new UI::UIStackBox(UI::UIOrientation::Vertical, 4.0f));
        box->Padding = UI::UIThickness::All(10.0f);
        box->FixedSize = { 340.0f, 0.0f };
        auto syncs = std::make_shared<std::vector<std::function<void()>>>();
        InspectorCardBuilder builder{ box, *syncs, [this](bool) { MarkEdited(true); }, ctx };
        // (built again next frame, not from inside the click that changed its shape: that would destroy the button running it)
        AddValueEditor(builder, nodeId, key, key, [this, nodeId, key, at]() { m_DeferredPopup = [this, nodeId, key, at]() { ShowValuePopup(nodeId, key, at); }; });
        for (const auto& sync : *syncs)
            sync();
        m_PopupSyncs = syncs;
        m_PopupOpen = UI::UIPopup::Show(*ctx, at, box);
        m_DetailsDirty = true; // (Details shows the same value: built again so both point at it)
    }

    void NodeGraphEditorV2::ShowBonePicker(uint32_t nodeId, const std::string& key, const UI::UIRect& at)
    {
        UI::UIContext* ctx = UI::UIContext::GetCurrent();
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
            UI::UIContext* context = ctx;
            NodeGraphEditorV2* self = this;
            context->ClearOverlay(); // (nothing of this lambda is used after: it goes with the popup)
            for (GraphNode& n : self->ShownGraph().Nodes)
                if (n.Id == id)
                    n.Properties[property] = chosen;
            self->MarkEdited(true);
            self->m_DetailsDirty = true;
        };

        Ref<UI::UIStackBox> box(new UI::UIStackBox(UI::UIOrientation::Vertical, 6.0f));
        box->Padding = UI::UIThickness::All(10.0f);
        box->FixedSize = { 300.0f, 0.0f };
        const Skeleton* skeleton = m_PreviewSkeleton != 0 ? AssetManager::FindLoadedAsset<Skeleton>(m_PreviewSkeleton) : nullptr;
        if (m_PreviewSkeleton != 0 && !skeleton)
            AssetManager::RequestAsset(m_PreviewSkeleton);
        auto syncs = std::make_shared<std::vector<std::function<void()>>>();
        if (!skeleton)
        {
            box->AddChild(MutedLabel(m_PreviewSkeleton != 0 ? "The preview skeleton is loading: open the picker again in a moment."
                                                             : "No preview mesh: pick the skeletal mesh this graph animates to list its bones, or type the bone's name below."));
            InspectorCardBuilder slot{ box, *syncs, {}, ctx };
            slot.AddAssetSlot("Preview Mesh", m_PreviewMesh, { AssetType::Mesh }, [this, nodeId, key, at](AssetHandle mesh)
            {
                SetPreviewMesh(mesh);
                m_DeferredPopup = [this, nodeId, key, at]() { ShowBonePicker(nodeId, key, at); }; // (next frame: this slot goes with the popup)
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

            Ref<UI::UITextInput> search(new UI::UITextInput("", "Search bones..."));
            search->WidthMode = UI::UISizeMode::Fill;
            box->AddChild(search);
            Ref<UI::UIScrollView> scroll(new UI::UIScrollView());
            scroll->WidthMode = UI::UISizeMode::Fill;
            scroll->HeightMode = UI::UISizeMode::Fixed;
            scroll->FixedSize = { 0.0f, 260.0f };
            box->AddChild(scroll);
            Ref<UI::UIStackBox> list(new UI::UIStackBox(UI::UIOrientation::Vertical, 1.0f));
            list->WidthMode = UI::UISizeMode::Fill;
            scroll->AddChild(list);
            UI::UIStackBox* listRaw = list.get();
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
                    Ref<UI::UIButton> row(new UI::UIButton(shown, [choose, name]() { choose(name); }));
                    row->WidthMode = UI::UISizeMode::Fill;
                    if (name == current)
                        row->StyleVariant = "primary";
                    listRaw->AddChild(row);
                }
            };
            search->OnChanged = [fill](const std::string& text) { fill(text); };
            fill("");
        }
        // Or typed (Enter): a bone the preview skeleton does not have (another rig), or clearing it.
        Ref<UI::UITextInput> typed(new UI::UITextInput(current, "Bone name, Enter to set"));
        typed->WidthMode = UI::UISizeMode::Fill;
        typed->OnSubmit = [choose](const std::string& text) { choose(text); };
        box->AddChild(typed);
        m_PopupSyncs = syncs;
        m_PopupOpen = UI::UIPopup::Show(*ctx, at, box);
    }

    void NodeGraphEditorV2::FindPreviewMesh()
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

    void NodeGraphEditorV2::SetPreviewMesh(AssetHandle mesh)
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

    void NodeGraphEditorV2::RefreshCompileStatus()
    {
        m_CompileStatusDirty = false;
        if (!m_CompileStatus || !m_Asset)
            return;
        // Compiled here from the graph as it is on the canvas, so an error shows while editing.
        const MaterialGraphProgram program = CompileMaterialGraph(m_Asset->Graph);
        m_CompileStatus->UseColorOverride = true;
        if (!program.Valid)
        {
            m_CompileStatus->ColorOverride = { 1.0f, 0.45f, 0.40f, 1.0f };
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
        m_CompileStatus->ColorOverride = wpoWithoutBounds ? UI::UIColor{ 1.0f, 0.80f, 0.30f, 1.0f } : UI::UIColor{ 0.55f, 0.82f, 0.60f, 1.0f };
        m_CompileStatus->Text = text;
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    NodeGraph& NodeGraphEditorV2::ShownGraph()
    {
        return m_Canvas && m_Canvas->GetGraph() ? *m_Canvas->GetGraph() : m_Asset->Graph;
    }

    void NodeGraphEditorV2::ResolvePath()
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

    void NodeGraphEditorV2::RebuildBreadcrumb()
    {
        if (!m_Breadcrumb)
            return;
        m_Breadcrumb->ClearChildren();
        m_Breadcrumb->Visible = m_PathLabels.size() > 1;
        if (!m_Breadcrumb->Visible)
            return;
        // Up one level, then every level: a click goes back to it.
        // (every click here only changes the path: the next update shows it and builds this bar again, never mid-click)
        Ref<UI::UIButton> up(new UI::UIButton("Up", [this]() { if (!m_Path.empty()) m_Path.pop_back(); }));
        up->Tooltip = "Back to where this was opened from";
        up->Padding = UI::UIThickness::Symmetric(8.0f, 2.0f);
        m_Breadcrumb->AddChild(up);
        for (size_t k = 0; k < m_PathLabels.size(); ++k)
        {
            if (k > 0)
            {
                Ref<UI::UILabel> separator = MutedLabel(">", 13.0f);
                separator->WordWrap = false;
                separator->WidthMode = UI::UISizeMode::Auto;
                separator->VAlign = UI::UIAlignment::Center;
                m_Breadcrumb->AddChild(separator);
            }
            if (k + 1 == m_PathLabels.size())
            {
                Ref<UI::UILabel> here(new UI::UILabel(m_PathLabels[k], 13.5f));
                here->VAlign = UI::UIAlignment::Center;
                m_Breadcrumb->AddChild(here);
                continue;
            }
            Ref<UI::UIButton> level(new UI::UIButton(m_PathLabels[k], [this, k]() { m_Path.resize(k); }));
            level->Padding = UI::UIThickness::Symmetric(8.0f, 2.0f);
            m_Breadcrumb->AddChild(level);
        }
    }

    void NodeGraphEditorV2::BuildMachineDetails(GraphNode& machine)
    {
        Ref<UI::UIStackBox> body(new UI::UIStackBox(UI::UIOrientation::Vertical, 4.0f));
        body->WidthMode = UI::UISizeMode::Fill;
        m_Details->AddChild(body);
        InspectorCardBuilder builder{ body, m_DetailSyncs, [this](bool) { MarkEdited(true); }, UI::UIContext::GetCurrent() };
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
            m_Details->AddChild(Ref<UI::UILabel>(new UI::UILabel("Transition: " + stateName(transition.FromState) + " -> " + stateName(transition.ToState), 14.5f)));
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
                Ref<UI::UIButton> remove(new UI::UIButton("Remove Rule", [this, &machine, machineIndex, r]()
                {
                    if (machineIndex < machine.Transitions.size() && r < machine.Transitions[machineIndex].Rules.size())
                        machine.Transitions[machineIndex].Rules.erase(machine.Transitions[machineIndex].Rules.begin() + static_cast<std::ptrdiff_t>(r));
                    MarkEdited(true);
                    m_DetailsDirty = true;
                }));
                remove->Padding = UI::UIThickness::Symmetric(8.0f, 2.0f);
                builder.AddRow("", remove);
            }
            Ref<UI::UIStackBox> row(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
            Ref<UI::UIButton> addRule(new UI::UIButton("+ Add Rule", [this, &machine, machineIndex]()
            {
                if (machineIndex < machine.Transitions.size())
                    machine.Transitions[machineIndex].Rules.push_back({ m_Asset->Graph.Parameters.empty() ? std::string() : m_Asset->Graph.Parameters.front().Name,
                                                                       TransitionCompare::Greater, 0.0f });
                MarkEdited(true);
                m_DetailsDirty = true;
            }));
            row->AddChild(addRule);
            Ref<UI::UIButton> remove(new UI::UIButton("Delete Transition", [this, &machine, machineIndex]()
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
            m_Details->AddChild(Ref<UI::UILabel>(new UI::UILabel("State", 14.5f)));
            builder.AddString("Name", &state->Name, "State", [this]() { MarkEdited(true); m_PathLabels.clear(); });
            const bool entry = StateMachineViewV2::EntryState(machine) == state->Id;
            if (entry)
                builder.AddReadonly("Entry", "The machine starts in this state");
            Ref<UI::UIStackBox> row(new UI::UIStackBox(UI::UIOrientation::Horizontal, 6.0f));
            const uint32_t id = state->Id;
            Ref<UI::UIButton> open(new UI::UIButton("Open State Graph", [this, id]() { m_Path.push_back(id); }));
            open->Tooltip = "What this state plays (double-click the state too)";
            row->AddChild(open);
            if (!entry)
            {
                Ref<UI::UIButton> setEntry(new UI::UIButton("Set as Entry State", [this, &machine, id]()
                {
                    StateMachineViewV2::SetEntryState(machine, id);
                    MarkEdited(true);
                    m_DetailsDirty = true;
                }));
                row->AddChild(setEntry);
            }
            builder.AddRow("", row);
            Ref<UI::UIButton> remove(new UI::UIButton("Delete State", [this, &machine, id]()
            {
                StateMachineViewV2::DeleteState(machine, id);
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
}
