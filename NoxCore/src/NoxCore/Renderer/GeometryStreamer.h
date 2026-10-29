#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "DataTypes.h"
#include "UploadManager.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    class Renderer;

    // What the streamer keeps of a streamed submesh's cooked data (§5.7.2): its page table and the cluster records it patches
    // as pages come and go. Copied out of MeshData when the upload begins (the loader drops the MeshData once it is in staging).
    struct StreamedMeshSource
    {
        std::filesystem::path File;
        std::vector<MeshPage> Pages;
        std::vector<uint32_t> Dependencies;
        std::vector<shaderio::MeshletDraw> Draws; // per cluster, offsets into the page-ordered arrays of the cooked mesh
        std::vector<uint32_t> RefinedPages;       // per cluster: MeshletBounds::refinedPage
        uint32_t RootPageCount = 0;
    };

    // Geometry streaming (§5.7.2, Nanite's page streaming): a streamed mesh keeps its cluster records and root pages; its other
    // pages are read from the cooked file into the geometry streams as the feedback pass asks for them, and evicted, least
    // recently wanted first, when the pool is full.
    //
    // - Residency is dependency closed: a page loads once the pages it depends on (the coarser pages its groups were simplified
    //   into) are resident or loading, becomes drawable after them, and is evicted only when no resident page depends on it -- so
    //   the cut the task shader draws stays exact.
    // - A page's data arrives on the transfer queue into ranges nothing reads yet; it becomes drawable in a frame (its cluster
    //   records patched, its resident flag set) by that frame's own commands (Renderer::addGeometryPagesUpdatePass), as an
    //   eviction does, so every pass of a frame sees the same residency.
    // - Ray tracing traces the finest resident clusters (the leaf cut): a mesh whose residency changed gets a new BLAS built from
    //   them (ClusterIndices.slang writes their triangles, then the build). Evicted pages stay allocated until that BLAS is in use,
    //   the hit shaders read their vertices until then.
    //
    // Owned by the renderer, main thread.
    class GeometryStreamer
    {
    public:
        // Pages started per frame (the upload budget bounds their bytes too).
        static constexpr uint32_t StartsPerFrame = 128;
        // A page is kept this long after the feedback last wanted it before it may be evicted (feedback is a few frames old, and
        // resident pages report every GeometryTouchInterval frames).
        static constexpr uint64_t HoldFrames = 4 * shaderio::GeometryTouchInterval;
        // Pages evicted per frame to bring the streamed pages back within the pool (when nothing new needs the room).
        static constexpr uint32_t EvictionsPerFrame = 512;
        // A mesh's BLAS is built from its leaf cut again at most this often while its pages change.
        static constexpr uint64_t BlasRebuildFrames = 10;

        struct Stats
        {
            uint32_t Meshes = 0;
            uint32_t Pages = 0;          // streamed (not root) pages of all streamed meshes
            uint32_t ResidentPages = 0;
            uint32_t LoadingPages = 0;
            uint64_t ResidentBytes = 0;  // streamed pages' data in the geometry streams
            uint64_t RootBytes = 0;      // root pages' data (always resident)
            uint64_t PoolBytes = 0;
            uint64_t WantedBytes = 0;    // streamed pages the feedback wanted recently (resident or not)
            uint64_t PageLoads = 0;      // since start
            uint64_t PageEvictions = 0;
            uint32_t Requests = 0;       // in the newest feedback
            uint32_t DroppedRequests = 0;
            uint32_t WantedNotResident = 0; // requested pages the pool had no room for (last update)
        };

        explicit GeometryStreamer(Renderer& renderer) : m_Renderer(renderer) {}
        GeometryStreamer(const GeometryStreamer&) = delete;
        GeometryStreamer& operator=(const GeometryStreamer&) = delete;

        // A published streamed submesh; returns its id (MeshHandle::streamingId).
        uint32_t Register(const MeshHandle& handle, std::shared_ptr<StreamedMeshSource> source, uint32_t pageOffset, bool isOpaque);
        // The submesh was unloaded: its streamed pages and RT index ranges go once frames are done with them.
        void Unregister(uint32_t id);

        // The newest feedback readback (GeometryStreamingFeedback).
        void ApplyFeedback(const shaderio::GeometryStreamingFeedback& feedback, uint64_t frame);
        // Once per frame before the frame is recorded: finishes reads and copies, activates pages, plans within the pool,
        // evicts and starts reads within the upload budget (shared with the asset loads), and queues BLAS rebuilds.
        void Update(uint64_t& uploadBudget);

        // The BLAS build of a streamed mesh, recorded now (Renderer::addBLASBuildPass): its leaf cut's index range (allocated here),
        // the jobs that fill it, and its triangle count. False when the mesh is gone. Pages it no longer uses are released.
        bool PrepareBlasBuild(uint32_t id, std::vector<shaderio::ClusterIndexJob>& outJobs, GeometryRange& outIndices, uint32_t& outMaxVertex);
        // Its BLAS was evicted under the ray tracing budget (nothing traces it): pages waiting for a new BLAS go now. Built
        // again from its leaf cut when it is traced again.
        void OnBlasEvicted(uint32_t id);

        void SetEnabled(bool enabled) { m_Enabled = enabled; }
        bool IsEnabled() const { return m_Enabled; }
        // Pool size in bytes instead of the geometry budget's share (0: the budget), to force pressure while testing.
        void SetPoolOverride(uint64_t bytes) { m_PoolOverride = bytes; }
        uint64_t GetPoolOverride() const { return m_PoolOverride; }
        const Stats& GetStats() const { return m_Stats; }
        bool IsIdle() const;
        // While the renderer still exists: waits for reads in flight and drops every streamed page.
        void Shutdown();

    private:
        enum class PageState : uint8_t
        {
            Absent,
            Reading,   // the file read into staging is running
            Uploading, // copies into the streams are queued on the transfer queue
            Resident
        };

        struct PageRanges
        {
            GeometryRange Vertices;
            GeometryRange MeshletVertices;
            GeometryRange MeshletTriangles;
        };

        struct Page
        {
            PageState State = PageState::Absent;
            PageRanges Ranges;
            uint64_t LastWanted = 0;     // frame of the newest feedback that wanted it
            float Priority = 0.0f;       // how far above the threshold its error was then (resident pages: every touch)
            uint32_t Dependents = 0;     // pages depending on it that are resident or loading
            StagingSpan Staging;
            TaskFuture<bool> Read;
            uint64_t CopiesComplete = 0; // upload value its copies complete at
            bool Failed = false;         // its read failed: not asked for again, the coarser clusters stand in
        };

        struct StreamedMesh
        {
            bool Valid = false;
            std::shared_ptr<StreamedMeshSource> Source;
            uint32_t MeshSlot = UINT32_MAX;
            uint32_t BlasId = UINT32_MAX;
            uint32_t DrawsOffset = 0;  // its clusters' MeshletDraw records in the draw stream
            uint32_t PageOffset = 0;   // its pages in the page table
            uint32_t RootVertexEnd = 0; // one past the root pages' vertices in the vertex stream
            bool IsOpaque = true;
            std::vector<Page> Pages;
            bool BlasDirty = false;    // residency changed since its BLAS's leaf cut
            bool BlasQueued = false;
            uint64_t LastBlasFrame = 0;
            GeometryRange RtIndices;   // the streamer's index range of its current BLAS (the first one is the handle's)
            std::vector<PageRanges> Retired; // evicted pages the current BLAS may still use
        };

        // A page whose mesh went while it was read: its staging returns once the read is done.
        struct OrphanRead
        {
            StagingSpan Staging;
            TaskFuture<bool> Read;
            PageRanges Ranges;
        };

        // An eviction candidate (gatherVictims): pages no longer wanted first, least recently wanted first, then wanted ones, least
        // important first (a pool smaller than what the view wants keeps the most important pages: Nanite's priority replacement).
        struct Victim
        {
            bool Wanted = false;
            uint64_t LastWanted = 0;
            float Priority = 0.0f;
            uint32_t Mesh = 0;
            uint32_t Page = 0;
        };
        // A wanted page is replaced only by one this much more important (no swapping back and forth between similar ones).
        static constexpr float ReplacementMargin = 1.25f;
        void gatherVictims(std::vector<Victim>& outVictims) const;
        // Evicts victims from nextVictim on until the streamed pages fit targetBytes, at most maxEvictions of them, stopping at the first
        // wanted page whose priority is not below priorityLimit.
        void evictDownTo(uint64_t targetBytes, const std::vector<Victim>& victims, size_t& nextVictim, uint32_t maxEvictions, float priorityLimit);
        void advanceLoads(StreamedMesh& mesh, uint32_t id);
        void startLoad(StreamedMesh& mesh, uint32_t page, uint64_t& uploadBudget);
        void activate(StreamedMesh& mesh, uint32_t page);
        void evict(StreamedMesh& mesh, uint32_t page);
        // Ranges nothing will read once the frames in flight are done.
        void releaseRanges(const PageRanges& ranges, uint32_t frames);
        void stageResident(const StreamedMesh& mesh, uint32_t page, bool resident);
        uint64_t poolBytes() const;
        static uint64_t pageBytes(const MeshPage& page) { return page.PayloadBytes(); }

        Renderer& m_Renderer;
        std::vector<StreamedMesh> m_Meshes;
        std::vector<uint32_t> m_FreeIds;
        // Global page (page table index) -> mesh id, for the feedback.
        std::vector<uint32_t> m_PageOwners;
        // Pages the newest feedback asked for that are not resident: (priority, mesh id, page).
        struct Candidate
        {
            float Priority = 0.0f;
            uint32_t Mesh = 0;
            uint32_t Page = 0;
        };
        std::vector<Candidate> m_Candidates;
        std::vector<OrphanRead> m_Orphans;
        uint64_t m_Frame = 0;          // the streamer's clock (Update calls)
        uint64_t m_FeedbackFrame = 0;  // Update call of the newest feedback
        uint64_t m_ResidentBytes = 0;  // streamed pages resident or loading
        uint64_t m_PoolOverride = 0;
        bool m_Enabled = true;
        Stats m_Stats;
    };
}
