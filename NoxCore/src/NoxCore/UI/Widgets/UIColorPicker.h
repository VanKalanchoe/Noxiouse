#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIContext;
    class UIDragScalar;

    // The picking area of a colour picker: the saturation / value square, the hue bar and (ShowAlpha) the alpha bar.
    // Colours here are the values as displayed (sRGB), as the swatches show them.
    class UIColorPicker : public UIElement
    {
    public:
        explicit UIColorPicker(
            const UIColor& initialColor = { 0.24f, 0.56f, 0.96f, 1.0f },
            std::function<void(const UIColor&)> onColorChanged = {});

        bool ShowAlpha = true;

        // Keeps the hue of a grey and the saturation of a black: they are not in the RGB value, the picker remembers them.
        void SetColor(const UIColor& color, bool notify = false);
        UIColor GetColor() const;
        void SetHsv(float hueDegrees, float saturation, float value, bool notify = false);
        void GetHsv(float& hueDegrees, float& saturation, float& value) const;
        void SetLinearColor(const UIColor& linearColor, bool notify = false);
        UIColor GetLinearColor() const;
        std::string GetHexString() const;

        static UIColor SrgbToLinear(const UIColor& srgb);
        static UIColor LinearToSrgb(const UIColor& linear);
        static UIColor HsvToRgb(float hueDegrees, float saturation, float value, float alpha = 1.0f);
        static void RgbToHsv(const UIColor& color, float& hueDegrees, float& saturation, float& value);
        // "#RRGGBB" / "#RRGGBBAA". Parse reads 3, 4, 6 or 8 hex digits with or without the '#'.
        static std::string FormatHex(const UIColor& color, bool withAlpha = true);
        static bool ParseHex(std::string_view text, UIColor& outColor);

        std::function<void(const UIColor&)> OnColorChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

        // The square stays a square: given less width than it wants (a small window), the picker gets lower with it.
        float ComputeHeightForWidth(float width) override;

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
        };

        LayoutRects ComputeRects() const;
        void UpdateFromPointer(const UIVec2& pos);

        float m_Hue = 210.0f;
        float m_Sat = 0.75f;
        float m_Val = 0.96f;
        float m_Alpha = 1.0f;
        DragRegion m_Dragging = DragRegion::None;
    };

    // A colour property: the colour's swatch and, where there is room, its R / G / B (/ A) values as number fields (0..255:
    // drag them or click to type). A click on the swatch opens the picker in a popup: the square, hue and alpha bars, the new
    // colour beside the one it replaces, RGB, HSV and hex fields. The fields leave first when the editor gets narrow -- the
    // swatch then takes the whole width and the picker holds the numbers.
    class UIColorEdit : public UIElement
    {
    public:
        explicit UIColorEdit(
            UIContext* context = nullptr,
            const UIColor& initialColor = { 0.24f, 0.56f, 0.96f, 1.0f },
            std::function<void(const UIColor&)> onColorChanged = {});

        // false: an RGB colour (no A field, no alpha bar; alpha stays 1).
        bool ShowAlpha = true;

        // The popup opens in the context the click came from; this one is only the fallback outside of an event.
        void SetContext(UIContext* context) { m_Context = context; }
        void SetColor(const UIColor& color, bool notify = false);
        const UIColor& GetColor() const { return m_Color; }
        bool IsPickerOpen() const { return m_PickerOpen && *m_PickerOpen; }

        std::function<void(const UIColor&)> OnColorChanged;

        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void SyncFields();
        void OpenPicker();
        int FieldCount() const { return ShowAlpha ? 4 : 3; }

        UIContext* m_Context = nullptr;
        UIColor m_Color{ 0.24f, 0.56f, 0.96f, 1.0f };
        Ref<UIDragScalar> m_Fields[4];
        UIRect m_SwatchRect{};
        std::shared_ptr<const bool> m_PickerOpen;
    };
}
