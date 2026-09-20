#include "TextureImporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define TINYDDSLOADER_IMPLEMENTATION
#include "NoxCore/Renderer/tinyddsloader.h"

#include <ktx.h>

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Core/Buffer.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Utils/Utils.h"
#include "NoxCore/Tasks/JobSystem.h"

#include <bc7enc.h>

namespace Nox
{
    static std::filesystem::path GetCookedTexturePath(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        std::filesystem::path path = assetDirectory / metadata.FilePath;
        if (path.extension() != ".ntex")
            path.replace_extension(".ntex");
        return path;
    }

    static std::filesystem::path GetTextureSourcePath(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        return assetDirectory / (metadata.SourceFilePath.empty() ? metadata.FilePath : metadata.SourceFilePath);
    }

    static bool IsCookedTextureSource(const std::filesystem::path& sourcePath)
    {
        return sourcePath.extension() == ".ntex";
    }

    // Sources cooked into .ntex on import.
    static bool IsCookableTextureSource(const std::filesystem::path& sourcePath)
    {
        const std::filesystem::path extension = sourcePath.extension();
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".dds" || extension == ".ktx2";
    }

    // Identifies what a cooked texture was made from: the source content and the import settings.
    static XXH128_hash_t TextureCookHash(const std::filesystem::path& sourcePath, const TextureSpecification& spec)
    {
        struct TextureCookSettings
        {
            XXH128_hash_t source;
            uint8_t flip;
            uint8_t generateMips;
            uint16_t format;
        };
        TextureCookSettings cookSettings{};
        cookSettings.source = Utility::calcul_hash_streaming(sourcePath.string());
        cookSettings.flip = static_cast<uint8_t>(spec.flip);
        cookSettings.generateMips = static_cast<uint8_t>(spec.generateMips);
        cookSettings.format = static_cast<uint16_t>(spec.format);
        return XXH3_128bits(&cookSettings, sizeof(cookSettings));
    }

    // An RGBA8 mip chain to BC7 (1 byte per texel instead of 4): every mip encoded by bc7enc, blocks spread over the
    // workers (a 2048² texture is 262 k blocks). Perceptual weights for color, linear ones for data (normals,
    // roughness/metalness, occlusion). Edge texels are repeated into the blocks of mips smaller than 4x4 or not a multiple.
    static void EncodeBC7(TextureData& cpuData)
    {
        static std::once_flag s_EncoderInit;
        std::call_once(s_EncoderInit, [] { bc7enc_compress_block_init(); });

        const bool srgb = cpuData.Format == NRI::ImageFormat::SRGBA8;
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        if (!srgb)
            bc7enc_compress_block_params_init_linear_weights(&params);

        std::vector<size_t> offsets;
        size_t total = 0;
        for (uint32_t mip = 0; mip < cpuData.MipLevels; ++mip)
        {
            const uint32_t blocksX = (std::max(cpuData.Width >> mip, 1u) + 3) / 4;
            const uint32_t blocksY = (std::max(cpuData.Height >> mip, 1u) + 3) / 4;
            offsets.push_back(total);
            total += static_cast<size_t>(blocksX) * blocksY * BC7ENC_BLOCK_SIZE;
        }

        Buffer encoded(total);
        for (uint32_t mip = 0; mip < cpuData.MipLevels; ++mip)
        {
            const uint32_t width = std::max(cpuData.Width >> mip, 1u);
            const uint32_t height = std::max(cpuData.Height >> mip, 1u);
            const uint32_t blocksX = (width + 3) / 4;
            const uint32_t blocksY = (height + 3) / 4;
            const uint8_t* source = cpuData.Data.Data + cpuData.MipOffsets[mip];
            uint8_t* destination = encoded.Data + offsets[mip];

            JobSystem::Get().ParallelFor("Encode BC7", blocksY, 4, [&](uint32_t, uint32_t begin, uint32_t end)
            {
                color_rgba block[16];
                for (uint32_t blockY = begin; blockY < end; ++blockY)
                {
                    for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
                    {
                        for (uint32_t texel = 0; texel < 16; ++texel)
                        {
                            const uint32_t x = std::min(blockX * 4 + texel % 4, width - 1);
                            const uint32_t y = std::min(blockY * 4 + texel / 4, height - 1);
                            memcpy(block[texel].m_c, source + (static_cast<size_t>(y) * width + x) * 4, 4);
                        }
                        bc7enc_compress_block(destination + (static_cast<size_t>(blockY) * blocksX + blockX) * BC7ENC_BLOCK_SIZE, block, &params);
                    }
                }
            });
        }

        cpuData.Data.Release();
        cpuData.Data = encoded;
        cpuData.MipOffsets = std::move(offsets);
        cpuData.Format = srgb ? NRI::ImageFormat::BC7_UNorm_SRGB : NRI::ImageFormat::BC7_UNorm;
    }

