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
    }

    bool UIWindow::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        const UIRect& b = GetBounds();
        const UIRect resizeRect{
            { b.Max.x - kResizeGripSize, b.Max.y - kResizeGripSize },
            b.Max
        };
        if (resizeRect.Contains(e.Position))
        {
            m_DraggingResize = true;
            return true;
        }

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
            FixedSize.x = std::max(MinSize.x, FixedSize.x + e.Delta.x);
            FixedSize.y = std::max(MinSize.y, FixedSize.y + e.Delta.y);
            return true;
        }
        return false;
    }

    bool UIWindow::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && (m_DraggingMove || m_DraggingResize))
        {
            const bool wasMove = m_DraggingMove;
            m_DraggingMove = false;
            m_DraggingResize = false;
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
        drawList.AddRoundedRect(
            b.Min,
            { b.Max.x, b.Min.y + m_TitleBarHeight },
            theme.GetToken("Surface1", { 0.145f, 0.153f, 0.180f, 1.0f }),
            { 6.0f, 6.0f, 0.0f, 0.0f }
        );
        drawList.AddRoundedRect(
            { b.Min.x, b.Min.y + m_TitleBarHeight - 1.0f },
            { b.Max.x, b.Min.y + m_TitleBarHeight },
            { 0.26f, 0.30f, 0.38f, 0.9f }
        );

        // Bottom-right resize grip indicator
        drawList.AddBezier(
            { b.Max.x - 10.0f, b.Max.y - 3.0f },
            { b.Max.x - 6.5f, b.Max.y - 6.5f },
            { b.Max.x - 6.5f, b.Max.y - 6.5f },
            { b.Max.x - 3.0f, b.Max.y - 10.0f },
            1.5f,
            { 0.55f, 0.60f, 0.70f, 0.9f }
        );
    }
}
