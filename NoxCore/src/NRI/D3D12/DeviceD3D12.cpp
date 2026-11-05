#include "DeviceD3D12.h"
#include "SwapchainD3D12.h"
#include "PipelineD3D12.h"
#include "CommandAllocatorD3D12.h"
#include "CommandBufferD3D12.h"
#include "TextureD3D12.h"
#include "BufferD3D12.h"
#include "DescriptorHeapD3D12.h"
#include "AccelerationStructureD3D12.h"
#include "QueryPoolD3D12.h"
#include "GpuProfilerD3D12.h"
#include "TimelineSemaphoreD3D12.h"
#include "MemoryAllocatorD3D12.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Window.h"
#include <SDL3/SDL.h>

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_dx12.h>

namespace NRI
{
    DeviceD3D12::DeviceD3D12(Nox::Window& window)
    {
        initD3D12(window);
        createGlobalRootSignature();
    }

    DeviceD3D12::~DeviceD3D12()
    {
        shutdown();
    }

    void DeviceD3D12::initD3D12(Nox::Window& window)
    {
        m_hwnd = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window.getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

        UINT dxgiFlags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

        // Enumerate adapters
        ComPtr<IDXGIAdapter1> adapter1;
        SIZE_T maxVram = 0;
        for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter1->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc.DedicatedVideoMemory > maxVram)
            {
                if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device5), nullptr)))
                {
                    maxVram = desc.DedicatedVideoMemory;
                    adapter1.As(&m_adapter);
                }
            }
        }

        if (!m_adapter)
        {
            throw std::runtime_error("D3D12: No suitable hardware GPU found supporting D3D_FEATURE_LEVEL_12_1");
        }

        ThrowIfFailed(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");

        // Queues
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_directQueue)), "Create Direct CommandQueue");

        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeQueue));

        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_copyQueue));

        // WaitIdle sync
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_waitIdleFence)), "CreateFence");
        m_waitIdleEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

        // Memory Allocator
        m_allocator = std::make_unique<MemoryAllocatorD3D12>(*this);
    }

    void DeviceD3D12::createGlobalRootSignature()
    {
        // 32 root constants (128 bytes) mapped to b0, space0 for push constants
        D3D12_ROOT_PARAMETER1 rootParam{};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParam.Constants.ShaderRegister = 0;
        rootParam.Constants.RegisterSpace = 0;
        rootParam.Constants.Num32BitValues = 32;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc{};
        rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootSigDesc.Desc_1_1.NumParameters = 1;
        rootSigDesc.Desc_1_1.pParameters = &rootParam;
        rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
        rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
        rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                     D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

        ComPtr<ID3DBlob> signatureBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            // Fallback for drivers without DIRECTLY_INDEXED flags
            rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ThrowIfFailed(D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob), "SerializeRootSignature");
        }

        ThrowIfFailed(m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                                    IID_PPV_ARGS(&m_globalRootSignature)), "CreateRootSignature");
    }

    void DeviceD3D12::shutdown()
    {
        waitIdle();
        if (m_waitIdleEvent)
        {
            CloseHandle(m_waitIdleEvent);
            m_waitIdleEvent = nullptr;
        }
    }

    void DeviceD3D12::waitIdle()
    {
        if (!m_directQueue || !m_waitIdleFence || !m_waitIdleEvent) return;

        m_waitIdleFenceValue++;
        m_directQueue->Signal(m_waitIdleFence.Get(), m_waitIdleFenceValue);
        if (m_waitIdleFence->GetCompletedValue() < m_waitIdleFenceValue)
        {
            m_waitIdleFence->SetEventOnCompletion(m_waitIdleFenceValue, m_waitIdleEvent);
            WaitForSingleObject(m_waitIdleEvent, INFINITE);
        }
    }

    void DeviceD3D12::submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex)
    {
        auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(&cmdBuffer);
        if (!cmdD3D12) return;

        ID3D12CommandList* lists[] = { cmdD3D12->getNativeCommandList() };
        m_directQueue->ExecuteCommandLists(1, lists);
        waitIdle();
    }

    void DeviceD3D12::submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain,
                                           uint32_t frameIndex, uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits)
    {
        for (const auto& w : timelineWaits)
        {
            if (auto* fence = dynamic_cast<TimelineSemaphoreD3D12*>(w.semaphore))
                m_directQueue->Wait(fence->getNativeFence(), w.value);
        }

        std::vector<ID3D12CommandList*> lists;
        lists.reserve(cmdBuffers.size());
        for (auto* cb : cmdBuffers)
        {
            if (auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(cb))
                lists.push_back(cmdD3D12->getNativeCommandList());
        }

        if (!lists.empty())
        {
            m_directQueue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        }
    }

    void DeviceD3D12::submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers,
                             std::span<const TimelinePoint> waits, std::span<const TimelinePoint> signals)
    {
        ID3D12CommandQueue* targetQueue = m_directQueue.Get();
        if (queue == QueueType::Compute && m_computeQueue)
            targetQueue = m_computeQueue.Get();
        else if (queue == QueueType::Transfer && m_copyQueue)
            targetQueue = m_copyQueue.Get();

        for (const auto& w : waits)
        {
            if (auto* fence = dynamic_cast<TimelineSemaphoreD3D12*>(w.semaphore))
                targetQueue->Wait(fence->getNativeFence(), w.value);
        }

        std::vector<ID3D12CommandList*> lists;
        for (auto* cb : cmdBuffers)
        {
            if (auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(cb))
                lists.push_back(cmdD3D12->getNativeCommandList());
        }

        if (!lists.empty())
            targetQueue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());

        for (const auto& s : signals)
        {
            if (auto* fence = dynamic_cast<TimelineSemaphoreD3D12*>(s.semaphore))
                targetQueue->Signal(fence->getNativeFence(), s.value);
        }
    }

    void DeviceD3D12::initImGui(Nox::Window& window)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 128;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_imguiSrvDescHeap)), "ImGui Desc Heap");

        ImGui_ImplSDL3_InitForD3D(window.getHandle());
        ImGui_ImplDX12_Init(m_device.Get(), 3, DXGI_FORMAT_R8G8B8A8_UNORM, m_imguiSrvDescHeap.Get(),
                            m_imguiSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                            m_imguiSrvDescHeap->GetGPUDescriptorHandleForHeapStart());
        m_imguiInitialized = true;
    }

    void DeviceD3D12::shutdownImGui()
    {
        if (m_imguiInitialized)
        {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            m_imguiInitialized = false;
        }
    }

    void DeviceD3D12::beginImGui()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void DeviceD3D12::endImGui()
    {
        ImGui::Render();
    }

    std::unique_ptr<Swapchain> DeviceD3D12::createSwapchain(const SwapchainDesc& desc)
    {
        return std::make_unique<SwapchainD3D12>(*this, desc);
    }

    std::unique_ptr<Pipeline> DeviceD3D12::createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        return std::make_unique<PipelineD3D12>(*this, desc, compiler);
    }

    std::unique_ptr<CommandAllocator> DeviceD3D12::createCommandAllocator(CommandBufferReset resetMode, QueueType queue)
    {
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (queue == QueueType::Compute) type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        else if (queue == QueueType::Transfer) type = D3D12_COMMAND_LIST_TYPE_COPY;
        return std::make_unique<CommandAllocatorD3D12>(*this, resetMode, type);
    }

    std::unique_ptr<TimelineSemaphore> DeviceD3D12::createTimelineSemaphore(uint64_t initialValue)
    {
        return std::make_unique<TimelineSemaphoreD3D12>(*this, initialValue);
    }

    Nox::Ref<Texture2D> DeviceD3D12::createTexture(const TextureDesc& desc)
    {
        return Nox::CreateRef<TextureD3D12>(*this, desc);
    }

    std::unique_ptr<Buffer> DeviceD3D12::createBuffer(const BufferDesc& desc)
    {
        return std::make_unique<BufferD3D12>(*this, desc);
    }

    std::unique_ptr<DescriptorHeap> DeviceD3D12::createDescriptorHeap(const DescriptorHeapDesc& desc)
    {
        return std::make_unique<DescriptorHeapD3D12>(*this, desc);
    }

    AccelerationStructureBuildSizes DeviceD3D12::getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        if (desc.type == AccelerationStructureType::BottomLevel)
        {
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = static_cast<UINT>(desc.triangles.size());
        }
        else
        {
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = desc.instances.instanceCount;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);

        AccelerationStructureBuildSizes sizes{};
        sizes.accelerationStructureSize = prebuildInfo.ResultDataMaxSizeInBytes;
        sizes.buildScratchSize = prebuildInfo.ScratchDataSizeInBytes;
        sizes.updateScratchSize = prebuildInfo.UpdateScratchDataSizeInBytes;
        return sizes;
    }

    std::unique_ptr<AccelerationStructure> DeviceD3D12::createAccelerationStructure(const AccelerationStructureDesc& desc)
    {
        return std::make_unique<AccelerationStructureD3D12>(*this, desc);
    }

    std::unique_ptr<QueryPool> DeviceD3D12::createQueryPool(const QueryPoolDesc& desc)
    {
        return std::make_unique<QueryPoolD3D12>(*this, desc);
    }

    std::unique_ptr<GpuProfiler> DeviceD3D12::createGpuProfiler(uint32_t framesInFlight)
    {
        return std::make_unique<GpuProfilerD3D12>(*this, framesInFlight);
    }

    void DeviceD3D12::beginFrame(uint32_t frameNumber)
    {
        if (m_allocator)
        {
            m_allocator->setCurrentFrameIndex(frameNumber);
        }
    }

    void DeviceD3D12::getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const
    {
        if (m_allocator)
        {
            m_allocator->getHeapStats(outHeaps);
            return;
        }

        outHeaps.clear();
        DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
        if (m_adapter && SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo)))
        {
            MemoryHeapStats local{};
            local.deviceLocal = true;
            local.usage = videoMemoryInfo.CurrentUsage;
            local.budget = videoMemoryInfo.Budget;
            outHeaps.push_back(local);
        }
    }
}
