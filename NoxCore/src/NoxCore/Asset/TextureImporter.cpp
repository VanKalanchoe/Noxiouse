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
#include "NoxCore/Core/VirtualFileSystem.h"

#include <ktx.h>

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Core/Buffer.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Utils/Utils.h"
#include "NoxCore/Tasks/JobSystem.h"

#include <bc7enc.h>
#include <rgbcx.h>

namespace Nox
{
    namespace
    {
        // stb_image over a file's bytes: a loose file, else a mounted pack's entry (VirtualFileSystem.h).
        stbi_uc* LoadImageFile(const std::filesystem::path& path, int* width, int* height, int* channels)
        {
            std::vector<char> bytes;
            if (!VFS::ReadBytes(path, bytes) || bytes.empty())
                return nullptr;
            return stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), width, height, channels, STBI_rgb_alpha);
        }

        float* LoadImageFileHDR(const std::filesystem::path& path, int* width, int* height, int* channels)
        {
            std::vector<char> bytes;
            if (!VFS::ReadBytes(path, bytes) || bytes.empty())
                return nullptr;
            return stbi_loadf_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), width, height, channels, STBI_rgb_alpha);
        }
    }

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
            // In what was padding (zeroed by the value-initialization below): Default hashes as every cook before it did.
            uint8_t compression;
            uint8_t virtualTexture; // padding before too: Auto hashes as before
        };
        TextureCookSettings cookSettings{};
        cookSettings.source = Utility::calcul_hash_streaming(sourcePath.string());
        cookSettings.flip = static_cast<uint8_t>(spec.flip);
        cookSettings.generateMips = static_cast<uint8_t>(spec.generateMips);
        cookSettings.format = static_cast<uint16_t>(spec.format);
        cookSettings.compression = static_cast<uint8_t>(spec.compression);
        cookSettings.virtualTexture = static_cast<uint8_t>(spec.virtualTexture);
        return XXH3_128bits(&cookSettings, sizeof(cookSettings));
    }

    // The mips of a width x height texture as an .ntex lays them out: one after the other, each its rows of blocks.
    static std::vector<size_t> TightMipOffsets(uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t blockSize, uint32_t blockBytes, uint64_t& outTotal)
    {
        std::vector<size_t> offsets;
        uint64_t total = 0;
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            offsets.push_back(total);
            const uint64_t blocksX = (std::max(width >> mip, 1u) + blockSize - 1) / blockSize;
            const uint64_t blocksY = (std::max(height >> mip, 1u) + blockSize - 1) / blockSize;
            total += blocksX * blocksY * blockBytes;
        }
        outTotal = total;
        return offsets;
    }

    // The tiles of a virtual texture of this size and format (offsets in the file left out); false when it cannot be one.
    static bool MakeVirtualTextureLayout(uint32_t width, uint32_t height, uint32_t mipLevels, NRI::ImageFormat format, VirtualTextureLayout& out)
    {
        out = VirtualTextureLayout{};
        if (!NRI::imageFormatBlock(format, out.BlockBytes, out.BlockSize) || out.TileSize % out.BlockSize != 0 || out.Border % out.BlockSize != 0)
            return false;
        while (out.TiledMips < mipLevels && std::max(width >> out.TiledMips, height >> out.TiledMips) > out.TileSize)
            ++out.TiledMips;
        // Larger than a tile, with the tail down to a tile (the always-resident fallback the tiles stream above).
        if (out.TiledMips == 0 || out.TiledMips >= mipLevels)
            return false;

        const uint32_t tileBlocks = out.TileSize / out.BlockSize;
        for (uint32_t mip = 0; mip < out.TiledMips; ++mip)
        {
            const uint32_t blocksX = (std::max(width >> mip, 1u) + out.BlockSize - 1) / out.BlockSize;
            const uint32_t blocksY = (std::max(height >> mip, 1u) + out.BlockSize - 1) / out.BlockSize;
            VirtualTextureLayout::Mip& tiles = out.Mips.emplace_back();
            tiles.TilesX = (blocksX + tileBlocks - 1) / tileBlocks;
            tiles.TilesY = (blocksY + tileBlocks - 1) / tileBlocks;
            tiles.FirstTile = out.TileCount;
            out.TileCount += tiles.TilesX * tiles.TilesY;
        }
        return true;
    }

    bool TextureImporter::ResolveVirtualTexture(VirtualTextureMode mode, const TextureData& texture)
    {
        if (mode == VirtualTextureMode::Off || texture.ArrayLayers != 1 || texture.IsCubeMap || texture.DirectFormat != UINT32_MAX)
            return false;
        // Power-of-two sides: every mip is exactly half the one before, so a tile's texels are exactly a quarter of the tile above
        // (the page table maps a missing tile to its ancestor) and a mip's tiles end at its edge.
        if (texture.Width == 0 || texture.Height == 0 || (texture.Width & (texture.Width - 1)) != 0 || (texture.Height & (texture.Height - 1)) != 0)
            return false;
        if (mode == VirtualTextureMode::Auto && uint64_t(texture.Width) * texture.Height < uint64_t(VirtualTextureAutoSize) * VirtualTextureAutoSize)
            return false;
        VirtualTextureLayout layout;
        if (!MakeVirtualTextureLayout(texture.Width, texture.Height, texture.MipLevels, texture.Format, layout))
            return false;
        // The tiles are cut from mips laid out as an .ntex lays them out.
        uint64_t total = 0;
        return texture.MipOffsets == TightMipOffsets(texture.Width, texture.Height, texture.MipLevels, layout.BlockSize, layout.BlockBytes, total);
    }

    // An RGBA8 mip chain to 4x4 blocks of `blockSize` bytes, each encoded by `encodeBlock` from its 16 RGBA texels: every mip,
    // blocks spread over the workers (a 2048² texture is 262 k blocks). Edge texels are repeated into the blocks of mips smaller
    // than 4x4 or not a multiple.
    template <typename EncodeBlock>
    static void EncodeBlocks(TextureData& cpuData, uint32_t blockSize, const char* name, const EncodeBlock& encodeBlock)
    {
        std::vector<size_t> offsets;
        size_t total = 0;
        for (uint32_t mip = 0; mip < cpuData.MipLevels; ++mip)
        {
            const uint32_t blocksX = (std::max(cpuData.Width >> mip, 1u) + 3) / 4;
            const uint32_t blocksY = (std::max(cpuData.Height >> mip, 1u) + 3) / 4;
            offsets.push_back(total);
            total += static_cast<size_t>(blocksX) * blocksY * blockSize;
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

            JobSystem::Get().ParallelFor(name, blocksY, 4, [&](uint32_t, uint32_t begin, uint32_t end)
            {
                uint8_t block[16 * 4];
                for (uint32_t blockY = begin; blockY < end; ++blockY)
                {
                    for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
                    {
                        for (uint32_t texel = 0; texel < 16; ++texel)
                        {
                            const uint32_t x = std::min(blockX * 4 + texel % 4, width - 1);
                            const uint32_t y = std::min(blockY * 4 + texel / 4, height - 1);
                            memcpy(block + texel * 4, source + (static_cast<size_t>(y) * width + x) * 4, 4);
                        }
                        encodeBlock(destination + (static_cast<size_t>(blockY) * blocksX + blockX) * blockSize, block);
                    }
                }
            });
        }

        cpuData.Data.Release();
        cpuData.Data = encoded;
        cpuData.MipOffsets = std::move(offsets);
    }

    // BC7 (1 byte per texel instead of 4) by bc7enc: perceptual weights for color, linear ones for data (roughness/metalness,
    // occlusion).
    static void EncodeBC7(TextureData& cpuData)
    {
        static std::once_flag s_EncoderInit;
        std::call_once(s_EncoderInit, [] { bc7enc_compress_block_init(); });

        const bool srgb = cpuData.Format == NRI::ImageFormat::SRGBA8;
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        if (!srgb)
            bc7enc_compress_block_params_init_linear_weights(&params);

        EncodeBlocks(cpuData, BC7ENC_BLOCK_SIZE, "Encode BC7", [&](uint8_t* destination, const uint8_t* block)
        {
            bc7enc_compress_block(destination, block, &params);
        });
        cpuData.Format = srgb ? NRI::ImageFormat::BC7_UNorm_SRGB : NRI::ImageFormat::BC7_UNorm;
    }

    // BC1 (half a byte per texel, no alpha) by rgbcx: generated content (CookGeneratedTexture) whose encode time matters more.
    static void EncodeBC1(TextureData& cpuData)
    {
        static std::once_flag s_EncoderInit;
        std::call_once(s_EncoderInit, [] { rgbcx::init(); });
        const bool srgb = cpuData.Format == NRI::ImageFormat::SRGBA8;
        EncodeBlocks(cpuData, 8, "Encode BC1", [](uint8_t* destination, const uint8_t* block) { rgbcx::encode_bc1(5, destination, block, false, false); });
        cpuData.Format = srgb ? NRI::ImageFormat::BC1_UNorm_SRGB : NRI::ImageFormat::BC1_UNorm;
    }

    // BC5 (normal maps: red and green, 1 byte per texel) and BC4 (grayscale: red, half a byte per texel) by rgbcx, each channel
    // with its own endpoints -- a normal map's X and Y no longer share BC7's partitions with an unused blue and alpha.
    static void EncodeBC4BC5(TextureData& cpuData, bool twoChannels)
    {
        static std::once_flag s_EncoderInit;
        std::call_once(s_EncoderInit, [] { rgbcx::init(); });

        if (twoChannels)
            EncodeBlocks(cpuData, 16, "Encode BC5", [](uint8_t* destination, const uint8_t* block) { rgbcx::encode_bc5_hq(destination, block, 0, 1, 4); });
        else
            EncodeBlocks(cpuData, 8, "Encode BC4", [](uint8_t* destination, const uint8_t* block) { rgbcx::encode_bc4_hq(destination, block, 4); });
        cpuData.Format = twoChannels ? NRI::ImageFormat::BC5_UNorm : NRI::ImageFormat::BC4_UNorm;
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

        // Same test as ImportTexture2D: a cooked source is its own cook, anything else must match the recorded hash -- unless the
        // source is gone (a packaged game ships the cooked file alone), then the cooked file is taken as it is.
        if (!IsCookedTextureSource(sourcePath) && VFS::Exists(sourcePath))
        {
            XXH128_hash_t cookedHash{};
            if (!Utility::loadHashFromFile(cookedPath.string() + ".hash", cookedHash) ||
                !XXH128_isEqual(TextureCookHash(sourcePath, metadata.TextureSpec), cookedHash))
            {
                return std::nullopt;
            }
        }

        std::unique_ptr<std::istream> file = VFS::Open(cookedPath); // a loose file, else a mounted pack's entry
        if (!file)
            return std::nullopt;

        CookedTextureHeader header;
        header.Path = cookedPath;
        if (!ReadCookedHeader(*file, header))
            return std::nullopt;
        // Cooked before its virtual texture setting resolved the other way (Auto decides by the size the cook found).
        if (!IsCookedTextureSource(sourcePath) && VFS::Exists(sourcePath) &&
            ResolveVirtualTexture(metadata.TextureSpec.virtualTexture, header.Texture) != (header.Virtual != nullptr))
        {
            return std::nullopt;
        }

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
        if (!header.Virtual)
            return VFS::ReadRange(header.Path, header.DataOffset + begin, end - begin, destination);

        // A virtual texture: its tiled mips put together from the tiles' insides, a row of tiles per read; the tail as it is.
        const VirtualTextureLayout& layout = *header.Virtual;
        const uint32_t tileBlocks = layout.TileSize / layout.BlockSize;
        const uint32_t borderBlocks = layout.Border / layout.BlockSize;
        const uint32_t storedBlocks = layout.TileBlocks();
        const uint64_t tileBytes = layout.TileBytes();
        std::vector<uint8_t> row;
        for (uint32_t mip = firstMip; mip < endMip; ++mip)
        {
            if (mip >= layout.TiledMips)
            {
                const uint64_t tailBegin = offsets[mip];
                return VFS::ReadRange(header.Path, layout.TailOffset + (tailBegin - offsets[layout.TiledMips]), end - tailBegin,
                                      destination + (tailBegin - begin));
            }

            const VirtualTextureLayout::Mip& tiles = layout.Mips[mip];
            const uint32_t blocksX = (std::max(header.Texture.Width >> mip, 1u) + layout.BlockSize - 1) / layout.BlockSize;
            const uint32_t blocksY = (std::max(header.Texture.Height >> mip, 1u) + layout.BlockSize - 1) / layout.BlockSize;
            uint8_t* mipDestination = destination + (offsets[mip] - begin);
            row.resize(tileBytes * tiles.TilesX);
            for (uint32_t tileY = 0; tileY < tiles.TilesY; ++tileY)
            {
                if (!VFS::ReadRange(header.Path, layout.TileFileOffset(mip, 0, tileY), row.size(), row.data()))
                    return false;
                for (uint32_t tileX = 0; tileX < tiles.TilesX; ++tileX)
                {
                    const uint32_t x = tileX * tileBlocks;
                    const uint32_t width = std::min(tileBlocks, blocksX - x);
                    const uint8_t* tile = row.data() + tileX * tileBytes;
                    for (uint32_t line = 0; line < tileBlocks && tileY * tileBlocks + line < blocksY; ++line)
                    {
                        const uint32_t y = tileY * tileBlocks + line;
                        memcpy(mipDestination + (uint64_t(y) * blocksX + x) * layout.BlockBytes,
                               tile + (uint64_t(line + borderBlocks) * storedBlocks + borderBlocks) * layout.BlockBytes, uint64_t(width) * layout.BlockBytes);
                    }
                }
            }
        }
        return true;
    }

    bool TextureImporter::ReadVirtualTextureTile(const CookedTextureHeader& header, uint32_t mip, uint32_t tileX, uint32_t tileY, uint8_t* destination)
    {
        if (!header.Virtual || mip >= header.Virtual->TiledMips || tileX >= header.Virtual->Mips[mip].TilesX || tileY >= header.Virtual->Mips[mip].TilesY)
            return false;
        return VFS::ReadRange(header.Path, header.Virtual->TileFileOffset(mip, tileX, tileY), header.Virtual->TileBytes(), destination);
    }

    bool TextureImporter::CookedVirtualTextureMatches(const std::filesystem::path& cookedPath, const TextureSpecification& spec)
    {
        std::unique_ptr<std::istream> file = VFS::Open(cookedPath);
        CookedTextureHeader header;
        header.Path = cookedPath;
        return file && ReadCookedHeader(*file, header) && ResolveVirtualTexture(spec.virtualTexture, header.Texture) == (header.Virtual != nullptr);
    }

    Ref<Texture2D> TextureImporter::ImportTexture2D(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path cookedPath = GetCookedTexturePath(assetDirectory, metadata);
        std::filesystem::path sourcePath = GetTextureSourcePath(assetDirectory, metadata);

        // A packaged game ships the cooked file without its source: with the source gone it is taken as it is.
        const bool sourceIsCooked = IsCookedTextureSource(sourcePath);
        const bool sourceExists = VFS::Exists(sourcePath);
        const auto hashPath = cookedPath.string() + ".hash";
        const XXH128_hash_t sourceHash = sourceIsCooked || !sourceExists ? XXH128_hash_t{} : TextureCookHash(sourcePath, metadata.TextureSpec);
        XXH128_hash_t cookedHash{};
        const bool cookedIsCurrent = sourceIsCooked || !sourceExists ||
            (Utility::loadHashFromFile(hashPath, cookedHash) && XXH128_isEqual(sourceHash, cookedHash) &&
             CookedVirtualTextureMatches(cookedPath, metadata.TextureSpec));

        if (VFS::Exists(cookedPath) && cookedIsCurrent)
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
            decoded = DecodeDDS(sourcePath, metadata.TextureSpec, cpuData);
        else if (sourcePath.extension() == ".ktx2")
            decoded = DecodeKTX(sourcePath, metadata.TextureSpec, cpuData);
        else
            decoded = DecodeSTB(sourcePath, metadata.TextureSpec, cpuData);
        if (!decoded)
            return false;
        // PNG/JPG decode to RGBA8 with generated mips, compressed as the texture's settings say (BC7, normal maps BC5, grayscale
        // BC4: always linear); DDS and KTX2 keep the format they ship in.
        if (cpuData.Format == NRI::ImageFormat::SRGBA8 || cpuData.Format == NRI::ImageFormat::RGBA8)
        {
            if (metadata.TextureSpec.compression == TextureCompression::Default)
                EncodeBC7(cpuData);
            else
                EncodeBC4BC5(cpuData, metadata.TextureSpec.compression == TextureCompression::Normalmap);
        }

        std::error_code error;
        std::filesystem::create_directories(cookedPath.parent_path(), error);
        const bool isVirtual = ResolveVirtualTexture(metadata.TextureSpec.virtualTexture, cpuData);
        const bool saved = isVirtual ? SaveNTV(cookedPath, cpuData) : SaveNTEX(cookedPath, cpuData);
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

    bool TextureImporter::CookGeneratedTexture(TextureData& cpuData, const std::filesystem::path& cookedPath, bool fast)
    {
        if (cpuData.Width == 0 || cpuData.Height == 0 || cpuData.Data.Size < static_cast<uint64_t>(cpuData.Width) * cpuData.Height * 4 ||
            (cpuData.Format != NRI::ImageFormat::SRGBA8 && cpuData.Format != NRI::ImageFormat::RGBA8))
            return false;
        cpuData.MipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(cpuData.Width, cpuData.Height)))) + 1;
        cpuData.MipOffsets = { 0 };
        GenerateRGBA8MipChain(cpuData);
        if (fast)
            EncodeBC1(cpuData);
        else
            EncodeBC7(cpuData);
        std::error_code error;
        std::filesystem::create_directories(cookedPath.parent_path(), error);
        const bool saved = SaveNTEX(cookedPath, cpuData);
        cpuData.Data.Release();
        if (!saved)
            NOX_CORE_ERROR("TextureImporter::CookGeneratedTexture - could not write {}", cookedPath.string());
        return saved;
    }

    bool TextureImporter::DecodeSTB(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData)
    {
        stbi_set_flip_vertically_on_load_thread(spec.flip);

        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = LoadImageFile(path, &texWidth, &texHeight, &texChannels);
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
        
        if (!VFS::Exists(path))
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
        stbi_uc* pixels = LoadImageFile(path, &texWidth, &texHeight, &texChannels);
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
        float* pixels = LoadImageFileHDR(path, &texWidth, &texHeight, &texChannels);
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
        if (!DecodeDDS(path, spec, cpuData))
            return Ref<Texture2D>(nullptr);

        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        cpuData.Data.Release();
        return texture;
    }

    bool TextureImporter::DecodeDDS(const std::filesystem::path& path, const TextureSpecification& spec, TextureData& cpuData)
    {
        tinyddsloader::DDSFile dds;

        std::vector<char> ddsBytes;
        auto result = VFS::ReadBytes(path, ddsBytes) ? dds.Load(std::vector<uint8_t>(ddsBytes.begin(), ddsBytes.end()))
                                                     : tinyddsloader::Result::ErrorRead;

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

        // Legacy Bistro DDS files generally use BC*_UNORM even for textures that glTF declares as
        // color data. RTXPT/Donut creates an sRGB view from the material texture role instead of
        // trusting that legacy header. Do the same here. Hardware sRGB decoding affects RGB only,
        // leaving alpha linear for the glossiness channel in specular-glossiness textures.
        const bool useSRGB = spec.format == NRI::ImageFormat::SRGBA8;

        switch (dds.GetFormat())
        {
        case tinyddsloader::DDSFile::DXGIFormat::BC1_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC1_UNorm:
        case tinyddsloader::DDSFile::DXGIFormat::BC1_UNorm_SRGB:
            cpuData.Format = useSRGB ? NRI::ImageFormat::BC1_UNorm_SRGB : NRI::ImageFormat::BC1_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC2_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC2_UNorm:
        case tinyddsloader::DDSFile::DXGIFormat::BC2_UNorm_SRGB:
            cpuData.Format = useSRGB ? NRI::ImageFormat::BC2_UNorm_SRGB : NRI::ImageFormat::BC2_UNorm;
            break;
        case tinyddsloader::DDSFile::DXGIFormat::BC3_Typeless:
        case tinyddsloader::DDSFile::DXGIFormat::BC3_UNorm:
        case tinyddsloader::DDSFile::DXGIFormat::BC3_UNorm_SRGB:
            cpuData.Format = useSRGB ? NRI::ImageFormat::BC3_UNorm_SRGB : NRI::ImageFormat::BC3_UNorm;
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
        case tinyddsloader::DDSFile::DXGIFormat::BC7_UNorm_SRGB:
            cpuData.Format = useSRGB ? NRI::ImageFormat::BC7_UNorm_SRGB : NRI::ImageFormat::BC7_UNorm;
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

    bool TextureImporter::SaveNTV(const std::filesystem::path& path, const TextureData& cpuData)
    {
        VirtualTextureLayout layout;
        uint64_t total = 0;
        if (!MakeVirtualTextureLayout(cpuData.Width, cpuData.Height, cpuData.MipLevels, cpuData.Format, layout) ||
            cpuData.MipOffsets != TightMipOffsets(cpuData.Width, cpuData.Height, cpuData.MipLevels, layout.BlockSize, layout.BlockBytes, total) ||
            cpuData.Data.Size != total)
            return false;

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return false;

        // Header: magic, version, the texture, the tiling, each tiled mip's tile counts, where the tiles and the tail are.
        const uint32_t fields[] = { 1u, cpuData.Width, cpuData.Height, cpuData.MipLevels, static_cast<uint32_t>(cpuData.Usage),
                                    static_cast<uint32_t>(cpuData.Format), layout.TileSize, layout.Border, layout.TiledMips };
        const uint64_t headerBytes = 4 + sizeof(fields) + 2 * sizeof(uint32_t) * layout.TiledMips + 3 * sizeof(uint64_t);
        layout.TilesOffset = headerBytes;
        layout.TailOffset = layout.TilesOffset + layout.TileBytes() * layout.TileCount;
        layout.TailSize = cpuData.Data.Size - cpuData.MipOffsets[layout.TiledMips];

        stream.write("NTV1", 4);
        stream.write(reinterpret_cast<const char*>(fields), sizeof(fields));
        for (const VirtualTextureLayout::Mip& mip : layout.Mips)
        {
            stream.write(reinterpret_cast<const char*>(&mip.TilesX), sizeof(uint32_t));
            stream.write(reinterpret_cast<const char*>(&mip.TilesY), sizeof(uint32_t));
        }
        stream.write(reinterpret_cast<const char*>(&layout.TilesOffset), sizeof(uint64_t));
        stream.write(reinterpret_cast<const char*>(&layout.TailOffset), sizeof(uint64_t));
        stream.write(reinterpret_cast<const char*>(&layout.TailSize), sizeof(uint64_t));

        // The tiles, a row of them at a time: each block of a tile is the mip's block at that place, wrapped around its edges.
        const uint32_t tileBlocks = layout.TileSize / layout.BlockSize;
        const int64_t borderBlocks = layout.Border / layout.BlockSize;
        const uint32_t storedBlocks = layout.TileBlocks();
        const uint64_t tileBytes = layout.TileBytes();
        std::vector<uint8_t> row;
        for (uint32_t mip = 0; mip < layout.TiledMips; ++mip)
        {
            const VirtualTextureLayout::Mip& tiles = layout.Mips[mip];
            const int64_t blocksX = (std::max(cpuData.Width >> mip, 1u) + layout.BlockSize - 1) / layout.BlockSize;
            const int64_t blocksY = (std::max(cpuData.Height >> mip, 1u) + layout.BlockSize - 1) / layout.BlockSize;
            const uint8_t* source = cpuData.Data.Data + cpuData.MipOffsets[mip];
            row.resize(tileBytes * tiles.TilesX);
            for (uint32_t tileY = 0; tileY < tiles.TilesY; ++tileY)
            {
                for (uint32_t tileX = 0; tileX < tiles.TilesX; ++tileX)
                {
                    uint8_t* tile = row.data() + tileX * tileBytes;
                    for (uint32_t line = 0; line < storedBlocks; ++line)
                    {
                        const int64_t y = ((int64_t(tileY) * tileBlocks - borderBlocks + line) % blocksY + blocksY) % blocksY;
                        for (uint32_t column = 0; column < storedBlocks; ++column)
                        {
                            const int64_t x = ((int64_t(tileX) * tileBlocks - borderBlocks + column) % blocksX + blocksX) % blocksX;
                            memcpy(tile + (uint64_t(line) * storedBlocks + column) * layout.BlockBytes,
                                   source + (uint64_t(y) * blocksX + x) * layout.BlockBytes, layout.BlockBytes);
                        }
                    }
                }
                stream.write(reinterpret_cast<const char*>(row.data()), row.size());
            }
        }

        stream.write(reinterpret_cast<const char*>(cpuData.Data.Data + cpuData.MipOffsets[layout.TiledMips]), layout.TailSize);
        if (!stream.good())
            return false;
        NOX_CORE_INFO("TextureImporter: {} cooked as a virtual texture, {} tiles of {} KB in {} mips, a {} KB tail",
                      path.filename().string(), layout.TileCount, tileBytes / 1024, layout.TiledMips, layout.TailSize / 1024);
        return true;
    }

    bool TextureImporter::ReadNTEX(const std::filesystem::path& path, TextureData& outData)
    {
        std::unique_ptr<std::istream> file = VFS::Open(path);
        if (!file)
            return false;
        std::istream& stream = *file;

        CookedTextureHeader header;
        header.Path = path;
        if (!ReadCookedHeader(stream, header))
            return false;
        outData = header.Texture;
        outData.Data.Allocate(header.DataSize);
        if (header.Virtual)
            return ReadCookedTextureMips(header, 0, std::max(outData.MipLevels, 1u), outData.Data.Data);
        if (header.DataSize > 0)
            stream.read(reinterpret_cast<char*>(outData.Data.Data), header.DataSize);

        return !stream.fail();
    }

    bool TextureImporter::ReadCookedHeader(std::istream& stream, CookedTextureHeader& header)
    {
        TextureData& outData = header.Texture;
        char magic[5] = {};
        stream.read(magic, 4);
        if (strcmp(magic, "NTV1") == 0)
        {
            uint32_t fields[9] = {};
            stream.read(reinterpret_cast<char*>(fields), sizeof(fields));
            if (stream.fail() || fields[0] != 1)
                return false;
            outData.Width = fields[1];
            outData.Height = fields[2];
            outData.MipLevels = fields[3];
            outData.Usage = static_cast<NRI::TextureUsage>(fields[4]);
            outData.Format = static_cast<NRI::ImageFormat>(fields[5]);
            outData.ArrayLayers = 1;
            outData.IsCubeMap = false;
            outData.DirectFormat = UINT32_MAX;

            // The tiling again from the size and format; the file has to agree with it.
            auto layout = std::make_shared<VirtualTextureLayout>();
            if (!MakeVirtualTextureLayout(outData.Width, outData.Height, outData.MipLevels, outData.Format, *layout) ||
                layout->TileSize != fields[6] || layout->Border != fields[7] || layout->TiledMips != fields[8])
                return false;
            for (const VirtualTextureLayout::Mip& mip : layout->Mips)
            {
                uint32_t tiles[2] = {};
                stream.read(reinterpret_cast<char*>(tiles), sizeof(tiles));
                if (tiles[0] != mip.TilesX || tiles[1] != mip.TilesY)
                    return false;
            }
            stream.read(reinterpret_cast<char*>(&layout->TilesOffset), sizeof(uint64_t));
            stream.read(reinterpret_cast<char*>(&layout->TailOffset), sizeof(uint64_t));
            stream.read(reinterpret_cast<char*>(&layout->TailSize), sizeof(uint64_t));

            outData.MipOffsets = TightMipOffsets(outData.Width, outData.Height, outData.MipLevels, layout->BlockSize, layout->BlockBytes, header.DataSize);
            if (stream.fail() || layout->TailSize != header.DataSize - outData.MipOffsets[layout->TiledMips])
                return false;
            header.DataOffset = 0;
            header.Virtual = std::move(layout);
            return true;
        }
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

        stream.read(reinterpret_cast<char*>(&header.DataSize), sizeof(uint64_t));
        header.DataOffset = static_cast<uint64_t>(stream.tellg());
        header.Virtual.reset();
        return !stream.fail();
    }
    
    Ref<Texture2D> TextureImporter::LoadTexture2DFromMemory(TextureData& cpuData, const TextureSpecification& spec, Renderer* renderer)
    {
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        return texture;
    }
}
