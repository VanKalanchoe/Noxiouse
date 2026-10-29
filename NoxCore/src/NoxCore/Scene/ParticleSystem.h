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
        void Simulate(ParticleEmitterComponent& emitter, const glm::dmat4& world, float dt);
        // No particles, the timeline back at its start (the next update bursts again).
        void Restart(ParticleEmitterComponent& emitter);

        // How a particle looks now, from its age, in the space centred on `space` (the renderer's origin: Renderer::GetRenderOrigin).
        struct Appearance
        {
            glm::vec3 Position;  // world units, relative to `space`
            glm::vec3 PreviousPosition; // where it was `dt` earlier (its motion for DLSS / TAA)
            float Size;          // world units, edge of the quad
            glm::vec4 Color;     // linear, Intensity applied; alpha = opacity
            uint32_t Frame;      // flipbook frame
        };
        // dt: the frame's step, for PreviousPosition.
        Appearance Evaluate(const ParticleEmitterComponent& emitter, const ParticleSimulation::Particle& particle, const glm::dmat4& world,
                            const glm::dvec3& space, float dt = 0.0f);
        // The curves over life at t (0..1): the keys when there are two or more, else the start / end pair.
        glm::vec4 ColorOverLife(const ParticleEmitterComponent& emitter, float t);
        float SizeOverLife(const ParticleEmitterComponent& emitter, float t); // meters
    }
}
