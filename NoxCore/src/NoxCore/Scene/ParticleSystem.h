#pragma once
#include <glm/glm.hpp>

#include "Components.h"

namespace Nox
{
    // The CPU side of ParticleEmitterComponent (docs/Hybrid_Renderer_Plan_2026-10.md §6 V1). Simulated on the CPU, like Unity's
    // particle system and Niagara's CPU emitters: a few thousand particles per emitter cost well under a millisecond, the state is
    // plain data scripts can drive (Emit, Play / Stop), and nothing waits on the GPU. The renderer gets the particles to draw each
    // frame (Renderer::SubmitParticles).
    namespace ParticleSystem
    {
        // Advances the emitter by `dt` seconds at its world transform: moves the particles (gravity, drag, spin), retires the old
        // ones, spawns new ones (the rate, a burst when a loop starts, Emit(count)).
        void Simulate(ParticleEmitterComponent& emitter, const glm::mat4& world, float dt);
        // No particles, the timeline back at its start (the next update bursts again).
        void Restart(ParticleEmitterComponent& emitter);

        // How a particle looks now, from its age.
        struct Appearance
        {
            glm::vec3 Position;  // world units, world space
            float Size;          // world units, edge of the quad
            glm::vec4 Color;     // linear, Intensity applied; alpha = opacity
            uint32_t Frame;      // flipbook frame
        };
        Appearance Evaluate(const ParticleEmitterComponent& emitter, const ParticleSimulation::Particle& particle, const glm::mat4& world);
    }
}
