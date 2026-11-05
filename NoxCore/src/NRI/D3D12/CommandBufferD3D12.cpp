#include "CommandBufferD3D12.h"
#include "DeviceD3D12.h"
#include "CommandAllocatorD3D12.h"
#include "PipelineD3D12.h"
#include "TextureD3D12.h"
#include "BufferD3D12.h"
#include "DescriptorHeapD3D12.h"
#include "AccelerationStructureD3D12.h"
#include "QueryPoolD3D12.h"
#include "SwapchainD3D12.h"
#include <backends/imgui_impl_dx12.h>
#include <algorithm>

namespace NRI
{
    namespace
    {
        D3D12_RESOURCE_STATES TranslateTextureLayoutToD3D12(TextureLayout layout)
        {
            switch (layout)
            {
            case TextureLayout::Undefined: return D3D12_RESOURCE_STATE_COMMON;
            case TextureLayout::ColorAttachment: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case TextureLayout::DepthAttachment: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case TextureLayout::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case TextureLayout::TransferSrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case TextureLayout::TransferDst: return D3D12_RESOURCE_STATE_COPY_DEST;
            case TextureLayout::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case TextureLayout::General: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            default: return D3D12_RESOURCE_STATE_COMMON;
            }
        }
    }

    CommandBufferD3D12::CommandBufferD3D12(DeviceD3D12& device, CommandAllocatorD3D12& allocator)
        : m_deviceD3D12(device), m_allocator(allocator)
    {
        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateCommandList(
            0, m_allocator.getCommandListType(), m_allocator.getNativeAllocator(),
            nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
        m_cmdList->Close();

        // Create temporary RTV and DSV descriptor heaps for dynamic rendering
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 8;
        m_deviceD3D12.getNativeDevice()->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap));
        m_rtvDescriptorSize = m_deviceD3D12.getNativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.NumDescriptors = 1;
        m_deviceD3D12.getNativeDevice()->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap));
        m_dsvDescriptorSize = m_deviceD3D12.getNativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }

    void CommandBufferD3D12::begin(uint32_t index, bool oneTimeSubmit)
    {
        ThrowIfFailed(m_cmdList->Reset(m_allocator.getNativeAllocator(), nullptr), "CommandList Reset");
        m_isRecording = true;

        if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            ID3D12RootSignature* rootSig = m_deviceD3D12.getGlobalRootSignature();
            if (rootSig)
            {
                if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
                    m_cmdList->SetGraphicsRootSignature(rootSig);
                m_cmdList->SetComputeRootSignature(rootSig);
            }
        }
    }

    void CommandBufferD3D12::end(uint32_t index)
    {
        ThrowIfFailed(m_cmdList->Close(), "CommandList Close");
        m_isRecording = false;
    }

    void CommandBufferD3D12::beginRendering(RenderDesc& desc)
    {
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (size_t i = 0; i < desc.colorAttachments.size() && i < 8; ++i)
        {
            auto& attach = desc.colorAttachments[i];
            if (attach.attachment)
            {
                auto* tex = dynamic_cast<TextureD3D12*>(attach.attachment);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvStart;
                rtvHandle.ptr += i * m_rtvDescriptorSize;

                m_deviceD3D12.getNativeDevice()->CreateRenderTargetView(tex->getNativeResource(), nullptr, rtvHandle);
                rtvHandles.push_back(rtvHandle);

                if (attach.loadOP == LoadOP::clear)
                {
                    const float clearColor[4] = { attach.clearColor.r, attach.clearColor.g, attach.clearColor.b, attach.clearColor.a };
                    m_cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                }
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{ 0 };
        bool hasDepth = false;
        if (desc.depthAttachment.attachment)
        {
            auto* depthTex = dynamic_cast<TextureD3D12*>(desc.depthAttachment.attachment);
            dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            m_deviceD3D12.getNativeDevice()->CreateDepthStencilView(depthTex->getNativeResource(), nullptr, dsvHandle);
            hasDepth = true;

            if (desc.depthAttachment.loadOP == LoadOP::clear)
            {
                m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, desc.depthAttachment.clearDepth.depth, 0, 0, nullptr);
            }
        }

        m_cmdList->OMSetRenderTargets(
            static_cast<UINT>(rtvHandles.size()),
            rtvHandles.data(),
            FALSE,
            hasDepth ? &dsvHandle : nullptr
        );

        // Viewport and Scissor
        D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(desc.renderArea.width), static_cast<float>(desc.renderArea.height), 0.0f, 1.0f };
        D3D12_RECT sci{ 0, 0, static_cast<LONG>(desc.renderArea.width), static_cast<LONG>(desc.renderArea.height) };
        m_cmdList->RSSetViewports(1, &vp);
        m_cmdList->RSSetScissorRects(1, &sci);
    }

    void CommandBufferD3D12::endRendering()
    {
    }

    void CommandBufferD3D12::renderImGui()
    {
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_cmdList.Get());
    }

    void CommandBufferD3D12::bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline)
    {
        auto& pipeD3D12 = static_cast<PipelineD3D12&>(pipeline);
        if (pipeD3D12.getNativePipeline())
            m_cmdList->SetPipelineState(pipeD3D12.getNativePipeline());
    }

    void CommandBufferD3D12::setViewport(Extent2D swapChainExtent)
    {
        D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(swapChainExtent.width), static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        m_cmdList->RSSetViewports(1, &vp);
    }

    void CommandBufferD3D12::setScissor(Extent2D swapChainExtent)
    {
        D3D12_RECT sci{ 0, 0, static_cast<LONG>(swapChainExtent.width), static_cast<LONG>(swapChainExtent.height) };
        m_cmdList->RSSetScissorRects(1, &sci);
    }

    void CommandBufferD3D12::setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth)
    {
        D3D12_VIEWPORT vp{ bounds.x, bounds.y, bounds.width, bounds.height, minDepth, maxDepth };
        m_cmdList->RSSetViewports(1, &vp);
    }

    void CommandBufferD3D12::setScissorWithCount(const Extent2D& swapChainExtent)
    {
        setScissor(swapChainExtent);
    }

    void CommandBufferD3D12::setScissorRect(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        const D3D12_RECT rect{ x, y, x + static_cast<LONG>(width), y + static_cast<LONG>(height) };
        m_cmdList->RSSetScissorRects(1, &rect);
    }

    void CommandBufferD3D12::setPrimitiveTopology(const PrimitiveTopology& topology)
    {
        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void CommandBufferD3D12::bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap)
    {
        ID3D12DescriptorHeap* heaps[2]{};
        UINT count = 0;
        if (resourceHeap)
            heaps[count++] = static_cast<DescriptorHeapD3D12*>(resourceHeap)->getNativeHeap();
        if (samplerHeap)
            heaps[count++] = static_cast<DescriptorHeapD3D12*>(samplerHeap)->getNativeHeap();

        if (count > 0)
            m_cmdList->SetDescriptorHeaps(count, heaps);
    }

    void CommandBufferD3D12::pushData(const void* data, uint32_t size)
    {
        UINT numValues = size / 4;
        if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
            m_cmdList->SetGraphicsRoot32BitConstants(0, numValues, data, 0);
        m_cmdList->SetComputeRoot32BitConstants(0, numValues, data, 0);
    }

    void CommandBufferD3D12::bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset)
    {
        D3D12_VERTEX_BUFFER_VIEW view{};
        view.BufferLocation = buffer.getDeviceAddress() + offset;
        view.SizeInBytes = static_cast<UINT>(buffer.getSize() - offset);
        view.StrideInBytes = 36;
        m_cmdList->IASetVertexBuffers(firstBinding, 1, &view);
    }

    void CommandBufferD3D12::bindIndexBuffer(Buffer& buffer, uint64_t offet)
    {
        D3D12_INDEX_BUFFER_VIEW view{};
        view.BufferLocation = buffer.getDeviceAddress() + offet;
        view.SizeInBytes = static_cast<UINT>(buffer.getSize() - offet);
        view.Format = DXGI_FORMAT_R32_UINT;
        m_cmdList->IASetIndexBuffer(&view);
    }

    void CommandBufferD3D12::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        m_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void CommandBufferD3D12::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        m_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void CommandBufferD3D12::drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        m_cmdList->DispatchMesh(groupCountX, groupCountY, groupCountZ);
    }

    void CommandBufferD3D12::drawMeshTasksIndirect(Buffer& indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        // Handled via ExecuteIndirect when command signature is bound
    }

    void CommandBufferD3D12::drawMeshTasksIndirect(uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
    }

    void CommandBufferD3D12::drawMeshTasksIndirectCount(Buffer& indirectBuffer, uint64_t offset, Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
    {
    }

    void CommandBufferD3D12::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        m_cmdList->Dispatch(groupCountX, groupCountY, groupCountZ);
    }

    void CommandBufferD3D12::traceRays(const StridedDeviceAddressRegion& rayGen, const StridedDeviceAddressRegion& miss,
                                       const StridedDeviceAddressRegion& hitGroup, const StridedDeviceAddressRegion& callable,
                                       uint32_t width, uint32_t height, uint32_t depth)
    {
        D3D12_DISPATCH_RAYS_DESC desc{};
        desc.RayGenerationShaderRecord.StartAddress = rayGen.deviceAddress;
        desc.RayGenerationShaderRecord.SizeInBytes = rayGen.size;

        desc.MissShaderTable.StartAddress = miss.deviceAddress;
        desc.MissShaderTable.SizeInBytes = miss.size;
        desc.MissShaderTable.StrideInBytes = miss.stride;

        desc.HitGroupTable.StartAddress = hitGroup.deviceAddress;
        desc.HitGroupTable.SizeInBytes = hitGroup.size;
        desc.HitGroupTable.StrideInBytes = hitGroup.stride;

        desc.CallableShaderTable.StartAddress = callable.deviceAddress;
        desc.CallableShaderTable.SizeInBytes = callable.size;
        desc.CallableShaderTable.StrideInBytes = callable.stride;

        desc.Width = width;
        desc.Height = height;
        desc.Depth = depth;

        m_cmdList->DispatchRays(&desc);
    }

    void CommandBufferD3D12::copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, const BufferCopyRegion& region)
    {
        auto* srcD3D12 = static_cast<BufferD3D12*>(&srcBuffer);
        auto* dstD3D12 = static_cast<BufferD3D12*>(&dstBuffer);
        uint64_t size = region.size > 0 ? region.size : std::min(srcBuffer.getSize(), dstBuffer.getSize());
        m_cmdList->CopyBufferRegion(dstD3D12->getNativeBuffer(), region.dstOffset, srcD3D12->getNativeBuffer(), region.srcOffset, size);
    }

    void CommandBufferD3D12::fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint32_t value)
    {
        // In D3D12, buffer filling can be recorded as a UAV clear or staging copy
    }

    void CommandBufferD3D12::copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, std::span<const BufferCopyRegion> regions)
    {
        auto* srcD3D12 = static_cast<BufferD3D12*>(&srcBuffer);
        auto* dstD3D12 = static_cast<BufferD3D12*>(&dstBuffer);
        for (const auto& reg : regions)
        {
            m_cmdList->CopyBufferRegion(dstD3D12->getNativeBuffer(), reg.dstOffset, srcD3D12->getNativeBuffer(), reg.srcOffset, reg.size);
        }
    }

    void CommandBufferD3D12::transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* texD3D12 = static_cast<TextureD3D12*>(&texture);
        D3D12_RESOURCE_STATES stateBefore = TranslateTextureLayoutToD3D12(oldLayout);
        D3D12_RESOURCE_STATES stateAfter = TranslateTextureLayoutToD3D12(newLayout);

        if (stateBefore != stateAfter)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texD3D12->getNativeResource();
            barrier.Transition.StateBefore = stateBefore;
            barrier.Transition.StateAfter = stateAfter;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_cmdList->ResourceBarrier(1, &barrier);
        }
    }

    void CommandBufferD3D12::transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* swapD3D12 = static_cast<SwapchainD3D12*>(&swapchain);
        TextureD3D12* backBuffer = swapD3D12->getBackBuffer(imageIndex);
        if (backBuffer)
        {
            transitionTextureLayout(*backBuffer, oldLayout, newLayout);
        }
    }

    void CommandBufferD3D12::resolveImage(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height)
    {
        auto* srcD3D12 = static_cast<TextureD3D12*>(&srcTexture);
        auto* dstD3D12 = static_cast<TextureD3D12*>(&dstTexture);
        m_cmdList->ResolveSubresource(
            dstD3D12->getNativeResource(), 0,
            srcD3D12->getNativeResource(), 0,
            srcD3D12->getDxgiFormat());
    }

    void CommandBufferD3D12::copyTexture(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height)
    {
        auto* srcD3D12 = static_cast<TextureD3D12*>(&srcTexture);
        auto* dstD3D12 = static_cast<TextureD3D12*>(&dstTexture);

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstD3D12->getNativeResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcD3D12->getNativeResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_BOX box{ 0, 0, 0, width, height, 1 };
        m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
    }

    void CommandBufferD3D12::copyTextureMips(Texture& srcTexture, uint32_t srcFirstMip, Texture& dstTexture, uint32_t dstFirstMip, uint32_t mipCount)
    {
        auto* srcD3D12 = static_cast<TextureD3D12*>(&srcTexture);
        auto* dstD3D12 = static_cast<TextureD3D12*>(&dstTexture);

        for (uint32_t i = 0; i < mipCount; ++i)
        {
            const uint32_t srcMip = srcFirstMip + i;
            const uint32_t dstMip = dstFirstMip + i;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dstD3D12->getNativeResource();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = dstMip;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = srcD3D12->getNativeResource();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = srcMip;

            uint32_t mipW = std::max(1u, srcTexture.GetWidth() >> srcMip);
            uint32_t mipH = std::max(1u, srcTexture.GetHeight() >> srcMip);
            D3D12_BOX box{ 0, 0, 0, mipW, mipH, 1 };

            m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
        }
    }

    void CommandBufferD3D12::buildAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& dstAS)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.DestAccelerationStructureData = dstAS.getDeviceAddress();
        desc.ScratchAccelerationStructureData = scratchAddress;

        if (buildDesc.type == AccelerationStructureType::BottomLevel)
        {
            desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            desc.Inputs.NumDescs = static_cast<UINT>(buildDesc.triangles.size());
            desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs(buildDesc.triangles.size());
            for (size_t i = 0; i < buildDesc.triangles.size(); ++i)
            {
                const auto& tri = buildDesc.triangles[i];
                geomDescs[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geomDescs[i].Flags = tri.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                geomDescs[i].Triangles.VertexBuffer.StartAddress = tri.vertexBufferAddress;
                geomDescs[i].Triangles.VertexBuffer.StrideInBytes = tri.vertexStride;
                geomDescs[i].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geomDescs[i].Triangles.VertexCount = tri.maxVertex;
                geomDescs[i].Triangles.IndexBuffer = tri.indexBufferAddress + tri.primitiveOffset;
                geomDescs[i].Triangles.IndexCount = tri.primitiveCount * 3;
                geomDescs[i].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            }
            desc.Inputs.pGeometryDescs = geomDescs.data();
            m_cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
        else
        {
            desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            desc.Inputs.NumDescs = buildDesc.instances.instanceCount;
            desc.Inputs.InstanceDescs = buildDesc.instances.instanceBufferAddress;
            desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            m_cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
    }

    void CommandBufferD3D12::updateAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& srcAS, AccelerationStructure& dstAS)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.SourceAccelerationStructureData = srcAS.getDeviceAddress();
        desc.DestAccelerationStructureData = dstAS.getDeviceAddress();
        desc.ScratchAccelerationStructureData = scratchAddress;

        if (buildDesc.type == AccelerationStructureType::BottomLevel)
        {
            desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            desc.Inputs.NumDescs = static_cast<UINT>(buildDesc.triangles.size());
            desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs(buildDesc.triangles.size());
            for (size_t i = 0; i < buildDesc.triangles.size(); ++i)
            {
                const auto& tri = buildDesc.triangles[i];
                geomDescs[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geomDescs[i].Flags = tri.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                geomDescs[i].Triangles.VertexBuffer.StartAddress = tri.vertexBufferAddress;
                geomDescs[i].Triangles.VertexBuffer.StrideInBytes = tri.vertexStride;
                geomDescs[i].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geomDescs[i].Triangles.VertexCount = tri.maxVertex;
                geomDescs[i].Triangles.IndexBuffer = tri.indexBufferAddress + tri.primitiveOffset;
                geomDescs[i].Triangles.IndexCount = tri.primitiveCount * 3;
                geomDescs[i].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            }
            desc.Inputs.pGeometryDescs = geomDescs.data();
            m_cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
        else
        {
            desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            desc.Inputs.NumDescs = buildDesc.instances.instanceCount;
            desc.Inputs.InstanceDescs = buildDesc.instances.instanceBufferAddress;
            desc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            m_cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        }
    }

    void CommandBufferD3D12::accelerationStructureBarrier(AccelerationStructureBarrierType barrierType)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        m_cmdList->ResourceBarrier(1, &b);
    }

    void CommandBufferD3D12::resetQueries(QueryPool& pool, uint32_t first, uint32_t count)
    {
    }

    void CommandBufferD3D12::writeCompactedSizes(std::span<AccelerationStructure* const> structures, QueryPool& pool, uint32_t firstQuery)
    {
        auto* qp = dynamic_cast<QueryPoolD3D12*>(&pool);
        if (qp && qp->getReadbackBuffer())
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuildDesc{};
            postbuildDesc.DestBuffer = qp->getReadbackBuffer()->GetGPUVirtualAddress() + firstQuery * sizeof(uint64_t);
            postbuildDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;

            std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addresses;
            addresses.reserve(structures.size());
            for (auto* s : structures)
                addresses.push_back(s->getDeviceAddress());

            m_cmdList->EmitRaytracingAccelerationStructurePostbuildInfo(&postbuildDesc, static_cast<UINT>(addresses.size()), addresses.data());
        }
    }

    void CommandBufferD3D12::copyAccelerationStructure(AccelerationStructure& src, AccelerationStructure& dst, bool compact)
    {
        m_cmdList->CopyRaytracingAccelerationStructure(
            dst.getDeviceAddress(), src.getDeviceAddress(),
            compact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
    }

    void CommandBufferD3D12::executionBarrier()
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        m_cmdList->ResourceBarrier(1, &b);
    }

    void CommandBufferD3D12::transferBarrier()
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        m_cmdList->ResourceBarrier(1, &b);
    }

    void CommandBufferD3D12::resourceBarriers(std::span<const TextureBarrierDesc> textures, std::span<const BufferBarrierDesc> buffers)
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const auto& tb : textures)
        {
            if (tb.texture)
            {
                auto* texD3D12 = static_cast<TextureD3D12*>(tb.texture);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = texD3D12->getNativeResource();
                barriers.push_back(b);
            }
        }
        for (const auto& bb : buffers)
        {
            if (bb.buffer)
            {
                auto* bufD3D12 = static_cast<BufferD3D12*>(bb.buffer);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = bufD3D12->getNativeBuffer();
                barriers.push_back(b);
            }
        }

        if (barriers.empty())
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = nullptr;
            barriers.push_back(b);
        }

        m_cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    void CommandBufferD3D12::beginDebugLabel(const char* label)
    {
    }

    void CommandBufferD3D12::endDebugLabel()
    {
    }
}
