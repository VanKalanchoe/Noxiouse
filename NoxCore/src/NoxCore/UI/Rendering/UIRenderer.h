#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "NRI/NRI.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/Utils/NOXWatcher.h"

namespace Nox::UI
{
    // Sub-range within the current frame's uploaded primitive/clip buffers.
    struct UIBatch
    {
        uint32_t PrimOffset = 0;
        uint32_t PrimCount = 0;
        uint32_t ClipOffset = 0;
        uint32_t ClipCount = 0;
    };

    // The primitive / clip buffers one OS window's UI is uploaded into: one pair per frame in flight, written from the start
    // every frame. Each window has its own, so a window's frames in flight -- guarded by its own swapchain's fences -- never
    // share memory with another window's (the main window waits only for its own previous frame, not for a floating
    // window's submission, so a shared buffer could be overwritten while that window's GPU work still reads it).
    class UIUploadBuffers
    {
    public:
        static constexpr uint32_t kSlots = 2; // = MAX_FRAMES_IN_FLIGHT (the swapchains' fences per frame slot)

        UIUploadBuffers(NRI::Device& device, size_t maxPrimitives, size_t maxClipRects);
        ~UIUploadBuffers();

        // Starts a new frame: the next upload writes from the start of the slot's buffers.
        void Reset();
        // Copies the draw list into the slot's buffers after what this frame uploaded so far; clipped to the capacity.
        UIBatch Upload(const UIDrawList& drawList, uint32_t slot);

        uint64_t GetPrimitiveAddress(uint32_t slot, uint32_t offset) const;
        uint64_t GetClipRectAddress(uint32_t slot, uint32_t offset) const;

    private:
        size_t m_MaxPrimitives = 0;
        size_t m_MaxClipRects = 0;
        std::vector<std::unique_ptr<NRI::Buffer>> m_PrimitiveBuffers;
        std::vector<void*> m_PrimitiveBuffersMapped;
        std::vector<std::unique_ptr<NRI::Buffer>> m_ClipBuffers;
        std::vector<void*> m_ClipBuffersMapped;
        uint32_t m_PrimWriteOffset = 0;
        uint32_t m_ClipWriteOffset = 0;
    };

    // GPU-driven bindless renderer for UIDrawList (VK_EXT_descriptor_heap + BDA).
    class UIRenderer
    {
    public:
        UIRenderer(NRI::Device& device, NRI::ShaderCompiler& shaderCompiler, Utils::NOXWatcher& fileWatcher);
        ~UIRenderer();

        // Reloads UIPrimitives.slang if modified on disk and resets per-frame buffer write offsets.
        void BeginFrame();

        // Access the draw list for the main viewport.
        UIDrawList& GetDrawList() { return m_DrawList; }
        const UIDrawList& GetDrawList() const { return m_DrawList; }

        // Upload buffers for another OS window's UI (a floating panel's window); the main window's are owned here.
        std::unique_ptr<UIUploadBuffers> CreateUploadBuffers(size_t maxPrimitives = kMaxSecondaryPrimitives,
                                                             size_t maxClipRects = kMaxSecondaryClipRects) const;

        // Uploads a draw list into the buffers' frame slot and returns its batch handle.
        UIBatch UploadDrawList(UIUploadBuffers& buffers, const UIDrawList& drawList, uint32_t frameIndex);

        // Records a batch uploaded into `buffers` into the currently active raster render pass.
        void RenderBatch(NRI::CommandBuffer& cmd, const UIUploadBuffers& buffers, const UIBatch& batch, const glm::vec2& viewportSize,
                         uint32_t imageHeapOffset, uint32_t frameIndex);

        // Uploads the main draw list primitives/clips to the frame-slot buffers.
        void Upload(uint32_t frameIndex);

        // Records the main UI draw pass into the currently active raster render pass.
        void Render(NRI::CommandBuffer& cmd, const glm::vec2& viewportSize, uint32_t imageHeapOffset, uint32_t frameIndex);

    private:
        void CreatePipeline(bool forceCompile);

        NRI::Device& m_Device;
        NRI::ShaderCompiler& m_ShaderCompiler;
        Utils::NOXWatcher& m_FileWatcher;

        std::unique_ptr<NRI::Pipeline> m_Pipeline;
        std::atomic<bool> m_ReloadShader{ false };

        static constexpr size_t kMaxPrimitives = 131072;
        static constexpr size_t kMaxClipRects = 8192;
        // A floating panel's window shows one panel: a quarter of the main window's capacity (about 5 MB per frame slot).
        static constexpr size_t kMaxSecondaryPrimitives = 32768;
        static constexpr size_t kMaxSecondaryClipRects = 2048;

        std::unique_ptr<UIUploadBuffers> m_MainBuffers;
        UIBatch m_MainBatch{};
        UIDrawList m_DrawList;
    };
}
