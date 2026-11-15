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

        // Uploads an arbitrary draw list into the current frame's ring region and returns its batch handle.
        UIBatch UploadDrawList(const UIDrawList& drawList, uint32_t frameIndex);

        // Records a previously uploaded batch into the currently active raster render pass.
        void RenderBatch(NRI::CommandBuffer& cmd, const UIBatch& batch, const glm::vec2& viewportSize, uint32_t imageHeapOffset, uint32_t frameIndex);

        // Uploads the main draw list primitives/clips to the frame-slot buffers.
        void Upload(uint32_t frameIndex);

        // Records the main UI draw pass into the currently active raster render pass.
        void Render(NRI::CommandBuffer& cmd, const glm::vec2& viewportSize, uint32_t imageHeapOffset, uint32_t frameIndex);

    private:
        void CreatePipeline(bool forceCompile);
        void CreateBuffers();

        NRI::Device& m_Device;
        NRI::ShaderCompiler& m_ShaderCompiler;
        Utils::NOXWatcher& m_FileWatcher;

        std::unique_ptr<NRI::Pipeline> m_Pipeline;
        std::atomic<bool> m_ReloadShader{ false };

        static constexpr uint32_t kMaxFramesInFlight = 2;
        static constexpr size_t kMaxPrimitives = 131072;
        static constexpr size_t kMaxClipRects = 8192;

        std::vector<std::unique_ptr<NRI::Buffer>> m_PrimitiveBuffers;
        std::vector<void*> m_PrimitiveBuffersMapped;
        std::vector<std::unique_ptr<NRI::Buffer>> m_ClipBuffers;
        std::vector<void*> m_ClipBuffersMapped;

        uint32_t m_PrimWriteOffset = 0;
        uint32_t m_ClipWriteOffset = 0;
        UIBatch m_MainBatch{};
        UIDrawList m_DrawList;
    };
}
