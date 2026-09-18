#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "NRI/CommandAllocator.h"
#include "NRI/CommandBuffer.h"
#include "NRI/Texture.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Tasks/JobSystem.h"
#include "RGBlackboard.h"
#include "RGFrameReport.h"
#include "RGResourcePool.h"
#include "RGTypes.h"

namespace NRI
{
    class Buffer;
    class DescriptorHeap;
    class Device;
    class Swapchain;
}

namespace Nox
{
    class RenderGraph;

    // What a pass's execute callback gets: its command buffer and the graph's resources. Execute callbacks only record:
    // they may run on any worker thread, in parallel with other passes' callbacks, so every CPU-side decision belongs in
    // the pass's setup (main thread).
    class RGPassContext
    {
    public:
        NRI::CommandBuffer& Cmd() const { return m_Cmd; }
        NRI::Texture2D& Texture(RGTexture texture) const;
        NRI::Buffer& Buffer(RGBuffer buffer) const;
        // Bindless sampled slot of a texture.
        uint32_t Slot(RGTexture texture) const;
        // Sampled slot, or `fallback` when the handle is invalid or the texture was not allocated this frame.
        uint32_t SlotOr(RGTexture texture, uint32_t fallback) const;
        // Storage (UAV) slot of one mip of a Storage-usage graph texture.
        uint32_t StorageSlot(RGTexture texture, uint32_t mip = 0) const;
        NRI::Extent2D RenderArea() const { return m_RenderArea; }

    private:
        friend class RenderGraph;
        // passIndex: the pass whose declarations validation checks against; RGInvalidIndex for graph tooling (inspection).
        RGPassContext(RenderGraph& graph, NRI::CommandBuffer& cmd, NRI::Extent2D renderArea, uint32_t passIndex);

    private:
        RenderGraph& m_Graph;
        NRI::CommandBuffer& m_Cmd;
        NRI::Extent2D m_RenderArea;
        uint32_t m_PassIndex;
    };

    // Declares one pass's resource accesses and render targets (setup callback of RenderGraph::AddPass).
    class RGBuilder
    {
    public:
        RGTexture Read(RGTexture texture, RGTextureAccess access = RGTextureAccess::Sampled);
        RGTexture Write(RGTexture texture, RGTextureAccess access = RGTextureAccess::StorageWrite);
        RGBuffer Read(RGBuffer buffer, RGBufferAccess access = RGBufferAccess::Read);
        RGBuffer Write(RGBuffer buffer, RGBufferAccess access = RGBufferAccess::Write);

        // Raster passes: color targets bind in declaration order. LoadOP::load also counts as a read.
        void ColorTarget(RGTexture texture, NRI::LoadOP load, NRI::StoreOP store, NRI::ClearColor clear = {});
        void DepthTarget(RGTexture texture, NRI::LoadOP load, NRI::StoreOP store, NRI::ClearDepth clear = {});
        void SwapchainTarget(NRI::Swapchain& swapchain, uint32_t imageIndex, NRI::LoadOP load, NRI::StoreOP store);
        void SetRenderArea(NRI::Extent2D area, RGViewport viewport = RGViewport::FlippedY);

        // The execute callback calls into a library that is not thread-safe (e.g. "NRD", "Streamline", "ImGui"): passes
        // with the same key never record at the same time. The key must outlive the graph (string literal).
        void RecordExclusive(const char* key);

    private:
        friend class RenderGraph;
        RGBuilder(RenderGraph& graph, uint32_t passIndex) : m_Graph(graph), m_PassIndex(passIndex) {}

    private:
        RenderGraph& m_Graph;
        uint32_t m_PassIndex;
    };

    struct RGFrameDesc
    {
        uint64_t FrameNumber = 0;
        NRI::Extent2D RenderExtent{};
        NRI::Extent2D OutputExtent{};
    };

