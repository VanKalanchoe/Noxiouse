#pragma once
#include <deque>
#include <memory>
#include <vector>

#include <NRI/NRI.h>

namespace Nox
{
    // Room in staging memory for one upload: write `data`, then queue the copy that reads it.
    struct StagingSpan
    {
        NRI::Buffer* buffer = nullptr; // the staging ring, or an oversized upload's own buffer
        uint8_t* data = nullptr;
        uint64_t offset = 0;           // of data inside buffer
        uint64_t size = 0;
    };

    // Uploads on the transfer queue (§5.11.4, Vulkan Tutorial "Transfer Queues & Asset Streaming"): a persistently
    // mapped staging ring, copies recorded into transfer command buffers and submitted with a timeline semaphore as the
    // clock. Nothing here waits for an upload to finish before a frame is drawn: the frame waits on the GPU for the last
    // value it may read (GetSubmittedValue), and staging space returns to the ring once the timeline has passed it
    // (Poll). The CPU only blocks when a single load needs more staging than the ring holds.
    //
    // Main thread only for now; letting loader threads reserve staging is Phase 5b.
    class UploadManager
    {
    public:
        void Init(NRI::Device& device, uint64_t ringCapacity);

        StagingSpan ReserveStaging(uint64_t size, uint64_t alignment = 16);
        void CopyToBuffer(const StagingSpan& source, NRI::Buffer& destination, uint64_t destinationOffset);
        // Every mip of an image created with sharedAcrossQueues; mip i lives at source.offset + mipOffsets[i].
        void CopyToTexture(const StagingSpan& source, NRI::Texture2D& destination, const std::vector<size_t>& mipOffsets);
        // A growing stream copies itself over, after every write queued to it so far and before the ones that follow.
        void CopyBuffer(NRI::Buffer& source, NRI::Buffer& destination, uint64_t size);

        // Submits the copies queued since the last call; returns the value they complete at.
        uint64_t Flush();
        // Returns command buffers, staging space and oversized staging buffers of completed submissions.
        void Poll();

        NRI::TimelineSemaphore& GetTimeline() const { return *m_timeline; }
        // The value the last submitted copies complete at: a frame waiting on it reads every upload made before it.
        uint64_t GetSubmittedValue() const { return m_submittedValue; }

    private:
        struct RingAllocation
        {
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t value = 0; // the submission that reads it
        };

        struct Submission
        {
            uint64_t value = 0;
            std::unique_ptr<NRI::CommandBuffer> commandBuffer;
            std::vector<std::unique_ptr<NRI::Buffer>> retained; // oversized staging it reads
        };

        NRI::CommandBuffer& recording();
        // Waits (flushing first when needed) until the ring allocations overlapping the range are no longer read.
        void waitForRingSpace(uint64_t offset, uint64_t size);

        NRI::Device* m_device = nullptr;
        std::unique_ptr<NRI::CommandAllocator> m_allocator;
        std::unique_ptr<NRI::TimelineSemaphore> m_timeline;

        std::unique_ptr<NRI::Buffer> m_ring;
        uint8_t* m_ringMapped = nullptr;
        uint64_t m_ringCapacity = 0;
        uint64_t m_ringHead = 0;
        std::deque<RingAllocation> m_ringAllocations; // in ring order, oldest first

        std::unique_ptr<NRI::CommandBuffer> m_recording;      // copies queued since the last Flush
        std::vector<std::unique_ptr<NRI::Buffer>> m_retained; // oversized staging m_recording reads
        std::vector<std::unique_ptr<NRI::CommandBuffer>> m_freeCommandBuffers;
        std::deque<Submission> m_inFlight;
        uint64_t m_submittedValue = 0;
    };
}
