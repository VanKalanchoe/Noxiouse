#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <NoxAudio/AcousticBake.h>

#include "AcousticData.h"
#include "SpatialAudio.h"

namespace Nox
{
    using NoxAudio::AcousticBakeVolume;

    class AcousticBaker
    {
    public:
        AcousticBaker() = default;
        ~AcousticBaker() = default;
        AcousticBaker(const AcousticBaker&) = delete;
        AcousticBaker& operator=(const AcousticBaker&) = delete;

        void Start(const std::vector<SpatialAudio::Instance>& instances,
                   std::vector<std::pair<std::shared_ptr<const AcousticMesh>, glm::dmat4>> meshes,
                   std::vector<AcousticBakeVolume> volumes)
        {
            m_Baker.Start(instances, std::move(meshes), std::move(volumes));
        }

        bool IsRunning() const { return m_Baker.IsRunning(); }
        float GetProgress() const { return m_Baker.GetProgress(); }
        std::string GetStatus() const { return m_Baker.GetStatus(); }
        void Cancel() { m_Baker.Cancel(); }
        bool WasCancelled() const { return m_Baker.WasCancelled(); }

        struct Result
        {
            uint64_t Key = 0;
            Ref<AcousticData> Data;
            std::string Error;
        };
        std::vector<Result> TakeResults();

    private:
        NoxAudio::AcousticBaker m_Baker;
    };
}