    // Frame render graph (§5.4): passes declare resource access; Compile culls passes whose outputs nobody consumes,
    // allocates transient resources from the pool (aliasing non-overlapping lifetimes) and derives where synchronization
    // is needed; Execute splits the executed passes into consecutive chunks by their measured recording cost, records each
    // chunk into its own command buffer (in parallel on the JobSystem when it is worth it), with a CPU and GPU profiler
    // scope per pass and group, and returns the command buffers in submission order.
    // Main thread except the execute callbacks. Pass, group and resource names must outlive the frame (string literals).
    class RenderGraph
    {
    public:
        RenderGraph() = default;
        ~RenderGraph();

        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        void Initialize(NRI::Device& device, NRI::DescriptorHeap& resourceHeap, uint32_t framesInFlight);
        // Destroys all graph-owned GPU resources (resources and command buffers) immediately. The GPU must be idle
        // (shutdown, resize after waitIdle).
        void ReleaseResources();

        // Recorded at the start of every command buffer, before its first pass (descriptor heaps, baseline dynamic state):
        // a pass never depends on the state left by a pass in another command buffer.
        void SetCommandBufferSetup(std::function<void(NRI::CommandBuffer&)> setup) { m_CommandBufferSetup = std::move(setup); }
        // Off: the whole frame records into one command buffer on the calling thread.
        void SetParallelRecording(bool enabled) { m_ParallelRecording = enabled; }
        bool IsParallelRecording() const { return m_ParallelRecording; }
        // Takes effect with the next Compile.
        void SetSynchronization(RGSynchronization synchronization) { m_Synchronization = synchronization; }
        RGSynchronization GetSynchronization() const { return m_Synchronization; }

        // Starts a new graph: drops the previous frame's passes, resources and blackboard entries.
        void Reset(const RGFrameDesc& frame);

        // Frame-lifetime resource from the pool, allocated only if an executed pass accesses it.
        RGTexture CreateTexture(const char* name, const RGTextureDesc& desc);
        RGBuffer CreateBuffer(const char* name, const RGBufferDesc& desc);
        // Persistent resources under `key` (see RGTextureHistory). Buffers are zeroed on (re)creation.
        RGTextureHistory GetHistoryTexture(const char* key, const RGTextureDesc& desc, uint32_t count);
        RGBufferHistory GetHistoryBuffer(const char* key, const RGBufferDesc& desc, uint32_t count);
        // Resources owned outside the graph; importing the same object twice returns the same handle.
        RGTexture ImportTexture(const char* name, NRI::Texture2D* texture, RGImportAccess access = RGImportAccess::ReadWrite);
        RGBuffer ImportBuffer(const char* name, NRI::Buffer* buffer, RGImportAccess access = RGImportAccess::ReadWrite);

        // Invalidates a history from the next frame on: graph histories under `key` report WasReset, and features with
        // history outside the graph (NRD, DLSS) read it through WasHistoryReset. Replaces per-feature reset flags.
        void ResetHistory(const char* key);
        void ResetAllHistory();
        bool WasHistoryReset(const char* key) const;

        template <typename SetupCallback, typename ExecuteCallback>
        void AddPass(const char* name, RGPassFlags flags, SetupCallback&& setup, ExecuteCallback&& execute)
        {
            const uint32_t passIndex = BeginPass(name, flags);
            RGBuilder builder(*this, passIndex);
            setup(builder);
            StoreExecute(passIndex, std::forward<ExecuteCallback>(execute));
        }

        // Nests the following passes under a named profiler scope (e.g. "ReSTIR GI").
        void PushGroup(const char* name);
        void PopGroup();

        RGBlackboard& GetBlackboard() { return m_Blackboard; }
        // Pooled transient memory (the transient budget category).
        const RGResourcePool& GetResourcePool() const { return m_Pool; }

        void Compile();
        // Records the compiled frame for a frame-in-flight slot whose previous submission has finished. The returned
        // command buffers stay valid until the next Execute and are submitted together, in order.
        std::span<NRI::CommandBuffer* const> Execute(uint32_t frameSlot);

        // After Compile (e.g. to upload bindless slots before Execute).
        NRI::Texture2D* GetTexture(RGTexture texture) const;
        uint32_t GetSlotOr(RGTexture texture, uint32_t fallback) const;

