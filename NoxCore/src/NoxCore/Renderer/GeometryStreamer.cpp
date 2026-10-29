#include "GeometryStreamer.h"

#include <algorithm>
#include <cstddef>
#include <limits>

#include "Renderer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Profiling/Profiler.h"

namespace Nox
{
    uint32_t GeometryStreamer::Register(const MeshHandle& handle, std::shared_ptr<StreamedMeshSource> source, uint32_t pageOffset, bool isOpaque)
    {
        uint32_t id;
        if (!m_FreeIds.empty())
        {
            id = m_FreeIds.back();
            m_FreeIds.pop_back();
        }
        else
        {
            id = static_cast<uint32_t>(m_Meshes.size());
            m_Meshes.emplace_back();
        }

        StreamedMesh& mesh = m_Meshes[id];
        mesh = StreamedMesh{};
        mesh.Valid = true;
        mesh.MeshSlot = handle.gpuSceneMesh;
        mesh.BlasId = handle.blasId;
        mesh.DrawsOffset = handle.meshletDraws.offset;
        mesh.PageOffset = pageOffset;
        mesh.RootVertexEnd = handle.vertices.offset + handle.vertices.count;
        mesh.IsOpaque = isOpaque;
        mesh.Pages.resize(source->Pages.size());
        for (uint32_t page = 0; page < source->RootPageCount; ++page)
        {
            mesh.Pages[page].State = PageState::Resident;
            m_Stats.RootBytes += pageBytes(source->Pages[page]);
        }
        m_Stats.Pages += static_cast<uint32_t>(source->Pages.size() - source->RootPageCount);
        ++m_Stats.Meshes;

        const size_t end = size_t(pageOffset) + source->Pages.size();
        if (m_PageOwners.size() < end)
            m_PageOwners.resize(end, UINT32_MAX);
        std::fill(m_PageOwners.begin() + pageOffset, m_PageOwners.begin() + end, id);

        // Its page table, in the frame that publishes it (the root pages resident, the others not yet).
        std::vector<shaderio::GpuGeometryPage> records(source->Pages.size());
        for (size_t index = 0; index < records.size(); ++index)
        {
            const MeshPage& page = source->Pages[index];
            shaderio::GpuGeometryPage& record = records[index];
            record.lodCenter = page.LodCenter;
            record.lodRadius = page.LodRadius;
            record.lodError = page.IsRoot() ? shaderio::ClusterTerminalError : page.LodError;
            record.resident = page.IsRoot() ? 1u : 0u;
            record.requestFrame = 0xFFFFFFFF;
            record.padding = 0;
        }
        m_Renderer.stageGeometryPatch(Renderer::GeometryPatchTarget::Pages, sizeof(shaderio::GpuGeometryPage) * uint64_t(pageOffset),
                                      records.data(), sizeof(shaderio::GpuGeometryPage) * records.size());

        mesh.Source = std::move(source);
        return id;
    }

