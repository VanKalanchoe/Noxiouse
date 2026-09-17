#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RGResourcePool.h"
#include "RGTypes.h"

namespace Nox
{
    // Snapshot of one compiled and executed render graph frame for tooling (render graph panel, DOT export, §5.4.7).
    // Filled only while RenderGraph::SetReportEnabled(true). Names point at the graph's string literals.
    struct RGFrameReport
    {
        // Frame order: a group row is followed by its passes and nested groups (Depth + 1).
        struct Row
        {
            const char* Name = nullptr;
            uint32_t Depth = 0;
            uint32_t Pass = RGInvalidIndex; // index into Passes; RGInvalidIndex for a group
        };

        struct Access
        {
            uint32_t Resource = RGInvalidIndex; // index into Textures or Buffers
            bool IsTexture = true;
            bool IsWrite = false;
        };

        struct Pass
        {
            const char* Name = nullptr;
            bool Culled = false;
            bool Raster = false;
            bool Presents = false;
            bool SynchronizeAfter = false; // Blanket barrier after the pass
            uint32_t Barriers = 0;         // Precise barriers before the pass
            uint32_t Chunk = 0;         // command buffer it was recorded into
            float RecordMs = 0.0f;      // CPU recording time this frame
            uint32_t ExclusiveMask = 0; // bits into ExclusiveKeys
            uint32_t FirstAccess = 0;   // range in Accesses
            uint32_t AccessCount = 0;
        };

        struct Resource
        {
            const char* Name = nullptr;
            RGResourceKind Kind = RGResourceKind::Imported;
            bool ReadOnly = false;
            RGTextureKey Texture;       // textures (imports: size only)
            RGBufferKey Buffer;         // buffers
            uint32_t FirstPass = RGInvalidIndex; // executed passes accessing it; RGInvalidIndex when unused this frame
            uint32_t LastPass = 0;
            uint32_t PhysicalId = RGInvalidIndex; // same id = same GPU resource (aliasing, history slots)
            uint64_t Bytes = 0;                   // graph-owned resources only
        };

        struct UnusedWrite
        {
            uint32_t Pass = 0;
            uint32_t Resource = 0;
            bool IsTexture = true;
        };

        uint64_t FrameNumber = 0;
        uint32_t CommandBuffers = 0;
        RGSynchronization Synchronization = RGSynchronization::Blanket;
        uint64_t PoolTextureBytes = 0;
        uint64_t PoolBufferBytes = 0;
        std::vector<Row> Rows;
        std::vector<Pass> Passes;
        std::vector<Access> Accesses;
        std::vector<Resource> Textures;
        std::vector<Resource> Buffers;
        std::vector<const char*> ExclusiveKeys;
        std::vector<std::string> ResetHistoryKeys; // resets that took effect this frame
        bool ResetAllHistory = false;
        std::vector<std::string> ValidationMessages;
        std::vector<UnusedWrite> UnusedWrites; // transient writes no later pass reads (informational)
    };

    // GraphViz DOT: passes as boxes (culled ones dashed), resources as ellipses, edges for reads and writes.
    bool WriteRenderGraphDot(const RGFrameReport& report, const std::filesystem::path& path);
}