        // Texture inspection (§5.4.7). Before Compile: the `occurrence`-th texture named `name` this frame (history slots
        // share their key as name), invalid when there is none; its resolved size and format (imports: size only).
        RGTexture FindTexture(std::string_view name, uint32_t occurrence) const;
        const RGTextureKey& GetTextureKey(RGTexture texture) const;
        // `inspector` records right after the last executed pass that writes `texture` (else the last one reading it, else
        // at the end of the frame), in that pass's command buffer, so transients are seen before a later pass reuses their
        // memory. It may use any graph resource through its context. One texture per frame; nothing happens when a
        // transient texture is not used this frame.
        void InspectTexture(RGTexture texture, std::function<void(RGPassContext& context)> inspector);
        // After Compile: whether the inspector records this frame.
        bool IsInspecting() const { return m_InspectionActive; }

        // Validation (§5.4.7): undeclared resource use in execute callbacks, stale handles, reads of transients nobody wrote
        // this frame, writes to read-only imports, no presenting pass. Each distinct message is logged once.
        void SetValidationEnabled(bool enabled) { m_ValidationEnabled = enabled; }
        bool IsValidationEnabled() const { return m_ValidationEnabled; }

        // Frame report for tooling, filled by Execute while enabled.
        void SetReportEnabled(bool enabled) { m_ReportEnabled = enabled; }
        const RGFrameReport& GetReport() const { return m_Report; }

    private:
        friend class RGBuilder;
        friend class RGPassContext;

        using InvokeFn = void (*)(void* payload, RGPassContext& context);
        using DestroyFn = void (*)(void* payload);

        enum class EventType : uint8_t
        {
            Pass,
            GroupBegin,
            GroupEnd
        };

        struct Event
        {
            EventType Type;
            uint32_t Index; // pass index, or group index
        };

        struct TextureEntry
        {
            const char* Name = nullptr;
            RGResourceKind Kind = RGResourceKind::Imported;
            bool ReadOnly = false;
            RGTextureKey Key;
            NRI::Texture2D* Texture = nullptr;
            const std::vector<uint32_t>* StorageSlots = nullptr;
            uint32_t FirstPass = RGInvalidIndex;
            uint32_t LastPass = 0;
        };

        struct BufferEntry
        {
            const char* Name = nullptr;
            RGResourceKind Kind = RGResourceKind::Imported;
            bool ReadOnly = false;
            RGBufferKey Key;
            NRI::Buffer* Buffer = nullptr;
            uint32_t FirstPass = RGInvalidIndex;
            uint32_t LastPass = 0;
        };

        struct ResourceAccess
        {
            uint32_t Resource = RGInvalidIndex;
            bool IsTexture = true;
            bool IsWrite = false;
            NRI::ResourceState State; // access + pipeline stages (Precise synchronization)
        };

        struct Attachment
        {
            uint32_t Texture = RGInvalidIndex;
            NRI::LoadOP Load = NRI::LoadOP::dontCare;
            NRI::StoreOP Store = NRI::StoreOP::dontCare;
            NRI::ClearColor ClearColor{};
            NRI::ClearDepth ClearDepth{};
        };

        struct Pass
        {
            const char* Name = nullptr;
            RGPassFlags Flags = RGPassFlags::None;
            std::vector<ResourceAccess> Accesses;
            std::vector<Attachment> ColorTargets;
            Attachment DepthTarget;
            NRI::Swapchain* Swapchain = nullptr;
            uint32_t SwapchainImage = 0;
            NRI::LoadOP SwapchainLoad = NRI::LoadOP::load;
            NRI::StoreOP SwapchainStore = NRI::StoreOP::store;
            NRI::Extent2D RenderArea{};
            RGViewport Viewport = RGViewport::FlippedY;
            void* Payload = nullptr;
            InvokeFn Invoke = nullptr;
            DestroyFn Destroy = nullptr;
            uint32_t ExclusiveMask = 0;  // RecordExclusive keys
            uint32_t ScopeId = 0;        // profiler scope, resolved on the main thread before recording
            uint32_t Chunk = 0;          // command buffer it records into
            float RecordMs = 0.0f;       // measured by the recording thread
            bool Culled = false;
            bool SynchronizeAfter = false; // Blanket
            uint32_t FirstTextureBarrier = 0; // Precise: ranges in m_TextureBarriers / m_BufferBarriers, recorded before the pass
            uint32_t TextureBarrierCount = 0;
            uint32_t FirstBufferBarrier = 0;
            uint32_t BufferBarrierCount = 0;
        };