    static void GenerateRGBA8MipChain(TextureData& cpuData)
    {
        if (cpuData.MipLevels <= 1 || cpuData.Data.Size == 0)
        {
            cpuData.MipOffsets = { 0 };
            return;
        }

        std::vector<uint8_t> mipData;
        mipData.reserve(cpuData.Data.Size + cpuData.Data.Size / 3);
        mipData.insert(mipData.end(), cpuData.Data.Data, cpuData.Data.Data + cpuData.Data.Size);

        std::vector<uint8_t> previous(cpuData.Data.Data, cpuData.Data.Data + cpuData.Data.Size);
        uint32_t previousWidth = cpuData.Width;
        uint32_t previousHeight = cpuData.Height;

        cpuData.MipOffsets.clear();
        cpuData.MipOffsets.push_back(0);

        for (uint32_t level = 1; level < cpuData.MipLevels; level++)
        {
            uint32_t width = std::max(previousWidth / 2, 1u);
            uint32_t height = std::max(previousHeight / 2, 1u);
            std::vector<uint8_t> current(static_cast<size_t>(width) * height * 4);

            for (uint32_t y = 0; y < height; y++)
            {
                for (uint32_t x = 0; x < width; x++)
                {
                    uint32_t srcX = x * 2;
                    uint32_t srcY = y * 2;
                    uint32_t count = 0;
                    uint32_t rgba[4] = {};

                    for (uint32_t oy = 0; oy < 2; oy++)
                    {
                        for (uint32_t ox = 0; ox < 2; ox++)
                        {
                            uint32_t sampleX = std::min(srcX + ox, previousWidth - 1);
                            uint32_t sampleY = std::min(srcY + oy, previousHeight - 1);
                            const uint8_t* src = &previous[(static_cast<size_t>(sampleY) * previousWidth + sampleX) * 4];
                            rgba[0] += src[0];
                            rgba[1] += src[1];
                            rgba[2] += src[2];
                            rgba[3] += src[3];
                            count++;
                        }
                    }

                    uint8_t* dst = &current[(static_cast<size_t>(y) * width + x) * 4];
                    dst[0] = static_cast<uint8_t>(rgba[0] / count);
                    dst[1] = static_cast<uint8_t>(rgba[1] / count);
                    dst[2] = static_cast<uint8_t>(rgba[2] / count);
                    dst[3] = static_cast<uint8_t>(rgba[3] / count);
                }
            }

            cpuData.MipOffsets.push_back(mipData.size());
            mipData.insert(mipData.end(), current.begin(), current.end());
            previous = std::move(current);
            previousWidth = width;
            previousHeight = height;
        }

        cpuData.Data.Release();
        cpuData.Data.Allocate(mipData.size());
        memcpy(cpuData.Data.Data, mipData.data(), mipData.size());
    }

