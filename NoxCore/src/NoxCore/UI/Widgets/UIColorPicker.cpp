#include "UIColorPicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIPopup.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        float SrgbChannelToLinear(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearChannelToSrgb(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
        }

        float ToByte(float channel)
        {
            return std::round(std::clamp(channel, 0.0f, 1.0f) * 255.0f);
        }

        // The grey squares behind a colour that is not opaque.
        void DrawChecker(UIDrawList& drawList, const UIRect& rect, float cell)
        {
            drawList.AddRoundedRect(rect.Min, rect.Max, { 0.30f, 0.30f, 0.30f, 1.0f });
            drawList.PushClipRect(rect.Min, rect.Max);
            int row = 0;
            for (float y = rect.Min.y; y < rect.Max.y; y += cell, ++row)
            {
                int column = row % 2;
                for (float x = rect.Min.x + (row % 2 ? cell : 0.0f); x < rect.Max.x; x += cell * 2.0f, column += 2)
                    drawList.AddRoundedRect({ x, y }, { x + cell, y + cell }, { 0.52f, 0.52f, 0.52f, 1.0f });
            }
            drawList.PopClipRect();
        }

        // A colour's swatch: opaque on its left half; a colour with alpha shows through the checker on its right half.
        void DrawSwatch(UIDrawList& drawList, const UIRect& rect, const UIColor& srgb, bool showAlpha, float radius,
                        float borderThickness, const UIColor& borderColor)
        {
            const UIColor linear = UIColorPicker::SrgbToLinear(srgb);
            const UIColor opaque{ linear.r, linear.g, linear.b, 1.0f };
            if (showAlpha && srgb.a < 0.999f && rect.Width() > 12.0f)
            {
                const float mid = std::floor((rect.Min.x + rect.Max.x) * 0.5f);
                drawList.PushClipRect(rect.Min, rect.Max, glm::vec4(radius));
                drawList.AddRoundedRect(rect.Min, { mid, rect.Max.y }, opaque);
                const UIRect right{ { mid, rect.Min.y }, rect.Max };
                DrawChecker(drawList, right, std::max(4.0f, std::floor(rect.Height() / 3.0f)));
                drawList.AddRoundedRect(right.Min, right.Max, { linear.r, linear.g, linear.b, std::clamp(srgb.a, 0.0f, 1.0f) });
                drawList.PopClipRect();
                drawList.AddRoundedRect(rect.Min, rect.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(radius), borderThickness, borderColor);
            }
            else
            {
                drawList.AddRoundedRect(rect.Min, rect.Max, opaque, glm::vec4(radius), borderThickness, borderColor);
            }
        }

        // The colour being picked beside the one the picker was opened with; a click on the old one brings it back.
        class UIColorCompare final : public UIElement
        {
        public:
            UIColorCompare()
            {
                HitTestMode = UIHitTestMode::Self;
                WidthMode = UISizeMode::Fill;
                Tooltip = "Left: the new colour. Right: the colour before -- click it to go back to it.";
            }

            UIColor Current{ 1.0f };
            UIColor Original{ 1.0f };
            bool ShowAlpha = true;
            std::function<void()> OnRevert;

            bool OnPointerUp(const UIPointerEvent& e) override
            {
                const UIRect& b = GetBounds();
                if (e.Button == 1 && b.Contains(e.Position) && e.Position.x > (b.Min.x + b.Max.x) * 0.5f && OnRevert)
                    OnRevert();
                return true;
            }

        protected:
            UIVec2 ComputeDesiredSize() override { return { 80.0f, 26.0f }; }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                const UIRect& b = GetBounds();
                const float mid = std::floor((b.Min.x + b.Max.x) * 0.5f);
                const UIColor border = theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
                drawList.PushClipRect(b.Min, b.Max, glm::vec4(4.0f));
                DrawSwatch(drawList, { b.Min, { mid, b.Max.y } }, Current, ShowAlpha, 0.0f, 0.0f, border);
                DrawSwatch(drawList, { { mid, b.Min.y }, b.Max }, Original, ShowAlpha, 0.0f, 0.0f, border);
                drawList.PopClipRect();
                drawList.AddRoundedRect(b.Min, b.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(4.0f), 1.0f, border);
            }
        };

        // What the swatch's popup shows: the picking area, new / old colour, and the colour as RGB, HSV and hex -- each one
        // editable, the others following.
        class UIColorPickerPanel final : public UIStackBox
        {
        public:
            UIColorPickerPanel(const UIColor& color, bool showAlpha, std::function<void(const UIColor&)> onChanged)
                : UIStackBox(UIOrientation::Vertical, 8.0f), m_ShowAlpha(showAlpha), m_OnChanged(std::move(onChanged))
            {
                StyleClass = "MenuPopup";
                DrawBackground = true;
                HitTestMode = UIHitTestMode::Self; // a press between the controls stays in the popup
                Padding = UIThickness::All(10.0f);

                m_Picker = Ref<UIColorPicker>(new UIColorPicker(color, [this](const UIColor&) { Changed(m_Picker.get()); }));
                m_Picker->ShowAlpha = showAlpha;
                AddChild(m_Picker);

                m_Compare = Ref<UIColorCompare>(new UIColorCompare());
                m_Compare->Current = color;
                m_Compare->Original = color;
                m_Compare->ShowAlpha = showAlpha;
                m_Compare->OnRevert = [this, original = color]()
                {
                    m_Picker->SetColor(original);
                    Changed(nullptr);
                };
                AddChild(m_Compare);

                static constexpr const char* kRgbNames[4] = { "R", "G", "B", "A" };
                static constexpr UIColor kRgbColors[4] = {
                    { 0.80f, 0.26f, 0.26f, 1.0f }, { 0.24f, 0.62f, 0.32f, 1.0f }, { 0.24f, 0.46f, 0.86f, 1.0f }, { 0.42f, 0.45f, 0.52f, 1.0f } };
                Ref<UIStackBox> rgbRow(new UIStackBox(UIOrientation::Horizontal, 4.0f));
                rgbRow->WidthMode = UISizeMode::Fill;
                for (int i = 0; i < (showAlpha ? 4 : 3); ++i)
                {
                    m_Rgb[i] = MakeField(kRgbNames[i], kRgbColors[i], 255.0f);
                    UIDragScalar* field = m_Rgb[i].get();
                    field->OnChanged = [this, field](float)
                    {
                        UIColor next = m_Picker->GetColor();
                        for (int c = 0; c < 4; ++c)
                        {
                            if (m_Rgb[c])
                                next[c] = m_Rgb[c]->Value / 255.0f;
                        }
                        m_Picker->SetColor(next);
                        Changed(field);
                    };
                    rgbRow->AddChild(m_Rgb[i]);
                }
                AddChild(rgbRow);

                static constexpr const char* kHsvNames[3] = { "H", "S", "V" };
                static constexpr float kHsvMax[3] = { 360.0f, 100.0f, 100.0f };
                Ref<UIStackBox> hsvRow(new UIStackBox(UIOrientation::Horizontal, 4.0f));
                hsvRow->WidthMode = UISizeMode::Fill;
                for (int i = 0; i < 3; ++i)
                {
                    m_Hsv[i] = MakeField(kHsvNames[i], { 0.30f, 0.33f, 0.40f, 1.0f }, kHsvMax[i]);
                    UIDragScalar* field = m_Hsv[i].get();
                    field->OnChanged = [this, field](float)
                    {
                        m_Picker->SetHsv(m_Hsv[0]->Value, m_Hsv[1]->Value / 100.0f, m_Hsv[2]->Value / 100.0f);
                        Changed(field);
                    };
                    hsvRow->AddChild(m_Hsv[i]);
                }
                AddChild(hsvRow);

                Ref<UIStackBox> hexRow(new UIStackBox(UIOrientation::Horizontal, 6.0f));
                hexRow->WidthMode = UISizeMode::Fill;
                Ref<UILabel> hexLabel(new UILabel("Hex", 15.0f));
                hexLabel->VAlign = UIAlignment::Center;
                hexRow->AddChild(hexLabel);
                m_Hex = Ref<UITextInput>(new UITextInput(UIColorPicker::FormatHex(color, showAlpha)));
                m_Hex->WidthMode = UISizeMode::Fill;
                m_Hex->Tooltip = showAlpha ? "#RRGGBBAA (or #RRGGBB, #RGB)" : "#RRGGBB (or #RGB)";
                m_Hex->OnChanged = [this](const std::string& text)
                {
                    UIColor parsed{};
                    if (!UIColorPicker::ParseHex(text, parsed))
                        return; // not a colour yet: nothing changes while it is being typed
                    if (!m_ShowAlpha)
                        parsed.a = 1.0f;
                    m_Picker->SetColor(parsed);
                    Changed(m_Hex.get());
                };
                hexRow->AddChild(m_Hex);
                AddChild(hexRow);

                Refresh(nullptr);
            }

        private:
            static Ref<UIDragScalar> MakeField(const char* name, const UIColor& tagColor, float maxValue)
            {
                Ref<UIDragScalar> field(new UIDragScalar(0.0f, maxValue / 255.0f));
                field->Prefix = name;
                field->PrefixColor = tagColor;
                field->Min = 0.0f;
                field->Max = maxValue;
                field->Step = 1.0f;
                field->Format = "{:.0f}";
                field->WidthMode = UISizeMode::Fill;
                return field;
            }

            // Every control but `source` (the one the user is working) follows the picker's colour.
            void Refresh(const UIElement* source)
            {
                const UIColor color = m_Picker->GetColor();
                for (int i = 0; i < 4; ++i)
                {
                    if (m_Rgb[i] && m_Rgb[i].get() != source && !m_Rgb[i]->IsInteracting())
                        m_Rgb[i]->Value = ToByte(color[i]);
                }
                float hsv[3]{};
                m_Picker->GetHsv(hsv[0], hsv[1], hsv[2]);
                hsv[1] *= 100.0f;
                hsv[2] *= 100.0f;
                for (int i = 0; i < 3; ++i)
                {
                    if (m_Hsv[i].get() != source && !m_Hsv[i]->IsInteracting())
                        m_Hsv[i]->Value = std::round(hsv[i]);
                }
                if (m_Hex.get() != source && !HasState(m_Hex->GetState(), UIState::Focused))
                    m_Hex->Text = UIColorPicker::FormatHex(color, m_ShowAlpha);
                m_Compare->Current = color;
            }

            void Changed(const UIElement* source)
            {
                Refresh(source);
                if (m_OnChanged)
                    m_OnChanged(m_Picker->GetColor());
            }

            bool m_ShowAlpha = true;
            std::function<void(const UIColor&)> m_OnChanged;
            Ref<UIColorPicker> m_Picker;
            Ref<UIColorCompare> m_Compare;
            Ref<UIDragScalar> m_Rgb[4];
            Ref<UIDragScalar> m_Hsv[3];
            Ref<UITextInput> m_Hex;
        };
    }

    UIColor UIColorPicker::HsvToRgb(float hDeg, float s, float v, float a)
    {
        float h = std::fmod(hDeg, 360.0f);
        if (h < 0.0f)
            h += 360.0f;

        const float c = v * s;
        const float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        const float m = v - c;

        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (h < 60.0f)       { r = c; g = x; b = 0.0f; }
        else if (h < 120.0f) { r = x; g = c; b = 0.0f; }
        else if (h < 180.0f) { r = 0.0f; g = c; b = x; }
        else if (h < 240.0f) { r = 0.0f; g = x; b = c; }
        else if (h < 300.0f) { r = x; g = 0.0f; b = c; }
        else                 { r = c; g = 0.0f; b = x; }

        return { r + m, g + m, b + m, a };
    }

    void UIColorPicker::RgbToHsv(const UIColor& c, float& outH, float& outS, float& outV)
    {
        const float maxC = std::max({ c.r, c.g, c.b });
        const float minC = std::min({ c.r, c.g, c.b });
        const float delta = maxC - minC;

        outV = maxC;
        outS = (maxC > 1e-5f) ? (delta / maxC) : 0.0f;

        if (delta <= 1e-5f)
            outH = 0.0f;
        else if (maxC == c.r)
            outH = 60.0f * std::fmod((c.g - c.b) / delta, 6.0f);
        else if (maxC == c.g)
            outH = 60.0f * (((c.b - c.r) / delta) + 2.0f);
        else
            outH = 60.0f * (((c.r - c.g) / delta) + 4.0f);
        if (outH < 0.0f)
            outH += 360.0f;
    }

    std::string UIColorPicker::FormatHex(const UIColor& c, bool withAlpha)
    {
        char buf[16]{};
        if (withAlpha)
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", static_cast<unsigned>(ToByte(c.r)), static_cast<unsigned>(ToByte(c.g)),
                          static_cast<unsigned>(ToByte(c.b)), static_cast<unsigned>(ToByte(c.a)));
        else
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", static_cast<unsigned>(ToByte(c.r)), static_cast<unsigned>(ToByte(c.g)),
                          static_cast<unsigned>(ToByte(c.b)));
        return buf;
    }

    bool UIColorPicker::ParseHex(std::string_view text, UIColor& outColor)
    {
        int digits[8]{};
        int count = 0;
        for (const char ch : text)
        {
            if (ch == '#' || ch == ' ' || ch == '\t')
                continue;
            int value = -1;
            if (ch >= '0' && ch <= '9') value = ch - '0';
            else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
            if (value < 0 || count >= 8)
                return false;
            digits[count++] = value;
        }

        float channels[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (count == 3 || count == 4)
        {
            for (int i = 0; i < count; ++i)
                channels[i] = static_cast<float>(digits[i] * 17) / 255.0f; // "F" is "FF"
        }
        else if (count == 6 || count == 8)
        {
            for (int i = 0; i < count / 2; ++i)
                channels[i] = static_cast<float>(digits[i * 2] * 16 + digits[i * 2 + 1]) / 255.0f;
        }
        else
        {
            return false;
        }
        outColor = { channels[0], channels[1], channels[2], channels[3] };
        return true;
    }

    UIColorPicker::UIColorPicker(
        const UIColor& initialColor,
        std::function<void(const UIColor&)> onColorChanged)
        : OnColorChanged(std::move(onColorChanged))
    {
        StyleClass = "Panel";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        // The hue of a grey start colour is unknown: 0 until the user picks one.
        m_Hue = 0.0f;
        m_Sat = 0.0f;
        SetColor(initialColor, false);
    }

    void UIColorPicker::SetColor(const UIColor& color, bool notify)
    {
        float hue = 0.0f, sat = 0.0f, val = 0.0f;
        RgbToHsv(color, hue, sat, val);
        if (val > 1e-4f)
        {
            m_Sat = sat;
            if (sat > 1e-4f)
                m_Hue = hue;
        }
        m_Val = val;
        m_Alpha = std::clamp(color.a, 0.0f, 1.0f);
        if (notify && OnColorChanged)
            OnColorChanged(GetColor());
    }

    UIColor UIColorPicker::GetColor() const
    {
        return HsvToRgb(m_Hue, m_Sat, m_Val, ShowAlpha ? m_Alpha : 1.0f);
    }

    void UIColorPicker::SetHsv(float hueDegrees, float saturation, float value, bool notify)
    {
        m_Hue = std::clamp(hueDegrees, 0.0f, 360.0f);
        m_Sat = std::clamp(saturation, 0.0f, 1.0f);
        m_Val = std::clamp(value, 0.0f, 1.0f);
        if (notify && OnColorChanged)
            OnColorChanged(GetColor());
    }

    void UIColorPicker::GetHsv(float& hueDegrees, float& saturation, float& value) const
    {
        hueDegrees = m_Hue;
        saturation = m_Sat;
        value = m_Val;
    }

    UIColor UIColorPicker::SrgbToLinear(const UIColor& srgb)
    {
        return {
            SrgbChannelToLinear(srgb.r),
            SrgbChannelToLinear(srgb.g),
            SrgbChannelToLinear(srgb.b),
            srgb.a
        };
    }

    UIColor UIColorPicker::LinearToSrgb(const UIColor& linear)
    {
        return {
            LinearChannelToSrgb(linear.r),
            LinearChannelToSrgb(linear.g),
            LinearChannelToSrgb(linear.b),
            linear.a
        };
    }

    void UIColorPicker::SetLinearColor(const UIColor& linearColor, bool notify)
    {
        SetColor(LinearToSrgb(linearColor), notify);
    }

    UIColor UIColorPicker::GetLinearColor() const
    {
        return SrgbToLinear(GetColor());
    }

    std::string UIColorPicker::GetHexString() const
    {
        return FormatHex(GetColor(), ShowAlpha);
    }

    namespace
    {
        constexpr float kBarWidth = 18.0f;
        constexpr float kBarGap = 8.0f;
        constexpr float kSquare = 220.0f;
    }

    UIColorPicker::LayoutRects UIColorPicker::ComputeRects() const
    {
        const UIRect b = GetBounds().Inset(Padding);
        const float bars = (kBarGap + kBarWidth) * (ShowAlpha ? 2.0f : 1.0f);
        const float size = std::max(40.0f, std::min(b.Width() - bars, b.Height()));

        LayoutRects r{};
        r.SV = { b.Min, { b.Min.x + size, b.Min.y + size } };
        r.Hue = { { r.SV.Max.x + kBarGap, b.Min.y }, { r.SV.Max.x + kBarGap + kBarWidth, b.Min.y + size } };
        if (ShowAlpha)
            r.Alpha = { { r.Hue.Max.x + kBarGap, b.Min.y }, { r.Hue.Max.x + kBarGap + kBarWidth, b.Min.y + size } };
        return r;
    }

    void UIColorPicker::UpdateFromPointer(const UIVec2& pos)
    {
        const LayoutRects r = ComputeRects();
        if (m_Dragging == DragRegion::SV)
        {
            m_Sat = std::clamp((pos.x - r.SV.Min.x) / std::max(1.0f, r.SV.Width()), 0.0f, 1.0f);
            m_Val = 1.0f - std::clamp((pos.y - r.SV.Min.y) / std::max(1.0f, r.SV.Height()), 0.0f, 1.0f);
        }
        else if (m_Dragging == DragRegion::Hue)
        {
            const float t = std::clamp((pos.y - r.Hue.Min.y) / std::max(1.0f, r.Hue.Height()), 0.0f, 1.0f);
            m_Hue = t * 360.0f;
        }
        else if (m_Dragging == DragRegion::Alpha)
        {
            // Opaque at the top, like the value in the square.
            m_Alpha = 1.0f - std::clamp((pos.y - r.Alpha.Min.y) / std::max(1.0f, r.Alpha.Height()), 0.0f, 1.0f);
        }

        if (m_Dragging != DragRegion::None && OnColorChanged)
            OnColorChanged(GetColor());
    }

    bool UIColorPicker::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        // A few pixels around each area still take the press: the bars are narrow and their ends are where 0 and 1 are.
        auto grown = [](UIRect rect) { rect.Min -= UIVec2{ 3.0f, 3.0f }; rect.Max += UIVec2{ 3.0f, 3.0f }; return rect; };
        const LayoutRects r = ComputeRects();
        if (r.SV.Contains(e.Position))
            m_Dragging = DragRegion::SV;
        else if (grown(r.Hue).Contains(e.Position))
            m_Dragging = DragRegion::Hue;
        else if (ShowAlpha && grown(r.Alpha).Contains(e.Position))
            m_Dragging = DragRegion::Alpha;
        else
            m_Dragging = DragRegion::None;

        if (m_Dragging != DragRegion::None)
        {
            if (UIContext* ctx = UIContext::GetCurrent())
                ctx->CancelPendingDrag();
            UpdateFromPointer(e.Position);
        }
        return true;
    }

    bool UIColorPicker::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Dragging == DragRegion::None)
            return false;
        UpdateFromPointer(e.Position);
        return true;
    }

    bool UIColorPicker::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button == 1 && m_Dragging != DragRegion::None)
        {
            UpdateFromPointer(e.Position);
            m_Dragging = DragRegion::None;
            return true;
        }
        return false;
    }

    UIVec2 UIColorPicker::ComputeDesiredSize()
    {
        const float bars = (kBarGap + kBarWidth) * (ShowAlpha ? 2.0f : 1.0f);
        return { kSquare + bars + Padding.Horizontal(), kSquare + Padding.Vertical() };
    }

    float UIColorPicker::ComputeHeightForWidth(float width)
    {
        const float bars = (kBarGap + kBarWidth) * (ShowAlpha ? 2.0f : 1.0f);
        return std::clamp(width - Padding.Horizontal() - bars, 40.0f, kSquare) + Padding.Vertical();
    }

    void UIColorPicker::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const LayoutRects r = ComputeRects();
        const UIColor border = theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
        const UIColor markerFill{ 1.0f, 1.0f, 1.0f, 1.0f };
        const UIColor markerEdge{ 0.04f, 0.05f, 0.07f, 1.0f };

        // 1. The saturation (left to right) / value (top to bottom) square, in small gradient tiles: the renderer blends
        //    in linear light, so a single gradient from the top colour to black would look far too bright in its middle.
        constexpr int kColumns = 32;
        constexpr int kRows = 10;
        const float tileW = r.SV.Width() / static_cast<float>(kColumns);
        const float tileH = r.SV.Height() / static_cast<float>(kRows);
        drawList.PushClipRect(r.SV.Min, r.SV.Max, glm::vec4(3.0f));
        for (int column = 0; column < kColumns; ++column)
        {
            const float sat = (static_cast<float>(column) + 0.5f) / static_cast<float>(kColumns);
            const float x0 = r.SV.Min.x + static_cast<float>(column) * tileW;
            for (int row = 0; row < kRows; ++row)
            {
                const float valueTop = 1.0f - static_cast<float>(row) / static_cast<float>(kRows);
                const float valueBottom = 1.0f - static_cast<float>(row + 1) / static_cast<float>(kRows);
                const float y0 = r.SV.Min.y + static_cast<float>(row) * tileH;
                drawList.AddRoundedRectGradient(
                    { x0, y0 }, { x0 + tileW + 0.5f, y0 + tileH + 0.5f },
                    SrgbToLinear(HsvToRgb(m_Hue, sat, valueTop)),
                    SrgbToLinear(HsvToRgb(m_Hue, sat, valueBottom)));
            }
        }
        drawList.PopClipRect();
        drawList.AddRoundedRect(r.SV.Min, r.SV.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(3.0f), 1.0f, border);

        // The picked point: a white ring in a dark one, readable on every colour.
        const UIVec2 point{ r.SV.Min.x + m_Sat * r.SV.Width(), r.SV.Min.y + (1.0f - m_Val) * r.SV.Height() };
        drawList.AddRoundedRect(point - UIVec2{ 7.0f, 7.0f }, point + UIVec2{ 7.0f, 7.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(7.0f), 1.0f, markerEdge);
        drawList.AddRoundedRect(point - UIVec2{ 6.0f, 6.0f }, point + UIVec2{ 6.0f, 6.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(6.0f), 2.0f, markerFill);

        // 2. The hue bar, red at both ends.
        constexpr int kHueSteps = 36;
        const float hueStep = r.Hue.Height() / static_cast<float>(kHueSteps);
        drawList.PushClipRect(r.Hue.Min, r.Hue.Max, glm::vec4(3.0f));
        for (int i = 0; i < kHueSteps; ++i)
        {
            const float y0 = r.Hue.Min.y + static_cast<float>(i) * hueStep;
            drawList.AddRoundedRectGradient(
                { r.Hue.Min.x, y0 }, { r.Hue.Max.x, y0 + hueStep + 0.5f },
                SrgbToLinear(HsvToRgb(360.0f * static_cast<float>(i) / static_cast<float>(kHueSteps), 1.0f, 1.0f)),
                SrgbToLinear(HsvToRgb(360.0f * static_cast<float>(i + 1) / static_cast<float>(kHueSteps), 1.0f, 1.0f)));
        }
        drawList.PopClipRect();
        drawList.AddRoundedRect(r.Hue.Min, r.Hue.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(3.0f), 1.0f, border);

        auto drawBarMarker = [&](const UIRect& bar, float t)
        {
            const float y = bar.Min.y + std::clamp(t, 0.0f, 1.0f) * bar.Height();
            drawList.AddRoundedRect({ bar.Min.x - 3.0f, y - 4.0f }, { bar.Max.x + 3.0f, y + 4.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(3.0f), 1.0f, markerEdge);
            drawList.AddRoundedRect({ bar.Min.x - 2.0f, y - 3.0f }, { bar.Max.x + 2.0f, y + 3.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(2.5f), 2.0f, markerFill);
        };
        drawBarMarker(r.Hue, m_Hue / 360.0f);

        // 3. The alpha bar: the colour, opaque at the top, fading into the checker.
        if (ShowAlpha)
        {
            drawList.PushClipRect(r.Alpha.Min, r.Alpha.Max, glm::vec4(3.0f));
            DrawChecker(drawList, r.Alpha, kBarWidth * 0.5f);
            const UIColor solid = SrgbToLinear(HsvToRgb(m_Hue, m_Sat, m_Val, 1.0f));
            drawList.AddRoundedRectGradient(r.Alpha.Min, r.Alpha.Max, solid, { solid.r, solid.g, solid.b, 0.0f });
            drawList.PopClipRect();
            drawList.AddRoundedRect(r.Alpha.Min, r.Alpha.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(3.0f), 1.0f, border);
            drawBarMarker(r.Alpha, 1.0f - m_Alpha);
        }
    }

    namespace
    {
        constexpr float kSwatchWidth = 36.0f;
        constexpr float kFieldGap = 4.0f;
        constexpr float kFieldFontSize = 15.0f; // the rows' label size: four fields fit in an inspector's value column
    }

    UIColorEdit::UIColorEdit(
        UIContext* context,
        const UIColor& initialColor,
        std::function<void(const UIColor&)> onColorChanged)
        : OnColorChanged(std::move(onColorChanged))
        , m_Context(context)
        , m_Color(initialColor)
    {
        // The number fields' style, for their font size (the editor itself draws only the swatch).
        StyleClass = "DragScalar";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
        MinSize = { kSwatchWidth, 26.0f };
        Tooltip = "Click the colour to open the picker";

        // Compact: the number alone, the channel told by the field's coloured edge (and its tooltip).
        static constexpr UIColor kStripes[4] = {
            { 0.86f, 0.27f, 0.27f, 1.0f }, { 0.26f, 0.70f, 0.36f, 1.0f }, { 0.26f, 0.52f, 0.94f, 1.0f }, { 0.62f, 0.66f, 0.74f, 1.0f } };
        static constexpr const char* kTips[4] = {
            "Red, 0 - 255. Drag, or click to type.", "Green, 0 - 255. Drag, or click to type.",
            "Blue, 0 - 255. Drag, or click to type.", "Alpha (opacity), 0 - 255. Drag, or click to type." };
        for (int i = 0; i < 4; ++i)
        {
            Ref<UIDragScalar> field(new UIDragScalar(0.0f, 1.0f));
            field->Min = 0.0f;
            field->Max = 255.0f;
            field->Step = 1.0f;
            field->Format = "{:.0f}";
            field->FontSize = kFieldFontSize;
            field->Stripe = kStripes[i];
            field->Tooltip = kTips[i];
            field->OnChanged = [this, i](float value)
            {
                m_Color[i] = value / 255.0f;
                if (OnColorChanged)
                    OnColorChanged(m_Color);
            };
            m_Fields[i] = field;
            AddChild(field);
        }
        SyncFields();
    }

    void UIColorEdit::SetColor(const UIColor& color, bool notify)
    {
        m_Color = color;
        if (!ShowAlpha)
            m_Color.a = 1.0f;
        SyncFields();
        if (notify && OnColorChanged)
            OnColorChanged(m_Color);
    }

    void UIColorEdit::SyncFields()
    {
        for (int i = 0; i < 4; ++i)
        {
            if (m_Fields[i] && !m_Fields[i]->IsInteracting())
                m_Fields[i]->Value = ToByte(m_Color[i]);
        }
    }

    bool UIColorEdit::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;
        if (m_SwatchRect.Contains(e.Position))
            OpenPicker();
        return true;
    }

    UIVec2 UIColorEdit::ComputeDesiredSize()
    {
        float height = MinSize.y;
        for (int i = 0; i < 4; ++i)
        {
            if (m_Fields[i])
                height = std::max(height, m_Fields[i]->Measure().y);
        }
        return { MinSize.x, height };
    }

    void UIColorEdit::ArrangeChildren(const UIRect& contentRect)
    {
        const UIStyleBox& style = GetResolvedStyle();
        const int count = FieldCount();
        // A field is worth showing when "255" fits in it.
        const float minField = UILabel::MeasureText("255", nullptr, kFieldFontSize).x + style.Padding.Horizontal() + 6.0f;
        const float fieldsWidth = contentRect.Width() - kSwatchWidth - kFieldGap * static_cast<float>(count);
        const bool showFields = fieldsWidth >= minField * static_cast<float>(count);

        m_SwatchRect = contentRect;
        if (showFields)
        {
            m_SwatchRect.Max.x = contentRect.Min.x + kSwatchWidth;
            const float fieldW = fieldsWidth / static_cast<float>(count);
            float x = m_SwatchRect.Max.x + kFieldGap;
            for (int i = 0; i < count; ++i)
            {
                m_Fields[i]->Visible = true;
                m_Fields[i]->Arrange({ { x, contentRect.Min.y }, { x + fieldW, contentRect.Max.y } });
                x += fieldW + kFieldGap;
            }
        }
        for (int i = showFields ? count : 0; i < 4; ++i)
            m_Fields[i]->Visible = false;
    }

    void UIColorEdit::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        // Under the pointer the swatch takes the accent colour's border: it is a button.
        const bool lit = HasState(GetState(), UIState::Hovered) || IsPickerOpen();
        const UIColor border = lit
            ? theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f })
            : theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
        DrawSwatch(drawList, m_SwatchRect, m_Color, ShowAlpha, 3.0f, lit ? 1.5f : 1.0f, border);
    }

    void UIColorEdit::OpenPicker()
    {
        // The context of the click (a panel may sit in the main window or in a floating one); the stored one outside events.
        UIContext* context = UIContext::GetCurrent() ? UIContext::GetCurrent() : m_Context;
        if (!context)
            return;

        // The popup keeps the editor alive: it can outlast the panel that made the editor (a panel rebuilt under it).
        Ref<UIColorEdit> self(this);
        Ref<UIColorPickerPanel> panel(new UIColorPickerPanel(m_Color, ShowAlpha, [self](const UIColor& color)
        {
            self->SetColor(color, true);
        }));
        m_PickerOpen = UIPopup::Show(*context, m_SwatchRect, panel);
    }
}
