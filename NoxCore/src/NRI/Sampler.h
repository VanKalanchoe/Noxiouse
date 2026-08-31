#pragma once
#include <cstdint>
#include "NRITypes.h"

namespace NRI
{
    enum class Filter : uint8_t
    {
        Nearest = 0,
        Linear
    };

    enum class SamplerMipmapMode : uint8_t
    {
        Nearest = 0,
        Linear
    };

    enum class SamplerAddressMode : uint8_t
    {
        Repeat = 0,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
        MirrorClampToEdge
    };
    
    enum class BorderColor : uint8_t
    {
        FloatTransparentBlack = 0,
        IntTransparentBlack,
        FloatOpaqueBlack,
        IntOpaqueBlack,
        FloatOpaqueWhite,
        IntOpaqueWhite,
    };

    struct SamplerDesc
    {
        Filter magFilter = Filter::Nearest;
        Filter minFilter = Filter::Nearest;
        SamplerMipmapMode mipmapMode = SamplerMipmapMode::Nearest;
        SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
        SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
        SamplerAddressMode addressModeW = SamplerAddressMode::Repeat;
        float mipLodBias = 0.0f;
        bool anisotropyEnable = false;
        float maxAnisotropy = 0.0f;
        bool compareEnable = false;
        CompareOp compareOp = CompareOp::Never;
        float minLod = 0.0f;
        float maxLod = 0.0f; // vk::LodClampNone equivalent is 1000.0f
        BorderColor borderColor = BorderColor::FloatTransparentBlack;
    };
}