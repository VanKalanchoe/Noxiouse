#pragma once

#include <chrono>
#include <functional>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/Audio/AudioGraph.h"
#include <NoxUI/Core/UITypes.h>
#include <glm/glm.hpp>
#include "Panels/AssetEditor.h"

namespace NoxUI
{
        class UIButton;
        class UIContext;
        class UIElement;
        class UILabel;
        class UIStackBox;
    }

namespace Nox
{
    class NodeGraphCanvas;
    class StateMachineView;
    class AnimationGraphInstance;
    struct GraphNode;
    struct InspectorCardBuilder;

    // One open graph (.nanimgraph / .naudiograph / .nmatgraph) in NoxEditor: toolbar, the graph's parameters on the left, the
    // canvas in the middle, the selected node's properties on the right (Unreal's Details panel).
    //
    // It edits the loaded NodeGraphAsset in place and recompiles it after each change (a playing scene picks the change up);
    // Save writes the file. History: the whole graph after each finished edit (a drag or a connection is one step; a value
    // dragged in Details is one step when the mouse button comes up), Ctrl+Z / Ctrl+Y.
    class NodeGraphEditor final : public AssetEditor
    {
    public:
        explicit NodeGraphEditor(AssetHandle graph);
        ~NodeGraphEditor() override;

        Ref<NoxUI::UIElement> BuildUI() override;
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
        NoxUI::UIVec2 GetDefaultSize() const override { return { 1180.0f, 700.0f }; }
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
        bool HandleKey(const NoxUI::UIKeyEvent& e);
        void FollowDiskChanges();
        // One value's editor (a node property or an input's value), in Details or in the popup beside a node: by what it holds and
        // what the node type declares (range, choices, asset kind, bone, curve, parameter). `rebuild`: what to build again when the
        // editor's shape changes (a curve key added).
        void AddValueEditor(InspectorCardBuilder& builder, uint32_t nodeId, const std::string& key, const std::string& label, std::function<void()> rebuild);
        // A value on the canvas clicked: its editor in a popup beside it.
        void ShowValuePopup(uint32_t nodeId, const std::string& key, const NoxUI::UIRect& at);
        // A bone property: the preview skeleton's bones (searchable, by depth) or a name typed.
        void ShowBonePicker(uint32_t nodeId, const std::string& key, const NoxUI::UIRect& at);
        void ClosePopup();
        // Shows a popup again next frame (UIContext::Defer): unless ClosePopup ran meanwhile or the editor is gone.
        void DeferPopup(std::function<void()> show);
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
        // The preview card above Details (G4). An animation graph: played on a skeletal mesh (Mesh menu, Play / Pause, Restart) with
        // its parameters set as a script would; the last notify it passed. An audio graph: Play / Stop, the output's level (L / R, dB),
        // its parameters while it plays (Reset back to the graph's default; triggers: Fire). Neither is saved; closing stops them.
        void RebuildPreview();
        void UpdateAnimationPreview();
        void UpdateAudioPreview();
        void StopAudioPreview();
        void ShowPreviewMeshMenu(const NoxUI::UIRect& anchor);

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

        Ref<NoxUI::UIElement> m_Root;
        Ref<NodeGraphCanvas> m_Canvas;
        Ref<StateMachineView> m_StateView;
        Ref<NoxUI::UIStackBox> m_Breadcrumb;
        std::vector<uint32_t> m_Path, m_ShownPath;
        std::vector<std::string> m_PathLabels;
        GraphNode* m_ShownMachine = nullptr;
        Ref<NoxUI::UIStackBox> m_Details;
        Ref<NoxUI::UIStackBox> m_Parameters;
        Ref<NoxUI::UIButton> m_SaveButton;
        Ref<NoxUI::UIButton> m_UndoButton;
        Ref<NoxUI::UIButton> m_RedoButton;
        Ref<NoxUI::UILabel> m_Status;
        Ref<NoxUI::UILabel> m_CompileStatus;    // a material graph's compile status under the toolbar
        bool m_CompileStatusDirty = true;
        // An open popup's rows run while it is open (they point into the graph): dropped when it closes or the graph is replaced.
        std::shared_ptr<const bool> m_PopupOpen;                                    // our popup is the overlay (ClosePopup closes it)
        NoxUI::UIContext* m_PopupContext = nullptr;                                    // the context it is the overlay of
        std::shared_ptr<uint32_t> m_PopupGeneration = std::make_shared<uint32_t>(0); // bumped by ClosePopup; DeferPopup checks it
        Ref<NoxUI::UIStackBox> m_PreviewBox;
        bool m_PreviewDirty = true;
        std::unique_ptr<AnimationGraphInstance> m_AnimPreview;
        bool m_AnimPlaying = true;
        std::vector<glm::mat4> m_PreviewBones;
        Ref<NoxUI::UIElement> m_PreviewImage;
        std::string m_PreviewNotify;
        float m_PreviewNotifyAge = 10.0f;
        std::chrono::steady_clock::time_point m_PreviewTick{};
        uint64_t m_AudioVoice = 0;
        bool m_AudioWanted = false;
        std::shared_ptr<const AudioGraphProgram> m_AudioProgram;
        std::string m_AudioError;
        std::unordered_map<std::string, NodeGraphValue> m_AudioValues; // set in the preview (not saved)
        AssetHandle m_PreviewMesh = 0;
        AssetHandle m_PreviewSkeleton = 0;
        bool m_PreviewSearched = false;
        bool m_DetailsDirty = true;
        bool m_ParametersDirty = true;
    };
}
