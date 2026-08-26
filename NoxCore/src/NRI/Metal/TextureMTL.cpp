#include "TextureMTL.h"

#include "imgui_impl_metal4.h"

#include "DeviceMTL.h"
#include "CommandBufferMTL.h"
#include "DescriptorHeapMTL.h"

namespace NRI
{

static MTL::PixelFormat translateFormat(
                                        const TextureDesc& desc)
{
    if (desc.directFormat != UINT32_MAX)
    {
        return static_cast<MTL::PixelFormat>(
                                             desc.directFormat);
    }
    
    switch (desc.format)
    {
        case ImageFormat::RGBA8:
            return MTL::PixelFormat::PixelFormatRGBA8Unorm;
            
        case ImageFormat::SRGBA8:
            return MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB;
            
        case ImageFormat::RGB8:
            // Metal does not have RGB8 as a normal texture
            // format. You normally need RGBA8.
            return MTL::PixelFormat::PixelFormatRGBA8Unorm;
            
        case ImageFormat::SRGB8:
            return MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB;
            
        case ImageFormat::R16G16:
            return MTL::PixelFormat::PixelFormatRG16Float;
            
        case ImageFormat::R32SINT:
            return MTL::PixelFormat::PixelFormatR32Sint;
            
        case ImageFormat::R32G32B32A32_SFLOAT:
            return MTL::PixelFormat::PixelFormatRGBA32Float;
            
        case ImageFormat::BC7_UNorm:
            return MTL::PixelFormat::PixelFormatBC7_RGBAUnorm;
            
        case ImageFormat::BC7_UNorm_SRGB:
            return MTL::PixelFormat::PixelFormatBC7_RGBAUnorm_sRGB;
            
        default:
            throw std::runtime_error(
                                     "Unsupported ImageFormat for Metal");
    }
}

TextureMTL::TextureMTL(
    DeviceMTL& device,
    const TextureDesc& desc)
    : m_deviceMTL(device)
    , m_desc(desc)
{
    m_pixelFormat = translateFormat(desc);

    if (desc.usage == TextureUsage::ShaderResource)
    {
        // Created later by DescriptorHeapMTL::registerTexture().
        return;
    }

    MTL::TextureDescriptor* descriptor =
        MTL::TextureDescriptor::alloc()->init();

    descriptor->setTextureType(
        desc.sampleCount > 1
            ? MTL::TextureType::TextureType2DMultisample
            : MTL::TextureType::TextureType2D);

    descriptor->setPixelFormat(m_pixelFormat);
    descriptor->setWidth(desc.width);
    descriptor->setHeight(desc.height);

    descriptor->setMipmapLevelCount(
        desc.sampleCount > 1 ? 1 : desc.mipLevels);

    descriptor->setSampleCount(desc.sampleCount);

    MTL::TextureUsage usage = MTL::TextureUsageUnknown;

    switch (desc.usage)
    {
    case TextureUsage::ColorAttachment:
        usage = MTL::TextureUsageRenderTarget;
        break;

    case TextureUsage::DepthStencilAttachment:
        usage = MTL::TextureUsageRenderTarget;
        break;

    case TextureUsage::ColorResolveAttachment:
        usage =
            MTL::TextureUsageRenderTarget |
            MTL::TextureUsageShaderRead;
        break;

    default:
        break;
    }

    descriptor->setUsage(usage);
    descriptor->setStorageMode(
        MTL::StorageMode::StorageModePrivate);

    m_texture =
        m_deviceMTL.getDevice()
            ->newTexture(descriptor);

    descriptor->release();

    if (!m_texture)
        throw std::runtime_error(
            "Failed to create Metal texture");

    m_deviceMTL.getResidencySet()
        ->addAllocation(m_texture);

    m_deviceMTL.getResidencySet()->commit();
}

TextureMTL::~TextureMTL()
{
    if (m_deviceMTL.getResidencySet() &&
        m_texture)
    {
        m_deviceMTL.getResidencySet()
            ->removeAllocation(m_texture);

        m_deviceMTL.getResidencySet()->commit();
    }

    if (m_texture)
    {
        m_texture->release();
        m_texture = nullptr;
    }
}

ImTextureID TextureMTL::getImTextureID()
{
    return static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(m_texture));
}
}