    void GeometryStreamer::Unregister(uint32_t id)
    {
        if (id >= m_Meshes.size() || !m_Meshes[id].Valid)
            return;
        StreamedMesh& mesh = m_Meshes[id];
        const StreamedMeshSource& source = *mesh.Source;

        for (uint32_t index = source.RootPageCount; index < mesh.Pages.size(); ++index)
        {
            Page& page = mesh.Pages[index];
            switch (page.State)
            {
            case PageState::Reading:
                // The read writes into its staging: that returns once the read is done.
                m_Orphans.push_back({ page.Staging, std::move(page.Read), page.Ranges });
                m_ResidentBytes -= std::min(m_ResidentBytes, pageBytes(source.Pages[index]));
                --m_Stats.LoadingPages;
                break;
            case PageState::Uploading:
                // Its copies are queued on the transfer queue, ahead of any later copy into the same ranges.
                releaseRanges(page.Ranges, MAX_FRAMES_IN_FLIGHT + 1);
                m_ResidentBytes -= std::min(m_ResidentBytes, pageBytes(source.Pages[index]));
                --m_Stats.LoadingPages;
                break;
            case PageState::Resident:
                releaseRanges(page.Ranges, MAX_FRAMES_IN_FLIGHT + 2);
                m_ResidentBytes -= std::min(m_ResidentBytes, pageBytes(source.Pages[index]));
                --m_Stats.ResidentPages;
                break;
            case PageState::Absent:
                break;
            }
        }
        for (const PageRanges& ranges : mesh.Retired)
            releaseRanges(ranges, MAX_FRAMES_IN_FLIGHT + 2);
        if (mesh.RtIndices.IsValid())
        {
            MeshHandle indices;
            indices.rtIndices = mesh.RtIndices;
            std::scoped_lock lock(m_Renderer.m_deferredReleasesMutex);
            m_Renderer.m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT + 2, indices });
        }

        for (uint32_t page = 0; page < source.RootPageCount; ++page)
            m_Stats.RootBytes -= std::min(m_Stats.RootBytes, pageBytes(source.Pages[page]));
        m_Stats.Pages -= static_cast<uint32_t>(source.Pages.size() - source.RootPageCount);
        --m_Stats.Meshes;

        const size_t end = std::min(m_PageOwners.size(), size_t(mesh.PageOffset) + source.Pages.size());
        for (size_t page = mesh.PageOffset; page < end; ++page)
            m_PageOwners[page] = UINT32_MAX;
        mesh = StreamedMesh{};
        m_FreeIds.push_back(id);
    }

    void GeometryStreamer::ApplyFeedback(const shaderio::GeometryStreamingFeedback& feedback, uint64_t /*frame*/)
    {
        m_FeedbackFrame = m_Frame;
        m_Candidates.clear();
        const uint32_t count = std::min(feedback.count, shaderio::GeometryRequestCapacity);
        m_Stats.Requests = count;
        m_Stats.DroppedRequests = feedback.count - count;
        for (uint32_t index = 0; index < count; ++index)
        {
            const shaderio::GeometryPageRequest& request = feedback.requests[index];
            const uint32_t global = request.page & ~shaderio::GeometryRequestResident;
            if (global >= m_PageOwners.size() || m_PageOwners[global] == UINT32_MAX)
                continue; // unloaded since
            const uint32_t id = m_PageOwners[global];
            StreamedMesh& mesh = m_Meshes[id];
            const uint32_t local = global - mesh.PageOffset;
            if (local >= mesh.Pages.size())
                continue;
            Page& page = mesh.Pages[local];
            page.LastWanted = m_Frame;
            page.Priority = request.priority;
            if (page.State == PageState::Absent && !page.Failed)
                m_Candidates.push_back({ request.priority, id, local });
        }
    }

    void GeometryStreamer::Update(uint64_t& uploadBudget)
    {
        NOX_PROFILE_SCOPE("Geometry Streaming");
        ++m_Frame;

        std::erase_if(m_Orphans, [this](OrphanRead& orphan)
        {
            if (!orphan.Read.IsReady())
                return false;
            try { orphan.Read.Get(); } catch (...) {}
            m_Renderer.m_uploads.ReleaseStaging(orphan.Staging);
            releaseRanges(orphan.Ranges, 1); // never copied to
            return true;
        });

        for (uint32_t id = 0; id < m_Meshes.size(); ++id)
        {
            if (m_Meshes[id].Valid)
                advanceLoads(m_Meshes[id], id);
        }

        // The wanted pages, most wanted first, within the pool: what has not been wanted for a while makes room.
        const uint64_t pool = poolBytes();
        m_Stats.PoolBytes = pool;
        m_Stats.WantedNotResident = 0;
        if (m_Enabled && !m_Candidates.empty())
        {
            std::sort(m_Candidates.begin(), m_Candidates.end(), [](const Candidate& a, const Candidate& b) { return a.Priority > b.Priority; });

            std::vector<Victim> victims; // gathered once needed
            bool victimsGathered = false;
            size_t nextVictim = 0;
            uint32_t starts = 0;
            for (size_t index = 0; index < m_Candidates.size(); ++index)
            {
                const Candidate& candidate = m_Candidates[index];
                StreamedMesh& mesh = m_Meshes[candidate.Mesh];
                if (!mesh.Valid || candidate.Page >= mesh.Pages.size() || mesh.Pages[candidate.Page].State != PageState::Absent)
                    continue;
                const StreamedMeshSource& source = *mesh.Source;
                const MeshPage& info = source.Pages[candidate.Page];

                // Its dependencies first (they are wanted too, with a larger error: earlier in this list).
                bool dependenciesThere = true;
                for (uint32_t dependency = 0; dependency < info.DependencyCount; ++dependency)
                    dependenciesThere &= mesh.Pages[source.Dependencies[info.FirstDependency + dependency]].State != PageState::Absent;
                if (!dependenciesThere)
                    continue;

                const uint64_t bytes = pageBytes(info);
                if (m_ResidentBytes + bytes > pool)
                {
                    if (!victimsGathered)
                    {
                        victimsGathered = true;
                        gatherVictims(victims);
                    }
                    // Room from pages not wanted any more, then from wanted ones clearly less important than this one.
                    evictDownTo(pool - std::min(pool, bytes), victims, nextVictim, UINT32_MAX, candidate.Priority / ReplacementMargin);
                }
                if (m_ResidentBytes + bytes > pool)
                {
                    // The pool holds what is wanted: the rest waits (less detail where it is wanted least).
                    m_Stats.WantedNotResident = static_cast<uint32_t>(m_Candidates.size() - index);
                    break;
                }
                if (starts >= StartsPerFrame || bytes > uploadBudget)
                    break;
                const uint64_t before = m_ResidentBytes;
                startLoad(mesh, candidate.Page, uploadBudget);
                if (m_ResidentBytes == before)
                    break; // no staging or stream space this frame
                ++starts;
            }
        }
        m_Candidates.clear();

        // Over the pool without anything new to load (the pool was made smaller, or the camera left what it had loaded): pages go,
        // a bounded number a frame, those not wanted any more first, then the least important wanted ones (they ask again, and come
        // back only in place of something clearly less important).
        if (m_ResidentBytes > pool)
        {
            std::vector<Victim> victims;
            gatherVictims(victims);
            size_t nextVictim = 0;
            evictDownTo(pool, victims, nextVictim, EvictionsPerFrame, std::numeric_limits<float>::infinity());
        }

        // Meshes whose residency changed: a new BLAS from their leaf cut, not more often than BlasRebuildFrames.
        for (uint32_t id = 0; id < m_Meshes.size(); ++id)
        {
            StreamedMesh& mesh = m_Meshes[id];
            if (!mesh.Valid || !mesh.BlasDirty || mesh.BlasQueued || mesh.BlasId == UINT32_MAX || m_Frame < mesh.LastBlasFrame + BlasRebuildFrames)
                continue;
            if (mesh.BlasId < m_Renderer.m_blasSources.size() && m_Renderer.m_blasSources[mesh.BlasId].evicted)
                continue; // built when it is traced again
            Renderer::BlasBuild build;
            build.blasId = mesh.BlasId;
            build.meshSlot = mesh.MeshSlot;
            build.isOpaque = mesh.IsOpaque;
            build.streamingId = id;
            m_Renderer.m_blasBuilds.push_back(build);
            mesh.BlasQueued = true;
        }

        m_Stats.ResidentBytes = m_ResidentBytes;
        // What the feedback wants now: resident pages wanted within HoldFrames, and the requested ones still missing.
        m_Stats.WantedBytes = 0;
        for (const StreamedMesh& mesh : m_Meshes)
        {
            if (!mesh.Valid)
                continue;
            for (uint32_t page = mesh.Source->RootPageCount; page < mesh.Pages.size(); ++page)
            {
                if (mesh.Pages[page].LastWanted != 0 && mesh.Pages[page].LastWanted + HoldFrames >= m_FeedbackFrame && !mesh.Pages[page].Failed)
                    m_Stats.WantedBytes += pageBytes(mesh.Source->Pages[page]);
            }
        }
        // In the stats report: whether streaming is engaged at all (streamed meshes), what it holds and what the feedback asks.
        NOX_PROFILE_COUNTER("Streamed Geometry Meshes", m_Stats.Meshes);
        NOX_PROFILE_COUNTER("Streamed Geometry Pages", m_Stats.ResidentPages);
        NOX_PROFILE_COUNTER("Streamed Geometry Pages Total", m_Stats.Pages);
        NOX_PROFILE_COUNTER("Streamed Geometry Pages Loading", m_Stats.LoadingPages);
        NOX_PROFILE_COUNTER("Streamed Geometry MB", static_cast<int64_t>(m_ResidentBytes >> 20));
        NOX_PROFILE_COUNTER("Streamed Geometry Root MB", static_cast<int64_t>(m_Stats.RootBytes >> 20));
        NOX_PROFILE_COUNTER("Streamed Geometry Wanted MB", static_cast<int64_t>(m_Stats.WantedBytes >> 20));
        NOX_PROFILE_COUNTER("Geometry Pool MB", static_cast<int64_t>(m_Stats.PoolBytes >> 20));
        NOX_PROFILE_COUNTER("Geometry Page Requests", m_Stats.Requests);
        NOX_PROFILE_COUNTER("Geometry Pages Evicted", static_cast<int64_t>(m_Stats.PageEvictions));
    }

    void GeometryStreamer::gatherVictims(std::vector<Victim>& outVictims) const
    {
        // Resident streamed pages nothing resident or loading depends on (their finer pages go first): not wanted for HoldFrames, least
        // recently wanted first, then wanted ones, least important first.
        for (uint32_t id = 0; id < m_Meshes.size(); ++id)
        {
            const StreamedMesh& owner = m_Meshes[id];
            if (!owner.Valid)
                continue;
            for (uint32_t page = owner.Source->RootPageCount; page < owner.Pages.size(); ++page)
            {
                const Page& state = owner.Pages[page];
                if (state.State == PageState::Resident && state.Dependents == 0)
                    outVictims.push_back({ state.LastWanted + HoldFrames >= m_FeedbackFrame, state.LastWanted, state.Priority, id, page });
            }
        }
        std::sort(outVictims.begin(), outVictims.end(), [](const Victim& a, const Victim& b)
        {
            if (a.Wanted != b.Wanted)
                return !a.Wanted;
            return a.Wanted ? a.Priority < b.Priority : a.LastWanted < b.LastWanted;
        });
    }

    void GeometryStreamer::evictDownTo(uint64_t targetBytes, const std::vector<Victim>& victims, size_t& nextVictim, uint32_t maxEvictions,
                                       float priorityLimit)
    {
        uint32_t evicted = 0;
        while (m_ResidentBytes > targetBytes && nextVictim < victims.size() && evicted < maxEvictions)
        {
            if (victims[nextVictim].Wanted && !(victims[nextVictim].Priority < priorityLimit))
                break; // the rest are at least as important
            const Victim& victim = victims[nextVictim++];
            StreamedMesh& owner = m_Meshes[victim.Mesh];
            if (owner.Valid && owner.Pages[victim.Page].State == PageState::Resident && owner.Pages[victim.Page].Dependents == 0)
            {
                evict(owner, victim.Page);
                ++evicted;
            }
        }
    }

    void GeometryStreamer::advanceLoads(StreamedMesh& mesh, uint32_t /*id*/)
    {
        const StreamedMeshSource& source = *mesh.Source;
        // In page order: a page's dependencies (earlier pages) become drawable before it, in the same frame at the latest.
        for (uint32_t index = source.RootPageCount; index < mesh.Pages.size(); ++index)
        {
            Page& page = mesh.Pages[index];
            if (page.State == PageState::Reading)
            {
                if (!page.Read.IsReady())
                    continue;
                bool read = false;
                try { read = page.Read.Get(); } catch (...) { read = false; }
                const MeshPage& info = source.Pages[index];
                if (!read)
                {
                    NOX_CORE_ERROR("Geometry streaming: could not read page {} of {}", index, source.File.generic_string());
                    m_Renderer.m_uploads.ReleaseStaging(page.Staging);
                    releaseRanges(page.Ranges, 1);
                    page.Ranges = {};
                    page.State = PageState::Absent;
                    page.Failed = true;
                    for (uint32_t dependency = 0; dependency < info.DependencyCount; ++dependency)
                        --mesh.Pages[source.Dependencies[info.FirstDependency + dependency]].Dependents;
                    m_ResidentBytes -= std::min(m_ResidentBytes, pageBytes(info));
                    --m_Stats.LoadingPages;
                    continue;
                }

                // The three blocks into the streams' current buffers (they may have grown since the ranges were allocated).
                UploadManager& uploads = m_Renderer.m_uploads;
                uploads.CopyToBuffer(page.Staging, 0, sizeof(shaderio::Vertex) * uint64_t(info.VertexCount),
                                     m_Renderer.m_vertexStream.GetBuffer(), sizeof(shaderio::Vertex) * uint64_t(page.Ranges.Vertices.offset));
                uploads.CopyToBuffer(page.Staging, info.VertexBlockBytes(), sizeof(uint32_t) * uint64_t(info.MeshletVertexCount),
                                     m_Renderer.m_meshletVertexStream.GetBuffer(), sizeof(uint32_t) * uint64_t(page.Ranges.MeshletVertices.offset));
                uploads.CopyToBuffer(page.Staging, info.VertexBlockBytes() + info.MeshletVertexBlockBytes(), info.MeshletTriangleCount,
                                     m_Renderer.m_meshletTriangleStream.GetBuffer(), page.Ranges.MeshletTriangles.offset);
                page.CopiesComplete = uploads.Commit(page.Staging, false);
                page.Staging = {};
                page.State = PageState::Uploading;
            }
            if (page.State == PageState::Uploading && m_Renderer.GetCompletedUploadValue() >= page.CopiesComplete)
            {
                const MeshPage& info = source.Pages[index];
                bool dependenciesResident = true;
                for (uint32_t dependency = 0; dependency < info.DependencyCount; ++dependency)
                    dependenciesResident &= mesh.Pages[source.Dependencies[info.FirstDependency + dependency]].State == PageState::Resident;
                if (dependenciesResident)
                    activate(mesh, index);
            }
        }
    }

    void GeometryStreamer::startLoad(StreamedMesh& mesh, uint32_t index, uint64_t& uploadBudget)
    {
        const StreamedMeshSource& source = *mesh.Source;
        const MeshPage& info = source.Pages[index];
        const uint64_t bytes = pageBytes(info);
        std::optional<StagingSpan> staging = m_Renderer.m_uploads.TryReserveStaging(bytes);
        if (!staging)
            return;

        PageRanges ranges;
        ranges.Vertices = m_Renderer.allocateGeometry(m_Renderer.m_vertexStream, info.VertexCount);
        ranges.MeshletVertices = m_Renderer.allocateGeometry(m_Renderer.m_meshletVertexStream, info.MeshletVertexCount);
        ranges.MeshletTriangles = m_Renderer.allocateGeometry(m_Renderer.m_meshletTriangleStream, info.MeshletTriangleCount);
        if (!ranges.Vertices.IsValid() || !ranges.MeshletVertices.IsValid() || !ranges.MeshletTriangles.IsValid())
        {
            m_Renderer.m_uploads.ReleaseStaging(*staging);
            releaseRanges(ranges, 1);
            return;
        }

        Page& page = mesh.Pages[index];
        page.State = PageState::Reading;
        page.Ranges = ranges;
        page.Staging = *staging;
        // From the cooked file straight into staging (a pack's entry as a loose file's range).
        page.Read = JobSystem::Get().AsyncIO("Read Geometry Page", JobPriority::High,
            [file = source.File, offset = info.PayloadOffset, bytes, destination = staging->data](const CancellationToken&)
            {
                return VFS::ReadRange(file, offset, bytes, destination);
            });
        for (uint32_t dependency = 0; dependency < info.DependencyCount; ++dependency)
            ++mesh.Pages[source.Dependencies[info.FirstDependency + dependency]].Dependents;
        m_ResidentBytes += bytes;
        uploadBudget -= std::min(uploadBudget, bytes);
        ++m_Stats.LoadingPages;
    }

    void GeometryStreamer::activate(StreamedMesh& mesh, uint32_t index)
    {
        const StreamedMeshSource& source = *mesh.Source;
        const MeshPage& info = source.Pages[index];
        Page& page = mesh.Pages[index];

        // Its clusters' records point at its ranges (the cooked offsets are into the page-ordered arrays).
        std::vector<shaderio::MeshletDraw> draws(source.Draws.begin() + info.FirstCluster, source.Draws.begin() + info.FirstCluster + info.ClusterCount);
        for (shaderio::MeshletDraw& draw : draws)
        {
            draw.vertexOffset = page.Ranges.MeshletVertices.offset + (draw.vertexOffset - info.FirstMeshletVertex);
            draw.triangleOffset = page.Ranges.MeshletTriangles.offset + (draw.triangleOffset - info.FirstMeshletTriangle);
            draw.globalVertexOffset = page.Ranges.Vertices.offset + (draw.globalVertexOffset - info.FirstVertex);
        }
        m_Renderer.stageGeometryPatch(Renderer::GeometryPatchTarget::Draws,
                                      sizeof(shaderio::MeshletDraw) * (uint64_t(mesh.DrawsOffset) + info.FirstCluster), draws.data(),
                                      sizeof(shaderio::MeshletDraw) * draws.size());
        stageResident(mesh, index, true);

        page.State = PageState::Resident;
        mesh.BlasDirty = true;
        --m_Stats.LoadingPages;
        ++m_Stats.ResidentPages;
        ++m_Stats.PageLoads;
    }

    void GeometryStreamer::evict(StreamedMesh& mesh, uint32_t index)
    {
        const StreamedMeshSource& source = *mesh.Source;
        const MeshPage& info = source.Pages[index];
        Page& page = mesh.Pages[index];

        stageResident(mesh, index, false);
        page.State = PageState::Absent;
        for (uint32_t dependency = 0; dependency < info.DependencyCount; ++dependency)
            --mesh.Pages[source.Dependencies[info.FirstDependency + dependency]].Dependents;
        m_ResidentBytes -= std::min(m_ResidentBytes, pageBytes(info));

        // The raster stops drawing it this frame; the BLAS (and the hit shaders through its index range) use it until the next
        // leaf cut's BLAS is in use.
        const bool traced = mesh.BlasId != UINT32_MAX &&
                            !(mesh.BlasId < m_Renderer.m_blasSources.size() && m_Renderer.m_blasSources[mesh.BlasId].evicted);
        if (traced)
        {
            mesh.Retired.push_back(page.Ranges);
            mesh.BlasDirty = true;
        }
        else
        {
            releaseRanges(page.Ranges, MAX_FRAMES_IN_FLIGHT + 1);
        }
        page.Ranges = {};
        --m_Stats.ResidentPages;
        ++m_Stats.PageEvictions;
    }

    bool GeometryStreamer::PrepareBlasBuild(uint32_t id, std::vector<shaderio::ClusterIndexJob>& outJobs, GeometryRange& outIndices, uint32_t& outMaxVertex)
    {
        if (id >= m_Meshes.size() || !m_Meshes[id].Valid)
            return false;
        StreamedMesh& mesh = m_Meshes[id];
        const StreamedMeshSource& source = *mesh.Source;
        mesh.BlasQueued = false;

        // The leaf cut: resident clusters whose finer version is not resident. The highest vertex its indices reach bounds the build.
        uint32_t triangles = 0;
        uint32_t vertexEnd = mesh.RootVertexEnd;
        const size_t firstJob = outJobs.size();
        for (uint32_t index = 0; index < mesh.Pages.size(); ++index)
        {
            if (mesh.Pages[index].State != PageState::Resident)
                continue;
            const MeshPage& info = source.Pages[index];
            const GeometryRange& vertices = mesh.Pages[index].Ranges.Vertices;
            if (vertices.IsValid())
                vertexEnd = std::max(vertexEnd, vertices.offset + vertices.count);
            for (uint32_t cluster = info.FirstCluster; cluster < info.FirstCluster + info.ClusterCount; ++cluster)
            {
                const uint32_t refined = source.RefinedPages[cluster];
                if (refined != shaderio::ClusterNoPage && mesh.Pages[refined].State == PageState::Resident)
                    continue;
                outJobs.push_back({ mesh.DrawsOffset + cluster, triangles * 3 });
                triangles += source.Draws[cluster].triangleCount;
            }
        }

        const GeometryRange indices = triangles > 0 ? m_Renderer.allocateGeometry(m_Renderer.m_rtIndexStream, triangles * 3) : GeometryRange{};
        if (!indices.IsValid())
        {
            outJobs.resize(firstJob);
            mesh.BlasDirty = true;
            return false;
        }
        for (size_t job = firstJob; job < outJobs.size(); ++job)
            outJobs[job].firstIndex += indices.offset;

        // What the BLAS it replaces used: once frames tracing it are done (it is in the TLAS until the next frame's instances).
        std::vector<PageRanges> retired = std::move(mesh.Retired);
        mesh.Retired.clear();
        for (const PageRanges& ranges : retired)
            releaseRanges(ranges, MAX_FRAMES_IN_FLIGHT + 2);
        if (mesh.RtIndices.IsValid())
        {
            MeshHandle old;
            old.rtIndices = mesh.RtIndices;
            std::scoped_lock lock(m_Renderer.m_deferredReleasesMutex);
            m_Renderer.m_deferredReleases.push_back({ MAX_FRAMES_IN_FLIGHT + 2, old });
        }
        mesh.RtIndices = indices;
        outMaxVertex = vertexEnd - 1;
        mesh.BlasDirty = false;
        mesh.LastBlasFrame = m_Frame;
        outIndices = indices;
        return true;
    }

    void GeometryStreamer::OnBlasEvicted(uint32_t id)
    {
        if (id >= m_Meshes.size() || !m_Meshes[id].Valid)
            return;
        StreamedMesh& mesh = m_Meshes[id];
        for (const PageRanges& ranges : mesh.Retired)
            releaseRanges(ranges, MAX_FRAMES_IN_FLIGHT + 2);
        mesh.Retired.clear();
        mesh.BlasDirty = true;
        mesh.BlasQueued = false;
    }

    void GeometryStreamer::releaseRanges(const PageRanges& ranges, uint32_t frames)
    {
        MeshHandle released;
        released.vertices = ranges.Vertices;
        released.meshletVertices = ranges.MeshletVertices;
        released.meshletTriangles = ranges.MeshletTriangles;
        std::scoped_lock lock(m_Renderer.m_deferredReleasesMutex);
        m_Renderer.m_deferredReleases.push_back({ frames, released });
    }

    void GeometryStreamer::stageResident(const StreamedMesh& mesh, uint32_t page, bool resident)
    {
        const uint32_t value = resident ? 1u : 0u;
        m_Renderer.stageGeometryPatch(Renderer::GeometryPatchTarget::Pages,
                                      sizeof(shaderio::GpuGeometryPage) * (uint64_t(mesh.PageOffset) + page) + offsetof(shaderio::GpuGeometryPage, resident),
                                      &value, sizeof(value));
    }

    uint64_t GeometryStreamer::poolBytes() const
    {
        constexpr uint64_t MinimumPool = 64ull << 20;
        if (m_PoolOverride != 0)
            return m_PoolOverride;
        // The geometry budget's share left by what does not stream (cluster records, root pages, meshes without pages, RT indices).
        const MemoryCategoryStats& geometry = m_Renderer.GetMemoryCategory(MemoryCategory::Geometry);
        if (geometry.Budget == 0)
            return 256ull << 20;
        const uint64_t notStreamed = geometry.Used - std::min(geometry.Used, m_ResidentBytes);
        return std::max(MinimumPool, geometry.Budget > notStreamed ? geometry.Budget - notStreamed : 0);
    }

    bool GeometryStreamer::IsIdle() const
    {
        return m_Stats.LoadingPages == 0 && m_Orphans.empty();
    }

    void GeometryStreamer::Shutdown()
    {
        for (StreamedMesh& mesh : m_Meshes)
            for (Page& page : mesh.Pages)
                page.Read.Wait();
        for (OrphanRead& orphan : m_Orphans)
            orphan.Read.Wait();
        m_Orphans.clear();
        m_Meshes.clear();
        m_FreeIds.clear();
        m_PageOwners.clear();
    }
}
