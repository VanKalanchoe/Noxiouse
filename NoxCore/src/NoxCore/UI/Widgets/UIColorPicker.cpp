#include "UIColorPicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        UIColor HsvToRgb(float hDeg, float s, float v, float a = 1.0f)
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

        void RgbToHsv(const UIColor& c, float& outH, float& outS, float& outV)
        {
            const float maxC = std::max({ c.r, c.g, c.b });
            const float minC = std::min({ c.r, c.g, c.b });
            const float delta = maxC - minC;

            outV = maxC;
            outS = (maxC > 1e-5f) ? (delta / maxC) : 0.0f;

            if (delta <= 1e-5f)
            {
                outH = 0.0f;
            }
            else if (maxC == c.r)
            {
                outH = 60.0f * std::fmod((c.g - c.b) / delta, 6.0f);
            }
            else if (maxC == c.g)
            {
                outH = 60.0f * (((c.b - c.r) / delta) + 2.0f);
            }
            else
            {
                outH = 60.0f * (((c.r - c.g) / delta) + 4.0f);
            }
            if (outH < 0.0f)
                outH += 360.0f;
        }

        std::string FormatHex(const UIColor& c)
        {
            auto toByte = [](float f) -> unsigned int
            {
                return static_cast<unsigned int>(std::clamp(std::round(f * 255.0f), 0.0f, 255.0f));
            };
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
                toByte(c.r), toByte(c.g), toByte(c.b), toByte(c.a));
            return buf;
        }

        class UIColorPopoverBackdrop : public UIAnchorBox
        {
        public:
            explicit UIColorPopoverBackdrop(UIContext* context)
                : m_Context(context)
            {
                HitTestMode = UIHitTestMode::Self;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button == 1 && m_Context)
                {
                    m_Context->ClearOverlay();
                    return true;
                }
                return false;
            }

        private:
            UIContext* m_Context = nullptr;
        };
    }

    UIColorPicker::UIColorPicker(
        const UIColor& initialColor,
        std::function<void(const UIColor&)> onColorChanged)
        : OnColorChanged(std::move(onColorChanged))
    {
        StyleClass = "Panel";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        Padding = UIThickness::All(10.0f);
        SetColor(initialColor, false);
    }

    void UIColorPicker::SetColor(const UIColor& color, bool notify)
    {
        RgbToHsv(color, m_Hue, m_Sat, m_Val);
        m_Alpha = std::clamp(color.a, 0.0f, 1.0f);
        if (notify && OnColorChanged)
            OnColorChanged(GetColor());
    }

    UIColor UIColorPicker::GetColor() const
    {
        return HsvToRgb(m_Hue, m_Sat, m_Val, m_Alpha);
    }

    std::string UIColorPicker::GetHexString() const
    {
        return FormatHex(GetColor());
    }

    UIColorPicker::LayoutRects UIColorPicker::ComputeRects() const
    {
        const UIRect& b = GetBounds();
        const float x0 = b.Min.x + Padding.Left;
        const float y0 = b.Min.y + Padding.Top;
        const float availW = std::max(120.0f, b.Width() - Padding.Horizontal());
        const float hueW = 20.0f;
        const float gap = 8.0f;
        const float svSize = std::max(100.0f, availW - hueW - gap);

        LayoutRects r{};
        r.SV = { { x0, y0 }, { x0 + svSize, y0 + svSize } };
        r.Hue = { { r.SV.Max.x + gap, y0 }, { r.SV.Max.x + gap + hueW, y0 + svSize } };
        r.Alpha = { { x0, r.SV.Max.y + gap }, { x0 + svSize + gap + hueW, r.SV.Max.y + gap + 16.0f } };
        r.Preview = { { x0, r.Alpha.Max.y + gap }, { x0 + 44.0f, r.Alpha.Max.y + gap + 22.0f } };
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
            m_Hue = t * 359.99f;
        }
        else if (m_Dragging == DragRegion::Alpha)
        {
            m_Alpha = std::clamp((pos.x - r.Alpha.Min.x) / std::max(1.0f, r.Alpha.Width()), 0.0f, 1.0f);
        }

        if (m_Dragging != DragRegion::None && OnColorChanged)
            OnColorChanged(GetColor());
    }

    bool UIColorPicker::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;

        const LayoutRects r = ComputeRects();
        if (r.SV.Contains(e.Position))
            m_Dragging = DragRegion::SV;
        else if (r.Hue.Contains(e.Position))
            m_Dragging = DragRegion::Hue;
        else if (r.Alpha.Contains(e.Position))
            m_Dragging = DragRegion::Alpha;
        else
            m_Dragging = DragRegion::None;

        if (m_Dragging != DragRegion::None)
            UpdateFromPointer(e.Position);
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
        return { 216.0f, 236.0f };
    }

    void UIColorPicker::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const LayoutRects r = ComputeRects();
        const UIColor pureHue = HsvToRgb(m_Hue, 1.0f, 1.0f, 1.0f);

        // 1. 2D Saturation-Value square via vertical gradient slices
        constexpr int kSlices = 24;
        const float sliceW = r.SV.Width() / static_cast<float>(kSlices);
        for (int i = 0; i < kSlices; ++i)
        {
            const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(kSlices);
            const UIColor topColor{
                1.0f + (pureHue.r - 1.0f) * t,
                1.0f + (pureHue.g - 1.0f) * t,
                1.0f + (pureHue.b - 1.0f) * t,
                1.0f
            };
            const float xA = r.SV.Min.x + static_cast<float>(i) * sliceW;
            const float xB = (i == kSlices - 1) ? r.SV.Max.x : (xA + sliceW + 0.5f);
            drawList.AddRoundedRectGradient(
                { xA, r.SV.Min.y },
                { xB, r.SV.Max.y },
                topColor,
                { 0.0f, 0.0f, 0.0f, 1.0f }
            );
        }
        drawList.AddRoundedRect(r.SV.Min, r.SV.Max, { 0.0f, 0.0f, 0.0f, 0.0f }, glm::vec4(2.0f), 1.0f, { 0.25f, 0.28f, 0.34f, 1.0f });

        // SV handle
        const UIVec2 svHandle{
            r.SV.Min.x + m_Sat * r.SV.Width(),
            r.SV.Min.y + (1.0f - m_Val) * r.SV.Height()
        };
        drawList.AddRoundedRect(
            svHandle - UIVec2{ 5.0f, 5.0f },
            svHandle + UIVec2{ 5.0f, 5.0f },
            { 0.0f, 0.0f, 0.0f, 0.0f },
            glm::vec4(5.0f),
            2.0f,
            m_Val > 0.5f ? UIColor{ 0.08f, 0.08f, 0.10f, 1.0f } : UIColor{ 0.96f, 0.96f, 0.98f, 1.0f }
        );

        // 2. Vertical Hue rainbow bar (6 segments)
        static constexpr UIColor kHueStops[7] = {
            { 1.0f, 0.0f, 0.0f, 1.0f },
            { 1.0f, 1.0f, 0.0f, 1.0f },
            { 0.0f, 1.0f, 0.0f, 1.0f },
            { 0.0f, 1.0f, 1.0f, 1.0f },
            { 0.0f, 0.0f, 1.0f, 1.0f },
            { 1.0f, 0.0f, 1.0f, 1.0f },
            { 1.0f, 0.0f, 0.0f, 1.0f }
        };
        const float segH = r.Hue.Height() / 6.0f;
        for (int i = 0; i < 6; ++i)
        {
            const float yA = r.Hue.Min.y + static_cast<float>(i) * segH;
            const float yB = (i == 5) ? r.Hue.Max.y : (yA + segH + 0.5f);
            drawList.AddRoundedRectGradient(
                { r.Hue.Min.x, yA },
                { r.Hue.Max.x, yB },
                kHueStops[i],
                kHueStops[i + 1]
            );
        }
        const float hueY = r.Hue.Min.y + (m_Hue / 360.0f) * r.Hue.Height();
        drawList.AddRoundedRect(
            { r.Hue.Min.x - 1.0f, hueY - 2.0f },
            { r.Hue.Max.x + 1.0f, hueY + 2.0f },
            { 1.0f, 1.0f, 1.0f, 0.95f },
            glm::vec4(1.0f),
            1.0f,
            { 0.05f, 0.06f, 0.08f, 1.0f }
        );

        // 3. Alpha bar
        drawList.AddRoundedRect(r.Alpha.Min, r.Alpha.Max, { 0.14f, 0.16f, 0.20f, 1.0f }, glm::vec4(4.0f), 1.0f, { 0.25f, 0.28f, 0.34f, 1.0f });
        const UIColor solidColor = HsvToRgb(m_Hue, m_Sat, m_Val, 1.0f);
        const float alphaW = std::max(0.0f, (r.Alpha.Width() - 2.0f) * m_Alpha);
        if (alphaW > 0.5f)
        {
            drawList.AddRoundedRect(
                { r.Alpha.Min.x + 1.0f, r.Alpha.Min.y + 1.0f },
                { r.Alpha.Min.x + 1.0f + alphaW, r.Alpha.Max.y - 1.0f },
                solidColor,
                glm::vec4(3.0f)
            );
        }
        const float alphaX = r.Alpha.Min.x + m_Alpha * r.Alpha.Width();
        drawList.AddRoundedRect(
            { alphaX - 2.0f, r.Alpha.Min.y - 1.0f },
            { alphaX + 2.0f, r.Alpha.Max.y + 1.0f },
            { 1.0f, 1.0f, 1.0f, 0.95f },
            glm::vec4(1.0f),
            1.0f,
            { 0.05f, 0.06f, 0.08f, 1.0f }
        );

        // 4. Preview swatch & hex text
        const UIColor cur = GetColor();
        drawList.AddRoundedRect(r.Preview.Min, r.Preview.Max, cur, glm::vec4(4.0f), 1.0f, { 0.28f, 0.32f, 0.38f, 1.0f });
        const std::string hex = FormatHex(cur);
        drawList.AddText(hex, nullptr, { r.Preview.Max.x + 10.0f, r.Preview.Min.y + 3.0f }, 13.0f, { 0.88f, 0.91f, 0.96f, 1.0f });
    }

    UIColorEdit::UIColorEdit(
        UIContext* context,
        const UIColor& initialColor,
        std::function<void(const UIColor&)> onColorChanged)
        : OnColorChanged(std::move(onColorChanged))
        , m_Context(context)
        , m_Color(initialColor)
    {
        StyleClass = "Button";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        Padding = UIThickness::Symmetric(8.0f, 5.0f);
        MinSize = { 130.0f, 28.0f };

        m_Label = Ref<UILabel>(new UILabel("", 13.0f));
        m_Label->HitTestMode = UIHitTestMode::None;
        m_Label->VAlign = UIAlignment::Center;
        AddChild(m_Label);

        SyncLabel();
    }

    void UIColorEdit::SetColor(const UIColor& color, bool notify)
    {
        m_Color = color;
        SyncLabel();
        if (notify && OnColorChanged)
            OnColorChanged(m_Color);
    }

    bool UIColorEdit::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;
        if (GetBounds().Contains(e.Position))
            OpenPicker();
        return true;
    }

    UIVec2 UIColorEdit::ComputeDesiredSize()
    {
        UIVec2 labelSize = m_Label ? m_Label->Measure() : UIVec2{ 70.0f, 18.0f };
        return {
            std::max(MinSize.x, labelSize.x + Padding.Horizontal() + 28.0f),
            std::max(MinSize.y, labelSize.y + Padding.Vertical())
        };
    }

    void UIColorEdit::ArrangeChildren(const UIRect& contentRect)
    {
        if (!m_Label)
            return;
        UIRect textRect = contentRect;
        textRect.Min.x = std::min(textRect.Max.x, textRect.Min.x + 24.0f);
        m_Label->Arrange(textRect);
    }

    void UIColorEdit::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const float swatchSize = std::max(12.0f, b.Height() - Padding.Vertical());
        const UIVec2 sMin{ b.Min.x + Padding.Left, b.Min.y + (b.Height() - swatchSize) * 0.5f };
        drawList.AddRoundedRect(
            sMin,
            sMin + UIVec2{ swatchSize, swatchSize },
            m_Color,
            glm::vec4(4.0f),
            1.0f,
            { 0.30f, 0.34f, 0.42f, 1.0f }
        );
    }

    void UIColorEdit::SyncLabel()
    {
        if (m_Label)
            m_Label->Text = FormatHex(m_Color);
    }

    void UIColorEdit::OpenPicker()
    {
        if (!m_Context)
            return;

        Ref<UIColorPopoverBackdrop> backdrop(new UIColorPopoverBackdrop(m_Context));
        Ref<UIColorPicker> picker(new UIColorPicker(m_Color, [this](const UIColor& c)
        {
            SetColor(c, true);
        }));
        picker->StyleClass = "MenuPopup";

        const UIRect& b = GetBounds();
        picker->AnchorMin = { 0.0f, 0.0f };
        picker->AnchorMax = { 0.0f, 0.0f };
        picker->AnchorOffsets = { b.Min.x, b.Max.y + 4.0f, 0.0f, 0.0f };

        backdrop->AddChild(picker);
        m_Context->SetOverlay(backdrop);
    }
}
