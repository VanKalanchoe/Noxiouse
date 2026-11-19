#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/UI/Core/UITypes.h"
#include "Panels/AssetEditorV2.h"

namespace Nox
{
    namespace UI
    {
        class UIButton;
        class UIElement;
        class UILabel;
        class UIStackBox;
    }
    class NodeGraphCanvasV2;
    class StateMachineViewV2;
    struct GraphNode;
    struct InspectorCardBuilder;

    // One open graph (.nanimgraph / .naudiograph / .nmatgraph) in NoxEditorV2: toolbar, the graph's parameters on the left, the
    // canvas in the middle, the selected node's properties on the right (Unreal's Details panel).
    //
    // It edits the loaded NodeGraphAsset in place and recompiles it after each change (a playing scene picks the change up);
    // Save writes the file. History: the whole graph after each finished edit (a drag or a connection is one step; a value
    // dragged in Details is one step when the mouse button comes up), Ctrl+Z / Ctrl+Y.
    class NodeGraphEditorV2 final : public AssetEditorV2
    {
    public:
        explicit NodeGraphEditorV2(AssetHandle graph);
        ~NodeGraphEditorV2() override;

        Ref<UI::UIElement> BuildUI() override;
        // Once a frame: finished edits become history steps, the window's labels follow.
        void Update() override;
        // Writes the file (nothing when nothing changed); IsDirty() says whether it worked.
        void Save() override;
        // Drops the unsaved edits: the graph is the file's again (and recompiled). False when the file could not be read (the
        // edits stay, the window says why).
        bool RevertToDisk() override;
        // A whole graph from outside the window (MCP's set_graph): one undo step here, compiled, saved. False: not written.
        bool ApplyExternalEdit(NodeGraph replacement);

        AssetHandle GetHandle() const override { return m_Handle; }
        const std::string& GetName() const override { return m_Title; }
        const char* GetKind() const override { return "graph"; }
        std::string GetWindowTitle() const override { return "Graph: " + m_Title; }
        UI::UIVec2 GetDefaultSize() const override { return { 1180.0f, 700.0f }; }
        bool IsDirty() const override { return m_Dirty; }
        bool IsValid() const override { return m_Asset != nullptr; }

    private:
        void OnCanvasEdited(bool structural);
        void MarkEdited(bool structural);
        void CommitHistory();
        void Undo();
        void Redo();
        void RestoreGraph(NodeGraph graph);
        void RebuildDetails();
        void RebuildParameters();
        void RefreshToolbar();
        bool HandleKey(int keyCode);
        void FollowDiskChanges();
        // One value's editor (a node property or an input's value), in Details or in the popup beside a node: by what it holds and
        // what the node type declares (range, choices, asset kind, bone, curve, parameter). `rebuild`: what to build again when the
        // editor's shape changes (a curve key added).
        void AddValueEditor(InspectorCardBuilder& builder, uint32_t nodeId, const std::string& key, const std::string& label, std::function<void()> rebuild);
        // A value on the canvas clicked: its editor in a popup beside it.
        void ShowValuePopup(uint32_t nodeId, const std::string& key, const UI::UIRect& at);
        // A bone property: the preview skeleton's bones (searchable, by depth) or a name typed.
        void ShowBonePicker(uint32_t nodeId, const std::string& key, const UI::UIRect& at);
        void ClosePopup();
        // The skeletal mesh bone pickers list the bones of (found from the graph's clips; picked in the bone picker otherwise).
        void FindPreviewMesh();
        void SetPreviewMesh(AssetHandle mesh);
        // A material graph compiled as it is on the canvas (microseconds): the error, or what it sets and its parameters.
        void RefreshCompileStatus();
        // Which graph the window shows (G3): m_Path is node, state, node, state ... from the asset's graph down -- an odd length ends
        // on a state machine (its states view), an even one on a graph (the canvas). Parts that no longer exist are dropped; the
        // canvas / states view and the breadcrumb follow. Run every frame and after the graph is replaced (undo, revert, reload).
        void ResolvePath();
        void RebuildBreadcrumb();
        void BuildMachineDetails(GraphNode& machine);
        // The graph the canvas shows (a state's graph when one is open, else the asset's).
        NodeGraph& ShownGraph();

        AssetHandle m_Handle = 0;
        Ref<NodeGraphAsset> m_Asset;
        std::string m_Title;
        std::string m_Kind; // "Animation Graph", "Audio Graph", "Material Graph"

        bool m_Dirty = false;
        bool m_EditPending = false;
        uint32_t m_DiskVersion = 0;          // the asset's file version this window last took
        NodeGraph m_Committed;               // the graph as of the last history step
        std::vector<NodeGraph> m_UndoStack;
        std::vector<NodeGraph> m_RedoStack;

        Ref<UI::UIElement> m_Root;
        Ref<NodeGraphCanvasV2> m_Canvas;
        Ref<StateMachineViewV2> m_StateView;
        Ref<UI::UIStackBox> m_Breadcrumb;
        std::vector<uint32_t> m_Path, m_ShownPath;
        std::vector<std::string> m_PathLabels;
        GraphNode* m_ShownMachine = nullptr;
        Ref<UI::UIStackBox> m_Details;
        Ref<UI::UIStackBox> m_Parameters;
        Ref<UI::UIButton> m_SaveButton;
        Ref<UI::UIButton> m_UndoButton;
        Ref<UI::UIButton> m_RedoButton;
        Ref<UI::UILabel> m_Status;
        Ref<UI::UILabel> m_CompileStatus;    // a material graph's compile status under the toolbar
        bool m_CompileStatusDirty = true;
        // An open popup's rows run while it is open (they point into the graph): dropped when it closes or the graph is replaced.
        std::shared_ptr<std::vector<std::function<void()>>> m_PopupSyncs;
        std::shared_ptr<const bool> m_PopupOpen;
        std::function<void()> m_DeferredPopup; // a popup built again next frame (its shape changed from inside one of its clicks)
        AssetHandle m_PreviewMesh = 0;
        AssetHandle m_PreviewSkeleton = 0;
        bool m_PreviewSearched = false;
        std::vector<std::function<void()>> m_DetailSyncs;
        std::vector<std::function<void()>> m_ParameterSyncs;
        bool m_DetailsDirty = true;
        bool m_ParametersDirty = true;
    };
}