    std::optional<CookedTextureHeader> TextureImporter::ReadCookedTextureHeader(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        const std::filesystem::path cookedPath = GetCookedTexturePath(assetDirectory, metadata);
        const std::filesystem::path sourcePath = GetTextureSourcePath(assetDirectory, metadata);

        // Same test as ImportTexture2D: a cooked source is its own cook, anything else must match the recorded hash.
        if (!IsCookedTextureSource(sourcePath))
        {
            XXH128_hash_t cookedHash{};
            if (!std::filesystem::exists(sourcePath) || !Utility::loadHashFromFile(cookedPath.string() + ".hash", cookedHash) ||
                !XXH128_isEqual(TextureCookHash(sourcePath, metadata.TextureSpec), cookedHash))
            {
                return std::nullopt;
            }
        }

        std::ifstream stream(cookedPath, std::ios::binary);
        if (!stream.is_open())
            return std::nullopt;

        CookedTextureHeader header;
        header.Path = cookedPath;
        if (!ReadNTEXHeader(stream, header.Texture, header.DataSize))
            return std::nullopt;
        header.DataOffset = static_cast<uint64_t>(stream.tellg());

        // Streamed textures are plain copies of one 2D layer with a complete mip chain (UploadTexture's transfer path).
        const TextureData& texture = header.Texture;
        if (texture.ArrayLayers != 1 || texture.IsCubeMap || (texture.MipLevels > 1 && texture.MipOffsets.size() != texture.MipLevels))
            return std::nullopt;
        return header;
    }

