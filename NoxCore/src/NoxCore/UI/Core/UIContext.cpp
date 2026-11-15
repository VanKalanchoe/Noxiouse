#include "UIContext.h"

#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UIContext::UIContext()
    {
        m_Theme = UITheme::CreateDefault();
    }

    void UIContext::SetRoot(const IntrusiveRef<UIElement>& root)
    {
        m_Root = root;
        m_Hovered.reset();
        m_Captured.reset();
        m_Focused.reset();
    }

    void UIContext::SetTheme(const IntrusiveRef<UITheme>& theme)
    {
        m_Theme = theme ? theme : UITheme::CreateDefault();
    }

    void UIContext::SetFocus(UIElement* element)
    {
        auto prev = m_Focused.lock();
        if (prev.get() == element)
            return;
        if (prev)
            prev->SetStateFlag(UIState::Focused, false);
        if (element && element->Focusable && element->IsEnabled())
        {
            element->SetStateFlag(UIState::Focused, true);
            m_Focused = WeakRef<UIElement>(element);
        }
        else
        {
            m_Focused.reset();
        }
    }

    void UIContext::UpdateHover(const UIVec2& pos)
    {
        UIElement* nextHover = m_Root ? m_Root->HitTest(pos) : nullptr;
        auto prevHover = m_Hovered.lock();
        if (prevHover.get() == nextHover)
            return;

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
            m_Hovered = WeakRef<UIElement>(nextHover);
        }
        else
        {
            m_Hovered.reset();
        }
    }

    bool UIContext::DispatchPointerMove(const UIVec2& pos)
    {
        const UIVec2 delta = pos - m_LastMousePos;
        m_LastMousePos = pos;

        if (auto captured = m_Captured.lock())
        {
            UIPointerEvent ev{ pos, delta, {}, 0, false };
            return captured->OnPointerMove(ev);
        }

        UpdateHover(pos);
        if (auto hovered = m_Hovered.lock())
        {
            UIPointerEvent ev{ pos, delta, {}, 0, false };
            for (UIElement* cur = hovered.get(); cur != nullptr; cur = cur->GetParent())
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
        UpdateHover(pos);

        auto hovered = m_Hovered.lock();
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
        auto captured = m_Captured.lock();
        m_Captured.reset();

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
        auto target = m_Hovered.lock();
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
        auto focused = m_Focused.lock();
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
    }
}
