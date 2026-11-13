#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

// Animation replication (docs/Networking_Plan_2026-11.md, NET5; Unreal replicates what drives the animation, never the poses): an
// animator graph's parameters (speed, aiming, crouched -- bool, int, float, vector2) and its montages (Unreal's RepAnimMontageInfo:
// which clip plays in which slot, how fast, since when, stopped or not). Clients run the same graph with them; the owning client sets
// its own. One property of the snapshot (NetReplication); nothing here knows the scene.
namespace Nox::Net
{
    using AnimValue = std::variant<bool, int32_t, float, glm::vec2>;

    struct AnimParameter
    {
        uint32_t NameHash = 0; // HashParameterName (both sides run the same graph: the names are not sent)
        AnimValue Value;
        bool operator==(const AnimParameter& other) const = default;
    };

    struct AnimMontage
    {
        std::string Slot;
        uint64_t Clip = 0;
        uint32_t Serial = 0;    // the server's count of montages played in this slot (a new one: a higher number)
        uint64_t StartTick = 0; // the server tick it started at (a client that sees it later starts it that far in)
        float Rate = 1.0f;
        float BlendIn = 0.2f;
        float BlendOut = 0.2f;
        bool Stopped = false;   // stopped early (StopMontage)
        float StopBlendOut = 0.2f;
        bool operator==(const AnimMontage& other) const = default;
    };

    struct AnimationNetState
    {
        std::vector<AnimParameter> Parameters; // by NameHash
        std::vector<AnimMontage> Montages;     // the last one of each slot
        bool operator==(const AnimationNetState& other) const = default;
    };

    uint32_t HashParameterName(std::string_view name);
    std::vector<uint8_t> EncodeAnimation(const AnimationNetState& state);
    bool DecodeAnimation(std::span<const uint8_t> bytes, AnimationNetState& state);
}
