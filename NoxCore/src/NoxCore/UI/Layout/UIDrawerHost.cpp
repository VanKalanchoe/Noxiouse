#include "UIDrawerHost.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    namespace
    {
        // The drawer's top edge: drag it to make the drawer taller or shorter.
        class UIDrawerGrip final : public UIElement
        {
        public:
            explicit UIDrawerGrip(std::function<void(float dy)> onDrag) : m_OnDrag(std::move(onDrag))
            {
                HitTestMode = UIHitTestMode::Self;
                WidthMode = UISizeMode::Fill;
                HeightMode = UISizeMode::Fixed;
                FixedSize = { 0.0f, 6.0f };
                Tooltip = "Drag to resize the drawer";
            }

            UICursorShape GetCursorShape(const UIVec2&) const override { return UICursorShape::ResizeNS; }
            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button != UIMouseButton::Left)
                    return false;
                m_Dragging = true;
                m_LastY = e.Position.y;
                if (UIContext* ctx = UIContext::GetCurrent())
                    ctx->CancelPendingDrag();
                return true;
            }
            bool OnPointerMove(const UIPointerEvent& e) override
            {
                if (!m_Dragging)
                    return false;
                if (m_OnDrag)
                    m_OnDrag(e.Position.y - m_LastY);
                m_LastY = e.Position.y;
                return true;
            }
            bool OnPointerUp(const UIPointerEvent&) override
            {
                m_Dragging = false;
                return true;
            }
            bool OnPointerEnter(const UIPointerEvent&) override { m_Hovered = true; return false; }
            bool OnPointerLeave(const UIPointerEvent&) override { m_Hovered = false; return false; }

        protected:
            void OnPaint(UIDrawList& d, const UITheme& theme) override
            {
                const UIRect& b = GetBounds();
                const bool lit = m_Hovered || m_Dragging;
                const UIColor line = lit ? theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f }) : theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
                d.AddRoundedRect({ b.Min.x, b.Min.y }, { b.Max.x, b.Min.y + (lit ? 2.0f : 1.0f) }, line);
                const float cx = b.Center().x;
                d.AddRoundedRect({ cx - 18.0f, b.Min.y + 2.0f }, { cx + 18.0f, b.Min.y + 4.0f }, { line.r, line.g, line.b, 0.9f }, glm::vec4(1.0f));
            }

        private:
            std::function<void(float)> m_OnDrag;
            bool m_Dragging = false;
            bool m_Hovered = false;
            float m_LastY = 0.0f;
        };

        // The drawer: on the panel colour, a shadow over what it covers; its empty parts take the pointer too.
        class UIDrawerFrame final : public UIStackBox
        {
        public:
            UIDrawerFrame() : UIStackBox(UIOrientation::Vertical, 0.0f)
            {
                WidthMode = UISizeMode::Fill;
                HeightMode = UISizeMode::Fill;
                HitTestMode = UIHitTestMode::Self;
            }

        protected:
            void OnPaint(UIDrawList& d, const UITheme& theme) override
            {
                const UIRect& b = GetBounds();
                d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Surface0", { 0.094f, 0.11f, 0.149f, 1.0f }), glm::vec4(0.0f), 0.0f, {},
                                 { 0.0f, 0.0f, 0.0f, 0.55f }, { 0.0f, -4.0f }, 18.0f);
            }
        };
    }

    UIDrawerHost::UIDrawerHost(Ref<UIElement> content)
    {
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Fill;

        Ref<UIDrawerFrame> frame(new UIDrawerFrame());
        frame->AddChild(Ref<UIElement>(new UIDrawerGrip([this](float dy)
        {
            const float height = GetBounds().Height();
            if (height < 1.0f)
                return;
            HeightFraction = std::clamp(HeightFraction - dy / height, MinFraction, MaxFraction);
            if (OnResized)
                OnResized(HeightFraction);
        })));
        m_PageHost = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 0.0f));
        m_PageHost->WidthMode = UISizeMode::Fill;
        m_PageHost->HeightMode = UISizeMode::Fill;
        frame->AddChild(m_PageHost);
        frame->Visible = false;
        m_Frame = frame;
        SetContent(content);
    }

    void UIDrawerHost::SetContent(const Ref<UIElement>& content)
    {
        ClearChildren();
        m_Content = content;
        if (m_Content)
            AddChild(m_Content);
        AddChild(m_Frame); // (after the content: drawn and hit over it)
    }

    void UIDrawerHost::SetPage(const Ref<UIElement>& page)
    {
        if (GetPage() == page)
            return;
        m_PageHost->ClearChildren();
        if (page)
        {
            m_PageHost->AddChild(page);
            page->Visible = true;
        }
    }

    Ref<UIElement> UIDrawerHost::GetPage() const
    {
        return m_PageHost->GetChildren().empty() ? Ref<UIElement>() : m_PageHost->GetChildren().front();
    }

    UIRect UIDrawerHost::DrawerRect() const
    {
        return m_Frame && m_Frame->Visible ? m_Frame->GetBounds() : UIRect{};
    }

    UIVec2 UIDrawerHost::ComputeDesiredSize()
    {
        if (m_Content)
            m_Content->Measure();
        m_Frame->Measure();
        return { 100.0f, 100.0f };
    }

    void UIDrawerHost::OnUpdate(float dt)
    {
        const float target = Open ? 1.0f : 0.0f;
        Openness += (target - Openness) * (1.0f - std::exp(-std::max(0.0f, dt) / std::max(SlideTimeConstant, 1e-4f)));
        if (std::abs(target - Openness) < 0.01f)
            Openness = target;
        m_Frame->Visible = Openness > 0.0f;
    }

    void UIDrawerHost::ArrangeChildren(const UIRect& rect)
    {
        if (m_Content)
            m_Content->Arrange(rect);
        const float eased = 1.0f - (1.0f - Openness) * (1.0f - Openness);
        const float height = std::floor(rect.Height() * std::clamp(HeightFraction, MinFraction, MaxFraction) * eased);
        m_Frame->Arrange({ { rect.Min.x, rect.Max.y - height }, { rect.Max.x, rect.Max.y } });
    }
}
