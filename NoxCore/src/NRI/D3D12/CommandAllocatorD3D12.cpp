#include "CommandAllocatorD3D12.h"
#include "CommandBufferD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    CommandAllocatorD3D12::CommandAllocatorD3D12(DeviceD3D12& device, CommandBufferReset resetMode, D3D12_COMMAND_LIST_TYPE type)
        : m_deviceD3D12(device), m_resetMode(resetMode), m_type(type)
    {
        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateCommandAllocator(
            m_type, IID_PPV_ARGS(&m_allocator)), "CreateCommandAllocator");
    }

    std::unique_ptr<CommandBuffer> CommandAllocatorD3D12::allocateCommandBuffer(uint32_t framesInFlight)
    {
        return std::make_unique<CommandBufferD3D12>(m_deviceD3D12, *this);
    }

    void CommandAllocatorD3D12::reset()
    {
        ThrowIfFailed(m_allocator->Reset(), "CommandAllocator Reset");
    }
}
