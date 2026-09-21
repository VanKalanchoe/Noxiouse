#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>

namespace Nox {

    class NoxJoltJobSystem final : public JPH::JobSystemWithBarrier
    {
    public:
        JPH_OVERRIDE_NEW_DELETE

        explicit NoxJoltJobSystem(JPH::uint inMaxJobs = 1024, JPH::uint inMaxBarriers = 128);
        virtual ~NoxJoltJobSystem() override = default;

        // JobSystem interface
        virtual int GetMaxConcurrency() const override;
        virtual JobHandle CreateJob(const char* inJobName, JPH::ColorArg inColor,
                                    const JobFunction& inJobFunction, JPH::uint32 inNumDependencies = 0) override;

    protected:
        virtual void QueueJob(Job* inJob) override;
        virtual void QueueJobs(Job** inJobs, JPH::uint inNumJobs) override;
        virtual void FreeJob(Job* inJob) override;

    private:
        using AvailableJobs = JPH::FixedSizeFreeList<Job>;
        AvailableJobs mJobs;
    };

} // namespace Nox
