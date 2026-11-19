#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIAnchorBox;
    class UIContext;
    class UITabPanel;
    class UIViewportManager;
    class UIWindow;

    enum class UIDockDropZone : uint8_t
    {
        None = 0,
        Center,
        Left,
        Right,
        Top,
        Bottom,
        OuterLeft,
        OuterRight,
        OuterTop,
        OuterBottom
    };

    enum class UIDockSplitDir : uint8_t
    {
        None = 0,
        Horizontal, // First (left) | Second (right)
        Vertical    // First (top)  | Second (bottom)
    };

    enum class UIDockNodeFlags : uint32_t
    {
        None          = 0,
        NoUndocking   = 1 << 0, // Cannot be dragged out / detached
        NoTabBar      = 1 << 1, // Hides the tab bar (e.g. permanent central viewport)
        NoDockingOver = 1 << 2, // Other panels cannot tab-dock into Center (only split around it)
        CentralNode   = NoUndocking | NoTabBar | NoDockingOver
    };

    inline UIDockNodeFlags operator|(UIDockNodeFlags a, UIDockNodeFlags b)
    {
        return static_cast<UIDockNodeFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline bool HasFlag(UIDockNodeFlags flags, UIDockNodeFlags test)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
    }

    struct UIDockNode : public RefCounted
    {
        UIDockSplitDir SplitDir = UIDockSplitDir::None;
        UIDockNodeFlags Flags = UIDockNodeFlags::None;
        float SplitRatio = 0.5f;
        UIRect Bounds{};
        UIRect SplitterRect{};

        Ref<UIDockNode> First;
        Ref<UIDockNode> Second;
        Ref<UITabPanel> TabPanel;

        bool IsLeaf() const { return SplitDir == UIDockSplitDir::None; }
    };

    // IDE-style recursive dock space with resizable splits, tab tear-out,
    // floating in-window panels, live 5-way + outer drop-zone previews,
    // and optional multi-OS-window popout via UIViewportManager.
    class UIDockSpace : public UIElement
    {
    public:
        UIDockSpace();

        // Dock a panel relative to an existing panel title (or root if targetTitle is empty / not found).
        Ref<UIDockNode> AddPanel(
            const std::string& title,
            const Ref<UIElement>& content,
            UIDockDropZone zone = UIDockDropZone::Center,
            const std::string& targetTitle = {},
            float ratio = 0.5f,
            UIDockNodeFlags flags = UIDockNodeFlags::None
        );

        // Dock a panel into a specific node & zone.
        Ref<UIDockNode> DockPanel(
            const std::string& title,
            const Ref<UIElement>& content,
            UIDockDropZone zone,
            const Ref<UIDockNode>& targetNode,
            float ratio = 0.5f,
            size_t insertTabIndex = SIZE_MAX
        );

        // Detach/float a panel as an in-window UIWindow (optionally starting an immediate drag).
        Ref<UIWindow> FloatPanel(
            const std::string& title,
            const Ref<UIElement>& content,
            UIVec2 pos,
            UIVec2 size = { 440.0f, 320.0f },
            bool startDrag = false
        );

        void SetViewportManager(UIViewportManager* manager) { m_ViewportManager = manager; }
        // Asked when the user closes a panel's window (a floating window's X, a torn-off OS window's X): false keeps it open -- an
        // editor with unsaved changes asks first, then closes it itself (RemovePanel / UIViewportManager::ClosePanel).
        std::function<bool(const std::string& title)> CanClosePanel;
        UIViewportManager* GetViewportManager() const { return m_ViewportManager; }
        void SetContext(UIContext* context) { m_Context = context; }
        UIContext* GetContext() const { return m_Context; }
        bool IsDragging() const { return m_DraggingSplitter != nullptr || m_ExternalDragActive; }

        // External drag integration (used by UIViewportManager when dragging a secondary OS window over the dockspace).
        void UpdateDockPreview(UIVec2 localPos);
        void ClearDockPreview();
        bool HasActiveDropZone() const { return m_HoverDropZone != UIDockDropZone::None; }
        bool CommitDockDrop(const std::string& title, const Ref<UIElement>& content);
        void PaintSplittersOverlay(UIDrawList& drawList, const UITheme& theme) const;
        void PaintOverlay(UIDrawList& drawList, const UITheme& theme) const;

        const Ref<UIDockNode>& GetRootNode() const { return m_RootNode; }
        Ref<UIDockNode> FindNodeByPanelTitle(const std::string& title) const;
        // The floating window showing `title` (null: none).
        Ref<UIWindow> FindFloatingWindow(const std::string& title) const;
        // `title` is open anywhere: docked, floating, or torn off into its own OS window.
        bool HasPanel(const std::string& title) const;
        bool SetPanelFlags(const std::string& title, UIDockNodeFlags flags);
        bool RemovePanel(const std::string& title);
        bool BeginPanelDrag(const std::string& title, UIVec2 pointerPos);

        void ClearPanels();
        bool SaveLayout(const std::filesystem::path& path) const;
        bool LoadLayout(const std::filesystem::path& path);

        UIElement* HitTest(const UIVec2& pos) override;
        UICursorShape GetCursorShape(const UIVec2& pos) const override;
        bool OnPointerLeave(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        Ref<UITabPanel> CreateLeafTabPanel();
        void HookTabPanel(const Ref<UITabPanel>& tabPanel);
        void HookFloatingWindow(const Ref<UIWindow>& win);
        void CloseFloatingWindow(const Ref<UIWindow>& win);
        void CompactTree();
        bool CompactNode(Ref<UIDockNode>& node);

        void ArrangeNode(const Ref<UIDockNode>& node, const UIRect& rect);
        UIDockNode* HitTestSplitter(UIDockNode* node, UIVec2 pos) const;
        Ref<UIDockNode> FindLeafAt(const Ref<UIDockNode>& node, UIVec2 pos) const;
        Ref<UIDockNode> FindNodeByTitleRecursive(const Ref<UIDockNode>& node, const std::string& title) const;
        Ref<UIDockNode> FindNodeByTabPanel(const Ref<UIDockNode>& node, const UITabPanel* panel) const;
        void PaintSplitters(UIDrawList& drawList, const UITheme& theme, const UIDockNode* node) const;
        void PaintDockOverlay(UIDrawList& drawList, const UITheme& theme) const;

        struct DropZoneHit
        {
            UIDockDropZone Zone = UIDockDropZone::None;
            Ref<UIDockNode> TargetNode;
            UIRect PreviewRect{};
            bool IsTabBarDrop = false;
            size_t InsertTabIndex = SIZE_MAX;
            float TabInsertX = 0.0f;
        };
        DropZoneHit ComputeDropZone(UIVec2 localPos) const;
        float ComputeDropRatio(UIDockDropZone zone, const UIDockNode* target, const std::string& title, UIElement* content = nullptr) const;
        void BringFloatingWindowToFront(UIWindow* win);

        static constexpr float kSplitterThickness = 1.0f;
        static constexpr float kSplitterHitHalfExtent = 4.0f;
        static constexpr float kMinNodeExtent = 34.0f;

        Ref<UIDockNode> m_RootNode;
        Ref<UIElement> m_DockHost;     // Hosts all leaf UITabPanels
        Ref<UIElement> m_SplitterHost; // Paints splitters above docked panels, below floating windows
        Ref<UIAnchorBox> m_FloatHost;  // Hosts floating UIWindows above docked panels & splitters
        Ref<UIElement> m_OverlayHost;  // Paints dock drop-zone previews on top
        std::vector<Ref<UIWindow>> m_FloatingWindows;
        std::unordered_map<std::string, UIDockNodeFlags> m_PanelFlags;
        std::unordered_map<std::string, UIVec2> m_PreferredPanelSizes;
        std::string m_ActiveDragTitle;
        UIVec2 m_ActiveDragDesiredSize{ 0.0f, 0.0f };

        UIDockNode* m_DraggingSplitter = nullptr;
        UIDockNode* m_HoveredSplitter = nullptr;
        bool m_ExternalDragActive = false;
        UIDockDropZone m_HoverDropZone = UIDockDropZone::None;
        Ref<UIDockNode> m_HoverTargetNode;
        UIRect m_HoverPreviewRect{};
        bool m_HoverIsTabBarDrop = false;
        size_t m_HoverInsertTabIndex = SIZE_MAX;
        float m_HoverTabInsertX = 0.0f;

        UIContext* m_Context = nullptr;
        UIViewportManager* m_ViewportManager = nullptr;
    };
}
