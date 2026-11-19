#include "UIDockSpace.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <SDL3/SDL_mouse.h>
#include <yaml-cpp/yaml.h>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UIViewportManager.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UITabPanel.h"
#include "NoxCore/UI/Layout/UIWindow.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    namespace
    {
        UIRect CenterBox(UIVec2 center, float halfSize)
        {
            return { { center.x - halfSize, center.y - halfSize }, { center.x + halfSize, center.y + halfSize } };
        }

        class UIDockSplitterElement final : public UIElement
        {
        public:
            explicit UIDockSplitterElement(const UIDockSpace& owner) : m_Owner(owner)
            {
                HitTestMode = UIHitTestMode::None;
                DrawBackground = false;
            }

        protected:
            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                m_Owner.PaintSplittersOverlay(drawList, theme);
            }

        private:
            const UIDockSpace& m_Owner;
        };

        class UIDockOverlayElement final : public UIElement
        {
        public:
            explicit UIDockOverlayElement(const UIDockSpace& owner) : m_Owner(owner)
            {
                HitTestMode = UIHitTestMode::None;
                DrawBackground = false;
            }

        protected:
            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                m_Owner.PaintOverlay(drawList, theme);
            }

        private:
            const UIDockSpace& m_Owner;
        };
    }

    UIDockSpace::UIDockSpace()
    {
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Fill;
        HitTestMode = UIHitTestMode::Self;
        ClipChildren = true;
        DrawBackground = false;

        m_DockHost = Ref<UIElement>(new UIElement());
        m_DockHost->HitTestMode = UIHitTestMode::PassThrough;
        AddChild(m_DockHost);

        m_SplitterHost = Ref<UIElement>(new UIDockSplitterElement(*this));
        AddChild(m_SplitterHost);

        m_FloatHost = Ref<UIAnchorBox>(new UIAnchorBox());
        m_FloatHost->HitTestMode = UIHitTestMode::PassThrough;
        AddChild(m_FloatHost);

        m_OverlayHost = Ref<UIElement>(new UIDockOverlayElement(*this));
        AddChild(m_OverlayHost);
    }

    Ref<UITabPanel> UIDockSpace::CreateLeafTabPanel()
    {
        Ref<UITabPanel> panel(new UITabPanel());
        panel->GetContentHost()->Padding = UIThickness::All(4.0f);
        HookTabPanel(panel);
        m_DockHost->AddChild(panel);
        return panel;
    }

    void UIDockSpace::HookTabPanel(const Ref<UITabPanel>& tabPanel)
    {
        UITabPanel* rawPanel = tabPanel.get();
        tabPanel->OnTabDragOut = [this, rawPanel](size_t tabIdx, UIVec2 pointerPos)
        {
            if (tabIdx >= rawPanel->GetTabCount())
                return;

            if (Ref<UIDockNode> owner = FindNodeByTabPanel(m_RootNode, rawPanel);
                owner && HasFlag(owner->Flags, UIDockNodeFlags::NoUndocking))
            {
                return;
            }

            const std::string title = rawPanel->GetTabTitle(tabIdx);
            Ref<UIElement> content = rawPanel->GetTabContent(tabIdx);
            const UIRect panelBounds = rawPanel->GetBounds();
            if (panelBounds.Width() > 1.0f && panelBounds.Height() > 1.0f)
                m_PreferredPanelSizes[title] = { panelBounds.Width(), panelBounds.Height() };

            m_ActiveDragTitle = title;
            m_ActiveDragDesiredSize = m_PreferredPanelSizes.contains(title)
                ? m_PreferredPanelSizes.at(title)
                : UIVec2{ panelBounds.Width(), panelBounds.Height() };

            const bool isCompactBar = panelBounds.Height() > 0.0f && panelBounds.Height() < 80.0f;
            const float minW = isCompactBar ? 340.0f : 300.0f;
            const float maxW = isCompactBar ? 540.0f : 680.0f;
            const float minH = isCompactBar ? 68.0f : 220.0f;
            const float maxH = isCompactBar ? 88.0f : 520.0f;
            UIVec2 floatSize{
                std::clamp(panelBounds.Width(), minW, maxW),
                std::clamp(panelBounds.Height(), minH, maxH)
            };

            rawPanel->RemoveTab(tabIdx);
            CompactTree();

            // Place floating window so its titlebar is right under the cursor.
            UIVec2 spawnPos{
                pointerPos.x - floatSize.x * 0.35f,
                pointerPos.y - 14.0f
            };
            FloatPanel(title, content, spawnPos, floatSize, true);
        };
    }

    void UIDockSpace::BringFloatingWindowToFront(UIWindow* win)
    {
        if (!win || !m_FloatHost)
            return;
        Ref<UIWindow> target;
        auto it = std::find_if(m_FloatingWindows.begin(), m_FloatingWindows.end(),
            [win](const Ref<UIWindow>& w) { return w.get() == win; });
        if (it == m_FloatingWindows.end() || it + 1 == m_FloatingWindows.end())
            return;
        target = *it;
        m_FloatingWindows.erase(it);
        m_FloatingWindows.push_back(target);
        m_FloatHost->RemoveChild(target);
        m_FloatHost->AddChild(target);
    }

    void UIDockSpace::HookFloatingWindow(const Ref<UIWindow>& win)
    {
        UIWindow* rawWin = win.get();
        win->OnActivate = [this, rawWin]()
        {
            BringFloatingWindowToFront(rawWin);
        };
        win->OnDragMove = [this, rawWin](UIVec2 pointerPos)
        {
            const std::string& title = rawWin->GetTitle();
            m_ActiveDragTitle = title;
            if (auto it = m_PreferredPanelSizes.find(title); it != m_PreferredPanelSizes.end())
                m_ActiveDragDesiredSize = it->second;
            else
                m_ActiveDragDesiredSize = rawWin->FixedSize;

            // Only promote to an external OS viewport once the cursor genuinely leaves the main OS window
            // (so dragging Toolbar or side panels near the MenuBar or edges stays an instant in-window float).
            UIRect promoteBounds = GetBounds();
            if (UIContext* ctx = GetContext(); ctx && ctx->GetRoot())
                promoteBounds = ctx->GetRoot()->GetBounds();
            constexpr float kOutMargin = 10.0f;
            const bool outsideMainWindow =
                pointerPos.x < promoteBounds.Min.x - kOutMargin ||
                pointerPos.x > promoteBounds.Max.x + kOutMargin ||
                pointerPos.y < promoteBounds.Min.y - kOutMargin ||
                pointerPos.y > promoteBounds.Max.y + kOutMargin;

            if (m_ViewportManager && outsideMainWindow)
            {
                Ref<UIElement> content;
                if (rawWin->GetContentBox() && !rawWin->GetContentBox()->GetChildren().empty())
                {
                    content = rawWin->GetContentBox()->GetChildren().front();
                    rawWin->GetContentBox()->RemoveChild(content);
                }
                const UIVec2 size = rawWin->FixedSize;
                const UIVec2 grabOffset{
                    std::clamp(pointerPos.x - rawWin->GetBounds().Min.x, 20.0f, std::max(40.0f, size.x - 40.0f)),
                    14.0f
                };
                const std::string titleCopy = title;

                // Close floating UIWindow first
                for (auto it = m_FloatingWindows.begin(); it != m_FloatingWindows.end(); ++it)
                {
                    if (it->get() == rawWin)
                    {
                        Ref<UIWindow> keep = *it;
                        CloseFloatingWindow(keep);
                        break;
                    }
                }
                ClearDockPreview();
                SDL_CaptureMouse(false);
                if (UIContext* ctx = GetContext())
                    ctx->ReleasePointerCapture();

                m_ViewportManager->PromoteToViewport(titleCopy, content, size, grabOffset);
                return;
            }

            UpdateDockPreview(pointerPos);
        };

        win->OnDragEnd = [this, rawWin](UIVec2 pointerPos)
        {
            SDL_CaptureMouse(false);
            const std::string title = rawWin->GetTitle();
            m_ActiveDragTitle = title;
            if (auto it = m_PreferredPanelSizes.find(title); it != m_PreferredPanelSizes.end())
                m_ActiveDragDesiredSize = it->second;
            else
                m_ActiveDragDesiredSize = rawWin->FixedSize;

            UpdateDockPreview(pointerPos);
            if (HasActiveDropZone())
            {
                Ref<UIElement> content;
                if (rawWin->GetContentBox() && !rawWin->GetContentBox()->GetChildren().empty())
                {
                    content = rawWin->GetContentBox()->GetChildren().front();
                    rawWin->GetContentBox()->RemoveChild(content);
                }
                for (auto it = m_FloatingWindows.begin(); it != m_FloatingWindows.end(); ++it)
                {
                    if (it->get() == rawWin)
                    {
                        Ref<UIWindow> keep = *it;
                        CloseFloatingWindow(keep);
                        break;
                    }
                }
                CommitDockDrop(title, content);
            }
            else
            {
                ClearDockPreview();
            }
            m_ActiveDragTitle.clear();
            m_ActiveDragDesiredSize = { 0.0f, 0.0f };
        };
    }

    void UIDockSpace::CloseFloatingWindow(const Ref<UIWindow>& win)
    {
        if (!win)
            return;
        m_FloatHost->RemoveChild(win);
        auto it = std::find(m_FloatingWindows.begin(), m_FloatingWindows.end(), win);
        if (it != m_FloatingWindows.end())
            m_FloatingWindows.erase(it);
    }

    Ref<UIWindow> UIDockSpace::FloatPanel(
        const std::string& title,
        const Ref<UIElement>& content,
        UIVec2 pos,
        UIVec2 size,
        bool startDrag)
    {
        Ref<UIWindow> win(new UIWindow(title, {}));
        UIWindow* rawWin = win.get();
        win->OnClose = [this, rawWin]()
        {
            if (CanClosePanel && !CanClosePanel(rawWin->GetTitle()))
                return;
            for (const auto& w : m_FloatingWindows)
            {
                if (w.get() == rawWin)
                {
                    Ref<UIWindow> keep = w;
                    CloseFloatingWindow(keep);
                    break;
                }
            }
        };
        win->FixedSize = size;
        win->AnchorOffsets = { pos.x, pos.y, 0.0f, 0.0f };
        if (content)
        {
            content->Visible = true;
            content->WidthMode = UISizeMode::Fill;
            content->HeightMode = UISizeMode::Fill;
            win->AddContent(content);
        }
        HookFloatingWindow(win);
        m_FloatHost->AddChild(win);
        m_FloatingWindows.push_back(win);

        if (startDrag)
        {
            win->BeginProgrammaticDrag();
            SDL_CaptureMouse(true);
            if (UIContext* ctx = GetContext())
                ctx->SetPointerCapture(win);
        }
        return win;
    }

    Ref<UIDockNode> UIDockSpace::AddPanel(
        const std::string& title,
        const Ref<UIElement>& content,
        UIDockDropZone zone,
        const std::string& targetTitle,
        float ratio,
        UIDockNodeFlags flags)
    {
        if (flags != UIDockNodeFlags::None)
            m_PanelFlags[title] = flags;
        Ref<UIDockNode> target;
        if (!targetTitle.empty())
            target = FindNodeByPanelTitle(targetTitle);
        if (!target)
            target = m_RootNode;
        Ref<UIDockNode> node = DockPanel(title, content, zone, target, ratio);
        const UIDockNodeFlags effectiveFlags = (flags != UIDockNodeFlags::None)
            ? flags
            : (m_PanelFlags.contains(title) ? m_PanelFlags.at(title) : UIDockNodeFlags::None);
        if (node && effectiveFlags != UIDockNodeFlags::None)
        {
            node->Flags = node->Flags | effectiveFlags;
            if (node->TabPanel)
            {
                const bool hideBar = HasFlag(node->Flags, UIDockNodeFlags::NoTabBar);
                const bool allowUndock = !HasFlag(node->Flags, UIDockNodeFlags::NoUndocking);
                node->TabPanel->SetTabBarVisible(!hideBar);
                node->TabPanel->SetShowUndockCornerHandle(hideBar && allowUndock);
                if (hideBar)
                    node->TabPanel->GetContentHost()->Padding = UIThickness::All(0.0f);
            }
        }
        return node;
    }

    Ref<UIDockNode> UIDockSpace::DockPanel(
        const std::string& title,
        const Ref<UIElement>& content,
        UIDockDropZone zone,
        const Ref<UIDockNode>& targetNode,
        float ratio,
        size_t insertTabIndex)
    {
        if (content)
            content->Visible = true;

        const UIDockNodeFlags knownFlags = m_PanelFlags.contains(title) ? m_PanelFlags.at(title) : UIDockNodeFlags::None;
        auto applyLeafFlags = [knownFlags](const Ref<UIDockNode>& leaf)
        {
            if (!leaf || knownFlags == UIDockNodeFlags::None)
                return;
            leaf->Flags = leaf->Flags | knownFlags;
            if (leaf->TabPanel)
            {
                const bool hideBar = HasFlag(leaf->Flags, UIDockNodeFlags::NoTabBar);
                const bool allowUndock = !HasFlag(leaf->Flags, UIDockNodeFlags::NoUndocking);
                leaf->TabPanel->SetTabBarVisible(!hideBar);
                leaf->TabPanel->SetShowUndockCornerHandle(hideBar && allowUndock);
                if (hideBar)
                    leaf->TabPanel->GetContentHost()->Padding = UIThickness::All(0.0f);
            }
        };

        if (!m_RootNode)
        {
            Ref<UIDockNode> root(new UIDockNode());
            root->TabPanel = CreateLeafTabPanel();
            root->TabPanel->AddTab(title, content);
            applyLeafFlags(root);
            m_RootNode = root;
            return root;
        }

        // Outer zones split the root node itself.
        if (zone == UIDockDropZone::OuterLeft || zone == UIDockDropZone::OuterRight ||
            zone == UIDockDropZone::OuterTop || zone == UIDockDropZone::OuterBottom)
        {
            Ref<UIDockNode> newLeaf(new UIDockNode());
            newLeaf->TabPanel = CreateLeafTabPanel();
            newLeaf->TabPanel->AddTab(title, content);
            applyLeafFlags(newLeaf);

            Ref<UIDockNode> newRoot(new UIDockNode());
            const bool horizontal = (zone == UIDockDropZone::OuterLeft || zone == UIDockDropZone::OuterRight);
            const bool newIsFirst = (zone == UIDockDropZone::OuterLeft || zone == UIDockDropZone::OuterTop);
            newRoot->SplitDir = horizontal ? UIDockSplitDir::Horizontal : UIDockSplitDir::Vertical;
            newRoot->SplitRatio = std::clamp(newIsFirst ? ratio : (1.0f - ratio), 0.025f, 0.975f);
            newRoot->First = newIsFirst ? newLeaf : m_RootNode;
            newRoot->Second = newIsFirst ? m_RootNode : newLeaf;
            m_RootNode = newRoot;
            return newLeaf;
        }

        Ref<UIDockNode> target = targetNode ? targetNode : m_RootNode;
        // If target is a split node, descend to its first leaf for Center tabify, or split target directly.
        if (zone == UIDockDropZone::Center || zone == UIDockDropZone::None)
        {
            while (target && !target->IsLeaf())
                target = target->First ? target->First : target->Second;
            if (target && target->TabPanel)
            {
                const size_t idx = (insertTabIndex <= target->TabPanel->GetTabCount())
                    ? target->TabPanel->InsertTab(insertTabIndex, title, content)
                    : target->TabPanel->AddTab(title, content);
                target->TabPanel->SetActiveTab(idx);
                applyLeafFlags(target);
            }
            return target;
        }

        // Split target node into (existing | newLeaf).
        Ref<UIDockNode> existingCopy(new UIDockNode());
        existingCopy->SplitDir = target->SplitDir;
        existingCopy->Flags = target->Flags;
        existingCopy->SplitRatio = target->SplitRatio;
        existingCopy->First = target->First;
        existingCopy->Second = target->Second;
        existingCopy->TabPanel = target->TabPanel;
        target->Flags = UIDockNodeFlags::None;

        Ref<UIDockNode> newLeaf(new UIDockNode());
        newLeaf->TabPanel = CreateLeafTabPanel();
        newLeaf->TabPanel->AddTab(title, content);
        applyLeafFlags(newLeaf);

        const bool horizontal = (zone == UIDockDropZone::Left || zone == UIDockDropZone::Right);
        const bool newIsFirst = (zone == UIDockDropZone::Left || zone == UIDockDropZone::Top);

        target->TabPanel = nullptr;
        target->SplitDir = horizontal ? UIDockSplitDir::Horizontal : UIDockSplitDir::Vertical;
        target->SplitRatio = std::clamp(newIsFirst ? ratio : (1.0f - ratio), 0.025f, 0.975f);
        target->First = newIsFirst ? newLeaf : existingCopy;
        target->Second = newIsFirst ? existingCopy : newLeaf;
        return newLeaf;
    }

    void UIDockSpace::CompactTree()
    {
        if (!m_RootNode)
            return;
        if (CompactNode(m_RootNode))
            m_RootNode = nullptr;
    }

    bool UIDockSpace::CompactNode(Ref<UIDockNode>& node)
    {
        if (!node)
            return true;

        if (node->IsLeaf())
        {
            if (!node->TabPanel || node->TabPanel->GetTabCount() == 0)
            {
                if (node->TabPanel)
                    m_DockHost->RemoveChild(node->TabPanel);
                return true;
            }
            return false;
        }

        const bool firstEmpty = CompactNode(node->First);
        const bool secondEmpty = CompactNode(node->Second);
        if (firstEmpty && secondEmpty)
            return true;
        if (firstEmpty)
        {
            Ref<UIDockNode> keep = node->Second;
            node = keep;
        }
        else if (secondEmpty)
        {
            Ref<UIDockNode> keep = node->First;
            node = keep;
        }
        return false;
    }

    Ref<UIDockNode> UIDockSpace::FindNodeByPanelTitle(const std::string& title) const
    {
        return FindNodeByTitleRecursive(m_RootNode, title);
    }

    bool UIDockSpace::SetPanelFlags(const std::string& title, UIDockNodeFlags flags)
    {
        m_PanelFlags[title] = flags;
        Ref<UIDockNode> node = FindNodeByPanelTitle(title);
        if (!node)
            return false;

        node->Flags = flags;
        if (node->TabPanel)
        {
            const bool hideBar = HasFlag(node->Flags, UIDockNodeFlags::NoTabBar);
            const bool allowUndock = !HasFlag(node->Flags, UIDockNodeFlags::NoUndocking);
            node->TabPanel->SetTabBarVisible(!hideBar);
            node->TabPanel->SetShowUndockCornerHandle(hideBar && allowUndock);
            node->TabPanel->GetContentHost()->Padding = UIThickness::All(hideBar ? 0.0f : 4.0f);
        }
        return true;
    }

    Ref<UIWindow> UIDockSpace::FindFloatingWindow(const std::string& title) const
    {
        for (const auto& win : m_FloatingWindows)
            if (win && win->GetTitle() == title)
                return win;
        return nullptr;
    }

    bool UIDockSpace::HasPanel(const std::string& title) const
    {
        return FindNodeByPanelTitle(title) != nullptr || FindFloatingWindow(title) != nullptr ||
               (m_ViewportManager && m_ViewportManager->HasPanel(title));
    }

    bool UIDockSpace::RemovePanel(const std::string& title)
    {
        bool removed = false;
        if (Ref<UIDockNode> node = FindNodeByPanelTitle(title); node && node->TabPanel)
        {
            for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
            {
                if (node->TabPanel->GetTabTitle(i) == title)
                {
                    node->TabPanel->RemoveTab(i);
                    CompactTree();
                    removed = true;
                    break;
                }
            }
        }
        for (const auto& win : m_FloatingWindows)
        {
            if (win && win->GetTitle() == title)
            {
                Ref<UIWindow> keep = win;
                CloseFloatingWindow(keep);
                removed = true;
                break;
            }
        }
        return removed;
    }

    bool UIDockSpace::BeginPanelDrag(const std::string& title, UIVec2 pointerPos)
    {
        Ref<UIDockNode> node = FindNodeByPanelTitle(title);
        if (!node || !node->TabPanel || HasFlag(node->Flags, UIDockNodeFlags::NoUndocking))
            return false;
        for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
        {
            if (node->TabPanel->GetTabTitle(i) == title)
            {
                if (node->TabPanel->OnTabDragOut)
                    node->TabPanel->OnTabDragOut(i, pointerPos);
                return true;
            }
        }
        return false;
    }

    Ref<UIDockNode> UIDockSpace::FindNodeByTitleRecursive(const Ref<UIDockNode>& node, const std::string& title) const
    {
        if (!node)
            return nullptr;
        if (node->IsLeaf())
        {
            if (node->TabPanel)
            {
                for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
                {
                    if (node->TabPanel->GetTabTitle(i) == title)
                        return node;
                }
            }
            return nullptr;
        }
        if (auto f = FindNodeByTitleRecursive(node->First, title))
            return f;
        return FindNodeByTitleRecursive(node->Second, title);
    }

    Ref<UIDockNode> UIDockSpace::FindNodeByTabPanel(const Ref<UIDockNode>& node, const UITabPanel* panel) const
    {
        if (!node)
            return nullptr;
        if (node->IsLeaf())
            return node->TabPanel.get() == panel ? node : nullptr;
        if (auto f = FindNodeByTabPanel(node->First, panel))
            return f;
        return FindNodeByTabPanel(node->Second, panel);
    }

    Ref<UIDockNode> UIDockSpace::FindLeafAt(const Ref<UIDockNode>& node, UIVec2 pos) const
    {
        if (!node || !node->Bounds.Contains(pos))
            return nullptr;
        if (node->IsLeaf())
            return node;
        if (auto f = FindLeafAt(node->First, pos))
            return f;
        return FindLeafAt(node->Second, pos);
    }

    UIDockSpace::DropZoneHit UIDockSpace::ComputeDropZone(UIVec2 localPos) const
    {
        DropZoneHit hit{};
        const UIRect& b = GetBounds();
        if (!b.Contains(localPos))
            return hit;

        constexpr float kHalfBtn = 16.0f;
        constexpr float kOffset = 38.0f;
        constexpr float kOuterInset = 24.0f;
        const UIVec2 bc{ (b.Min.x + b.Max.x) * 0.5f, (b.Min.y + b.Max.y) * 0.5f };

        if (!m_RootNode)
        {
            if (CenterBox(bc, kHalfBtn).Contains(localPos))
            {
                hit.Zone = UIDockDropZone::Center;
                hit.PreviewRect = b;
            }
            return hit;
        }

        // Outer viewport edge squares (only trigger inside the square, not the entire edge)
        if (CenterBox({ b.Min.x + kOuterInset, bc.y }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::OuterLeft;
            const float r = ComputeDropRatio(hit.Zone, nullptr, m_ActiveDragTitle);
            hit.PreviewRect = { b.Min, { b.Min.x + b.Width() * r, b.Max.y } };
            return hit;
        }
        if (CenterBox({ b.Max.x - kOuterInset, bc.y }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::OuterRight;
            const float r = ComputeDropRatio(hit.Zone, nullptr, m_ActiveDragTitle);
            hit.PreviewRect = { { b.Max.x - b.Width() * r, b.Min.y }, b.Max };
            return hit;
        }
        if (CenterBox({ bc.x, b.Min.y + kOuterInset }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::OuterTop;
            const float r = ComputeDropRatio(hit.Zone, nullptr, m_ActiveDragTitle);
            hit.PreviewRect = { b.Min, { b.Max.x, b.Min.y + b.Height() * r } };
            return hit;
        }
        if (CenterBox({ bc.x, b.Max.y - kOuterInset }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::OuterBottom;
            const float r = ComputeDropRatio(hit.Zone, nullptr, m_ActiveDragTitle);
            hit.PreviewRect = { { b.Min.x, b.Max.y - b.Height() * r }, b.Max };
            return hit;
        }

        Ref<UIDockNode> leaf = FindLeafAt(m_RootNode, localPos);
        if (!leaf)
            return hit;

        hit.TargetNode = leaf;
        const UIRect& nb = leaf->Bounds;
        const UIVec2 c{ (nb.Min.x + nb.Max.x) * 0.5f, (nb.Min.y + nb.Max.y) * 0.5f };

        const bool allowCenter = !HasFlag(leaf->Flags, UIDockNodeFlags::NoDockingOver);
        if (allowCenter && CenterBox(c, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::Center;
            hit.PreviewRect = nb;
        }
        else if (allowCenter && leaf->TabPanel && leaf->TabPanel->GetTabBar() &&
                 leaf->TabPanel->GetTabBar()->Visible &&
                 leaf->TabPanel->GetTabBar()->GetBounds().Contains(localPos))
        {
            hit.Zone = UIDockDropZone::Center;
            hit.IsTabBarDrop = true;
            hit.InsertTabIndex = leaf->TabPanel->ComputeTabInsertIndex(localPos.x, hit.TabInsertX);
            hit.PreviewRect = leaf->TabPanel->GetTabBar()->GetBounds();
        }
        else if (CenterBox({ c.x - kOffset, c.y }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::Left;
            const float r = ComputeDropRatio(hit.Zone, leaf.get(), m_ActiveDragTitle);
            hit.PreviewRect = { nb.Min, { nb.Min.x + nb.Width() * r, nb.Max.y } };
        }
        else if (CenterBox({ c.x + kOffset, c.y }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::Right;
            const float r = ComputeDropRatio(hit.Zone, leaf.get(), m_ActiveDragTitle);
            hit.PreviewRect = { { nb.Max.x - nb.Width() * r, nb.Min.y }, nb.Max };
        }
        else if (CenterBox({ c.x, c.y - kOffset }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::Top;
            const float r = ComputeDropRatio(hit.Zone, leaf.get(), m_ActiveDragTitle);
            hit.PreviewRect = { nb.Min, { nb.Max.x, nb.Min.y + nb.Height() * r } };
        }
        else if (CenterBox({ c.x, c.y + kOffset }, kHalfBtn).Contains(localPos))
        {
            hit.Zone = UIDockDropZone::Bottom;
            const float r = ComputeDropRatio(hit.Zone, leaf.get(), m_ActiveDragTitle);
            hit.PreviewRect = { { nb.Min.x, nb.Max.y - nb.Height() * r }, nb.Max };
        }

        return hit;
    }

    float UIDockSpace::ComputeDropRatio(UIDockDropZone zone, const UIDockNode* target, const std::string& title, UIElement* content) const
    {
        const bool isOuter = (zone == UIDockDropZone::OuterLeft || zone == UIDockDropZone::OuterRight ||
                              zone == UIDockDropZone::OuterTop || zone == UIDockDropZone::OuterBottom);
        const UIRect& targetRect = (!isOuter && target && target->Bounds.Width() > 1.0f && target->Bounds.Height() > 1.0f)
            ? target->Bounds
            : GetBounds();

        UIVec2 desired{ 0.0f, 0.0f };
        if (!title.empty())
        {
            if (auto it = m_PreferredPanelSizes.find(title); it != m_PreferredPanelSizes.end())
                desired = it->second;
        }
        if (desired.x <= 1.0f && desired.y <= 1.0f)
            desired = m_ActiveDragDesiredSize;
        if (desired.x <= 1.0f && desired.y <= 1.0f && content)
            desired = content->Measure();

        const UIDockNodeFlags flags = (!title.empty() && m_PanelFlags.contains(title))
            ? m_PanelFlags.at(title)
            : UIDockNodeFlags::None;
        const bool isCompactBar = HasFlag(flags, UIDockNodeFlags::NoTabBar) || (desired.y > 1.0f && desired.y < 80.0f);

        if (zone == UIDockDropZone::Left || zone == UIDockDropZone::Right ||
            zone == UIDockDropZone::OuterLeft || zone == UIDockDropZone::OuterRight)
        {
            const float wantW = std::max(desired.x > 1.0f ? desired.x : 250.0f, 160.0f);
            return std::clamp(wantW / std::max(targetRect.Width(), 1.0f), 0.08f, 0.42f);
        }

        if (zone == UIDockDropZone::Top || zone == UIDockDropZone::Bottom ||
            zone == UIDockDropZone::OuterTop || zone == UIDockDropZone::OuterBottom)
        {
            float wantH = desired.y > 1.0f ? desired.y : (isCompactBar ? 40.0f : 220.0f);
            if (isCompactBar)
                wantH = std::clamp(wantH, 32.0f, 72.0f);
            else
                wantH = std::max(wantH, 140.0f);
            return std::clamp(wantH / std::max(targetRect.Height(), 1.0f), 0.025f, isCompactBar ? 0.14f : 0.42f);
        }

        return 0.5f;
    }

    void UIDockSpace::UpdateDockPreview(UIVec2 localPos)
    {
        m_ExternalDragActive = true;
        const DropZoneHit hit = ComputeDropZone(localPos);
        m_HoverDropZone = hit.Zone;
        m_HoverTargetNode = hit.TargetNode;
        m_HoverPreviewRect = hit.PreviewRect;
        m_HoverIsTabBarDrop = hit.IsTabBarDrop;
        m_HoverInsertTabIndex = hit.InsertTabIndex;
        m_HoverTabInsertX = hit.TabInsertX;
    }

    void UIDockSpace::ClearDockPreview()
    {
        m_ExternalDragActive = false;
        m_HoverDropZone = UIDockDropZone::None;
        m_HoverTargetNode = nullptr;
        m_HoverPreviewRect = {};
        m_HoverIsTabBarDrop = false;
        m_HoverInsertTabIndex = SIZE_MAX;
        m_HoverTabInsertX = 0.0f;
    }

    bool UIDockSpace::CommitDockDrop(const std::string& title, const Ref<UIElement>& content)
    {
        const UIDockDropZone zone = m_HoverDropZone;
        Ref<UIDockNode> target = m_HoverTargetNode;
        const size_t insertTabIdx = m_HoverInsertTabIndex;
        ClearDockPreview();

        if (zone == UIDockDropZone::None)
            return false;

        const float ratio = ComputeDropRatio(zone, target.get(), title, content.get());
        DockPanel(title, content, zone, target, ratio, insertTabIdx);
        return true;
    }

    UIDockNode* UIDockSpace::HitTestSplitter(UIDockNode* node, UIVec2 pos) const
    {
        if (!node || node->IsLeaf())
            return nullptr;

        UIRect hitRect = node->SplitterRect;
        if (node->SplitDir == UIDockSplitDir::Horizontal)
        {
            hitRect.Min.x -= kSplitterHitHalfExtent;
            hitRect.Max.x += kSplitterHitHalfExtent;
        }
        else if (node->SplitDir == UIDockSplitDir::Vertical)
        {
            hitRect.Min.y -= kSplitterHitHalfExtent;
            hitRect.Max.y += kSplitterHitHalfExtent;
        }

        if (hitRect.Contains(pos))
            return node;

        if (auto* f = HitTestSplitter(node->First.get(), pos))
            return f;
        return HitTestSplitter(node->Second.get(), pos);
    }

    UIElement* UIDockSpace::HitTest(const UIVec2& pos)
    {
        if (!Visible || HitTestMode == UIHitTestMode::None)
            return nullptr;
        if (!GetBounds().Contains(pos))
            return nullptr;

        // Floating windows sit above docked splitters and panels.
        if (m_FloatHost)
        {
            if (UIElement* hit = m_FloatHost->HitTest(pos))
                return hit;
        }

        // Window-edge splitters straddle adjacent docked panels, so claim them before child content.
        if (HitTestSplitter(m_RootNode.get(), pos))
            return this;

        if (m_DockHost)
        {
            if (UIElement* hit = m_DockHost->HitTest(pos))
                return hit;
        }

        return HitTestMode == UIHitTestMode::Self ? this : nullptr;
    }

    UICursorShape UIDockSpace::GetCursorShape(const UIVec2& pos) const
    {
        const UIDockNode* node = m_DraggingSplitter ? m_DraggingSplitter : HitTestSplitter(m_RootNode.get(), pos);
        if (!node)
            return UICursorShape::Default;
        return (node->SplitDir == UIDockSplitDir::Horizontal)
            ? UICursorShape::ResizeEW
            : UICursorShape::ResizeNS;
    }

    bool UIDockSpace::OnPointerLeave(const UIPointerEvent&)
    {
        m_HoveredSplitter = nullptr;
        return false;
    }

    bool UIDockSpace::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        if (auto* split = HitTestSplitter(m_RootNode.get(), e.Position))
        {
            m_DraggingSplitter = split;
            m_HoveredSplitter = split;
            return true;
        }
        return false;
    }

    bool UIDockSpace::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_DraggingSplitter)
        {
            const UIRect& b = m_DraggingSplitter->Bounds;
            if (m_DraggingSplitter->SplitDir == UIDockSplitDir::Horizontal)
            {
                const float w = std::max(1.0f, b.Width() - kSplitterThickness);
                const float minR = std::min(0.45f, kMinNodeExtent / w);
                const float r = (e.Position.x - b.Min.x) / w;
                m_DraggingSplitter->SplitRatio = std::clamp(r, minR, 1.0f - minR);
            }
            else if (m_DraggingSplitter->SplitDir == UIDockSplitDir::Vertical)
            {
                const float h = std::max(1.0f, b.Height() - kSplitterThickness);
                const float minR = std::min(0.45f, kMinNodeExtent / h);
                const float r = (e.Position.y - b.Min.y) / h;
                m_DraggingSplitter->SplitRatio = std::clamp(r, minR, 1.0f - minR);
            }
            return true;
        }

        m_HoveredSplitter = HitTestSplitter(m_RootNode.get(), e.Position);
        return m_HoveredSplitter != nullptr;
    }

    bool UIDockSpace::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_DraggingSplitter)
        {
            m_DraggingSplitter = nullptr;
            m_HoveredSplitter = HitTestSplitter(m_RootNode.get(), e.Position);
            return true;
        }
        return false;
    }

    UIVec2 UIDockSpace::ComputeDesiredSize()
    {
        for (const auto& child : GetChildren())
            child->Measure();
        return FixedSize;
    }

    void UIDockSpace::ArrangeNode(const Ref<UIDockNode>& node, const UIRect& rect)
    {
        if (!node)
            return;
        node->Bounds = rect;

        if (node->IsLeaf())
        {
            node->SplitterRect = {};
            if (node->TabPanel)
                node->TabPanel->Arrange(rect);
            return;
        }

        if (node->SplitDir == UIDockSplitDir::Horizontal)
        {
            const float avail = std::max(0.0f, rect.Width() - kSplitterThickness);
            const float firstW = std::floor(avail * node->SplitRatio);
            const float splitX = rect.Min.x + firstW;

            node->SplitterRect = {
                { splitX, rect.Min.y },
                { splitX + kSplitterThickness, rect.Max.y }
            };
            ArrangeNode(node->First, { rect.Min, { splitX, rect.Max.y } });
            ArrangeNode(node->Second, { { splitX + kSplitterThickness, rect.Min.y }, rect.Max });
        }
        else
        {
            const float avail = std::max(0.0f, rect.Height() - kSplitterThickness);
            const float firstH = std::floor(avail * node->SplitRatio);
            const float splitY = rect.Min.y + firstH;

            node->SplitterRect = {
                { rect.Min.x, splitY },
                { rect.Max.x, splitY + kSplitterThickness }
            };
            ArrangeNode(node->First, { rect.Min, { rect.Max.x, splitY } });
            ArrangeNode(node->Second, { { rect.Min.x, splitY + kSplitterThickness }, rect.Max });
        }
    }

    void UIDockSpace::ArrangeChildren(const UIRect& contentRect)
    {
        if (m_DockHost)
            m_DockHost->Arrange(contentRect);
        if (m_RootNode)
            ArrangeNode(m_RootNode, contentRect);
        if (m_SplitterHost)
            m_SplitterHost->Arrange(contentRect);
        if (m_FloatHost)
            m_FloatHost->Arrange(contentRect);
        if (m_OverlayHost)
            m_OverlayHost->Arrange(contentRect);
    }

    void UIDockSpace::PaintSplitters(UIDrawList& drawList, const UITheme& theme, const UIDockNode* node) const
    {
        if (!node || node->IsLeaf())
            return;

        const bool highlighted = (m_DraggingSplitter == node || m_HoveredSplitter == node);
        UIVec2 rMin = node->SplitterRect.Min;
        UIVec2 rMax = node->SplitterRect.Max;
        if (highlighted)
        {
            // Expand 1px seam to a 2px accent highlight on hover/drag for clear ImGui-style feedback.
            if (node->SplitDir == UIDockSplitDir::Horizontal)
            {
                rMin.x -= 0.5f;
                rMax.x += 0.5f;
            }
            else if (node->SplitDir == UIDockSplitDir::Vertical)
            {
                rMin.y -= 0.5f;
                rMax.y += 0.5f;
            }
        }
        const UIColor color = highlighted
            ? theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 0.95f })
            : theme.GetToken("Border", { 0.204f, 0.216f, 0.255f, 1.0f });
        drawList.AddRoundedRect(rMin, rMax, color);

        PaintSplitters(drawList, theme, node->First.get());
        PaintSplitters(drawList, theme, node->Second.get());
    }

    void UIDockSpace::PaintDockOverlay(UIDrawList& drawList, const UITheme& theme) const
    {
        if (!m_ExternalDragActive)
            return;

        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor accentHover = theme.GetToken("AccentHover", { 0.349f, 0.592f, 0.976f, 1.0f });
        const UIColor surface1 = theme.GetToken("Surface1", { 0.141f, 0.149f, 0.173f, 0.96f });
        const UIColor border = theme.GetToken("Border", { 0.204f, 0.216f, 0.255f, 1.0f });

        if (m_HoverDropZone != UIDockDropZone::None && m_HoverPreviewRect.Width() > 1.0f && m_HoverPreviewRect.Height() > 1.0f)
        {
            drawList.AddRoundedRect(
                m_HoverPreviewRect.Min,
                m_HoverPreviewRect.Max,
                { accent.r, accent.g, accent.b, m_HoverIsTabBarDrop ? 0.20f : 0.28f },
                { 4.0f, 4.0f, 4.0f, 4.0f },
                m_HoverIsTabBarDrop ? 1.5f : 2.0f,
                { accentHover.r, accentHover.g, accentHover.b, 0.95f }
            );
            if (m_HoverIsTabBarDrop)
            {
                drawList.AddRoundedRect(
                    { m_HoverTabInsertX - 1.5f, m_HoverPreviewRect.Min.y + 3.0f },
                    { m_HoverTabInsertX + 1.5f, m_HoverPreviewRect.Max.y - 3.0f },
                    accentHover,
                    { 1.5f, 1.5f, 1.5f, 1.5f }
                );
            }
        }

        // Draw 5-way compass on hovered leaf (or center of dockspace if empty)
        const UIRect& b = GetBounds();
        const UIRect targetRect = m_HoverTargetNode ? m_HoverTargetNode->Bounds : b;
        const UIVec2 c{ (targetRect.Min.x + targetRect.Max.x) * 0.5f, (targetRect.Min.y + targetRect.Max.y) * 0.5f };
        const UIVec2 bc{ (b.Min.x + b.Max.x) * 0.5f, (b.Min.y + b.Max.y) * 0.5f };
        constexpr float kHalfBtn = 16.0f;
        constexpr float kOffset = 38.0f;
        constexpr float kOuterInset = 24.0f;

        auto drawZoneBtn = [&](UIVec2 pos, UIDockDropZone z)
        {
            const bool active = (m_HoverDropZone == z);
            const UIRect r = CenterBox(pos, kHalfBtn);
            drawList.AddRoundedRect(
                r.Min,
                r.Max,
                active ? UIColor{ accent.r, accent.g, accent.b, 0.95f } : UIColor{ surface1.r, surface1.g, surface1.b, 0.94f },
                { 4.0f, 4.0f, 4.0f, 4.0f },
                1.5f,
                active ? UIColor{ 0.70f, 0.85f, 1.0f, 1.0f } : border
            );

            // Inner directional glyph so each dock square clearly shows placement
            const UIColor glyphCol = active ? UIColor{ 1.0f, 1.0f, 1.0f, 0.95f } : theme.GetToken("TextMuted", { 0.60f, 0.63f, 0.68f, 0.80f });
            const UIRect inner = { { r.Min.x + 5.0f, r.Min.y + 5.0f }, { r.Max.x - 5.0f, r.Max.y - 5.0f } };
            UIRect g = inner;
            if (z == UIDockDropZone::Left || z == UIDockDropZone::OuterLeft)
                g.Max.x = inner.Min.x + inner.Width() * 0.45f;
            else if (z == UIDockDropZone::Right || z == UIDockDropZone::OuterRight)
                g.Min.x = inner.Max.x - inner.Width() * 0.45f;
            else if (z == UIDockDropZone::Top || z == UIDockDropZone::OuterTop)
                g.Max.y = inner.Min.y + inner.Height() * 0.45f;
            else if (z == UIDockDropZone::Bottom || z == UIDockDropZone::OuterBottom)
                g.Min.y = inner.Max.y - inner.Height() * 0.45f;
            drawList.AddRoundedRect(g.Min, g.Max, glyphCol, { 2.0f, 2.0f, 2.0f, 2.0f });
        };

        if (!m_RootNode)
        {
            drawZoneBtn(bc, UIDockDropZone::Center);
            return;
        }

        if (m_HoverTargetNode)
        {
            if (!HasFlag(m_HoverTargetNode->Flags, UIDockNodeFlags::NoDockingOver))
                drawZoneBtn(c, UIDockDropZone::Center);
            drawZoneBtn({ c.x - kOffset, c.y }, UIDockDropZone::Left);
            drawZoneBtn({ c.x + kOffset, c.y }, UIDockDropZone::Right);
            drawZoneBtn({ c.x, c.y - kOffset }, UIDockDropZone::Top);
            drawZoneBtn({ c.x, c.y + kOffset }, UIDockDropZone::Bottom);
        }

        drawZoneBtn({ b.Min.x + kOuterInset, bc.y }, UIDockDropZone::OuterLeft);
        drawZoneBtn({ b.Max.x - kOuterInset, bc.y }, UIDockDropZone::OuterRight);
        drawZoneBtn({ bc.x, b.Min.y + kOuterInset }, UIDockDropZone::OuterTop);
        drawZoneBtn({ bc.x, b.Max.y - kOuterInset }, UIDockDropZone::OuterBottom);
    }

    void UIDockSpace::PaintSplittersOverlay(UIDrawList& drawList, const UITheme& theme) const
    {
        PaintSplitters(drawList, theme, m_RootNode.get());
    }

    void UIDockSpace::PaintOverlay(UIDrawList& drawList, const UITheme& theme) const
    {
        PaintDockOverlay(drawList, theme);
    }

    void UIDockSpace::ClearPanels()
    {
        m_RootNode = nullptr;
        m_FloatingWindows.clear();
        m_DraggingSplitter = nullptr;
        m_HoveredSplitter = nullptr;
        ClearDockPreview();
        if (m_DockHost)
            m_DockHost->ClearChildren();
        if (m_FloatHost)
            m_FloatHost->ClearChildren();
    }

    namespace
    {
        void SerializeDockNode(YAML::Emitter& out, const UIDockNode* node)
        {
            if (!node)
                return;

            out << YAML::BeginMap;
            if (node->IsLeaf())
            {
                out << YAML::Key << "type" << YAML::Value << "leaf";
                out << YAML::Key << "flags" << YAML::Value << static_cast<uint32_t>(node->Flags);
                out << YAML::Key << "active_tab" << YAML::Value
                    << (node->TabPanel ? node->TabPanel->GetActiveTab() : 0);
                out << YAML::Key << "tabs" << YAML::Value << YAML::BeginSeq;
                if (node->TabPanel)
                {
                    for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
                        out << node->TabPanel->GetTabTitle(i);
                }
                out << YAML::EndSeq;
            }
            else
            {
                out << YAML::Key << "type" << YAML::Value << "split";
                out << YAML::Key << "dir" << YAML::Value
                    << (node->SplitDir == UIDockSplitDir::Horizontal ? "horizontal" : "vertical");
                out << YAML::Key << "ratio" << YAML::Value << node->SplitRatio;
                out << YAML::Key << "first" << YAML::Value;
                SerializeDockNode(out, node->First.get());
                out << YAML::Key << "second" << YAML::Value;
                SerializeDockNode(out, node->Second.get());
            }
            out << YAML::EndMap;
        }

        struct SavedPanelInfo
        {
            Ref<UIElement> Content;
            UIDockNodeFlags Flags = UIDockNodeFlags::None;
        };

        void CollectDockPanels(
            const UIDockNode* node,
            std::unordered_map<std::string, SavedPanelInfo>& outMap,
            std::vector<std::string>& outOrder)
        {
            if (!node)
                return;
            if (node->IsLeaf() && node->TabPanel)
            {
                for (size_t i = 0; i < node->TabPanel->GetTabCount(); ++i)
                {
                    const std::string& title = node->TabPanel->GetTabTitle(i);
                    if (!outMap.contains(title))
                        outOrder.push_back(title);
                    outMap[title] = { node->TabPanel->GetTabContent(i), node->Flags };
                }
                return;
            }
            CollectDockPanels(node->First.get(), outMap, outOrder);
            CollectDockPanels(node->Second.get(), outMap, outOrder);
        }
    }

    bool UIDockSpace::SaveLayout(const std::filesystem::path& path) const
    {
        try
        {
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path());

            YAML::Emitter out;
            out << YAML::BeginMap;
            out << YAML::Key << "version" << YAML::Value << 2;
            if (m_RootNode)
            {
                out << YAML::Key << "root" << YAML::Value;
                SerializeDockNode(out, m_RootNode.get());
            }
            out << YAML::Key << "floating" << YAML::Value << YAML::BeginSeq;
            for (const auto& win : m_FloatingWindows)
            {
                if (!win)
                    continue;
                out << YAML::BeginMap;
                out << YAML::Key << "title" << YAML::Value << win->GetTitle();
                out << YAML::Key << "x" << YAML::Value << win->AnchorOffsets.Left;
                out << YAML::Key << "y" << YAML::Value << win->AnchorOffsets.Top;
                out << YAML::Key << "w" << YAML::Value << win->FixedSize.x;
                out << YAML::Key << "h" << YAML::Value << win->FixedSize.y;
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap;

            std::ofstream stream(path, std::ios::trunc);
            if (!stream.is_open())
                return false;
            stream << out.c_str();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool UIDockSpace::LoadLayout(const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        if (!stream.is_open())
            return false;

        try
        {
            std::stringstream ss;
            ss << stream.rdbuf();
            YAML::Node doc = YAML::Load(ss.str());
            if (!doc || !doc.IsMap() || doc["version"].as<int>(0) != 2)
                return false;

            std::unordered_map<std::string, SavedPanelInfo> panels;
            std::vector<std::string> panelOrder;
            CollectDockPanels(m_RootNode.get(), panels, panelOrder);

            for (const auto& win : m_FloatingWindows)
            {
                if (!win || !win->GetContentBox() || win->GetContentBox()->GetChildren().empty())
                    continue;
                const std::string& title = win->GetTitle();
                if (!panels.contains(title))
                    panelOrder.push_back(title);
                panels[title] = { win->GetContentBox()->GetChildren().front(), UIDockNodeFlags::None };
            }

            if (panels.empty())
                return false;

            std::unordered_map<std::string, bool> consumed;
            std::function<Ref<UIDockNode>(const YAML::Node&)> buildNode = [&](const YAML::Node& yaml) -> Ref<UIDockNode>
            {
                if (!yaml || !yaml.IsMap())
                    return nullptr;

                const std::string type = yaml["type"].as<std::string>("");
                if (type == "leaf")
                {
                    Ref<UIDockNode> leaf(new UIDockNode());
                    leaf->Flags = static_cast<UIDockNodeFlags>(yaml["flags"].as<uint32_t>(0));
                    leaf->TabPanel = CreateLeafTabPanel();

                    if (const YAML::Node tabs = yaml["tabs"]; tabs && tabs.IsSequence())
                    {
                        for (const auto& t : tabs)
                        {
                            const std::string title = t.as<std::string>("");
                            auto it = panels.find(title);
                            if (it != panels.end() && !consumed[title])
                            {
                                consumed[title] = true;
                                leaf->TabPanel->AddTab(title, it->second.Content);
                                leaf->Flags = leaf->Flags | it->second.Flags;
                                if (auto pit = m_PanelFlags.find(title); pit != m_PanelFlags.end())
                                    leaf->Flags = leaf->Flags | pit->second;
                            }
                        }
                    }

                    if (leaf->TabPanel->GetTabCount() == 0)
                        return nullptr;

                    if (HasFlag(leaf->Flags, UIDockNodeFlags::NoTabBar))
                    {
                        leaf->TabPanel->SetTabBarVisible(false);
                        leaf->TabPanel->SetShowUndockCornerHandle(!HasFlag(leaf->Flags, UIDockNodeFlags::NoUndocking));
                        leaf->TabPanel->GetContentHost()->Padding = UIThickness::All(0.0f);
                    }

                    const size_t activeTab = yaml["active_tab"].as<size_t>(0);
                    leaf->TabPanel->SetActiveTab(std::min(activeTab, leaf->TabPanel->GetTabCount() - 1));
                    return leaf;
                }

                if (type == "split")
                {
                    Ref<UIDockNode> first = buildNode(yaml["first"]);
                    Ref<UIDockNode> second = buildNode(yaml["second"]);
                    if (!first)
                        return second;
                    if (!second)
                        return first;

                    Ref<UIDockNode> split(new UIDockNode());
                    const std::string dir = yaml["dir"].as<std::string>("horizontal");
                    split->SplitDir = (dir == "vertical") ? UIDockSplitDir::Vertical : UIDockSplitDir::Horizontal;
                    split->SplitRatio = std::clamp(yaml["ratio"].as<float>(0.5f), 0.025f, 0.975f);
                    split->First = first;
                    split->Second = second;
                    return split;
                }

                return nullptr;
            };

            ClearPanels();
            if (doc["root"])
                m_RootNode = buildNode(doc["root"]);

            if (const YAML::Node floating = doc["floating"]; floating && floating.IsSequence())
            {
                for (const auto& fw : floating)
                {
                    const std::string title = fw["title"].as<std::string>("");
                    auto it = panels.find(title);
                    if (it == panels.end() || consumed[title])
                        continue;
                    consumed[title] = true;
                    FloatPanel(
                        title,
                        it->second.Content,
                        { fw["x"].as<float>(160.0f), fw["y"].as<float>(120.0f) },
                        { fw["w"].as<float>(440.0f), fw["h"].as<float>(320.0f) },
                        false
                    );
                }
            }

            // Re-dock any newly added panels not present in the saved layout.
            for (const std::string& title : panelOrder)
            {
                if (consumed[title])
                    continue;
                const auto& info = panels[title];
                AddPanel(title, info.Content, UIDockDropZone::Center, {}, 0.5f, info.Flags);
            }

            CompactTree();
            return m_RootNode != nullptr || !m_FloatingWindows.empty();
        }
        catch (...)
        {
            return false;
        }
    }

    void UIDockSpace::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);
    }
}
