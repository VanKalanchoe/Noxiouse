#include "NetAnimation.h"

#include <algorithm>
#include <cmath>

#include "NetBuffer.h"

namespace Nox::Net
{
    uint32_t HashParameterName(std::string_view name)
    {
        uint32_t hash = 2166136261u; // FNV-1a
        for (char c : name)
        {
            hash ^= static_cast<uint8_t>(c);
            hash *= 16777619u;
        }
        return hash;
    }

    std::vector<uint8_t> EncodeAnimation(const AnimationNetState& state)
    {
        NetWriter out(64);
        out.WriteVarU32(static_cast<uint32_t>(state.Parameters.size()));
        for (const AnimParameter& parameter : state.Parameters)
        {
            out.WriteU32(parameter.NameHash);
            out.WriteU8(static_cast<uint8_t>(parameter.Value.index()));
            std::visit([&](const auto& value)
            {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, bool>) out.WriteU8(value ? 1 : 0);
                else if constexpr (std::is_same_v<T, int32_t>) out.WriteVarU32(static_cast<uint32_t>((value << 1) ^ (value >> 31)));
                else if constexpr (std::is_same_v<T, float>) out.WriteFloat(value);
                else { out.WriteFloat(value.x); out.WriteFloat(value.y); }
            }, parameter.Value);
        }
        out.WriteVarU32(static_cast<uint32_t>(state.Montages.size()));
        for (const AnimMontage& montage : state.Montages)
        {
            out.WriteVarU32(static_cast<uint32_t>(montage.Slot.size()));
            out.WriteBytes(std::span(reinterpret_cast<const uint8_t*>(montage.Slot.data()), montage.Slot.size()));
            out.WriteVarU64(montage.Clip);
            out.WriteVarU32(montage.Serial);
            out.WriteVarU64(montage.StartTick);
            out.WriteFloat(montage.Rate);
            out.WriteFloat(montage.BlendIn);
            out.WriteFloat(montage.BlendOut);
            out.WriteU8(montage.Stopped ? 1 : 0);
            if (montage.Stopped)
                out.WriteFloat(montage.StopBlendOut);
        }
        return { out.GetData().begin(), out.GetData().end() };
    }

    bool DecodeAnimation(std::span<const uint8_t> bytes, AnimationNetState& state)
    {
        NetReader in(bytes);
        state = {};
        const uint32_t parameters = in.ReadVarU32();
        if (in.IsFailed() || parameters > 4096)
            return false;
        state.Parameters.resize(parameters);
        for (AnimParameter& parameter : state.Parameters)
        {
            parameter.NameHash = in.ReadU32();
            switch (in.ReadU8())
            {
            case 0: parameter.Value = in.ReadU8() != 0; break;
            case 1: { const uint32_t z = in.ReadVarU32(); parameter.Value = static_cast<int32_t>((z >> 1) ^ (~(z & 1) + 1)); break; }
            case 2: parameter.Value = in.ReadFloat(); break;
            case 3: { const float x = in.ReadFloat(); parameter.Value = glm::vec2(x, in.ReadFloat()); break; }
            default: return false;
            }
        }
        const uint32_t montages = in.ReadVarU32();
        if (in.IsFailed() || montages > 256)
            return false;
        state.Montages.resize(montages);
        for (AnimMontage& montage : state.Montages)
        {
            const uint32_t length = in.ReadVarU32();
            if (in.IsFailed() || length > 256 || length > in.GetRemaining())
                return false;
            const std::span<const uint8_t> slot = in.ReadBytes(length);
            montage.Slot.assign(reinterpret_cast<const char*>(slot.data()), slot.size());
            montage.Clip = in.ReadVarU64();
            montage.Serial = in.ReadVarU32();
            montage.StartTick = in.ReadVarU64();
            montage.Rate = in.ReadFloat();
            montage.BlendIn = in.ReadFloat();
            montage.BlendOut = in.ReadFloat();
            montage.Stopped = in.ReadU8() != 0;
            if (montage.Stopped)
                montage.StopBlendOut = in.ReadFloat();
            if (!std::isfinite(montage.Rate) || !std::isfinite(montage.BlendIn) || !std::isfinite(montage.BlendOut))
                return false;
        }
        for (const AnimParameter& parameter : state.Parameters)
        {
            if (const float* value = std::get_if<float>(&parameter.Value); value && !std::isfinite(*value))
                return false;
            if (const glm::vec2* value = std::get_if<glm::vec2>(&parameter.Value); value && !std::isfinite(value->x + value->y))
                return false;
        }
        return in.IsAtEnd();
    }
}
