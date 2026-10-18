#include "ParticleSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/gtc/constants.hpp>

#include "NoxCore/Core/WorldUnits.h"

namespace Nox::ParticleSystem
{
    namespace
    {
        // xorshift32 in [0, 1).
        float Next(uint32_t& state)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
        }

        float Range(uint32_t& state, float a, float b) { return a + (b - a) * Next(state); }
        float Signed(uint32_t& state) { return Next(state) * 2.0f - 1.0f; }

        // The emitter's rotation (its scale sizes the shape, never the speed).
        glm::mat3 RotationOf(const glm::mat4& world)
        {
            glm::mat3 rotation(world);
            for (int column = 0; column < 3; ++column)
            {
                const float length = glm::length(rotation[column]);
                if (length > 1e-6f)
                    rotation[column] /= length;
            }
            return rotation;
        }

        void Spawn(ParticleEmitterComponent& emitter, const glm::mat4& world, uint32_t count)
        {
            ParticleSimulation& simulation = emitter.Simulation;
            const size_t room = emitter.MaxParticles > simulation.Particles.size() ? emitter.MaxParticles - simulation.Particles.size() : 0;
            count = static_cast<uint32_t>(std::min<size_t>(count, room));
            if (count == 0)
                return;

            const float unit = WorldUnits::PerMeter();
            const glm::mat3 rotation = RotationOf(world);
            uint32_t& random = simulation.Random;
            simulation.Particles.reserve(simulation.Particles.size() + count);
            for (uint32_t index = 0; index < count; ++index)
            {
                ParticleSimulation::Particle particle;
                glm::vec3 local(0.0f);
                glm::vec3 velocity = emitter.Velocity;
                switch (emitter.Shape)
                {
                case ParticleShape::Sphere:
                {
                    glm::vec3 point;
                    do
                        point = glm::vec3(Signed(random), Signed(random), Signed(random));
                    while (glm::dot(point, point) > 1.0f);
                    local = point * emitter.ShapeSize.x;
                    break;
                }
                case ParticleShape::Box:
                    local = glm::vec3(Signed(random), Signed(random), Signed(random)) * emitter.ShapeSize;
                    break;
                case ParticleShape::Cone:
                {
                    // From a disc (uniform), along a direction within the cone around +Y, at the speed of Velocity.
                    const float radius = emitter.ShapeSize.x * std::sqrt(Next(random));
                    const float around = glm::two_pi<float>() * Next(random);
                    local = glm::vec3(radius * std::cos(around), 0.0f, radius * std::sin(around));
                    const float cosMax = std::cos(glm::radians(glm::clamp(emitter.ConeAngle, 0.0f, 180.0f)));
                    const float cosTheta = 1.0f - (1.0f - cosMax) * Next(random);
                    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
                    const float phi = glm::two_pi<float>() * Next(random);
                    velocity = glm::vec3(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi)) * glm::length(emitter.Velocity);
                    break;
                }
                case ParticleShape::Point:
                    break;
                }
                velocity += glm::vec3(Signed(random), Signed(random), Signed(random)) * emitter.VelocityRandom;
                local *= unit;
                velocity *= unit;

                if (emitter.LocalSpace)
                {
                    particle.Position = local;
                    particle.Velocity = velocity;
                }
                else
                {
                    particle.Position = glm::vec3(world * glm::vec4(local, 1.0f));
                    particle.Velocity = rotation * velocity;
                }
                particle.Lifetime = std::max(Range(random, emitter.LifetimeMin, emitter.LifetimeMax), 0.01f);
                particle.Rotation = glm::two_pi<float>() * Next(random);
                particle.Spin = glm::radians(Range(random, emitter.SpinMin, emitter.SpinMax));
                particle.SizeScale = std::max(1.0f + glm::clamp(emitter.SizeRandom, 0.0f, 1.0f) * Signed(random), 0.05f);
                particle.Variation = Next(random);
                simulation.Particles.push_back(particle);
            }
        }
    }

    void Restart(ParticleEmitterComponent& emitter)
    {
        ParticleSimulation& simulation = emitter.Simulation;
        simulation.Particles.clear();
        simulation.Time = 0.0f;
        simulation.SpawnCarry = 0.0f;
        simulation.BurstDone = false;
        simulation.Started = true;
        // Emitters differ (one sequence each), whatever their settings.
        simulation.Random = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&emitter) * 2654435761u) | 1u;
    }

    void Simulate(ParticleEmitterComponent& emitter, const glm::mat4& world, float dt)
    {
        ParticleSimulation& simulation = emitter.Simulation;
        if (!simulation.Started)
            Restart(emitter);
        dt = glm::clamp(dt, 0.0f, 0.1f); // a hitch does not throw every particle across the scene

        // 1. Move and retire. Gravity points down the world; in the emitter's space it turns with the entity.
        glm::vec3 gravity(0.0f, -9.81f * emitter.Gravity * WorldUnits::PerMeter(), 0.0f);
        if (emitter.LocalSpace)
            gravity = glm::transpose(RotationOf(world)) * gravity;
        const float drag = emitter.Drag > 0.0f ? std::exp(-emitter.Drag * dt) : 1.0f;
        std::vector<ParticleSimulation::Particle>& particles = simulation.Particles;
        for (size_t index = 0; index < particles.size();)
        {
            ParticleSimulation::Particle& particle = particles[index];
            particle.Age += dt;
            if (particle.Age >= particle.Lifetime)
            {
                particle = particles.back();
                particles.pop_back();
                continue;
            }
            particle.Velocity = (particle.Velocity + gravity * dt) * drag;
            particle.Position += particle.Velocity * dt;
            particle.Rotation += particle.Spin * dt;
            ++index;
        }

        // 2. Spawn.
        if (simulation.PendingBurst > 0)
        {
            Spawn(emitter, world, simulation.PendingBurst);
            simulation.PendingBurst = 0;
        }
        if (!emitter.Emitting)
            return;
        const float duration = std::max(emitter.Duration, 0.01f);
        if (!simulation.BurstDone)
        {
            Spawn(emitter, world, emitter.Burst);
            simulation.BurstDone = true;
        }
        if (emitter.Looping || simulation.Time < duration)
        {
            simulation.SpawnCarry += std::max(emitter.SpawnRate, 0.0f) * dt;
            const float whole = std::floor(simulation.SpawnCarry);
            simulation.SpawnCarry -= whole;
            Spawn(emitter, world, static_cast<uint32_t>(std::min(whole, 1.0e6f)));
        }
        simulation.Time += dt;
        if (emitter.Looping && simulation.Time >= duration)
        {
            simulation.Time = std::fmod(simulation.Time, duration);
            simulation.BurstDone = false;
        }
    }

    Appearance Evaluate(const ParticleEmitterComponent& emitter, const ParticleSimulation::Particle& particle, const glm::mat4& world)
    {
        const float t = glm::clamp(particle.Age / particle.Lifetime, 0.0f, 1.0f);
        Appearance appearance;
        appearance.Position = emitter.LocalSpace ? glm::vec3(world * glm::vec4(particle.Position, 1.0f)) : particle.Position;
        appearance.Size = glm::mix(emitter.SizeStart, emitter.SizeEnd, t) * particle.SizeScale * WorldUnits::PerMeter();
        appearance.Color = glm::mix(emitter.ColorStart, emitter.ColorEnd, t);
        appearance.Color = glm::vec4(glm::vec3(appearance.Color) * std::max(emitter.Intensity, 0.0f), glm::clamp(appearance.Color.a, 0.0f, 1.0f));

        const uint32_t frames = std::max(emitter.FlipbookColumns, 1u) * std::max(emitter.FlipbookRows, 1u);
        uint32_t frame = 0;
        if (frames > 1)
        {
            if (emitter.FlipbookFPS > 0.0f)
                frame = static_cast<uint32_t>(particle.Age * emitter.FlipbookFPS + particle.Variation * static_cast<float>(frames)) % frames;
            else
                frame = std::min(static_cast<uint32_t>(t * static_cast<float>(frames)), frames - 1);
        }
        appearance.Frame = frame;
        return appearance;
    }
}
