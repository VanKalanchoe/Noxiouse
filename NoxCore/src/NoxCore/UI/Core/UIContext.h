#pragma once

#include <string>
#include <vector>

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

        static UIContext* GetCurrent() { return s_Current; }

        void SetRoot(const Ref<UIElement>& root);
        const Ref<UIElement>& GetRoot() const { return m_Root; }

        void SetOverlay(const Ref<UIElement>& overlay);
        const Ref<UIElement>& GetOverlay() const { return m_Overlay; }
        void ClearOverlay() { SetOverlay(nullptr); }

        void PushToast(std::string message, float durationSeconds = 3.0f, const UIColor& accentColor = { 0.24f, 0.56f, 0.96f, 1.0f });
        // A toast with a severity (its icon and colour), a title and an optional detail line. A click on it dismisses it; the
        // pointer resting on it keeps it. At most five are shown, the newest at the bottom.
        void PushNotification(UINotification notification);
        // Space kept free under the toasts (a status bar at the bottom of the window).
        void SetToastBottomInset(float pixels) { m_ToastBottomInset = pixels; }

        void SetTheme(const Ref<UITheme>& theme);
        const Ref<UITheme>& GetTheme() const { return m_Theme; }

        void OnEvent(Event& event);
        void UpdateAndRender(UIDrawList& drawList, const UIVec2& viewportSize, float dt);

        UIElement* GetHovered() const { return m_Hovered.get(); }
        UIElement* GetFocused() const { return m_Focused.get(); }
        void SetFocus(UIElement* element);
        void SetPointerCapture(const Ref<UIElement>& element);
        void ReleasePointerCapture();
        // The press just dispatched is the element's own drag (a scroll bar's thumb, a number field's scrub): it must not
        // turn into a drag-and-drop of an ancestor's payload once the pointer moved far enough.
        void CancelPendingDrag() { m_DragPending = false; }
        // The size of the viewport the context was last laid out in (popups clamp to it).
        const UIVec2& GetViewportSize() const { return m_ViewportSize; }
        const UIVec2& GetLastMousePos() const { return m_LastMousePos; }
        bool IsDragging() const { return m_DragActive; }

        // Runs `action` at the start of this context's next frame, after the input of this one: what must not happen inside the
        // callback that asks for it (a list rebuilt from one of its own rows' clicks, a popup shown again from its own button).
        void Defer(std::function<void()> action) { m_Deferred.push_back(std::move(action)); }

        // The input state of the last event any context saw (every OS window's): the UIMod bits held, and whether a mouse button
        // is held (0: any; else a UIMouseButton). For code outside an event (a finished drag becomes one undo step).
        static uint16_t GetCurrentModifiers() { return s_Modifiers; }
        static bool IsMouseButtonHeld(int button = 0) { return button == 0 ? s_ButtonsHeld != 0 : (s_ButtonsHeld & (1u << button)) != 0; }
        const UIDragPayload& GetDragPayload() const { return m_ActivePayload; }

    private:
        // Whether the element is still part of what the context shows (its root or its overlay). A panel that rebuilds itself
        // drops its old elements while the context may still hold one as focused or pressed: such an element gets no more
        // input and no callbacks (what it would call back into may be gone with the panel).
        bool IsInTree(const UIElement* element) const;
        void DropDetachedInput();
        void UpdateHover(const UIVec2& pos);
        void UpdateSystemCursor(const UIVec2& pos) const;
        bool DispatchPointerMove(const UIVec2& pos);
        bool DispatchPointerDown(const UIVec2& pos, int button);
        bool DispatchPointerUp(const UIVec2& pos, int button);
        bool DispatchScroll(const UIVec2& scrollDelta);
        bool DispatchKeyDown(int keyCode, uint16_t mods, bool repeat);
        bool DispatchTextInput(std::string_view text);

        Ref<UIElement> m_Root;
        Ref<UIElement> m_Overlay;
        Ref<UITheme> m_Theme;

        inline static UIContext* s_Current = nullptr;
        inline static uint16_t s_Modifiers = 0;
        inline static uint32_t s_ButtonsHeld = 0;
        std::vector<std::function<void()>> m_Deferred;

        Ref<UIElement> m_Hovered;
        Ref<UIElement> m_Captured;
        Ref<UIElement> m_Focused;

        UIVec2 m_LastMousePos{ 0.0f, 0.0f };
        UIVec2 m_ViewportSize{ 0.0f, 0.0f };
        float m_HoverTime = 0.0f;

        bool m_DragPending = false;
        bool m_DragActive = false;
        UIVec2 m_DragOrigin{ 0.0f, 0.0f };
        UIDragPayload m_ActivePayload{};

        struct ToastItem
        {
            UINotification Note;
            float Remaining = 3.0f;
            float Age = 0.0f;     // seconds shown (fade / slide in)
            bool HasAccent = false;
            UIColor Accent{ 0.24f, 0.56f, 0.96f, 1.0f };
            UIRect Bounds{};      // where it was drawn last frame (clicks)
        };
        void PaintToasts(UIDrawList& drawList, const UIVec2& viewportSize, float dt);
        bool DismissToastAt(const UIVec2& pos);
        std::vector<ToastItem> m_Toasts;
        float m_ToastBottomInset = 0.0f;
    };
}
