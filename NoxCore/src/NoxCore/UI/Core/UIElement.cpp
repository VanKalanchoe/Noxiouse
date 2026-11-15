#include "UIElement.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/Core/Log.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    namespace
    {
        std::atomic<uint64_t> s_NextElementId{ 1 };

        UIVec2 SanitizeSize(const UIVec2& v)
        {
            return {
                std::isfinite(v.x) ? std::max(0.0f, v.x) : 0.0f,
                std::isfinite(v.y) ? std::max(0.0f, v.y) : 0.0f
            };
        }
    }

    UIElement::UIElement()
        : m_Id(s_NextElementId.fetch_add(1, std::memory_order_relaxed))
    {
    }

    UIElement::~UIElement()
    {
        for (const auto& child : m_Children)
        {
            if (child && child->m_Parent == this)
                child->m_Parent = nullptr;
        }
    }

    void UIElement::AddChild(const Ref<UIElement>& child)
    {
        if (!child || child.get() == this)
            return;

        // Guard against cyclic parenting
        for (UIElement* p = m_Parent; p != nullptr; p = p->m_Parent)
        {
            if (p == child.get())
            {
                NOX_CORE_ERROR("UIElement::AddChild: cyclic hierarchy detected");
                return;
            }
        }

        if (child->m_Parent)
            child->m_Parent->RemoveChild(child);

        child->m_Parent = this;
        m_Children.push_back(child);
    }

    void UIElement::RemoveChild(const Ref<UIElement>& child)
    {
        if (!child)
            return;
        auto it = std::find(m_Children.begin(), m_Children.end(), child);
        if (it != m_Children.end())
        {
            (*it)->m_Parent = nullptr;
            m_Children.erase(it);
        }
    }

    void UIElement::ClearChildren()
    {
        for (auto& c : m_Children)
            c->m_Parent = nullptr;
        m_Children.clear();
    }

    UIVec2 UIElement::Measure()
    {
        if (!Visible)
        {
            m_DesiredSize = { 0.0f, 0.0f };
            return m_DesiredSize;
        }

        UIVec2 desired = ComputeDesiredSize();
        if (WidthMode == UISizeMode::Fixed)
            desired.x = FixedSize.x;
        if (HeightMode == UISizeMode::Fixed)
            desired.y = FixedSize.y;

        desired = glm::clamp(SanitizeSize(desired), MinSize, MaxSize);
        m_DesiredSize = {
            desired.x + Margin.Horizontal(),
            desired.y + Margin.Vertical()
        };
        return m_DesiredSize;
    }

    void UIElement::Arrange(const UIRect& allottedRect)
    {
        if (!Visible)
        {
            m_Bounds = {};
            return;
        }

        m_Bounds = allottedRect.Inset(Margin);
        const UIRect contentRect = m_Bounds.Inset(Padding);
        ArrangeChildren(contentRect);
    }

    UIVec2 UIElement::ComputeDesiredSize()
    {
        UIVec2 maxChild{ 0.0f, 0.0f };
        for (const auto& child : m_Children)
            maxChild = glm::max(maxChild, child->Measure());

        return {
            maxChild.x + Padding.Horizontal(),
            maxChild.y + Padding.Vertical()
        };
    }

    void UIElement::ArrangeChildren(const UIRect& contentRect)
    {
        for (const auto& child : m_Children)
        {
            if (!child->Visible)
                continue;

            const UIVec2 desired = child->GetDesiredSize();
            UIRect childRect = contentRect;

            if (child->HAlign != UIAlignment::Stretch && child->WidthMode != UISizeMode::Fill)
            {
                const float w = std::min(desired.x, contentRect.Width());
                if (child->HAlign == UIAlignment::Center)
                    childRect.Min.x = contentRect.Min.x + (contentRect.Width() - w) * 0.5f;
                else if (child->HAlign == UIAlignment::End)
                    childRect.Min.x = contentRect.Max.x - w;
                childRect.Max.x = childRect.Min.x + w;
            }

            if (child->VAlign != UIAlignment::Stretch && child->HeightMode != UISizeMode::Fill)
            {
                const float h = std::min(desired.y, contentRect.Height());
                if (child->VAlign == UIAlignment::Center)
                    childRect.Min.y = contentRect.Min.y + (contentRect.Height() - h) * 0.5f;
                else if (child->VAlign == UIAlignment::End)
                    childRect.Min.y = contentRect.Max.y - h;
                childRect.Max.y = childRect.Min.y + h;
            }

            child->Arrange(childRect);
        }
    }

    void UIElement::UpdateStylesRecursive(const UITheme& theme, float dt)
    {
        if (!Visible)
            return;

        OnUpdate(dt);

        const UIStyleBox target = theme.Resolve(StyleClass, StyleVariant, m_State);
        if (!m_HasResolvedStyle || target.TransitionSeconds <= 0.0f)
        {
            m_CurrentStyle = target;
            m_HasResolvedStyle = true;
        }
        else
        {
            const float alpha = 1.0f - std::exp(-dt / std::max(target.TransitionSeconds, 0.001f));
            m_CurrentStyle = UIStyleBox::Lerp(m_CurrentStyle, target, alpha);
        }

        for (const auto& child : m_Children)
            child->UpdateStylesRecursive(theme, dt);
    }

    void UIElement::PaintRecursive(UIDrawList& drawList, const UITheme& theme)
    {
        if (!Visible)
            return;

        OnPaint(drawList, theme);

        if (ClipChildren)
            drawList.PushClipRect(m_Bounds.Min, m_Bounds.Max, m_CurrentStyle.CornerRadius.ToVec4());

        for (const auto& child : m_Children)
            child->PaintRecursive(drawList, theme);

        if (ClipChildren)
            drawList.PopClipRect();
    }

    void UIElement::OnPaint(UIDrawList& drawList, const UITheme&)
    {
        if (!DrawBackground)
            return;

        if (m_CurrentStyle.UseGradient)
        {
            drawList.AddRoundedRectGradient(
                m_Bounds.Min, m_Bounds.Max,
                m_CurrentStyle.FillColor, m_CurrentStyle.FillColorBottom,
                m_CurrentStyle.CornerRadius.ToVec4(),
                m_CurrentStyle.BorderThickness, m_CurrentStyle.BorderColor,
                m_CurrentStyle.ShadowColor, m_CurrentStyle.ShadowOffset, m_CurrentStyle.ShadowBlur
            );
        }
        else
        {
            drawList.AddRoundedRect(
                m_Bounds.Min, m_Bounds.Max,
                m_CurrentStyle.FillColor,
                m_CurrentStyle.CornerRadius.ToVec4(),
                m_CurrentStyle.BorderThickness, m_CurrentStyle.BorderColor,
                m_CurrentStyle.ShadowColor, m_CurrentStyle.ShadowOffset, m_CurrentStyle.ShadowBlur
            );
        }
    }

    UIElement* UIElement::HitTest(const UIVec2& pos)
    {
        if (!Visible || HitTestMode == UIHitTestMode::None)
            return nullptr;

        if (ClipChildren && !m_Bounds.Contains(pos))
            return nullptr;

        // Reverse order: last painted child is visually on top.
        for (auto it = m_Children.rbegin(); it != m_Children.rend(); ++it)
        {
            if (UIElement* hit = (*it)->HitTest(pos))
                return hit;
        }

        if (HitTestMode == UIHitTestMode::Self && m_Bounds.Contains(pos))
            return this;

        return nullptr;
    }

    void UIElement::SetStateFlag(UIState flag, bool enabled)
    {
        const uint8_t bit = static_cast<uint8_t>(flag);
        uint8_t val = static_cast<uint8_t>(m_State);
        if (enabled)
            val |= bit;
        else
            val &= ~bit;
        m_State = static_cast<UIState>(val);
    }
}
