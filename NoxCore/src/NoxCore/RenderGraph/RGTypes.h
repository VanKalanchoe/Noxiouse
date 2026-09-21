#pragma once
#include <array>
#include <cstdint>
#include <type_traits>

#include "NRI/Buffer.h"
#include "NRI/Texture.h"

namespace Nox
{
    constexpr uint32_t RGInvalidIndex = ~0u;

    // Typed handles to graph resources (Decision D2). Valid only for the frame they were created in: Frame identifies that
    // graph frame, so validation catches a handle kept from an earlier frame.
    struct RGTexture
    {
        uint32_t Index = RGInvalidIndex;
        uint32_t Frame = 0;
        bool IsValid() const { return Index != RGInvalidIndex; }
    };

    struct RGBuffer
    {
        uint32_t Index = RGInvalidIndex;
        uint32_t Frame = 0;
        bool IsValid() const { return Index != RGInvalidIndex; }
    };

    enum class RGResourceKind : uint8_t
    {
        Imported,
        Transient,
        History
    };

    // Imported resources the graph's passes may only read (validation reports writes).
    enum class RGImportAccess : uint8_t
    {
        ReadWrite,
        ReadOnly
    };

    // Size of a graph texture: follows the render resolution (what DLSS upscales from), the output resolution (editor
    // viewport / swapchain), or is absolute. Resize and upscale-mode changes only change the resolved size.
    enum class RGSize : uint8_t
    {
        RenderResolution,
        OutputResolution,
        Absolute
    };

    struct RGTextureDesc
    {
        RGSize Size = RGSize::RenderResolution;
        uint32_t Width = 0;  // Absolute only
        uint32_t Height = 0; // Absolute only
        NRI::ImageFormat Format = NRI::ImageFormat::None;
        // Storage usage also registers a storage slot per mip (RGPassContext::StorageSlot).
        NRI::TextureUsage Usage = NRI::TextureUsage::ColorAttachment;
        uint32_t MipLevels = 1;
    };

    struct RGBufferDesc
    {
        uint64_t Size = 0;
        NRI::BufferUsage Usage = NRI::BufferUsage::Storage;
    };

    constexpr uint32_t RGMaxHistoryLength = 4;

    // Persistent resources kept by the graph across frames under one name (§5.4.3). The feature decides which element is
    // "current" (ping-pong, RTXDI buffer rotation); the graph owns lifetime, recreation on description change and
    // resets. WasReset is true in the frame the history was (re)created or reset: contents are undefined/zero.
    struct RGTextureHistory
    {
        std::array<RGTexture, RGMaxHistoryLength> Textures{};
        uint32_t Count = 0;
        bool WasReset = false;
    };

    struct RGBufferHistory
    {
        std::array<RGBuffer, RGMaxHistoryLength> Buffers{};
        uint32_t Count = 0;
        bool WasReset = false;
    };

    // How a pass uses a texture (§5.4.4). Layouts are not tracked: with VK_KHR_unified_image_layouts every image
    // stays in General, so access drives ordering, culling, validation and synchronization only.
    enum class RGTextureAccess : uint8_t
    {
        Sampled,
        StorageRead,
        StorageWrite,
        ColorTarget,
        DepthTarget,
        CopySource,
        CopyDestination
    };

    enum class RGBufferAccess : uint8_t
    {
        Read,        // storage/uniform read through a buffer device address
        IndirectRead, // draw arguments and draw counts, fetched by the indirect stage
        Write, // storage write
        AccelerationStructureBuild,
        AccelerationStructureBuildInput,
        AccelerationStructureRead,
        CopySource,
        CopyDestination // e.g. image -> staging buffer readback
    };

    // How the graph synchronizes consecutive passes (§5.4.5, Decision D13).
    enum class RGSynchronization : uint8_t
    {
        Blanket, // one memory barrier over all resources after a pass whose writes a later pass uses
        Precise  // per-resource image/buffer barriers from the previous to the new access, before the consuming pass
    };

    enum class RGPassFlags : uint32_t
    {
        None = 0,
        // The graph begins/ends rendering with the pass's declared color/depth targets.
        Raster = 1 << 0,
        // Kept even when nothing consumes its outputs (presentation, readbacks, CPU-visible side effects).
        NeverCull = 1 << 1,
        RayTracing = 1 << 2
    };

    inline RGPassFlags operator|(RGPassFlags a, RGPassFlags b)
    {
        return static_cast<RGPassFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline bool HasFlag(RGPassFlags flags, RGPassFlags flag)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
    }

    // Viewport set by the graph for Raster passes.
    enum class RGViewport : uint8_t
    {
        FlippedY, // {0, h, w, -h}: the engine's convention for 3D and 2D passes
        None      // the pass sets its own viewport
    };

    inline bool IsWrite(RGTextureAccess access)
    {
        return access == RGTextureAccess::StorageWrite || access == RGTextureAccess::ColorTarget ||
               access == RGTextureAccess::DepthTarget || access == RGTextureAccess::CopyDestination;
    }

    inline bool IsWrite(RGBufferAccess access)
    {
        return access == RGBufferAccess::Write || access == RGBufferAccess::AccelerationStructureBuild || access == RGBufferAccess::CopyDestination;
    }
}