    bool TextureImporter::ReadCookedTextureMips(const CookedTextureHeader& header, uint32_t firstMip, uint32_t mipCount, uint8_t* destination)
    {
        // Mips are stored largest first: a run of them is one range of the file.
        const std::vector<size_t>& offsets = header.Texture.MipOffsets;
        const uint32_t endMip = firstMip + mipCount;
        const uint64_t begin = offsets.empty() ? 0 : offsets[firstMip];
        const uint64_t end = endMip < offsets.size() ? offsets[endMip] : header.DataSize;

        std::ifstream stream(header.Path, std::ios::binary);
        if (!stream.is_open())
            return false;
        stream.seekg(static_cast<std::streamoff>(header.DataOffset + begin));
        stream.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(end - begin));
        return !stream.fail();
    }

    Ref<Texture2D> TextureImporter::ImportTexture2D(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path cookedPath = GetCookedTexturePath(assetDirectory, metadata);
        std::filesystem::path sourcePath = GetTextureSourcePath(assetDirectory, metadata);

        const bool sourceIsCooked = IsCookedTextureSource(sourcePath);
        const auto hashPath = cookedPath.string() + ".hash";
        const XXH128_hash_t sourceHash = sourceIsCooked ? XXH128_hash_t{} : TextureCookHash(sourcePath, metadata.TextureSpec);
        XXH128_hash_t cookedHash{};
        const bool cookedIsCurrent = sourceIsCooked ||
            (std::filesystem::exists(sourcePath) &&
             (Utility::loadHashFromFile(hashPath, cookedHash) &&
              XXH128_isEqual(sourceHash, cookedHash)));

        if (std::filesystem::exists(cookedPath) && cookedIsCurrent)
            return LoadTexture2D(cookedPath, metadata.TextureSpec);

        // Other formats (e.g. .hdr) load from the source every time.
        if (!IsCookableTextureSource(sourcePath))
            return LoadTexture2D(sourcePath, metadata.TextureSpec);

        if (!CookTexture(assetDirectory, metadata))
            return Ref<Texture2D>(nullptr);
        return LoadTexture2D(cookedPath, metadata.TextureSpec);
    }

    bool TextureImporter::CookTexture(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        const std::filesystem::path cookedPath = GetCookedTexturePath(assetDirectory, metadata);
        const std::filesystem::path sourcePath = GetTextureSourcePath(assetDirectory, metadata);
        if (!IsCookableTextureSource(sourcePath))
            return false;

        // Cook once into the GPU-ready .ntex: PNG/JPG get their mip chain generated here, DDS and KTX2 keep the mips
        // and block compression they ship with (a Basis KTX2 is transcoded here, not on every load).
        TextureData cpuData{};
        bool decoded = false;
        if (sourcePath.extension() == ".dds")
            decoded = DecodeDDS(sourcePath, cpuData);
        else if (sourcePath.extension() == ".ktx2")
            decoded = DecodeKTX(sourcePath, metadata.TextureSpec, cpuData);
        else
            decoded = DecodeSTB(sourcePath, metadata.TextureSpec, cpuData);
        if (!decoded)
            return false;
        // PNG/JPG decode to RGBA8 with generated mips; DDS and KTX2 keep the format they ship in.
        if (cpuData.Format == NRI::ImageFormat::SRGBA8 || cpuData.Format == NRI::ImageFormat::RGBA8)
            EncodeBC7(cpuData);

        std::error_code error;
        std::filesystem::create_directories(cookedPath.parent_path(), error);
        const bool saved = SaveNTEX(cookedPath, cpuData);
        cpuData.Data.Release();
        if (!saved)
        {
            NOX_CORE_ERROR("TextureImporter::CookTexture - could not write {}", cookedPath.string());
            return false;
        }
        // Last: a crash before this leaves no hash, so the texture is cooked again.
        Utility::saveHashToFile(cookedPath.string() + ".hash", TextureCookHash(sourcePath, metadata.TextureSpec));
        return true;
    }

    bool TextureImporter::DecodeSTB(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData)
    {
        stbi_set_flip_vertically_on_load_thread(spec.flip);

        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path.string().c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels)
        {
            NOX_CORE_ERROR("TextureImporter::DecodeSTB - Could not load texture from filepath: {}", path.string());
            return false;
        }

        cpuData.Width = texWidth;
        cpuData.Height = texHeight;
        cpuData.MipLevels = spec.generateMips
                                ? static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1
                                : 1;
        cpuData.Data = Buffer::Copy(Buffer(pixels, static_cast<uint64_t>(texWidth) * texHeight * 4));
        cpuData.Format = spec.format;
        cpuData.MipOffsets = { 0 };

        stbi_image_free(pixels);

        GenerateRGBA8MipChain(cpuData);
        return true;
    }

    Ref<Texture2D> TextureImporter::LoadTexture2D(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        std::filesystem::path absolutePath = std::filesystem::absolute(path);
        
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("TextureImporter::ImportTexture2D - file not found from filepath: {}", absolutePath.string());
            return Ref<Texture2D>(nullptr);
        }
        
        if (path.extension() == ".ntex")
            return LoadWithNTEX(path, renderer);

        if (path.extension() == ".png" || path.extension() == ".jpg" || path.extension() == ".jpeg")
            return LoadWithSTB(path, spec, renderer);
        
        if (path.extension() == ".hdr")
            return LoadWithSTBHDR(path, spec, renderer);
        
        if (path.extension() == ".dds")
            return LoadWithDDS(path, spec, renderer);
        
        return LoadWithKTX(path, spec, renderer);
    }
    
    Ref<Texture2D> TextureImporter::LoadWithSTB(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        if (spec.flip)
            stbi_set_flip_vertically_on_load_thread(true);
        else
            stbi_set_flip_vertically_on_load_thread(false);
        
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path.string().c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels)
        {
            NOX_CORE_ERROR("TextureImporter::LoadWithSTB - Could not load texture from filepath: {}", path.string());
            return Ref<Texture2D>(nullptr);
        }

        uint64_t imageSize = static_cast<uint64_t>(texWidth) * texHeight * 4;
        
        uint32_t mipLevels;
        if (spec.generateMips)
            mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;
        else
            mipLevels = 1;
        
        TextureData cpuData{};
        cpuData.Width = texWidth;
        cpuData.Height = texHeight;
        cpuData.MipLevels = mipLevels;
        cpuData.Data = Buffer::Copy(Buffer(pixels, imageSize));
        cpuData.Format = spec.format;
        cpuData.MipOffsets = { 0 };
        GenerateRGBA8MipChain(cpuData);
        
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        cpuData.Data.Release();
        
        stbi_image_free(pixels);
        
        return texture;
    }
    
    Ref<Texture2D> TextureImporter::LoadWithSTBHDR(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        if (spec.flip)
            stbi_set_flip_vertically_on_load_thread(true);
        else
            stbi_set_flip_vertically_on_load_thread(false);
        
        int texWidth, texHeight, texChannels;
        float* pixels = stbi_loadf(path.string().c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels)
        {
            NOX_CORE_ERROR("TextureImporter::LoadWithSTBHDR - Could not load texture from filepath: {}", path.string());
            return Ref<Texture2D>(nullptr);
        }
        
        // Convert FP32 (16 bytes/pixel) to FP16 (8 bytes/pixel) for R16G16B16A16_SFLOAT
        uint64_t totalElements = static_cast<uint64_t>(texWidth) * texHeight * 4;
        uint64_t fp16SizeBytes = totalElements * sizeof(uint16_t);

        std::vector<uint16_t> fp16Data(totalElements);
        for (size_t i = 0; i < totalElements; i += 2)
        {
            glm::vec2 val(pixels[i], pixels[i + 1]);
            uint32_t packed = glm::packHalf2x16(val);
            fp16Data[i]     = static_cast<uint16_t>(packed & 0xFFFF);
            fp16Data[i + 1] = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
        }
        
        uint32_t mipLevels;
        if (spec.generateMips)
            mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;
        else
            mipLevels = 1;

        TextureData cpuData{};
        cpuData.Width = texWidth;
        cpuData.Height = texHeight;
        cpuData.MipLevels = mipLevels;
        cpuData.MipOffsets = { 0 };
        cpuData.Data = Buffer(fp16Data.data(), fp16SizeBytes);
        cpuData.Format = NRI::ImageFormat::R16G16B16A16_SFLOAT;
        
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        stbi_image_free(pixels);
        
        return texture;
    }
    
    Ref<Texture2D> TextureImporter::LoadWithDDS(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        TextureData cpuData{};
        if (!DecodeDDS(path, cpuData))
            return Ref<Texture2D>(nullptr);

        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        cpuData.Data.Release();
        return texture;
    }

    bool TextureImporter::DecodeDDS(const std::filesystem::path& path, TextureData& cpuData)
    {
        tinyddsloader::DDSFile dds;

        auto result = dds.Load(path.string().c_str());

        if (result != tinyddsloader::Result::Success)
        {
            NOX_CORE_ERROR("TextureImporter::DecodeDDS - Failed to load DDS from: {} Result: {}", path.string(), static_cast<int>(result));
            return false;
        }

        cpuData.Width = dds.GetWidth();
        cpuData.Height = dds.GetHeight();
        cpuData.MipLevels = dds.GetMipCount();
        cpuData.IsCubeMap = dds.IsCubemap();
        cpuData.ArrayLayers = dds.GetArraySize();

        switch (dds.GetFormat())
        {
        case tinyddsloader::DDSFile::DXGIFormat::BC1_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC1_UNorm:
            cpuData.Format = NRI::ImageFormat::BC1_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC1_UNorm_SRGB:
            cpuData.Format = NRI::ImageFormat::BC1_UNorm_SRGB;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC2_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC2_UNorm:
            cpuData.Format = NRI::ImageFormat::BC2_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC2_UNorm_SRGB:
            cpuData.Format = NRI::ImageFormat::BC2_UNorm_SRGB;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC3_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC3_UNorm:
            cpuData.Format = NRI::ImageFormat::BC3_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC3_UNorm_SRGB:
            cpuData.Format = NRI::ImageFormat::BC3_UNorm_SRGB;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC4_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC4_UNorm:
            cpuData.Format = NRI::ImageFormat::BC4_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC4_SNorm:
            cpuData.Format = NRI::ImageFormat::BC4_SNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC5_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC5_UNorm:
            cpuData.Format = NRI::ImageFormat::BC5_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC5_SNorm:
            cpuData.Format = NRI::ImageFormat::BC5_SNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC6H_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC6H_UF16:
            cpuData.Format = NRI::ImageFormat::BC6H_UF16;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC6H_SF16:
            cpuData.Format = NRI::ImageFormat::BC6H_SF16;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC7_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC7_UNorm:
            cpuData.Format = NRI::ImageFormat::BC7_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC7_UNorm_SRGB:
            cpuData.Format = NRI::ImageFormat::BC7_UNorm_SRGB;
            break;
        default:
            NOX_CORE_ERROR("TextureImporter::DecodeDDS - Unsupported DDS format {} from: {}", static_cast<uint32_t>(dds.GetFormat()), path.string());
            return false;
        }

        // Keep the payload mip-major, with all array/cubemap layers contiguous inside
        // each mip. TextureVK uses these offsets to emit one copy region per layer.
        size_t currentOffset = 0;
        cpuData.MipOffsets.resize(dds.GetMipCount());

        for (uint32_t level = 0; level < dds.GetMipCount(); level++)
        {
            cpuData.MipOffsets[level] = currentOffset;
            for (uint32_t layer = 0; layer < cpuData.ArrayLayers; ++layer)
            {
                const auto* imageData = dds.GetImageData(level, layer);
                currentOffset += imageData->m_memSlicePitch;
            }
        }

        // tinyddsloader stores subresources in layer-major order. Repack them into
        // mip-major order expected by TextureVK's upload path.
        Buffer packed(currentOffset);
        size_t packedOffset = 0;
        for (uint32_t level = 0; level < dds.GetMipCount(); ++level)
        {
            for (uint32_t layer = 0; layer < cpuData.ArrayLayers; ++layer)
            {
                const auto* imageData = dds.GetImageData(level, layer);
                memcpy(static_cast<uint8_t*>(packed.Data) + packedOffset, imageData->m_mem, imageData->m_memSlicePitch);
                packedOffset += imageData->m_memSlicePitch;
            }
        }
        cpuData.Data = std::move(packed);
        return true;
    }

    // Returns bytes-per-texel for plain (non-block-compressed) VkFormats we might see out of
    // a KTX2 file, or 0 if the format is block-compressed / unrecognized (unsafe to row-flip).
    static uint32_t UncompressedVkFormatTexelSize(uint32_t vkFormat)
    {
        switch (vkFormat)
        {
            case 9:   // VK_FORMAT_R8G8B8_UNORM
            case 15:  // VK_FORMAT_R8G8B8_SRGB
            case 16:  // VK_FORMAT_B8G8R8_UNORM
            case 22:  // VK_FORMAT_B8G8R8_SRGB
                return 3;
            case 37:  // VK_FORMAT_R8G8B8A8_UNORM
            case 43:  // VK_FORMAT_R8G8B8A8_SRGB
            case 44:  // VK_FORMAT_B8G8R8A8_UNORM
            case 50:  // VK_FORMAT_B8G8R8A8_SRGB
                return 4;
            case 84:  // VK_FORMAT_R16G16B16A16_UNORM
            case 91:  // VK_FORMAT_R16G16B16A16_SFLOAT
                return 8;
            case 109: // VK_FORMAT_R32G32B32A32_SFLOAT
                return 16;
            default:
                return 0;
        }
    }

    Ref<Texture2D> TextureImporter::LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        TextureData cpuData{};
        if (!DecodeKTX(path, spec, cpuData))
            return Ref<Texture2D>(nullptr);

        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        cpuData.Data.Release();
        return texture;
    }

    bool TextureImporter::DecodeKTX(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData)
    {
        ktxTexture2* kTexture;
        KTX_error_code result = ktxTexture2_CreateFromNamedFile
        (
            path.string().c_str(),
            KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
            &kTexture
        );

        if (result != KTX_SUCCESS)
        {
            NOX_CORE_ERROR("TextureImporter::DecodeKTX - failed to load ktx texture image: {}", path.string());
            return false;
        }

        // KTX2's default/assumed row order is top-down (Y=down), matching glTF/PNG/Vulkan - the
        // convention this engine assumes everywhere else. Some KTX2 encoders instead bake images
        // bottom-up (Y=up, the classic desktop-GL convention) and record that in the file's own
        // KTXorientation metadata, which libktx already parses into kTexture->orientation for us.
        // That mismatch is a property of the FILE, not something the per-asset spec.flip toggle
        // was meant to express, so correct it unconditionally; spec.flip stacks on top of it for
        // an artist who additionally wants the (now-correct) image flipped again.
        bool needsOrientationFix = (kTexture->orientation.y == KTX_ORIENT_Y_UP);
        bool needsFlip = (needsOrientationFix != spec.flip);

        if (ktxTexture2_NeedsTranscoding(kTexture))
        {
            // BC7 is block-compressed (4x4 texel blocks) - a plain row-reverse of the compressed
            // bytes would scramble the blocks instead of flipping the image, so when a flip is
            // required, transcode to plain RGBA32 instead so the row-flip below is safe. This
            // costs VRAM only for the textures that actually need correcting.
            ktx_transcode_fmt_e transcodeTarget = needsFlip ? KTX_TTF_RGBA32 : KTX_TTF_BC7_RGBA;
            result = ktxTexture2_TranscodeBasis(kTexture, transcodeTarget, 0);

            if (result != KTX_SUCCESS)
            {
                NOX_CORE_ERROR("TextureImporter::DecodeKTX - failed to transcode Basis texture: {}", path.string());
                ktxTexture2_Destroy(kTexture);
                return false;
            }
        }

        // Get texture dimensions and data
        cpuData.Width = kTexture->baseWidth;
        cpuData.Height = kTexture->baseHeight;
        cpuData.MipLevels = kTexture->numLevels; // todo:

        cpuData.MipOffsets.resize(kTexture->numLevels);
        for (uint32_t level = 0; level < kTexture->numLevels; level++)
        {
            ktx_size_t offset;
            ktxTexture2_GetImageOffset(kTexture, level, 0, 0, &offset);
            cpuData.MipOffsets[level] = offset;
        }

        // Check if the KTX texture has a format
        if (kTexture->classId == ktxTexture2_c)
        {
            // For KTX2 files, we can get the format directly
            cpuData.DirectFormat = kTexture->vkFormat;
        }
        else
        {
            uint32_t directFormat = UINT32_MAX;
            // For KTX1 files or if we can't determine the format, use a reasonable default
            cpuData.DirectFormat = directFormat;
        }

        if (needsFlip)
        {
            // Block-compressed formats (a natively block-compressed KTX2 that didn't go through
            // Basis transcoding above) can't be vertically flipped by reversing rows of bytes -
            // the texel order *inside* each 4x4 block would also need flipping, which needs a
            // real BC codec. Only flip when we know the data is a plain uncompressed format.
            uint32_t texelSize = UncompressedVkFormatTexelSize(cpuData.DirectFormat);
            if (texelSize > 0)
            {
                for (uint32_t level = 0; level < kTexture->numLevels; level++)
                {
                    uint32_t levelWidth = std::max(1u, kTexture->baseWidth >> level);
                    uint32_t levelHeight = std::max(1u, kTexture->baseHeight >> level);
                    uint8_t* levelData = kTexture->pData + cpuData.MipOffsets[level];
                    size_t rowBytes = static_cast<size_t>(levelWidth) * texelSize;

                    std::vector<uint8_t> rowTemp(rowBytes);
                    for (uint32_t y = 0; y < levelHeight / 2; y++)
                    {
                        uint8_t* rowTop = levelData + static_cast<size_t>(y) * rowBytes;
                        uint8_t* rowBottom = levelData + static_cast<size_t>(levelHeight - 1 - y) * rowBytes;
                        memcpy(rowTemp.data(), rowTop, rowBytes);
                        memcpy(rowTop, rowBottom, rowBytes);
                        memcpy(rowBottom, rowTemp.data(), rowBytes);
                    }
                }
            }
            else
            {
                NOX_CORE_ERROR("TextureImporter::DecodeKTX - '{}' is block-compressed, flipping it at load time isn't safe. Flip the source image before baking it to KTX2.", path.string());
            }
        }

        // Owned (the KTX texture's memory goes away with it), and largest mip first like every other texture: KTX2 stores
        // the smallest first, and a run of mips has to be one range of a cooked file for texture streaming.
        std::vector<size_t> levelSizes(kTexture->numLevels);
        size_t totalSize = 0;
        for (uint32_t level = 0; level < kTexture->numLevels; level++)
        {
            levelSizes[level] = ktxTexture_GetImageSize(reinterpret_cast<ktxTexture*>(kTexture), level);
            totalSize += levelSizes[level];
        }
        cpuData.Data.Allocate(totalSize);
        size_t cursor = 0;
        for (uint32_t level = 0; level < kTexture->numLevels; level++)
        {
            memcpy(cpuData.Data.Data + cursor, kTexture->pData + cpuData.MipOffsets[level], levelSizes[level]);
            cpuData.MipOffsets[level] = cursor;
            cursor += levelSizes[level];
        }
        ktxTexture2_Destroy(kTexture);
        return true;
    }

    Ref<Texture2D> TextureImporter::LoadWithNTEX(const std::filesystem::path& path, Renderer* renderer)
    {
        TextureData cpuData{};
        if (!ReadNTEX(path, cpuData))
        {
            NOX_CORE_ERROR("TextureImporter::LoadWithNTEX - Failed to load cooked texture: {}", path.string());
            return Ref<Texture2D>(nullptr);
        }

        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        cpuData.Data.Release();
        return texture;
    }

    bool TextureImporter::SaveNTEX(const std::filesystem::path& path, const TextureData& cpuData)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return false;

        const char magic[5] = "NTX1";
        stream.write(magic, 4);

        uint32_t format = static_cast<uint32_t>(cpuData.Format);
        uint32_t directFormat = cpuData.DirectFormat;
        uint32_t usage = static_cast<uint32_t>(cpuData.Usage);
        uint64_t dataSize = cpuData.Data.Size;
        uint32_t mipOffsetCount = static_cast<uint32_t>(cpuData.MipOffsets.size());

        stream.write(reinterpret_cast<const char*>(&cpuData.Width), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&cpuData.Height), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&cpuData.ArrayLayers), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&cpuData.IsCubeMap), sizeof(bool));
        stream.write(reinterpret_cast<const char*>(&cpuData.MipLevels), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&usage), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&format), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&directFormat), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&mipOffsetCount), sizeof(uint32_t));

        for (size_t offset : cpuData.MipOffsets)
        {
            uint64_t storedOffset = static_cast<uint64_t>(offset);
            stream.write(reinterpret_cast<const char*>(&storedOffset), sizeof(uint64_t));
        }

        stream.write(reinterpret_cast<const char*>(&dataSize), sizeof(uint64_t));
        if (dataSize > 0)
            stream.write(reinterpret_cast<const char*>(cpuData.Data.Data), dataSize);

        return true;
    }

    bool TextureImporter::ReadNTEX(const std::filesystem::path& path, TextureData& outData)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
            return false;

        uint64_t dataSize = 0;
        if (!ReadNTEXHeader(stream, outData, dataSize))
            return false;

        outData.Data.Allocate(dataSize);
        if (dataSize > 0)
            stream.read(reinterpret_cast<char*>(outData.Data.Data), dataSize);

        return !stream.fail();
    }

    bool TextureImporter::ReadNTEXHeader(std::istream& stream, TextureData& outData, uint64_t& outDataSize)
    {
        char magic[5] = {};
        stream.read(magic, 4);
        if (strcmp(magic, "NTX1") != 0)
            return false;

        uint32_t format = 0;
        uint32_t directFormat = UINT32_MAX;
        uint32_t usage = 0;
        uint32_t mipOffsetCount = 0;

        stream.read(reinterpret_cast<char*>(&outData.Width), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&outData.Height), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&outData.ArrayLayers), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&outData.IsCubeMap), sizeof(bool));
        stream.read(reinterpret_cast<char*>(&outData.MipLevels), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&usage), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&format), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&directFormat), sizeof(uint32_t));
        stream.read(reinterpret_cast<char*>(&mipOffsetCount), sizeof(uint32_t));

        outData.Usage = static_cast<NRI::TextureUsage>(usage);
        outData.Format = static_cast<NRI::ImageFormat>(format);
        outData.DirectFormat = directFormat;
        outData.MipOffsets.resize(mipOffsetCount);

        for (uint32_t i = 0; i < mipOffsetCount; i++)
        {
            uint64_t storedOffset = 0;
            stream.read(reinterpret_cast<char*>(&storedOffset), sizeof(uint64_t));
            outData.MipOffsets[i] = static_cast<size_t>(storedOffset);
        }

        stream.read(reinterpret_cast<char*>(&outDataSize), sizeof(uint64_t));
        return !stream.fail();
    }
    
    Ref<Texture2D> TextureImporter::LoadTexture2DFromMemory(TextureData& cpuData, const TextureSpecification& spec, Renderer* renderer)
    {
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        return texture;
    }
}
