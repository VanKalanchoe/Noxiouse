#include "WorldUnits.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Nox::WorldUnits
{
    namespace
    {
        float s_PerMeter = 1.0f;
        DisplayUnit s_DisplayUnit = DisplayUnit::Centimeters;
    }

    float PerMeter() { return s_PerMeter; }
    void SetPerMeter(float unitsPerMeter) { s_PerMeter = std::max(unitsPerMeter, 1e-6f); }

    float FromMeters(float meters) { return meters * s_PerMeter; }
    float ToMeters(float worldUnits) { return worldUnits / s_PerMeter; }
    float FromCentimeters(float centimeters) { return centimeters * 0.01f * s_PerMeter; }
    float ToCentimeters(float worldUnits) { return worldUnits / s_PerMeter * 100.0f; }

    DisplayUnit GetDisplayUnit() { return s_DisplayUnit; }
    void SetDisplayUnit(DisplayUnit unit) { s_DisplayUnit = unit; }

    std::string Format(float worldLength)
    {
        const float meters = ToMeters(worldLength);
        const bool inMeters = s_DisplayUnit == DisplayUnit::Meters || (s_DisplayUnit == DisplayUnit::Auto && std::abs(meters) >= 1.0f);
        if (inMeters)
            return std::format("{:.2f} m", meters);

        const float centimeters = meters * 100.0f;
        // Whole numbers without a decimal point ("180 cm"), fractions with one ("12.5 cm").
        if (std::abs(centimeters - std::round(centimeters)) < 0.05f)
            return std::format("{:.0f} cm", centimeters);
        return std::format("{:.1f} cm", centimeters);
    }

    float PerSourceUnit(SourceUnit unit)
    {
        switch (unit)
        {
        case SourceUnit::Meters: return FromMeters(1.0f);
        case SourceUnit::Centimeters: return FromMeters(0.01f);
        case SourceUnit::Millimeters: return FromMeters(0.001f);
        case SourceUnit::Inches: return FromMeters(0.0254f);
        case SourceUnit::Feet: return FromMeters(0.3048f);
        }
        return FromMeters(1.0f);
    }
}
