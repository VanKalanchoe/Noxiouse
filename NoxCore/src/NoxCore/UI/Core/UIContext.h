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

        void SetTheme(const Ref<UITheme>& theme);
        const Ref<UITheme>& GetTheme() const { return m_Theme; }

        void OnEvent(Event& event);
        void UpdateAndRender(UIDrawList& drawList, const UIVec2& viewportSize, float dt);

        UIElement* GetHovered() const { return m_Hovered.get(); }
        UIElement* GetFocused() const { return m_Focused.get(); }
        void SetFocus(UIElement* element);
        void SetPointerCapture(const Ref<UIElement>& element);
        void ReleasePointerCapture();
        const UIVec2& GetLastMousePos() const { return m_LastMousePos; }
        bool IsDragging() const { return m_DragActive; }
        const UIDragPayload& GetDragPayload() const { return m_ActivePayload; }

    private:
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

        Ref<UIElement> m_Hovered;
        Ref<UIElement> m_Captured;
        Ref<UIElement> m_Focused;

        UIVec2 m_LastMousePos{ 0.0f, 0.0f };
        float m_HoverTime = 0.0f;

        bool m_DragPending = false;
        bool m_DragActive = false;
        UIVec2 m_DragOrigin{ 0.0f, 0.0f };
        UIDragPayload m_ActivePayload{};

        struct ToastItem
        {
            std::string Message;
            float Remaining = 3.0f;
            float Duration = 3.0f;
            UIColor Accent{ 0.24f, 0.56f, 0.96f, 1.0f };
        };
        std::vector<ToastItem> m_Toasts;
    };
}
