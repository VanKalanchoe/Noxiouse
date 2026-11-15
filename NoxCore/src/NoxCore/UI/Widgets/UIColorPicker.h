#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIContext;
    class UILabel;

    // Interactive HSVA color picker (2D Saturation-Value square, Hue bar, Alpha bar, hex readout).
    class UIColorPicker : public UIElement
    {
    public:
        explicit UIColorPicker(
            const UIColor& initialColor = { 0.24f, 0.56f, 0.96f, 1.0f },
            std::function<void(const UIColor&)> onColorChanged = {});

        void SetColor(const UIColor& color, bool notify = false);
        UIColor GetColor() const;
        void SetLinearColor(const UIColor& linearColor, bool notify = false);
        UIColor GetLinearColor() const;
        std::string GetHexString() const;

        static UIColor SrgbToLinear(const UIColor& srgb);
        static UIColor LinearToSrgb(const UIColor& linear);

        std::function<void(const UIColor&)> OnColorChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        enum class DragRegion : uint8_t
        {
            None = 0,
            SV,
            Hue,
            Alpha
        };

        struct LayoutRects
        {
            UIRect SV;
            UIRect Hue;
            UIRect Alpha;
            UIRect Preview;
        };

        LayoutRects ComputeRects() const;
        void UpdateFromPointer(const UIVec2& pos);

        float m_Hue = 210.0f;
        float m_Sat = 0.75f;
        float m_Val = 0.96f;
        float m_Alpha = 1.0f;
        DragRegion m_Dragging = DragRegion::None;
    };

    // Compact color swatch button + hex readout that opens a UIColorPicker overlay popover.
    class UIColorEdit : public UIElement
    {
    public:
        explicit UIColorEdit(
            UIContext* context = nullptr,
            const UIColor& initialColor = { 0.24f, 0.56f, 0.96f, 1.0f },
            std::function<void(const UIColor&)> onColorChanged = {});

        void SetContext(UIContext* context) { m_Context = context; }
        void SetColor(const UIColor& color, bool notify = false);
        const UIColor& GetColor() const { return m_Color; }

        std::function<void(const UIColor&)> OnColorChanged;

        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void SyncLabel();
        void OpenPicker();

        UIContext* m_Context = nullptr;
        UIColor m_Color{ 0.24f, 0.56f, 0.96f, 1.0f };
        Ref<UILabel> m_Label;
    };
}
