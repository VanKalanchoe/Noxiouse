#include "NetQuantize.h"

#include <algorithm>
#include <cmath>

#include "NetBuffer.h"

namespace Nox::Net
{
    std::vector<uint8_t> EncodePosition(const glm::dvec3& position, double quantum)
    {
        NetWriter writer(16);
        constexpr double limit = 4.0e18; // (steps beyond int64: not a place in any world)
        for (int axis = 0; axis < 3; ++axis)
        {
            const double steps = position[axis] / quantum;
            const int64_t value = std::isfinite(steps) ? static_cast<int64_t>(std::llround(std::clamp(steps, -limit, limit))) : 0;
            writer.WriteVarU64((static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63));
        }
        return { writer.GetData().begin(), writer.GetData().end() };
    }

    bool DecodePosition(std::span<const uint8_t> bytes, double quantum, glm::dvec3& position)
    {
        NetReader reader(bytes);
        glm::dvec3 decoded;
        for (int axis = 0; axis < 3; ++axis)
        {
            const uint64_t zigzag = reader.ReadVarU64();
            const int64_t value = static_cast<int64_t>(zigzag >> 1) ^ -static_cast<int64_t>(zigzag & 1);
            decoded[axis] = static_cast<double>(value) * quantum;
        }
        if (!reader.IsAtEnd())
            return false; // (the position stays as it was)
        position = decoded;
        return true;
    }

    std::vector<uint8_t> EncodeRotation(glm::quat rotation)
    {
        const float length = glm::length(rotation);
        rotation = std::isfinite(length) && length > 1e-12f ? rotation / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        float components[4] = { rotation.x, rotation.y, rotation.z, rotation.w };
        int largest = 0;
        for (int index = 1; index < 4; ++index)
            if (std::abs(components[index]) > std::abs(components[largest]))
                largest = index;
        const float sign = components[largest] < 0.0f ? -1.0f : 1.0f; // q and -q are the same rotation
        NetWriter writer(7);
        writer.WriteU8(static_cast<uint8_t>(largest));
        for (int index = 0; index < 4; ++index)
        {
            if (index == largest)
                continue;
            const float scaled = std::clamp(sign * components[index] * 1.41421356f, -1.0f, 1.0f); // |c| <= 1/sqrt(2)
            writer.WriteU16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(scaled * 32767.0f))));
        }
        return { writer.GetData().begin(), writer.GetData().end() };
    }

    bool DecodeRotation(std::span<const uint8_t> bytes, glm::quat& rotation)
    {
        NetReader reader(bytes);
        const uint8_t largest = reader.ReadU8();
        if (largest > 3)
            return false;
        float components[4];
        float sum = 0.0f;
        for (int index = 0; index < 4; ++index)
        {
            if (index == largest)
                continue;
            components[index] = static_cast<float>(static_cast<int16_t>(reader.ReadU16())) / 32767.0f / 1.41421356f;
            sum += components[index] * components[index];
        }
        components[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
        if (!reader.IsAtEnd())
            return false;
        rotation = glm::normalize(glm::quat(components[3], components[0], components[1], components[2]));
        return true;
    }

    std::vector<uint8_t> EncodeScale(const glm::vec3& scale)
    {
        NetWriter writer(12);
        writer.WriteFloat(scale.x);
        writer.WriteFloat(scale.y);
        writer.WriteFloat(scale.z);
        return { writer.GetData().begin(), writer.GetData().end() };
    }

    bool DecodeScale(std::span<const uint8_t> bytes, glm::vec3& scale)
    {
        NetReader reader(bytes);
        const glm::vec3 decoded = { reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat() };
        if (!reader.IsAtEnd() || !std::isfinite(decoded.x) || !std::isfinite(decoded.y) || !std::isfinite(decoded.z))
            return false;
        scale = decoded;
        return true;
    }
}