        // Consecutive events recorded into one command buffer.
        struct Chunk
        {
            uint32_t FirstEvent = 0;
            uint32_t EndEvent = 0;
            uint32_t OpenGroupsOffset = 0; // groups declared open at FirstEvent, outermost first (m_ChunkOpenGroups)
            uint32_t OpenGroupsCount = 0;
        };

        // One command buffer per (chunk, frame slot) with its own allocator: a chunk is recorded by one task, so no pool is
        // used by two threads (per-thread pools in Khronos' multithreading guidance), and the whole pool is reset once
        // the slot's previous submission finished.
        struct CommandRecorder
        {
            std::unique_ptr<NRI::CommandAllocator> Allocator;
            std::unique_ptr<NRI::CommandBuffer> CommandBuffer;
        };

        // Scratch of one recording thread; own cache lines, no false sharing between chunks recorded in parallel.
        struct alignas(64) ChunkScratch
        {
            std::vector<uint32_t> GroupStack;
            NRI::RenderDesc RenderDesc; // BeginRendering
        };

        uint32_t BeginPass(const char* name, RGPassFlags flags);

        template <typename ExecuteCallback>
        void StoreExecute(uint32_t passIndex, ExecuteCallback&& execute)
        {
            using Function = std::decay_t<ExecuteCallback>;
            Pass& pass = m_Passes[passIndex];
            pass.Payload = JobSystem::Get().GetFrameArena().New<Function>(std::forward<ExecuteCallback>(execute));
            pass.Invoke = [](void* payload, RGPassContext& context) { (*static_cast<Function*>(payload))(context); };
            pass.Destroy = [](void* payload) { static_cast<Function*>(payload)->~Function(); };
        }

        RGTextureKey ResolveKey(const RGTextureDesc& desc) const;
        void AddAccess(uint32_t passIndex, uint32_t resource, bool isTexture, bool isWrite, NRI::ResourceState state);
        void PlanBlanketSynchronization();
        void PlanPreciseSynchronization();
        void ValidateHandle(uint32_t handleFrame, const char* passName);
        void ValidateContextAccess(uint32_t passIndex, uint32_t resource, bool isTexture);
        // After Compile's culling and lifetimes (RGValidation.cpp).
        void ValidateCompiledFrame();
        void ReportValidation(std::string message);
        void PlaceInspection();
        void RecordInspection(NRI::CommandBuffer& cmd);
        void BuildReport();
        uint32_t GetExclusiveKeyBit(const char* key);
        bool HasSideEffects(const Pass& pass) const;
        void AllocateTransients();
        float GetEstimatedRecordMs(const Pass& pass) const;
        void PlanChunks();
        // Splits the executed passes into `chunkCount` consecutive chunks of about equal cost; returns the estimated
        // recording wall time (chunks sharing an exclusive key run one after another) plus the per-chunk overhead.
        float SplitIntoChunks(uint32_t chunkCount, float totalMs);
        void PrepareRecording(uint32_t frameSlot);
        void RecordChunk(uint32_t chunkIndex);
        void ExecutePass(Pass& pass, NRI::CommandBuffer& cmd, ChunkScratch& scratch, GpuScopeContext& gpuScopes);
        void BeginRendering(const Pass& pass, NRI::CommandBuffer& cmd, NRI::RenderDesc& desc);
        void ReleaseCallbacks();
        uint32_t GetScopeId(const char* name);

