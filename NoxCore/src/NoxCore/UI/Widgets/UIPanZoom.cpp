#include "UIPanZoom.h"

#include <format>
#include <string>
#include <utility>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    void UIPanZoom::PaintGrid(UIDrawList& d, const UIRect& b, float fineStep, float coarseStep, float fineAlpha, float coarseAlpha) const
    {
        for (const auto& [step, alpha] : { std::pair{ fineStep, fineAlpha }, std::pair{ coarseStep, coarseAlpha } })
        {
            const float px = step * Zoom;
            if (px < 8.0f)
                continue;
            const UIColor line{ 1.0f, 1.0f, 1.0f, alpha };
            for (float x = b.Min.x + std::fmod(Pan.x, px) - (Pan.x < 0 ? 0.0f : px); x < b.Max.x; x += px)
                d.AddRoundedRect({ x, b.Min.y }, { x + 1.0f, b.Max.y }, line);
            for (float y = b.Min.y + std::fmod(Pan.y, px) - (Pan.y < 0 ? 0.0f : px); y < b.Max.y; y += px)
                d.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + 1.0f }, line);
        }
    }

    void UIPanZoom::PaintZoomLabel(UIDrawList& d, const UIRect& b, const UITheme& theme) const
    {
        const std::string zoom = std::format("{:.0f} %", Zoom * 100.0f);
        d.AddText(zoom, nullptr, { b.Max.x - UILabel::MeasureText(zoom, nullptr, 12.0f).x - 10.0f, b.Max.y - 20.0f }, 12.0f,
                  theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f }));
    }
}
