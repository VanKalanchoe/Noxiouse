#include "TextureD3D12.h"
#include "DeviceD3D12.h"
#include "BufferD3D12.h"
#include "CommandBufferD3D12.h"

namespace NRI
{
    DXGI_FORMAT TranslateImageFormat(ImageFormat format)
    {
        switch (format)
        {
        case ImageFormat::None: return DXGI_FORMAT_UNKNOWN;
        case ImageFormat::Surface: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case ImageFormat::RGB8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case ImageFormat::SRGB8: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case ImageFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case ImageFormat::SRGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case ImageFormat::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
        case ImageFormat::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
        case ImageFormat::R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case ImageFormat::R16G16: return DXGI_FORMAT_R16G16_UNORM;
        case ImageFormat::R16G16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case ImageFormat::R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case ImageFormat::R32G32B32A32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case ImageFormat::R32G32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case ImageFormat::R32SINT: return DXGI_FORMAT_R32_SINT;
        case ImageFormat::R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case ImageFormat::R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;

        case ImageFormat::BC1_UNorm: return DXGI_FORMAT_BC1_UNORM;
        case ImageFormat::BC1_UNorm_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case ImageFormat::BC2_UNorm: return DXGI_FORMAT_BC2_UNORM;
        case ImageFormat::BC2_UNorm_SRGB: return DXGI_FORMAT_BC2_UNORM_SRGB;
        case ImageFormat::BC3_UNorm: return DXGI_FORMAT_BC3_UNORM;
        case ImageFormat::BC3_UNorm_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case ImageFormat::BC4_UNorm: return DXGI_FORMAT_BC4_UNORM;
        case ImageFormat::BC4_SNorm: return DXGI_FORMAT_BC4_SNORM;
        case ImageFormat::BC5_UNorm: return DXGI_FORMAT_BC5_UNORM;
        case ImageFormat::BC5_SNorm: return DXGI_FORMAT_BC5_SNORM;
        case ImageFormat::BC6H_UF16: return DXGI_FORMAT_BC6H_UF16;
        case ImageFormat::BC6H_SF16: return DXGI_FORMAT_BC6H_SF16;
        case ImageFormat::BC7_UNorm: return DXGI_FORMAT_BC7_UNORM;
        case ImageFormat::BC7_UNorm_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    TextureD3D12::TextureD3D12(DeviceD3D12& device, const TextureDesc& desc)
        : m_deviceD3D12(device), m_desc(desc), m_ownsResource(true)
    {
        m_dxgiFormat = TranslateImageFormat(desc.format);
        createResource();
        m_deviceD3D12.addTextureBytes(static_cast<int64_t>(estimateTextureBytes(m_desc)));
    }

    TextureD3D12::TextureD3D12(DeviceD3D12& device, ComPtr<ID3D12Resource> resource, const TextureDesc& desc)
        : m_deviceD3D12(device), m_desc(desc), m_resource(std::move(resource)), m_ownsResource(false)
    {
        m_dxgiFormat = TranslateImageFormat(desc.format);
    }

    TextureD3D12::~TextureD3D12()
    {
        if (m_ownsResource)
        {
            m_deviceD3D12.addTextureBytes(-static_cast<int64_t>(estimateTextureBytes(m_desc)));
        }
    }

    void TextureD3D12::createResource()
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Alignment = 0;
        resDesc.Width = m_desc.width;
        resDesc.Height = m_desc.height;
        resDesc.DepthOrArraySize = static_cast<UINT16>(m_desc.isCubeMap ? 6 * m_desc.arrayLayers : m_desc.arrayLayers);
        resDesc.MipLevels = static_cast<UINT16>(m_desc.mipLevels);
        resDesc.Format = m_dxgiFormat;
        resDesc.SampleDesc.Count = m_desc.sampleCount;
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = m_dxgiFormat;
        D3D12_CLEAR_VALUE* pClearValue = nullptr;

        if (m_desc.usage == TextureUsage::ColorAttachment || m_desc.usage == TextureUsage::ColorResolveAttachment)
        {
            resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            clearValue.Color[0] = 0.0f;
            clearValue.Color[1] = 0.0f;
            clearValue.Color[2] = 0.0f;
            clearValue.Color[3] = 0.0f;
            pClearValue = &clearValue;
        }
        else if (m_desc.usage == TextureUsage::DepthStencilAttachment)
        {
            resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            clearValue.DepthStencil.Depth = 0.0f; // Reverse-Z default clear
            clearValue.DepthStencil.Stencil = 0;
            pClearValue = &clearValue;
        }
        else if (m_desc.usage == TextureUsage::Storage)
        {
            resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        m_image = m_deviceD3D12.getAllocator()->createImage(resDesc, initialState, pClearValue);
        m_resource = m_image.resource;
    }

    void TextureD3D12::uploadFromBuffer(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint32_t width, uint32_t height,
                                        uint32_t mipLevels, const std::vector<size_t>& mipOffsets)
    {
        recordUpload(cmdBuffer, stagingBuffer, 0, mipOffsets);
    }

    void TextureD3D12::recordUpload(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, uint64_t stagingOffset,
                                    const std::vector<size_t>& mipOffsets)
    {
        auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(&cmdBuffer);
        auto* bufferD3D12 = dynamic_cast<BufferD3D12*>(&stagingBuffer);
        if (!cmdD3D12 || !bufferD3D12) return;

        auto* list = cmdD3D12->getNativeCommandList();

        // Copy each mip level
        uint32_t curWidth = m_desc.width;
        uint32_t curHeight = m_desc.height;
        uint32_t blockBytes = 4, blockSize = 1;
        imageFormatBlock(m_desc.format, blockBytes, blockSize);

        for (uint32_t mip = 0; mip < mipOffsets.size() && mip < m_desc.mipLevels; ++mip)
        {
            uint32_t blocksX = (std::max(curWidth, 1u) + blockSize - 1) / blockSize;
            uint32_t blocksY = (std::max(curHeight, 1u) + blockSize - 1) / blockSize;
            uint32_t rowPitch = (blocksX * blockBytes + 255) & ~255u; // 256-byte pitch alignment

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = bufferD3D12->getNativeBuffer();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = stagingOffset + mipOffsets[mip];
            src.PlacedFootprint.Footprint.Format = m_dxgiFormat;
            src.PlacedFootprint.Footprint.Width = curWidth;
            src.PlacedFootprint.Footprint.Height = curHeight;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = rowPitch;

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = m_resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = mip;

            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            curWidth = std::max(1u, curWidth / 2);
            curHeight = std::max(1u, curHeight / 2);
        }
    }

    void TextureD3D12::recordInitialLayout(CommandBuffer& cmdBuffer)
    {
        // No-op in D3D12 with COMMON initial layout
    }

    void TextureD3D12::recordRegionUploads(CommandBuffer& cmdBuffer, Buffer& stagingBuffer, std::span<const TextureRegionUpload> regions)
    {
        auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(&cmdBuffer);
        auto* bufferD3D12 = dynamic_cast<BufferD3D12*>(&stagingBuffer);
        if (!cmdD3D12 || !bufferD3D12) return;

        auto* list = cmdD3D12->getNativeCommandList();

        uint32_t blockBytes = 4, blockSize = 1;
        imageFormatBlock(m_desc.format, blockBytes, blockSize);

        for (const auto& reg : regions)
        {
            uint32_t blocksX = (reg.width + blockSize - 1) / blockSize;
            uint32_t rowPitch = (blocksX * blockBytes + 255) & ~255u;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = bufferD3D12->getNativeBuffer();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = reg.stagingOffset;
            src.PlacedFootprint.Footprint.Format = m_dxgiFormat;
            src.PlacedFootprint.Footprint.Width = reg.width;
            src.PlacedFootprint.Footprint.Height = reg.height;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = rowPitch;

            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = m_resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            list->CopyTextureRegion(&dst, reg.x, reg.y, 0, &src, nullptr);
        }
    }

    void TextureD3D12::copyImageToBuffer(CommandBuffer& commandBuffer, Buffer& dstBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(&commandBuffer);
        auto* bufferD3D12 = dynamic_cast<BufferD3D12*>(&dstBuffer);
        if (!cmdD3D12 || !bufferD3D12) return;

        auto* list = cmdD3D12->getNativeCommandList();

        D3D12_BOX srcBox{ x, y, 0, x + width, y + height, 1 };

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = m_resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        uint32_t blockBytes = 4, blockSize = 1;
        imageFormatBlock(m_desc.format, blockBytes, blockSize);
        uint32_t rowPitch = (width * blockBytes + 255) & ~255u;

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = bufferD3D12->getNativeBuffer();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = m_dxgiFormat;
        dst.PlacedFootprint.Footprint.Width = width;
        dst.PlacedFootprint.Footprint.Height = height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;

        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);
    }

    void TextureD3D12::blitTo(CommandBuffer& commandBuffer, Texture2D& dst)
    {
        // Blit implemented via copy or pass
    }

    void TextureD3D12::generateMipmaps(CommandBuffer& commandBuffer)
    {
        // Mips in Noxiouse are generated via compute pass
    }

    ImTextureID TextureD3D12::getImTextureID()
    {
        // In ImGui D3D12 backend, ImTextureID is D3D12_GPU_DESCRIPTOR_HANDLE.ptr or descriptor slot
        return reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(m_descriptorSlot));
    }
}
