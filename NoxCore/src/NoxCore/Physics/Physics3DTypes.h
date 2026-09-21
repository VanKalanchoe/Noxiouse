#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include "NoxCore/Scene/Entity.h"

namespace Nox {

    namespace PhysicsLayers {
        // Object Layers (16-bit)
        static constexpr uint16_t NON_MOVING = 0;
        static constexpr uint16_t MOVING     = 1;
        static constexpr uint16_t TRIGGER    = 2;
        static constexpr uint16_t CHARACTER  = 3;
        static constexpr uint16_t NUM_LAYERS = 4;

        // Masks
        static constexpr uint16_t MASK_ALL     = 0xFFFF;
        static constexpr uint16_t MASK_STATIC  = (1 << NON_MOVING);
        static constexpr uint16_t MASK_DYNAMIC = (1 << MOVING);
        static constexpr uint16_t MASK_ACTORS  = (1 << MOVING) | (1 << CHARACTER);
    }

    struct RayCastHit
    {
        bool Hit = false;
        Entity HitEntity{};
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        float Distance = 0.0f;
        float Fraction = 0.0f; // [0, 1] along ray
    };

    struct ShapeCastHit
    {
        bool Hit = false;
        Entity HitEntity{};
        glm::vec3 ContactPosition{ 0.0f };
        glm::vec3 ContactNormal{ 0.0f, 1.0f, 0.0f };
        float Distance = 0.0f;
        float Fraction = 0.0f;
    };

} // namespace Nox
