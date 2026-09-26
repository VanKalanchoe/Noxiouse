#pragma once

#include <string>

namespace Nox
{
    // The world's unit (docs/Units_And_World_Tools_Plan_2026.md). Everything that is a physical length asks here instead of assuming
    // meters or centimeters: `PerMeter` is a project setting, read when the project loads. 1 = the world is in meters (what the engine is
    // today), 100 = 1 unit is 1 cm (Unreal's convention, where the project moves in phase U6).
    namespace WorldUnits
    {
        float PerMeter();
        void SetPerMeter(float unitsPerMeter);

        float FromMeters(float meters);
        float ToMeters(float worldUnits);
        float FromCentimeters(float centimeters);
        float ToCentimeters(float worldUnits);

        // How lengths are shown in the editor: always cm, always m, or cm below one meter and m above.
        enum class DisplayUnit { Centimeters, Meters, Auto };
        DisplayUnit GetDisplayUnit();
        void SetDisplayUnit(DisplayUnit unit);
        // A length as text in the display unit: "180 cm", "1.80 m".
        std::string Format(float worldLength);

        // The unit a source file (glTF: meters, a model from a cm-based tool, ...) is authored in: world units per one of it.
        enum class SourceUnit { Meters, Centimeters, Millimeters, Inches, Feet };
        float PerSourceUnit(SourceUnit unit);
    }
}