    private:
        // Measured cost of one more chunk (task, command buffer begin/end, pool reset, baseline state): a split must save
        // more recording wall time than this (enough work per thread).
        static constexpr float ChunkOverheadMs = 0.05f;
        static constexpr uint32_t MaxChunks = 8;
        // Until a pass was measured once.
        static constexpr float DefaultPassRecordMs = 0.02f;

        NRI::Device* m_Device = nullptr;
        uint32_t m_FramesInFlight = 0;
        RGFrameDesc m_Frame;
        uint32_t m_FrameId = 0; // stamped into handles
        RGResourcePool m_Pool;

        std::vector<Pass> m_Passes; // entries reused across frames (their vectors keep capacity)
        uint32_t m_PassCount = 0;
        std::vector<Event> m_Events;
        std::vector<const char*> m_GroupNames;
        std::vector<uint32_t> m_GroupScopeIds;
        uint32_t m_OpenGroupDepth = 0;
        std::vector<const char*> m_ExclusiveKeys; // bit index = position

        std::vector<TextureEntry> m_Textures;
        std::vector<BufferEntry> m_Buffers;
        std::unordered_map<const void*, uint32_t> m_ImportedTextures;
        std::unordered_map<const void*, uint32_t> m_ImportedBuffers;

        // History resets requested during a frame take effect in the next one.
        std::unordered_set<std::string> m_PendingHistoryResets;
        std::unordered_set<std::string> m_FrameHistoryResets;
        bool m_PendingResetAll = false;
        bool m_FrameResetAll = false;

        RGBlackboard m_Blackboard;

        // Recording
        std::function<void(NRI::CommandBuffer&)> m_CommandBufferSetup;
        bool m_ParallelRecording = true;
        // D13: Precise (same GPU/CPU cost as Blanket in the Phase 2 captures; checkable by synchronization validation).
        RGSynchronization m_Synchronization = RGSynchronization::Precise;
        std::vector<NRI::TextureBarrierDesc> m_TextureBarriers;
        std::vector<NRI::BufferBarrierDesc> m_BufferBarriers;
        uint32_t m_FrameSlot = 0;
        std::vector<CommandRecorder> m_Recorders; // [chunk * framesInFlight + slot]
        std::vector<Chunk> m_Chunks;
        std::vector<uint32_t> m_ChunkOpenGroups;
        std::vector<uint32_t> m_ChunkExclusiveMasks;
        std::vector<float> m_ChunkRecordMs; // estimated, planning only
        std::vector<ChunkScratch> m_ChunkScratch;
        std::vector<GpuScopeContext> m_GpuScopeContexts;
        std::vector<NRI::CommandBuffer*> m_SubmitList;
        std::unordered_map<const char*, float> m_PassRecordMs; // smoothed recording cost per pass name

        // Inspection
        uint32_t m_InspectedTexture = RGInvalidIndex;
        std::function<void(RGPassContext& context)> m_Inspector;
        uint32_t m_InspectionPass = RGInvalidIndex; // after this executed pass; RGInvalidIndex: end of the frame
        bool m_InspectionActive = false;

        // Validation + report
#ifndef NDEBUG // on in Debug builds
        bool m_ValidationEnabled = true;
#else
        bool m_ValidationEnabled = false;
#endif
        std::mutex m_ValidationMutex; // execute callbacks report from recording threads
        std::vector<std::string> m_ValidationMessages;
        std::unordered_set<std::string> m_LoggedValidationMessages;
        std::vector<RGFrameReport::UnusedWrite> m_UnusedWrites;
        bool m_ReportEnabled = false;
        RGFrameReport m_Report;

        // Compile scratch (capacity reused)
        std::vector<bool> m_ResourceRead;
        std::vector<uint32_t> m_LastWriterScratch;
        std::vector<NRI::ResourceState> m_LastStateScratch;
        std::vector<bool> m_AccessedScratch;
        std::vector<std::pair<uint32_t, NRI::ResourceState>> m_PassStatesScratch;
        std::vector<uint32_t> m_AllocationOrderScratch;
        std::vector<uint32_t> m_PlanStackScratch;
        std::unordered_map<const char*, uint32_t> m_ScopeIds; // profiler scope per pass/group name
    };
}
