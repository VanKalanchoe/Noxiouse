#pragma once

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Events/Event.h"
#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Core/UITheme.h"

namespace Nox::UI
{
    class UIDrawList;

    // Owns a viewport's retained UIElement tree, routes input (hit-testing, capture, focus, bubbling),
    // and drives 2-pass layout + style transitions + painting.
    class UIContext
    {
    public:
        UIContext();

        void SetRoot(const IntrusiveRef<UIElement>& root);
        const IntrusiveRef<UIElement>& GetRoot() const { return m_Root; }

        void SetTheme(const IntrusiveRef<UITheme>& theme);
        const IntrusiveRef<UITheme>& GetTheme() const { return m_Theme; }

        void OnEvent(Event& event);
        void UpdateAndRender(UIDrawList& drawList, const UIVec2& viewportSize, float dt);

        UIElement* GetHovered() const { return m_Hovered.lock().get(); }
        UIElement* GetFocused() const { return m_Focused.lock().get(); }
        void SetFocus(UIElement* element);

    private:
        void UpdateHover(const UIVec2& pos);
        bool DispatchPointerMove(const UIVec2& pos);
        bool DispatchPointerDown(const UIVec2& pos, int button);
        bool DispatchPointerUp(const UIVec2& pos, int button);
        bool DispatchScroll(const UIVec2& scrollDelta);
        bool DispatchKeyDown(int keyCode, uint16_t mods, bool repeat);

        IntrusiveRef<UIElement> m_Root;
        IntrusiveRef<UITheme> m_Theme;

        WeakRef<UIElement> m_Hovered;
        WeakRef<UIElement> m_Captured;
        WeakRef<UIElement> m_Focused;

        UIVec2 m_LastMousePos{ 0.0f, 0.0f };
    };
}
