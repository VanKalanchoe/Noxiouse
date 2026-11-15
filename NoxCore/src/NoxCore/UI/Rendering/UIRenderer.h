#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "NRI/NRI.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/Utils/NOXWatcher.h"

namespace Nox::UI
{
    // GPU-driven bindless renderer for UIDrawList (VK_EXT_descriptor_heap + BDA).
    class UIRenderer
    {
    public:
        UIRenderer(NRI::Device& device, NRI::ShaderCompiler& shaderCompiler, Utils::NOXWatcher& fileWatcher);
        ~UIRenderer();

        // Reloads UIPrimitives.slang if modified on disk. Call at frame start before recording.
        void BeginFrame();

        // Access the draw list for the main viewport.
        UIDrawList& GetDrawList() { return m_DrawList; }
        const UIDrawList& GetDrawList() const { return m_DrawList; }

        // Uploads current draw list primitives/clips to the frame-slot buffers.
        void Upload(uint32_t frameIndex);

        // Records the UI draw pass into the currently active raster render pass.
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
        static constexpr size_t kMaxPrimitives = 65536;
        static constexpr size_t kMaxClipRects = 4096;

        std::vector<std::unique_ptr<NRI::Buffer>> m_PrimitiveBuffers;
        std::vector<void*> m_PrimitiveBuffersMapped;
        std::vector<std::unique_ptr<NRI::Buffer>> m_ClipBuffers;
        std::vector<void*> m_ClipBuffersMapped;

        uint32_t m_UploadedPrimCount = 0;
        UIDrawList m_DrawList;
    };
}
