#include "UIContext.h"

#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIContext::UIContext()
    {
        m_Theme = UITheme::CreateDefault();
    }

    void UIContext::SetRoot(const Ref<UIElement>& root)
    {
        m_Root = root;
        m_Overlay = nullptr;
        m_Hovered = nullptr;
        m_Captured = nullptr;
        m_Focused = nullptr;
    }

    void UIContext::SetOverlay(const Ref<UIElement>& overlay)
    {
        m_Overlay = overlay;
    }

    void UIContext::SetTheme(const Ref<UITheme>& theme)
    {
        m_Theme = theme ? theme : UITheme::CreateDefault();
    }

    void UIContext::SetFocus(UIElement* element)
    {
        Ref<UIElement> prev = m_Focused;
        if (prev.get() == element)
            return;
        if (prev)
            prev->SetStateFlag(UIState::Focused, false);
        if (element && element->Focusable && element->IsEnabled())
        {
            element->SetStateFlag(UIState::Focused, true);
            m_Focused = Ref<UIElement>(element);
        }
        else
        {
            m_Focused = nullptr;
        }
    }

    void UIContext::UpdateHover(const UIVec2& pos)
    {
        UIElement* nextHover = m_Overlay ? m_Overlay->HitTest(pos) : nullptr;
        if (!nextHover && m_Root)
            nextHover = m_Root->HitTest(pos);
        Ref<UIElement> prevHover = m_Hovered;
        if (prevHover.get() == nextHover)
            return;

        m_HoverTime = 0.0f;

        UIPointerEvent ev{};
        ev.Position = pos;
        if (prevHover)
        {
            prevHover->SetStateFlag(UIState::Hovered, false);
            prevHover->OnPointerLeave(ev);
        }
        if (nextHover && nextHover->IsEnabled())
        {
            nextHover->SetStateFlag(UIState::Hovered, true);
            nextHover->OnPointerEnter(ev);
            m_Hovered = Ref<UIElement>(nextHover);
        }
        else
        {
            m_Hovered = nullptr;
        }
    }

    bool UIContext::DispatchPointerMove(const UIVec2& pos)
    {
        const UIVec2 delta = pos - m_LastMousePos;
        m_LastMousePos = pos;

        if (m_Captured)
        {
            UIPointerEvent ev{ pos, delta, {}, 0, false };
            return m_Captured->OnPointerMove(ev);
        }

        UpdateHover(pos);
        if (m_Hovered)
        {
            UIPointerEvent ev{ pos, delta, {}, 0, false };
            for (UIElement* cur = m_Hovered.get(); cur != nullptr; cur = cur->GetParent())
            {
                if (cur->OnPointerMove(ev))
                    return true;
            }
        }
        return false;
    }

    bool UIContext::DispatchPointerDown(const UIVec2& pos, int button)
    {
        m_LastMousePos = pos;
        if (m_Overlay && !m_Overlay->HitTest(pos))
            m_Overlay = nullptr;

        UpdateHover(pos);

        Ref<UIElement> hovered = m_Hovered;
        if (!hovered || !hovered->IsEnabled())
        {
            SetFocus(nullptr);
            return false;
        }

        SetFocus(hovered.get());
        hovered->SetStateFlag(UIState::Pressed, true);
        m_Captured = hovered;

        UIPointerEvent ev{ pos, {}, {}, button, false };
        for (UIElement* cur = hovered.get(); cur != nullptr; cur = cur->GetParent())
        {
            if (cur->OnPointerDown(ev))
                return true;
        }
        return true;
    }

    bool UIContext::DispatchPointerUp(const UIVec2& pos, int button)
    {
        m_LastMousePos = pos;
        Ref<UIElement> captured = m_Captured;
        m_Captured = nullptr;

        bool handled = false;
        if (captured)
        {
            captured->SetStateFlag(UIState::Pressed, false);
            UIPointerEvent ev{ pos, {}, {}, button, false };
            handled = captured->OnPointerUp(ev);
        }

        UpdateHover(pos);
        return handled;
    }

    bool UIContext::DispatchScroll(const UIVec2& scrollDelta)
    {
        Ref<UIElement> target = m_Hovered;
        if (!target)
            return false;

        UIPointerEvent ev{ m_LastMousePos, {}, scrollDelta, 0, false };
        for (UIElement* cur = target.get(); cur != nullptr; cur = cur->GetParent())
        {
            if (cur->OnMouseWheel(ev))
                return true;
        }
        return false;
    }

    bool UIContext::DispatchKeyDown(int keyCode, uint16_t mods, bool repeat)
    {
        Ref<UIElement> focused = m_Focused;
        if (!focused)
            return false;

        UIKeyEvent ev{ keyCode, mods, repeat, false };
        for (UIElement* cur = focused.get(); cur != nullptr; cur = cur->GetParent())
        {
            if (cur->OnKeyDown(ev))
                return true;
        }
        return false;
    }

    bool UIContext::DispatchTextInput(std::string_view text)
    {
        Ref<UIElement> focused = m_Focused;
        if (!focused)
            return false;

        for (UIElement* cur = focused.get(); cur != nullptr; cur = cur->GetParent())
        {
            if (cur->OnTextInput(text))
                return true;
        }
        return false;
    }

    void UIContext::OnEvent(Event& event)
    {
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<MouseMovedEvent>([this](MouseMovedEvent& e)
        {
            return DispatchPointerMove({ e.GetX(), e.GetY() });
        });
        dispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent& e)
        {
            return DispatchPointerDown(m_LastMousePos, e.GetMouseButton());
        });
        dispatcher.Dispatch<MouseButtonReleasedEvent>([this](MouseButtonReleasedEvent& e)
        {
            return DispatchPointerUp(m_LastMousePos, e.GetMouseButton());
        });
        dispatcher.Dispatch<MouseScrolledEvent>([this](MouseScrolledEvent& e)
        {
            return DispatchScroll({ e.GetXOffset(), e.GetYOffset() });
        });
        dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent& e)
        {
            return DispatchKeyDown(e.GetKeyCode(), static_cast<uint16_t>(e.GetModifiers()), e.IsRepeat());
        });
        dispatcher.Dispatch<TextInputEvent>([this](TextInputEvent& e)
        {
            return DispatchTextInput(e.GetText());
        });
    }

    void UIContext::UpdateAndRender(UIDrawList& drawList, const UIVec2& viewportSize, float dt)
    {
        if (m_Theme)
            m_Theme->PollHotReload();

        drawList.Clear(viewportSize);
        if (!m_Root || !m_Theme)
            return;

        m_Root->Measure();
        m_Root->Arrange({ { 0.0f, 0.0f }, viewportSize });
        m_Root->UpdateStylesRecursive(*m_Theme, dt);
        m_Root->PaintRecursive(drawList, *m_Theme);

        if (m_Overlay)
        {
            m_Overlay->Measure();
            m_Overlay->Arrange({ { 0.0f, 0.0f }, viewportSize });
            m_Overlay->UpdateStylesRecursive(*m_Theme, dt);
            m_Overlay->PaintRecursive(drawList, *m_Theme);
        }

        if (m_Hovered && !m_Captured)
        {
            m_HoverTime += dt;
            if (m_HoverTime >= 0.45f)
            {
                std::string_view tip;
                for (UIElement* cur = m_Hovered.get(); cur != nullptr; cur = cur->GetParent())
                {
                    if (!cur->Tooltip.empty())
                    {
                        tip = cur->Tooltip;
                        break;
                    }
                }
                if (!tip.empty())
                {
                    constexpr float fontSize = 12.5f;
                    const UIVec2 textSize = UILabel::MeasureText(tip, nullptr, fontSize);
                    const UIVec2 pad{ 10.0f, 6.0f };
                    const UIVec2 boxSize{ textSize.x + pad.x * 2.0f, textSize.y + pad.y * 2.0f };
                    UIVec2 pos{ m_LastMousePos.x + 14.0f, m_LastMousePos.y + 18.0f };
                    if (pos.x + boxSize.x > viewportSize.x - 6.0f)
                        pos.x = std::max(6.0f, viewportSize.x - boxSize.x - 6.0f);
                    if (pos.y + boxSize.y > viewportSize.y - 6.0f)
                        pos.y = std::max(6.0f, m_LastMousePos.y - boxSize.y - 8.0f);

                    drawList.AddRoundedRect(
                        pos,
                        pos + boxSize,
                        { 0.09f, 0.10f, 0.13f, 0.96f },
                        glm::vec4(6.0f),
                        1.0f,
                        { 0.28f, 0.32f, 0.40f, 0.95f },
                        { 0.0f, 0.0f, 0.0f, 0.45f },
                        { 0.0f, 4.0f },
                        10.0f
                    );
                    drawList.AddText(tip, nullptr, pos + pad, fontSize, { 0.92f, 0.94f, 0.98f, 1.0f });
                }
            }
        }
        else
        {
            m_HoverTime = 0.0f;
        }
    }
}
