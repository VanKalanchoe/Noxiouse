#pragma once
#include "D3D12Common.h"
#include "../CommandAllocator.h"

namespace NRI
{
    class DeviceD3D12;

    class CommandAllocatorD3D12 final : public CommandAllocator
    {
    public:
        CommandAllocatorD3D12(DeviceD3D12& device, CommandBufferReset resetMode, D3D12_COMMAND_LIST_TYPE type);
        ~CommandAllocatorD3D12() override = default;

        std::unique_ptr<CommandBuffer> allocateCommandBuffer(uint32_t framesInFlight) override;
        void reset() override;

        ID3D12CommandAllocator* getNativeAllocator() const { return m_allocator.Get(); }
        D3D12_COMMAND_LIST_TYPE getCommandListType() const { return m_type; }

    private:
        DeviceD3D12& m_deviceD3D12;
        ComPtr<ID3D12CommandAllocator> m_allocator;
        D3D12_COMMAND_LIST_TYPE m_type;
        CommandBufferReset m_resetMode;
    };
}
