#include "NoxJoltJobSystem.h"
#include "NoxCore/Tasks/JobSystem.h"

#include <thread>
#include <chrono>

namespace Nox {

    NoxJoltJobSystem::NoxJoltJobSystem(JPH::uint inMaxJobs, JPH::uint inMaxBarriers)
        : JPH::JobSystemWithBarrier(inMaxBarriers)
    {
        mJobs.Init(inMaxJobs, inMaxJobs);
    }

    int NoxJoltJobSystem::GetMaxConcurrency() const
    {
        return static_cast<int>(::Nox::JobSystem::Get().GetWorkerCount()) + 1;
    }

    JPH::JobSystem::JobHandle NoxJoltJobSystem::CreateJob(const char* inJobName, JPH::ColorArg inColor,
                                                          const JobFunction& inJobFunction, JPH::uint32 inNumDependencies)
    {
        uint32_t index;
        for (;;)
        {
            index = mJobs.ConstructObject(inJobName, inColor, this, inJobFunction, inNumDependencies);
            if (index != AvailableJobs::cInvalidObjectIndex)
                break;
            JPH_ASSERT(false, "No jobs available in NoxJoltJobSystem!");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        Job* job = &mJobs.Get(index);
        JobHandle handle(job);

        if (inNumDependencies == 0)
            QueueJob(job);

        return handle;
    }

    void NoxJoltJobSystem::QueueJob(Job* inJob)
    {
        inJob->AddRef();
        ::Nox::JobSystem::Get().Async("JoltPhysicsJob", [inJob](const CancellationToken&)
        {
            inJob->Execute();
            inJob->Release();
        });
    }

    void NoxJoltJobSystem::QueueJobs(Job** inJobs, JPH::uint inNumJobs)
    {
        for (JPH::uint i = 0; i < inNumJobs; ++i)
            QueueJob(inJobs[i]);
    }

    void NoxJoltJobSystem::FreeJob(Job* inJob)
    {
        mJobs.DestructObject(inJob);
    }

} // namespace Nox
