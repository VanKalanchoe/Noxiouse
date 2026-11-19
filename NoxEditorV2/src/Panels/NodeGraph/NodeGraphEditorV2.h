#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/UI/Core/UITypes.h"

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

    // One open graph (.nanimgraph / .naudiograph / .nmatgraph) in NoxEditorV2: toolbar, the graph's parameters on the left, the
    // canvas in the middle, the selected node's properties on the right (Unreal's Details panel).
    //
    // It edits the loaded NodeGraphAsset in place and recompiles it after each change (a playing scene picks the change up);
    // Save writes the file. History: the whole graph after each finished edit (a drag or a connection is one step; a value
    // dragged in Details is one step when the mouse button comes up), Ctrl+Z / Ctrl+Y.
    class NodeGraphEditorV2
    {
    public:
        explicit NodeGraphEditorV2(AssetHandle graph);
        ~NodeGraphEditorV2();

        Ref<UI::UIElement> BuildUI();
        // Once a frame: finished edits become history steps, the window's labels follow.
        void Update();
        void Save();

        AssetHandle GetHandle() const { return m_Handle; }
        const std::string& GetTitle() const { return m_Title; }
        bool IsDirty() const { return m_Dirty; }
        bool IsValid() const { return m_Asset != nullptr; }
        // The window's root (the editor routes keys typed in it here, not to its own shortcuts).
        const Ref<UI::UIElement>& GetRoot() const { return m_Root; }

        std::function<void(UI::UINotification)> Notify;

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
        Ref<UI::UIStackBox> m_Details;
        Ref<UI::UIStackBox> m_Parameters;
        Ref<UI::UIButton> m_SaveButton;
        Ref<UI::UIButton> m_UndoButton;
        Ref<UI::UIButton> m_RedoButton;
        Ref<UI::UILabel> m_Status;
        std::vector<std::function<void()>> m_DetailSyncs;
        std::vector<std::function<void()>> m_ParameterSyncs;
        bool m_DetailsDirty = true;
        bool m_ParametersDirty = true;
    };
}
