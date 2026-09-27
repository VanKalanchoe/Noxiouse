#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <glm/glm.hpp>

#include "NoxCore/Core/UUID.h"

namespace Nox
{
    enum class ScriptLanguage : uint8_t { DotNet, Lua };
    enum class ScriptCallback : uint8_t { Create, Update, Destroy, AfterReload };
    enum class ScriptFieldType : uint8_t { Bool, Int, UInt, Long, ULong, Float, Double, String, Vector3, Entity };

    struct ScriptInstanceHandle
    {
        uint64_t ID = 0;
        uint32_t Generation = 0;
        explicit operator bool() const { return ID != 0; }
    };

    using ScriptValue = std::variant<std::monostate, bool, int32_t, uint32_t,
        int64_t, uint64_t, float, double, glm::vec2, glm::vec3, glm::vec4,
        std::string, UUID>;

    struct ScriptFieldInfo
    {
        std::string Name;
        ScriptFieldType Type;
        ScriptValue DefaultValue;
        bool DisallowSelf = false;
        // [Expose(Unit = "...")] (docs/Units_And_World_Tools_Plan_2026.md): "cm", "m", "cm/s", "m/s", "km/h", or empty for a value
        // that is not a length/speed. The Inspector shows and edits the field in this unit; the script still sees the raw value.
        std::string Unit;
    };
}
