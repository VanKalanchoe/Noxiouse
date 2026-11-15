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

        m_TitleLabel = Ref<UILabel>(new UILabel(std::move(title), 14.0f));
        m_TitleLabel->HitTestMode = UIHitTestMode::None;
        m_TitleLabel->VAlign = UIAlignment::Center;
        AddChild(m_TitleLabel);

        m_CloseBtn = Ref<UIButton>(new UIButton("X", [this]()
        {
            if (OnClose)
                OnClose();
        }));
        m_CloseBtn->WidthMode = UISizeMode::Fixed;
        m_CloseBtn->HeightMode = UISizeMode::Fixed;
        m_CloseBtn->FixedSize = { 22.0f, 22.0f };
        m_CloseBtn->Padding = UIThickness::All(0.0f);
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

    void UIWindow::AddContent(const Ref<UIElement>& child)
    {
        if (m_Content && child)
            m_Content->AddChild(child);
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
            { b.Max.x, b.Min.y + kTitleBarHeight }
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
            AnchorOffsets.Left = std::max(0.0f, AnchorOffsets.Left + e.Delta.x);
            AnchorOffsets.Top = std::max(0.0f, AnchorOffsets.Top + e.Delta.y);
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
            m_DraggingMove = false;
            m_DraggingResize = false;
            return true;
        }
        return false;
    }

    UIVec2 UIWindow::ComputeDesiredSize()
    {
        return FixedSize;
    }

    void UIWindow::ArrangeChildren(const UIRect& contentRect)
    {
        const UIRect& b = GetBounds();
        if (m_TitleLabel)
        {
            m_TitleLabel->Arrange({
                { b.Min.x + 10.0f, b.Min.y },
                { b.Max.x - 36.0f, b.Min.y + kTitleBarHeight }
            });
        }
        if (m_CloseBtn)
        {
            m_CloseBtn->Arrange({
                { b.Max.x - 28.0f, b.Min.y + 4.0f },
                { b.Max.x - 6.0f, b.Min.y + 26.0f }
            });
        }
        if (m_Content)
        {
            m_Content->Arrange({
                { b.Min.x, b.Min.y + kTitleBarHeight },
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
            { b.Max.x, b.Min.y + kTitleBarHeight },
            { 0.14f, 0.16f, 0.22f, 1.0f },
            { 6.0f, 6.0f, 0.0f, 0.0f }
        );
        drawList.AddRoundedRect(
            { b.Min.x, b.Min.y + kTitleBarHeight - 1.0f },
            { b.Max.x, b.Min.y + kTitleBarHeight },
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
