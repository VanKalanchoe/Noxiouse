#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "AcousticData.h"
#include "SpatialAudio.h"

namespace Nox
{
    // One Acoustic Probe Volume to bake.
    struct AcousticBakeVolume
    {
        uint64_t Key = 0;              // the caller's (the volume's entity id): the result's
        std::string Name;              // for the status line
        glm::dmat4 Box{ 1.0 };         // world units: the unit cube (-0.5 .. 0.5) onto the volume
        AcousticBakeSettings Settings;
    };

    // Bakes Acoustic Probe Volumes (Steam Audio's probe batch bake, as its Unity plugin's Bake buttons run it): the static meshes'
    // triangles with their acoustic materials make the scene; per volume, probes are placed over the floors inside it, the reverb
    // at each probe is traced (convolution, order-1 ambisonics) and the paths between probes are found; the probe batch is saved
    // as the volume's AcousticData. Runs on its own thread (minutes for a large level -- not a JobSystem task, which would hold a
    // worker that long), with Steam Audio's own worker threads (half the cores, as its Unity plugin uses).
    class AcousticBaker
    {
    public:
        AcousticBaker() = default;
        ~AcousticBaker(); // cancels a bake still running and waits for it
        AcousticBaker(const AcousticBaker&) = delete;
        AcousticBaker& operator=(const AcousticBaker&) = delete;

        // Game thread (it starts the meshes' reads). `instances`: the level's static meshes as the acoustic scene takes them;
        // `meshes`: triangles made by code, placed (tests, procedural geometry).
        void Start(const std::vector<SpatialAudio::Instance>& instances,
                   std::vector<std::pair<std::shared_ptr<const AcousticMesh>, glm::dmat4>> meshes,
                   std::vector<AcousticBakeVolume> volumes);
        bool IsRunning() const { return m_Running.load(std::memory_order_acquire); }
        float GetProgress() const { return m_Progress.load(std::memory_order_relaxed); } // 0..1 over every volume
        std::string GetStatus() const;
        void Cancel();

        struct Result
        {
            uint64_t Key = 0;
            Ref<AcousticData> Data; // null: failed (Error says why)
            std::string Error;
        };
        // Once it is no longer running: the volumes baked (all of them, unless cancelled), each once.
        std::vector<Result> TakeResults();
        bool WasCancelled() const { return m_Cancelled.load(std::memory_order_relaxed); }

    private:
        void Run();
        void SetStatus(std::string status);

        std::vector<AcousticMeshRead> m_Reads;              // one per instance (shared reads by mesh key)
        std::vector<SpatialAudio::Instance> m_Instances;
        std::vector<std::pair<std::shared_ptr<const AcousticMesh>, glm::dmat4>> m_Meshes;
        std::vector<AcousticBakeVolume> m_Volumes;
        std::vector<Result> m_Results;
        std::thread m_Thread;
        std::atomic<bool> m_Running{ false };
        std::atomic<bool> m_Cancelled{ false };
        std::atomic<float> m_Progress{ 0.0f };
        mutable std::mutex m_StatusMutex;
        std::string m_Status;
    };
}
