#pragma once
#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <NRI/NRI.h>

namespace Nox
{
    // Room in staging memory for one upload: write `data` (from any thread), record the copies that read it, then
    // Commit. A span reserved but never copied from is given back with ReleaseStaging.
    struct StagingSpan
    {
        NRI::Buffer* buffer = nullptr; // the staging ring, or an oversized upload's own buffer
        uint8_t* data = nullptr;
        uint64_t offset = 0;           // of data inside buffer
        uint64_t size = 0;
        uint64_t ticket = 0;           // identifies the reservation for Commit / ReleaseStaging
    };

    // Uploads on the transfer queue (§5.11.4, Vulkan Tutorial "Transfer Queues & Asset Streaming"): a persistently
    // mapped staging ring, copies recorded into transfer command buffers and submitted with a timeline semaphore as the
    // clock. Nothing here waits for an upload to finish before a frame is drawn: a frame waits on the GPU only for the
    // copies it reads (GetFrameWaitValue), streamed resources are published once the timeline has passed their copies,
    // and staging space returns to the ring at the same point (Poll).
    //
    // Main thread only, except writing a reserved span's memory: loader threads fill spans the main thread reserved
    // for them, so the ring needs no lock.
    class UploadManager
    {
    public:
        void Init(NRI::Device& device, uint64_t ringCapacity);

        // For copies recorded right away: waits (flushing first when needed) until the ring has room.
        StagingSpan ReserveStaging(uint64_t size, uint64_t alignment = 16);
        // For streamed uploads filled later, possibly on another thread: never waits, empty when the ring has no room
        // this frame. The space stays reserved until Commit or ReleaseStaging.
        std::optional<StagingSpan> TryReserveStaging(uint64_t size, uint64_t alignment = 16);
        // A span that no copy read: its space returns at once.
        void ReleaseStaging(const StagingSpan& span);

        void CopyToBuffer(const StagingSpan& source, uint64_t sourceOffset, uint64_t size, NRI::Buffer& destination, uint64_t destinationOffset);
        // Every mip of an image created with sharedAcrossQueues; mip i lives at source.offset + mipOffsets[i].
        void CopyToTexture(const StagingSpan& source, NRI::Texture2D& destination, const std::vector<size_t>& mipOffsets);
        // A growing stream copies itself over, after every write queued to it so far and before the ones that follow.
        // The next frame reads the new buffer, so it waits for this copy.
        void CopyBuffer(NRI::Buffer& source, NRI::Buffer& destination, uint64_t size);
        // After the span's copies are recorded: its space returns once they complete. nextFrameReads makes the next
        // frame wait for them (uploads published right away); streamed uploads publish after completion instead.
        // Returns the value the copies complete at.
        uint64_t Commit(const StagingSpan& span, bool nextFrameReads);

        // Submits the copies queued since the last call; returns the value they complete at.
        uint64_t Flush();
        // Returns command buffers, staging space and oversized staging buffers of completed submissions.
        void Poll();

        NRI::TimelineSemaphore& GetTimeline() const { return *m_timeline; }
        // How far the GPU had got at the last Poll: streamed uploads completed up to here can be published.
        uint64_t GetCompletedValue() const { return m_completedValue; }
        // What the frame being submitted waits for: copies it reads (see Commit), and every completion a resource was
        // published at (already signalled, it orders the transfer writes before the first read at no cost).
        uint64_t GetFrameWaitValue() const { return std::max(m_frameWaitValue, m_completedValue); }

    private:
        static constexpr uint64_t OpenValue = ~0ull; // reserved, no copy recorded yet

        struct RingAllocation
        {
            uint64_t offset = 0;
            uint64_t size = 0;
            uint64_t ticket = 0;
            uint64_t value = OpenValue; // the submission that reads it
        };

        struct Submission
        {
            uint64_t value = 0;
            std::unique_ptr<NRI::CommandBuffer> commandBuffer;
            std::vector<std::unique_ptr<NRI::Buffer>> retained; // oversized staging it reads
        };

        NRI::CommandBuffer& recording();
        // The ring offset a new allocation of the size starts at.
        uint64_t ringOffset(uint64_t size, uint64_t alignment) const;
        bool overlapsLiveAllocation(uint64_t offset, uint64_t size) const;
        // Waits (flushing first when needed) until no allocation overlaps the range; false when an open one does (it
        // completes only after its owner commits it, which this wait would never let happen).
        bool waitForRingSpace(uint64_t offset, uint64_t size);
        StagingSpan allocateRing(uint64_t offset, uint64_t size);
        StagingSpan allocateDedicated(uint64_t size);

        NRI::Device* m_device = nullptr;
        std::unique_ptr<NRI::CommandAllocator> m_allocator;
        std::unique_ptr<NRI::TimelineSemaphore> m_timeline;

        std::unique_ptr<NRI::Buffer> m_ring;
        uint8_t* m_ringMapped = nullptr;
        uint64_t m_ringCapacity = 0;
        uint64_t m_ringHead = 0;
        std::deque<RingAllocation> m_ringAllocations; // in reservation order
        // Oversized staging of spans not committed yet.
        std::unordered_map<uint64_t, std::unique_ptr<NRI::Buffer>> m_dedicated;
        uint64_t m_nextTicket = 1;

        std::unique_ptr<NRI::CommandBuffer> m_recording;      // copies queued since the last Flush
        std::vector<std::unique_ptr<NRI::Buffer>> m_retained; // oversized staging m_recording reads
        std::vector<std::unique_ptr<NRI::CommandBuffer>> m_freeCommandBuffers;
        std::deque<Submission> m_inFlight;
        uint64_t m_submittedValue = 0;
        uint64_t m_completedValue = 0;
        uint64_t m_frameWaitValue = 0;
    };
}
