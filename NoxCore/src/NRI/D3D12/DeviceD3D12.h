#pragma once
#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "D3D12Common.h"
#include "../Device.h"

namespace NRI
{
    class SwapchainD3D12;
    class MemoryAllocatorD3D12;

    class DeviceD3D12 final : public Device
    {
    public:
        explicit DeviceD3D12(Nox::Window& window);
        ~DeviceD3D12() override;

        GraphicsAPI getAPI() const override { return GraphicsAPI::D3D12; }

        ID3D12Device5* getNativeDevice() const { return m_device.Get(); }
        IDXGIFactory6* getNativeFactory() const { return m_factory.Get(); }
        IDXGIAdapter4* getNativeAdapter() const { return m_adapter.Get(); }
        ID3D12CommandQueue* getGraphicsQueue() const { return m_directQueue.Get(); }
        ID3D12CommandQueue* getComputeQueue() const { return m_computeQueue ? m_computeQueue.Get() : m_directQueue.Get(); }
        ID3D12CommandQueue* getTransferQueue() const { return m_copyQueue ? m_copyQueue.Get() : m_directQueue.Get(); }
        ID3D12RootSignature* getGlobalRootSignature() const { return m_globalRootSignature.Get(); }
        MemoryAllocatorD3D12* getAllocator() const { return m_allocator.get(); }

        void shutdown() override;
        uint32_t getMSAASampleCount() const override { return 1; }
        void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) override;
        void submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                  uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits = {}) override;
        void submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                    std::span<const TimelinePoint> signals) override;
        void waitIdle() override;

        void initImGui(Nox::Window& window) override;
        void shutdownImGui() override;
        void beginImGui() override;
        void endImGui() override;

        std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
        std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) override;
        std::unique_ptr<CommandAllocator> createCommandAllocator(CommandBufferReset resetMode, QueueType queue) override;
        std::unique_ptr<TimelineSemaphore> createTimelineSemaphore(uint64_t initialValue) override;
        Nox::Ref<Texture2D> createTexture(const TextureDesc& desc) override;
        std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
        std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) override;
        AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) override;
        std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) override;
        std::unique_ptr<QueryPool> createQueryPool(const QueryPoolDesc& desc) override;
        std::unique_ptr<GpuProfiler> createGpuProfiler(uint32_t framesInFlight) override;

        void beginFrame(uint32_t frameNumber) override;
        void getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const override;
        uint64_t getTextureBytes() const override { return m_textureBytes.load(std::memory_order_relaxed); }
        void addTextureBytes(int64_t bytes) { m_textureBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed); }

        bool hasDedicatedTransferQueue() const override { return m_copyQueue != nullptr; }
        bool hasAsyncComputeQueue() const override { return m_computeQueue != nullptr; }
        bool isMemoryBudgetSupported() const override { return true; }
        HWND getWindowHandle() const { return m_hwnd; }

        uint32_t getShaderGroupHandleSize() const override { return D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; }
        uint32_t getShaderGroupBaseAlignment() const override { return D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; }

    private:
        void initD3D12(Nox::Window& window);
        void createGlobalRootSignature();

    private:
        HWND m_hwnd = nullptr;
        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<IDXGIAdapter4> m_adapter;
        ComPtr<ID3D12Device5> m_device;

        ComPtr<ID3D12CommandQueue> m_directQueue;
        ComPtr<ID3D12CommandQueue> m_computeQueue;
        ComPtr<ID3D12CommandQueue> m_copyQueue;

        ComPtr<ID3D12RootSignature> m_globalRootSignature;

        ComPtr<ID3D12Fence> m_waitIdleFence;
        uint64_t m_waitIdleFenceValue = 0;
        HANDLE m_waitIdleEvent = nullptr;

        std::atomic<uint64_t> m_textureBytes = 0;

        // ImGui D3D12 descriptor heap
        ComPtr<ID3D12DescriptorHeap> m_imguiSrvDescHeap;
        bool m_imguiInitialized = false;

        std::unique_ptr<MemoryAllocatorD3D12> m_allocator;
    };
}
