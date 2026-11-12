#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

namespace Nox
{
    static constexpr uint32_t kInvalidGraphIndex = UINT32_MAX;

    // A single node property or unconnected-input literal, generic across every graph domain. uint64_t also
    // carries AssetHandle/UUID values (Asset.h's AssetHandle is a UUID, itself a uint64_t wrapper) since the
    // core has no notion of assets.
    // A curve property (Unreal's float curve): keys (x, y) -- kept sorted by x --, linear between them or smooth (Catmull-Rom tangents,
    // flat at the ends, like Unreal's auto tangents); before the first key its value, after the last key its value.
    struct NodeGraphCurve
    {
        std::vector<glm::vec2> Keys;
        bool Smooth = false;

        bool operator==(const NodeGraphCurve&) const = default;

        float Evaluate(float x) const
        {
            if (Keys.empty())
                return 0.0f;
            if (x <= Keys.front().x)
                return Keys.front().y;
            if (x >= Keys.back().x)
                return Keys.back().y;
            const size_t next = static_cast<size_t>(std::upper_bound(Keys.begin(), Keys.end(), x, [](float value, const glm::vec2& key) { return value < key.x; }) - Keys.begin());
            const glm::vec2& a = Keys[next - 1];
            const glm::vec2& b = Keys[next];
            const float span = std::max(b.x - a.x, 1e-9f);
            const float t = (x - a.x) / span;
            if (!Smooth)
                return a.y + (b.y - a.y) * t;
            // Hermite with Catmull-Rom slopes (per unit x), scaled to the span; flat at the first and last key.
            auto slope = [&](size_t i) -> float
            {
                if (i == 0 || i + 1 >= Keys.size())
                    return 0.0f;
                return (Keys[i + 1].y - Keys[i - 1].y) / std::max(Keys[i + 1].x - Keys[i - 1].x, 1e-9f);
            };
            const float m0 = slope(next - 1) * span, m1 = slope(next) * span;
            const float t2 = t * t, t3 = t2 * t;
            return (2.0f * t3 - 3.0f * t2 + 1.0f) * a.y + (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * b.y + (t3 - t2) * m1;
        }
    };

    using NodeGraphValue = std::variant<bool, int32_t, float, glm::vec2, uint64_t, std::string, NodeGraphCurve>;

    // Numeric view of a value, for consumers that only deal in floats (a node's output pin, a script getter):
    // bool -> 0/1, int -> float, float as is, anything else the fallback.
    inline float NodeGraphValueToFloat(const NodeGraphValue& value, float fallback = 0.0f)
    {
        if (const float* f = std::get_if<float>(&value))
            return *f;
        if (const int32_t* i = std::get_if<int32_t>(&value))
            return static_cast<float>(*i);
        if (const bool* b = std::get_if<bool>(&value))
            return *b ? 1.0f : 0.0f;
        return fallback;
    }

    // A node that reads a graph parameter stores its name in a string property with this name. The editor uses
    // that convention (not any domain's node type names) to offer "Parameters / <name>" entries in the create-node
    // menu and to keep such nodes pointing at the right parameter when one is renamed.
    inline constexpr const char* kParameterReferenceProperty = "Parameter";

    // One named, typed socket on a node type. Type is a domain-defined string ("Pose", "Float", "Bool",
    // "Vector2", ...) so the core never needs to know what any domain's types mean; GraphCompiler only compares
    // names when validating a link.
    struct NodePinDesc
    {
        std::string Name;
        std::string Type;
    };

    // True for exactly the alternatives NodeGraphValue holds. GraphEvalContext uses this to guard std::get_if
    // calls at compile time: a pin's runtime value type (e.g. an animation graph's AnimPose) is very often *not*
    // one of these, and std::get_if<T> on a variant that never held T is a hard compile error, not a false
    // result -- so unconnected-pin fallback code must not even instantiate that call for such T.
    template<typename T>
    inline constexpr bool kIsNodeGraphValueType = false;
    template<> inline constexpr bool kIsNodeGraphValueType<bool> = true;
    template<> inline constexpr bool kIsNodeGraphValueType<int32_t> = true;
    template<> inline constexpr bool kIsNodeGraphValueType<float> = true;
    template<> inline constexpr bool kIsNodeGraphValueType<glm::vec2> = true;
    template<> inline constexpr bool kIsNodeGraphValueType<uint64_t> = true;
    template<> inline constexpr bool kIsNodeGraphValueType<std::string> = true;
    template<> inline constexpr bool kIsNodeGraphValueType<NodeGraphCurve> = true;
}
