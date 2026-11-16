#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Core/UITypes.h"

namespace Nox::UI
{
    class UIDrawList;

    class UIElement : public RefCounted
    {
    public:
        UIElement();
        ~UIElement() override;

        uint64_t GetId() const { return m_Id; }

        // Tree management
        void AddChild(const Ref<UIElement>& child);
        void RemoveChild(const Ref<UIElement>& child);
        void ClearChildren();

        UIElement* GetParent() const { return m_Parent; }
        const std::vector<Ref<UIElement>>& GetChildren() const { return m_Children; }

        // Layout pass (Pass 1: bottom-up Measure, Pass 2: top-down Arrange)
        UIVec2 Measure();
        virtual float ComputeHeightForWidth(float /*width*/) { return m_DesiredSize.y; }
        void Arrange(const UIRect& allottedRect);

        const UIRect& GetBounds() const { return m_Bounds; }
        const UIVec2& GetDesiredSize() const { return m_DesiredSize; }

        // Style & Paint
        void UpdateStylesRecursive(const UITheme& theme, float dt);
        void PaintRecursive(UIDrawList& drawList, const UITheme& theme);

        // Hit-testing (reverse paint order)
        virtual UIElement* HitTest(const UIVec2& pos);
        virtual UICursorShape GetCursorShape(const UIVec2& /*pos*/) const { return UICursorShape::Default; }

        // State helpers (managed by UIContext)
        UIState GetState() const { return m_State; }
        void SetStateFlag(UIState flag, bool enabled);
        bool IsEnabled() const { return !HasState(m_State, UIState::Disabled); }
        void SetEnabled(bool enabled) { SetStateFlag(UIState::Disabled, !enabled); }

        // Public layout / visual properties
        std::string Name;
        std::string StyleClass = "Panel";
        std::string StyleVariant;
        std::string Tooltip;

        bool Visible = true;
        bool ClipChildren = false;
        bool DrawBackground = false;
        bool Focusable = false;
        // Tab goes to this element while it has the focus (a console line completes with it) instead of moving the focus on.
        bool CapturesTab = false;
        UIHitTestMode HitTestMode = UIHitTestMode::PassThrough;

        UISizeMode WidthMode = UISizeMode::Auto;
        UISizeMode HeightMode = UISizeMode::Auto;
        UIVec2 FixedSize{ 0.0f, 0.0f };
        UIVec2 MinSize{ 0.0f, 0.0f };
        UIVec2 MaxSize{ 1e6f, 1e6f };
        float FlexWeight = 1.0f;

        UIAlignment HAlign = UIAlignment::Stretch;
        UIAlignment VAlign = UIAlignment::Stretch;
        UIThickness Margin{};
        UIThickness Padding{}; // overrides or adds inner spacing

        // Godot-style anchors (used by UIAnchorBox)
        UIVec2 AnchorMin{ 0.0f, 0.0f };
        UIVec2 AnchorMax{ 0.0f, 0.0f };
        UIThickness AnchorOffsets{};

        // Events (return true if handled to stop bubbling)
        virtual bool OnPointerEnter(const UIPointerEvent&) { return false; }
        virtual bool OnPointerLeave(const UIPointerEvent&) { return false; }
        virtual bool OnPointerDown(const UIPointerEvent& e) { return OnMouseDown ? OnMouseDown(e) : false; }
        virtual bool OnPointerUp(const UIPointerEvent& e) { return OnMouseUp ? OnMouseUp(e) : false; }
        virtual bool OnPointerMove(const UIPointerEvent&) { return false; }
        virtual bool OnMouseWheel(const UIPointerEvent&) { return false; }
        virtual bool OnKeyDown(const UIKeyEvent&) { return false; }
        virtual bool OnTextInput(std::string_view) { return false; }
        // The keyboard focus came to the element or left it (a field applies what was typed when it leaves).
        virtual void OnFocusChanged(bool /*focused*/) {}

        std::function<bool(const UIPointerEvent&)> OnMouseDown;
        std::function<bool(const UIPointerEvent&)> OnMouseUp;

        // Drag & drop callbacks
        std::function<std::optional<UIDragPayload>()> OnDragStart;
        std::string AcceptedDropType;
        std::function<bool(const UIDragPayload&)> OnDrop;

    protected:
        virtual void OnUpdate(float dt) {}
        virtual UIVec2 ComputeDesiredSize();
        virtual void ArrangeChildren(const UIRect& contentRect);
        virtual void OnPaint(UIDrawList& drawList, const UITheme& theme);
        // Painted after the children and outside their clip rect: what must stay on top of the content (a scroll bar).
        virtual void OnPaintOverlay(UIDrawList& /*drawList*/, const UITheme& /*theme*/) {}
        // The rect children are clipped to when ClipChildren is set (the bounds; a scroll view leaves out its scroll bar's lane).
        virtual UIRect GetChildClipRect() const { return m_Bounds; }

        const UIStyleBox& GetResolvedStyle() const { return m_CurrentStyle; }

    private:
        uint64_t m_Id = 0;
        UIElement* m_Parent = nullptr;
        std::vector<Ref<UIElement>> m_Children;

        UIVec2 m_DesiredSize{ 0.0f, 0.0f };
        UIRect m_Bounds{};

        UIState m_State = UIState::Normal;
        UIStyleBox m_CurrentStyle{};
        bool m_HasResolvedStyle = false;
    };

    template <typename T, typename... Args>
    Ref<T> MakeUI(Args&&... args)
    {
        return Ref<T>(new T(std::forward<Args>(args)...));
    }
}
