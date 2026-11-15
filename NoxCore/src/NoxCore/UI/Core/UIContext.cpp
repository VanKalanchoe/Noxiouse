#include "UIContext.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

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

    void UIContext::PushToast(std::string message, float durationSeconds, const UIColor& accentColor)
    {
        if (message.empty())
            return;
        m_Toasts.push_back({ std::move(message), std::max(0.5f, durationSeconds), std::max(0.5f, durationSeconds), accentColor });
        if (m_Toasts.size() > 5)
            m_Toasts.erase(m_Toasts.begin());
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
        if (keyCode == SDL_SCANCODE_ESCAPE && m_Overlay)
        {
            ClearOverlay();
            return true;
        }

        if (keyCode == SDL_SCANCODE_TAB)
        {
            std::vector<UIElement*> focusables;
            auto collect = [&](auto& self, UIElement* node) -> void
            {
                if (!node || !node->Visible || !node->IsEnabled())
                    return;
                if (node->Focusable)
                    focusables.push_back(node);
                for (const auto& child : node->GetChildren())
                    self(self, child.get());
            };
            collect(collect, m_Overlay ? m_Overlay.get() : m_Root.get());

            if (!focusables.empty())
            {
                const bool reverse = (mods & SDL_KMOD_SHIFT) != 0;
                int currentIdx = -1;
                for (int i = 0; i < static_cast<int>(focusables.size()); ++i)
                {
                    if (focusables[i] == m_Focused.get())
                    {
                        currentIdx = i;
                        break;
                    }
                }
                int nextIdx = 0;
                if (currentIdx >= 0)
                {
                    const int count = static_cast<int>(focusables.size());
                    nextIdx = (currentIdx + (reverse ? -1 : 1) + count) % count;
                }
                else if (reverse)
                {
                    nextIdx = static_cast<int>(focusables.size()) - 1;
                }
                SetFocus(focusables[nextIdx]);
                return true;
            }
        }

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

        // Render bottom-right stacked toast notifications
        if (!m_Toasts.empty())
        {
            for (auto& toast : m_Toasts)
                toast.Remaining -= dt;
            std::erase_if(m_Toasts, [](const ToastItem& t) { return t.Remaining <= 0.0f; });

            float bottomY = viewportSize.y - 18.0f;
            constexpr float fontSize = 13.0f;
            const Ref<Font> font = Font::GetDefault();
            for (int i = static_cast<int>(m_Toasts.size()) - 1; i >= 0; --i)
            {
                const ToastItem& t = m_Toasts[i];
                const float alpha = std::clamp(t.Remaining / 0.4f, 0.0f, 1.0f);
                const UIVec2 textSize = UILabel::MeasureText(t.Message, font, fontSize);
                const UIVec2 boxSize{ std::max(180.0f, textSize.x + 32.0f), textSize.y + 18.0f };
                const UIVec2 boxMin{ viewportSize.x - boxSize.x - 18.0f, bottomY - boxSize.y };
                const UIVec2 boxMax = boxMin + boxSize;

                drawList.AddRoundedRect(
                    boxMin,
                    boxMax,
                    { 0.10f, 0.11f, 0.15f, 0.95f * alpha },
                    glm::vec4(6.0f),
                    1.0f,
                    { 0.26f, 0.30f, 0.38f, 0.9f * alpha },
                    { 0.0f, 0.0f, 0.0f, 0.4f * alpha },
                    { 0.0f, 4.0f },
                    10.0f
                );
                drawList.AddRoundedRect(
                    { boxMin.x + 2.0f, boxMin.y + 4.0f },
                    { boxMin.x + 6.0f, boxMax.y - 4.0f },
                    { t.Accent.r, t.Accent.g, t.Accent.b, t.Accent.a * alpha },
                    glm::vec4(2.0f)
                );
                drawList.AddText(
                    t.Message,
                    font,
                    { boxMin.x + 16.0f, boxMin.y + 9.0f },
                    fontSize,
                    { 0.92f, 0.95f, 0.98f, alpha }
                );
                bottomY = boxMin.y - 8.0f;
            }
        }
    }
}
