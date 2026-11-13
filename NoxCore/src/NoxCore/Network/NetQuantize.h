#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// How replicated transforms are written (docs/Networking_Plan_2026-11.md, NET2): positions quantized to a step (NetReplication: 0.1
// mm) as zigzag varints -- small near the origin, exact to the step anywhere (large worlds: doubles in, integers out); rotations as
// "smallest three" (which of x, y, z, w is largest, the other three to 16 bits: about 0.003 degrees); scale as three floats.
namespace Nox::Net
{
    std::vector<uint8_t> EncodePosition(const glm::dvec3& position, double quantum);
    bool DecodePosition(std::span<const uint8_t> bytes, double quantum, glm::dvec3& position);
    std::vector<uint8_t> EncodeRotation(glm::quat rotation);
    bool DecodeRotation(std::span<const uint8_t> bytes, glm::quat& rotation);
    std::vector<uint8_t> EncodeScale(const glm::vec3& scale);
    bool DecodeScale(std::span<const uint8_t> bytes, glm::vec3& scale);
}
