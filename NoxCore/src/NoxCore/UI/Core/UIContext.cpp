#include "UIContext.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        struct ScopedCurrentContext
        {
            UIContext*& Slot;
            UIContext* Prev;
            ScopedCurrentContext(UIContext*& slot, UIContext* next) : Slot(slot), Prev(slot) { Slot = next; }
            ~ScopedCurrentContext() { Slot = Prev; }
        };

        void ApplySystemCursor(UICursorShape shape)
        {
            if (!SDL_CursorVisible())
                return;

            static SDL_Cursor* s_Cursors[5] = {};
            static UICursorShape s_LastApplied = UICursorShape::Default;
            static bool s_Initialized = false;
            if (s_Initialized && s_LastApplied == shape)
                return;

            const size_t idx = static_cast<size_t>(shape);
            if (idx >= 5)
                return;

            if (!s_Cursors[idx])
            {
                SDL_SystemCursor sys = SDL_SYSTEM_CURSOR_DEFAULT;
                switch (shape)
                {
                case UICursorShape::Default:    sys = SDL_SYSTEM_CURSOR_DEFAULT; break;
                case UICursorShape::ResizeEW:   sys = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
                case UICursorShape::ResizeNS:   sys = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
                case UICursorShape::ResizeNWSE: sys = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
                case UICursorShape::ResizeNESW: sys = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
                }
                s_Cursors[idx] = SDL_CreateSystemCursor(sys);
            }

            if (s_Cursors[idx])
            {
                SDL_SetCursor(s_Cursors[idx]);
                s_LastApplied = shape;
                s_Initialized = true;
            }
        }
    }

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
        if (m_Overlay == overlay)
            return;

        // What the old overlay still holds of the input leaves with it, while it is still alive: a field in a popup applies
        // what was typed (its focus ends), and no element of a closed popup is left focused, hovered or holding the pointer.
        Ref<UIElement> old = m_Overlay;
        if (old)
        {
            auto inside = [&old](const Ref<UIElement>& element)
            {
                for (const UIElement* cur = element.get(); cur != nullptr; cur = cur->GetParent())
                {
                    if (cur == old.get())
                        return true;
                }
                return false;
            };
            if (m_Focused && inside(m_Focused))
                SetFocus(nullptr);
            if (m_Hovered && inside(m_Hovered))
            {
                m_Hovered->SetStateFlag(UIState::Hovered, false);
                m_Hovered = nullptr;
            }
            if (m_Captured && inside(m_Captured))
            {
                m_Captured->SetStateFlag(UIState::Pressed, false);
                m_Captured = nullptr;
                m_DragPending = false;
            }
        }
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

    bool UIContext::IsInTree(const UIElement* element) const
    {
        for (const UIElement* cur = element; cur != nullptr; cur = cur->GetParent())
        {
            if (cur == m_Root.get() || cur == m_Overlay.get())
                return true;
        }
        return false;
    }

    void UIContext::DropDetachedInput()
    {
        if (m_Focused && !IsInTree(m_Focused.get()))
        {
            m_Focused->SetStateFlag(UIState::Focused, false);
            m_Focused = nullptr;
        }
        if (m_Captured && !IsInTree(m_Captured.get()))
        {
            m_Captured->SetStateFlag(UIState::Pressed, false);
            m_Captured = nullptr;
            m_DragPending = false;
        }
        if (m_Hovered && !IsInTree(m_Hovered.get()))
        {
            m_Hovered->SetStateFlag(UIState::Hovered, false);
            m_Hovered = nullptr;
        }
    }

    void UIContext::SetFocus(UIElement* element)
    {
        Ref<UIElement> prev = m_Focused;
        if (prev.get() == element)
            return;
        if (prev)
        {
            prev->SetStateFlag(UIState::Focused, false);
            if (IsInTree(prev.get()))
                prev->OnFocusChanged(false);
        }
        if (element && element->Focusable && element->IsEnabled())
        {
            element->SetStateFlag(UIState::Focused, true);
            m_Focused = Ref<UIElement>(element);
            element->OnFocusChanged(true);
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

    void UIContext::UpdateSystemCursor(const UIVec2& pos) const
    {
        UICursorShape shape = UICursorShape::Default;
        if (!m_DragActive)
        {
            UIElement* target = m_Captured ? m_Captured.get() : m_Hovered.get();
            for (UIElement* cur = target; cur != nullptr; cur = cur->GetParent())
            {
                const UICursorShape s = cur->GetCursorShape(pos);
                if (s != UICursorShape::Default)
                {
                    shape = s;
                    break;
                }
            }
        }
        ApplySystemCursor(shape);
    }

    bool UIContext::DispatchPointerMove(const UIVec2& pos)
    {
        const UIVec2 delta = pos - m_LastMousePos;
        m_LastMousePos = pos;

        if (m_Captured)
        {
            if (m_DragPending && !m_DragActive)
            {
                const UIVec2 diff = pos - m_DragOrigin;
                if (diff.x * diff.x + diff.y * diff.y > 36.0f)
                {
                    for (UIElement* cur = m_Captured.get(); cur != nullptr; cur = cur->GetParent())
                    {
                        if (cur->OnDragStart)
                        {
                            if (auto payload = cur->OnDragStart())
                            {
                                m_ActivePayload = std::move(*payload);
                                m_DragActive = true;
                                break;
                            }
                        }
                    }
                    m_DragPending = false;
                }
            }
            if (m_DragActive)
            {
                UpdateSystemCursor(pos);
                return true;
            }

            UIPointerEvent ev{ pos, delta, {}, 0, false };
            const bool handled = m_Captured->OnPointerMove(ev);
            UpdateSystemCursor(pos);
            return handled;
        }

        UpdateHover(pos);
        bool handled = false;
        if (m_Hovered)
        {
            UIPointerEvent ev{ pos, delta, {}, 0, false };
            for (UIElement* cur = m_Hovered.get(); cur != nullptr; cur = cur->GetParent())
            {
                if (cur->OnPointerMove(ev))
                {
                    handled = true;
                    break;
                }
            }
        }
        UpdateSystemCursor(pos);
        return handled;
    }

    void UIContext::SetPointerCapture(const Ref<UIElement>& element)
    {
        if (m_Captured && m_Captured != element)
            m_Captured->SetStateFlag(UIState::Pressed, false);
        m_Captured = element;
    }

    void UIContext::ReleasePointerCapture()
    {
        if (m_Captured)
            m_Captured->SetStateFlag(UIState::Pressed, false);
        m_Captured = nullptr;
        UpdateSystemCursor(m_LastMousePos);
    }

    bool UIContext::DispatchPointerDown(const UIVec2& pos, int button)
    {
        m_LastMousePos = pos;
        if (m_Overlay && !m_Overlay->HitTest(pos))
            SetOverlay(nullptr);

        UpdateHover(pos);

        Ref<UIElement> hovered = m_Hovered;
        if (!hovered || !hovered->IsEnabled())
        {
            SetFocus(nullptr);
            UpdateSystemCursor(pos);
            return false;
        }

        SetFocus(hovered.get());
        hovered->SetStateFlag(UIState::Pressed, true);
        m_Captured = hovered;
        if (button == 1)
        {
            m_DragOrigin = pos;
            m_DragPending = true;
            m_DragActive = false;
        }

        UIPointerEvent ev{ pos, {}, {}, button, false };
        for (UIElement* cur = hovered.get(); cur != nullptr; cur = cur->GetParent())
        {
            if (cur->OnPointerDown(ev))
            {
                UpdateSystemCursor(pos);
                return true;
            }
        }
        UpdateSystemCursor(pos);
        return true;
    }

    bool UIContext::DispatchPointerUp(const UIVec2& pos, int button)
    {
        m_LastMousePos = pos;
        Ref<UIElement> captured = m_Captured;
        m_Captured = nullptr;

        bool handled = false;
        if (m_DragActive && button == 1)
        {
            UIElement* target = m_Overlay ? m_Overlay->HitTest(pos) : nullptr;
            if (!target && m_Root)
                target = m_Root->HitTest(pos);
            for (UIElement* cur = target; cur != nullptr; cur = cur->GetParent())
            {
                if (cur->OnDrop && (cur->AcceptedDropType.empty() || cur->AcceptedDropType == m_ActivePayload.Type))
                {
                    if (cur->OnDrop(m_ActivePayload))
                    {
                        handled = true;
                        break;
                    }
                }
            }
            m_DragActive = false;
            m_DragPending = false;
            m_ActivePayload = {};
            if (captured)
                captured->SetStateFlag(UIState::Pressed, false);
            UpdateHover(pos);
            UpdateSystemCursor(pos);
            return handled;
        }
        m_DragPending = false;

        if (captured)
        {
            captured->SetStateFlag(UIState::Pressed, false);
            UIPointerEvent ev{ pos, {}, {}, button, false };
            handled = captured->OnPointerUp(ev);
        }

        UpdateHover(pos);
        UpdateSystemCursor(pos);
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
            // A field being typed in inside the popup takes Escape first (it puts its old value back); else the popup closes.
            if (Ref<UIElement> focused = m_Focused)
            {
                bool inOverlay = false;
                for (const UIElement* cur = focused.get(); cur != nullptr; cur = cur->GetParent())
                    inOverlay = inOverlay || cur == m_Overlay.get();
                UIKeyEvent ev{ keyCode, mods, repeat, false };
                if (inOverlay && focused->OnKeyDown(ev))
                    return true;
            }
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
        ScopedCurrentContext currentGuard(s_Current, this);
        DropDetachedInput();
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
        ScopedCurrentContext currentGuard(s_Current, this);
        if (m_Theme)
            m_Theme->PollHotReload();

        drawList.Clear(viewportSize);
        m_ViewportSize = viewportSize;
        if (!m_Root || !m_Theme)
            return;

        // Styles first: layout reads them (font sizes, paddings). An element made this frame -- a panel rebuilt, a section
        // opened -- would otherwise be measured with the default style, drawn there for one frame and jump into place on the
        // next (the whole panel flashed).
        m_Root->UpdateStylesRecursive(*m_Theme, dt);
        if (m_Overlay)
            m_Overlay->UpdateStylesRecursive(*m_Theme, dt);

        m_Root->Measure();
        m_Root->Arrange({ { 0.0f, 0.0f }, viewportSize });
        if (m_Overlay)
        {
            m_Overlay->Measure();
            m_Overlay->Arrange({ { 0.0f, 0.0f }, viewportSize });
        }

        // What is under the pointer after this layout, also when the pointer did not move: content that scrolled, moved or
        // was rebuilt under a resting pointer is hovered right away (not on the next mouse move).
        if (!m_Captured && !m_DragActive)
            UpdateHover(m_LastMousePos);

        m_Root->PaintRecursive(drawList, *m_Theme);
        if (m_Overlay)
            m_Overlay->PaintRecursive(drawList, *m_Theme);

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
                    const UIStyleBox tipStyle = m_Theme ? m_Theme->Resolve("Tooltip", "", UIState::Normal) : UIStyleBox{};
                    const float fontSize = tipStyle.FontSize > 0.0f ? tipStyle.FontSize : 18.0f;
                    const UIVec2 textSize = UILabel::MeasureText(tip, nullptr, fontSize);
                    const UIVec2 pad{ tipStyle.Padding.Left > 0.0f ? tipStyle.Padding.Left : fontSize * 0.75f,
                                      tipStyle.Padding.Top > 0.0f ? tipStyle.Padding.Top : fontSize * 0.45f };
                    const UIVec2 boxSize{ textSize.x + pad.x * 2.0f, textSize.y + pad.y * 2.0f };
                    UIVec2 pos{ m_LastMousePos.x + fontSize * 1.1f, m_LastMousePos.y + fontSize * 1.4f };
                    if (pos.x + boxSize.x > viewportSize.x - 6.0f)
                        pos.x = std::max(6.0f, viewportSize.x - boxSize.x - 6.0f);
                    if (pos.y + boxSize.y > viewportSize.y - 6.0f)
                        pos.y = std::max(6.0f, m_LastMousePos.y - boxSize.y - 8.0f);

                    drawList.AddRoundedRect(
                        pos,
                        pos + boxSize,
                        tipStyle.FillColor,
                        { tipStyle.CornerRadius.TopLeft, tipStyle.CornerRadius.TopRight, tipStyle.CornerRadius.BottomRight, tipStyle.CornerRadius.BottomLeft },
                        tipStyle.BorderThickness > 0.0f ? tipStyle.BorderThickness : 1.0f,
                        tipStyle.BorderColor,
                        tipStyle.ShadowColor,
                        tipStyle.ShadowOffset,
                        tipStyle.ShadowBlur
                    );
                    drawList.AddText(tip, nullptr, pos + pad, fontSize, tipStyle.TextColor);
                }
            }
        }
        else
        {
            m_HoverTime = 0.0f;
        }

        if (m_DragActive)
        {
            const UIStyleBox tipStyle = m_Theme ? m_Theme->Resolve("Tooltip", "", UIState::Normal) : UIStyleBox{};
            const UIColor accent = m_Theme ? m_Theme->GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f }) : UIColor{ 0.259f, 0.529f, 0.961f, 1.0f };
            UIElement* target = m_Overlay ? m_Overlay->HitTest(m_LastMousePos) : nullptr;
            if (!target && m_Root)
                target = m_Root->HitTest(m_LastMousePos);
            for (UIElement* cur = target; cur != nullptr; cur = cur->GetParent())
            {
                if (cur->OnDrop && (cur->AcceptedDropType.empty() || cur->AcceptedDropType == m_ActivePayload.Type))
                {
                    const UIRect& tb = cur->GetBounds();
                    drawList.AddRoundedRect(
                        tb.Min,
                        tb.Max,
                        { accent.r, accent.g, accent.b, 0.14f },
                        glm::vec4(tipStyle.CornerRadius.TopLeft),
                        2.0f,
                        accent
                    );
                    break;
                }
            }

            const std::string label = m_ActivePayload.Text.empty() ? m_ActivePayload.Type : m_ActivePayload.Text;
            const Ref<Font> font = Font::GetDefault();
            const float fontSize = tipStyle.FontSize > 0.0f ? tipStyle.FontSize : 18.0f;
            const UIVec2 pad{ fontSize * 0.75f, fontSize * 0.45f };
            const UIVec2 textSize = UILabel::MeasureText(label, font, fontSize);
            const UIVec2 boxPos = m_LastMousePos + UIVec2{ fontSize * 1.1f, fontSize * 1.1f };
            const UIVec2 boxSize{ textSize.x + pad.x * 2.0f, textSize.y + pad.y * 2.0f };
            drawList.AddRoundedRect(
                boxPos,
                boxPos + boxSize,
                tipStyle.FillColor,
                glm::vec4(tipStyle.CornerRadius.TopLeft),
                1.5f,
                accent
            );
            drawList.AddText(label, font, boxPos + pad, fontSize, tipStyle.TextColor);
        }

        // Render bottom-right stacked toast notifications
        if (!m_Toasts.empty())
        {
            for (auto& toast : m_Toasts)
                toast.Remaining -= dt;
            std::erase_if(m_Toasts, [](const ToastItem& t) { return t.Remaining <= 0.0f; });

            const UIStyleBox toastStyle = m_Theme ? m_Theme->Resolve("Tooltip", "", UIState::Normal) : UIStyleBox{};
            const float fontSize = toastStyle.FontSize > 0.0f ? toastStyle.FontSize : 18.0f;
            const float margin = fontSize * 1.3f;
            float bottomY = viewportSize.y - margin;
            const Ref<Font> font = Font::GetDefault();
            for (int i = static_cast<int>(m_Toasts.size()) - 1; i >= 0; --i)
            {
                const ToastItem& t = m_Toasts[i];
                const float alpha = std::clamp(t.Remaining / 0.4f, 0.0f, 1.0f);
                const UIVec2 textSize = UILabel::MeasureText(t.Message, font, fontSize);
                const float padX = fontSize * 1.1f;
                const float padY = fontSize * 0.65f;
                const UIVec2 boxSize{ textSize.x + padX * 2.0f + 6.0f, textSize.y + padY * 2.0f };
                const UIVec2 boxMin{ viewportSize.x - boxSize.x - margin, bottomY - boxSize.y };
                const UIVec2 boxMax = boxMin + boxSize;

                drawList.AddRoundedRect(
                    boxMin,
                    boxMax,
                    { toastStyle.FillColor.r, toastStyle.FillColor.g, toastStyle.FillColor.b, toastStyle.FillColor.a * alpha },
                    glm::vec4(toastStyle.CornerRadius.TopLeft),
                    1.0f,
                    { toastStyle.BorderColor.r, toastStyle.BorderColor.g, toastStyle.BorderColor.b, toastStyle.BorderColor.a * alpha },
                    { toastStyle.ShadowColor.r, toastStyle.ShadowColor.g, toastStyle.ShadowColor.b, toastStyle.ShadowColor.a * alpha },
                    toastStyle.ShadowOffset,
                    toastStyle.ShadowBlur
                );
                drawList.AddRoundedRect(
                    { boxMin.x + 2.0f, boxMin.y + 4.0f },
                    { boxMin.x + 5.5f, boxMax.y - 4.0f },
                    { t.Accent.r, t.Accent.g, t.Accent.b, t.Accent.a * alpha },
                    glm::vec4(2.0f)
                );
                drawList.AddText(
                    t.Message,
                    font,
                    { boxMin.x + padX + 4.0f, boxMin.y + padY },
                    fontSize,
                    { toastStyle.TextColor.r, toastStyle.TextColor.g, toastStyle.TextColor.b, toastStyle.TextColor.a * alpha }
                );
                bottomY = boxMin.y - (fontSize * 0.6f);
            }
        }
    }
}
