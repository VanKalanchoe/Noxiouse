#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // A number field (Unreal's numeric entry box, Godot's spin slider): one widget to scrub and to type.
    //  - Drag left / right on it: the value follows the pointer's movement (it never jumps to the pointer). Shift: ten times finer.
    //  - Click without dragging: the text is edited, all of it selected -- type the exact value. Enter / Tab / a click elsewhere
    //    apply it, Escape puts the old value back. A click in the text places the caret. "," is read as ".".
    //  - Focused by Tab: typing a digit starts the edit, Enter opens it.
    //  - ShowFill with Min < Max: the field is also the slider -- its background fills up to the value, and dragging across
    //    its whole width goes through the whole range.
    class UIDragScalar : public UIElement
    {
    public:
        explicit UIDragScalar(float value = 0.0f, float speed = 0.05f,
                              std::function<void(float)> onChanged = {});

        float Value = 0.0f;
        float Speed = 0.05f; // value per pixel dragged (fields without a fill bar)
        float Min = 0.0f;
        float Max = 0.0f;    // Min < Max enables clamping
        float Step = 0.0f;   // > 0: the value snaps to multiples of it (1 for whole numbers)
        bool ShowFill = false;
        float FontSize = 0.0f;                    // 0: the style's size
        UIColor Stripe{ 0.0f, 0.0f, 0.0f, 0.0f }; // a thin coloured bar at the left edge (a colour's R / G / B fields)
        std::string Prefix;
        UIColor PrefixColor{ 0.24f, 0.56f, 0.96f, 1.0f };
        std::string Format = "{:.2f}";
        // Unreal's way of showing a number: Format's decimals at most, the trailing zeros dropped but one ("120.5", "0.0").
        bool TrimZeros = false;
        std::function<void(float)> OnChanged;
        // When a change is finished: a drag let go, a typed value entered (OnChanged runs on every step of a drag too). For
        // values that are expensive to apply (a pool made again).
        std::function<void(float)> OnCommitted;

        bool IsEditing() const { return m_Editing; }
        // Being dragged or typed in: whoever mirrors a value into the field leaves it alone meanwhile.
        bool IsInteracting() const { return m_Editing || m_Pressed; }

        UICursorShape GetCursorShape(const UIVec2& pos) const override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnKeyDown(const UIKeyEvent& e) override;
        bool OnTextInput(std::string_view text) override;
        void OnFocusChanged(bool focused) override;

    protected:
        void OnUpdate(float dt) override;
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void BeginEdit();
        void CommitEdit(bool apply = true);
        void SetValue(float next);
        float TextLeft() const;
        float TextSize() const;
        std::string DisplayText() const;
        bool HasRange() const { return Max > Min; }

        bool m_Pressed = false;   // the left button went down on the field (not while editing)
        bool m_Scrubbing = false; // ... and moved far enough: the value is being dragged
        float m_PressX = 0.0f;
        float m_ScrubValue = 0.0f; // the dragged value before clamping and snapping

        bool m_Editing = false;
        bool m_SelectAll = false;
        std::string m_EditText;
        size_t m_Cursor = 0;
        float m_BlinkTimer = 0.0f;
    };
}
