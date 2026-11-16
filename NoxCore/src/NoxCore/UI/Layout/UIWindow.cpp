#include "UIWindow.h"

#include <algorithm>

#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIWindow::UIWindow(std::string title, std::function<void()> onClose)
        : OnClose(std::move(onClose))
    {
        StyleClass = "Panel";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;

        WidthMode = UISizeMode::Fixed;
        HeightMode = UISizeMode::Fixed;
        FixedSize = { 380.0f, 260.0f };
        MinSize = { 220.0f, 140.0f };
        AnchorMin = { 0.0f, 0.0f };
        AnchorMax = { 0.0f, 0.0f };
        AnchorOffsets = { 160.0f, 120.0f, 0.0f, 0.0f };

        m_TitleLabel = Ref<UILabel>(new UILabel(std::move(title)));
        m_TitleLabel->HitTestMode = UIHitTestMode::None;
        m_TitleLabel->VAlign = UIAlignment::Center;
        AddChild(m_TitleLabel);

        m_CloseBtn = Ref<UIButton>(new UIButton("X", [this]()
        {
            auto cb = OnClose;
            if (cb)
                cb();
        }));
        m_CloseBtn->StyleClass = "TitleClose";
        m_CloseBtn->WidthMode = UISizeMode::Fixed;
        m_CloseBtn->HeightMode = UISizeMode::Fixed;
        AddChild(m_CloseBtn);

        m_Content = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 8.0f));
        m_Content->Padding = UIThickness::All(10.0f);
        AddChild(m_Content);
    }

    void UIWindow::SetTitle(const std::string& title)
    {
        if (m_TitleLabel)
            m_TitleLabel->Text = title;
    }

    const std::string& UIWindow::GetTitle() const
    {
        static const std::string kEmpty;
        return m_TitleLabel ? m_TitleLabel->Text : kEmpty;
    }

    void UIWindow::AddContent(const Ref<UIElement>& child)
    {
        if (m_Content && child)
            m_Content->AddChild(child);
    }

    void UIWindow::BeginProgrammaticDrag()
    {
        m_DraggingMove = true;
        m_DraggingResize = false;
        m_ResizeEdges = EdgeNone;
    }

    uint8_t UIWindow::HitTestResizeEdge(const UIVec2& pos) const
    {
        const UIRect& b = GetBounds();
        if (!b.Contains(pos) || pos.y < b.Min.y + m_TitleBarHeight)
            return EdgeNone;

        const bool inCornerBand = (pos.y >= b.Max.y - kResizeCornerExtent);
        const float sideThreshold = inCornerBand ? kResizeCornerExtent : kResizeEdgeThickness;
        const float bottomThreshold = (pos.x <= b.Min.x + kResizeCornerExtent || pos.x >= b.Max.x - kResizeCornerExtent)
            ? kResizeCornerExtent
            : kResizeEdgeThickness;

        uint8_t edges = EdgeNone;
        if (pos.x <= b.Min.x + sideThreshold)
            edges |= EdgeLeft;
        else if (pos.x >= b.Max.x - sideThreshold)
            edges |= EdgeRight;
        if (pos.y >= b.Max.y - bottomThreshold)
            edges |= EdgeBottom;
        return edges;
    }

    UIElement* UIWindow::HitTest(const UIVec2& pos)
    {
        if (!Visible || HitTestMode == UIHitTestMode::None)
            return nullptr;
        if (!GetBounds().Contains(pos))
            return nullptr;
        if (HitTestResizeEdge(pos) != EdgeNone)
            return this;
        return UIElement::HitTest(pos);
    }

    UICursorShape UIWindow::GetCursorShape(const UIVec2& pos) const
    {
        const uint8_t edges = m_DraggingResize ? m_ResizeEdges : HitTestResizeEdge(pos);
        if (edges == (EdgeRight | EdgeBottom))
            return UICursorShape::ResizeNWSE;
        if (edges == (EdgeLeft | EdgeBottom))
            return UICursorShape::ResizeNESW;
        if ((edges & (EdgeLeft | EdgeRight)) != 0)
            return UICursorShape::ResizeEW;
        if ((edges & EdgeBottom) != 0)
            return UICursorShape::ResizeNS;
        return UICursorShape::Default;
    }

    bool UIWindow::OnPointerLeave(const UIPointerEvent&)
    {
        m_HoverResizeEdges = EdgeNone;
        return false;
    }

    bool UIWindow::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        if (OnActivate)
            OnActivate();

        const uint8_t edges = HitTestResizeEdge(e.Position);
        if (edges != EdgeNone)
        {
            m_DraggingResize = true;
            m_ResizeEdges = edges;
            m_HoverResizeEdges = edges;
            return true;
        }

        const UIRect& b = GetBounds();
        const UIRect titleRect{
            b.Min,
            { b.Max.x, b.Min.y + m_TitleBarHeight }
        };
        if (titleRect.Contains(e.Position))
        {
            m_DraggingMove = true;
            return true;
        }

        return true;
    }

    bool UIWindow::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_DraggingMove)
        {
            AnchorOffsets.Left += e.Delta.x;
            AnchorOffsets.Top += e.Delta.y;
            auto cb = OnDragMove;
            if (cb)
                cb(e.Position);
            return true;
        }
        if (m_DraggingResize)
        {
            if ((m_ResizeEdges & EdgeLeft) != 0)
            {
                const float newW = std::max(MinSize.x, FixedSize.x - e.Delta.x);
                const float applied = FixedSize.x - newW;
                AnchorOffsets.Left += applied;
                FixedSize.x = newW;
            }
            if ((m_ResizeEdges & EdgeRight) != 0)
            {
                FixedSize.x = std::max(MinSize.x, FixedSize.x + e.Delta.x);
            }
            if ((m_ResizeEdges & EdgeBottom) != 0)
            {
                FixedSize.y = std::max(MinSize.y, FixedSize.y + e.Delta.y);
            }
            return true;
        }
        m_HoverResizeEdges = HitTestResizeEdge(e.Position);
        return m_HoverResizeEdges != EdgeNone;
    }

    bool UIWindow::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && (m_DraggingMove || m_DraggingResize))
        {
            const bool wasMove = m_DraggingMove;
            m_DraggingMove = false;
            m_DraggingResize = false;
            m_ResizeEdges = EdgeNone;
            m_HoverResizeEdges = HitTestResizeEdge(e.Position);
            if (wasMove)
            {
                auto cb = OnDragEnd;
                if (cb)
                    cb(e.Position);
            }
            return true;
        }
        return false;
    }

    UIVec2 UIWindow::ComputeDesiredSize()
    {
        const UIVec2 titleSz = m_TitleLabel ? m_TitleLabel->Measure() : UIVec2{};
        if (m_CloseBtn)
            m_CloseBtn->Measure();
        const UIVec2 contentSz = m_Content ? m_Content->Measure() : UIVec2{};
        m_TitleBarHeight = std::max(kTitleBarHeight, titleSz.y + 10.0f);
        return {
            std::max(titleSz.x + m_TitleBarHeight + 20.0f, contentSz.x),
            m_TitleBarHeight + contentSz.y
        };
    }

    void UIWindow::ArrangeChildren(const UIRect& contentRect)
    {
        (void)contentRect;
        const UIRect& b = GetBounds();
        const UIVec2 titleSz = m_TitleLabel ? m_TitleLabel->GetDesiredSize() : UIVec2{};
        m_TitleBarHeight = std::max(kTitleBarHeight, titleSz.y + 10.0f);
        const float closeW = m_TitleBarHeight;
        if (m_TitleLabel)
        {
            m_TitleLabel->Arrange({
                { b.Min.x + 10.0f, b.Min.y },
                { b.Max.x - closeW - 6.0f, b.Min.y + m_TitleBarHeight }
            });
        }
        if (m_CloseBtn)
        {
            m_CloseBtn->FixedSize = { closeW, m_TitleBarHeight };
            m_CloseBtn->Arrange({
                { b.Max.x - closeW, b.Min.y },
                { b.Max.x, b.Min.y + m_TitleBarHeight }
            });
        }
        if (m_Content)
        {
            m_Content->Arrange({
                { b.Min.x, b.Min.y + m_TitleBarHeight },
                { b.Max.x, b.Max.y }
            });
        }
    }

    void UIWindow::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const UIColor border = theme.GetToken("Border", { 0.204f, 0.216f, 0.255f, 1.0f });
        const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
        const UIColor muted = theme.GetToken("TextMuted", { 0.580f, 0.616f, 0.686f, 1.0f });
        drawList.AddRoundedRect(
            b.Min,
            { b.Max.x, b.Min.y + m_TitleBarHeight },
            theme.GetToken("Surface1", { 0.141f, 0.149f, 0.173f, 1.0f }),
            { 4.0f, 4.0f, 0.0f, 0.0f }
        );
        drawList.AddRoundedRect(
            { b.Min.x, b.Min.y + m_TitleBarHeight - 1.0f },
            { b.Max.x, b.Min.y + m_TitleBarHeight },
            border
        );

        // Highlight hovered/active window edge and subtle corner grip cue.
        const uint8_t activeEdges = m_DraggingResize ? m_ResizeEdges : m_HoverResizeEdges;
        if ((activeEdges & EdgeLeft) != 0)
            drawList.AddRoundedRect({ b.Min.x, b.Min.y + m_TitleBarHeight }, { b.Min.x + 2.0f, b.Max.y }, accent);
        if ((activeEdges & EdgeRight) != 0)
            drawList.AddRoundedRect({ b.Max.x - 2.0f, b.Min.y + m_TitleBarHeight }, b.Max, accent);
        if ((activeEdges & EdgeBottom) != 0)
            drawList.AddRoundedRect({ b.Min.x, b.Max.y - 2.0f }, b.Max, accent);

        drawList.AddBezier(
            { b.Max.x - 10.0f, b.Max.y - 3.0f },
            { b.Max.x - 6.5f, b.Max.y - 6.5f },
            { b.Max.x - 6.5f, b.Max.y - 6.5f },
            { b.Max.x - 3.0f, b.Max.y - 10.0f },
            1.5f,
            { muted.r, muted.g, muted.b, 0.7f }
        );
    }
}
