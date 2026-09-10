#include "TextureImporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define TINYDDSLOADER_IMPLEMENTATION
#include "NoxCore/Renderer/tinyddsloader.h"

#include <ktx.h>

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Core/Buffer.h"
#include "NoxCore/Debug/Instrumentor.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    static std::filesystem::path GetCookedTexturePath(const AssetMetadata& metadata)
    {
        std::filesystem::path path = Project::GetActiveAssetDirectory() / metadata.FilePath;
        if (path.extension() != ".ntex")
            path.replace_extension(".ntex");
        return path;
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

    Ref<Texture2D> TextureImporter::ImportTexture2D(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = GetCookedTexturePath(metadata);
        std::filesystem::path sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
        if (metadata.SourceFilePath.empty())
            sourcePath = Project::GetActiveAssetDirectory() / metadata.FilePath;

        const bool sourceIsCooked = sourcePath.extension() == ".ntex" || sourcePath.extension() == ".ktx2";
        const auto hashPath = cookedPath.string() + ".hash";
        XXH128_hash_t sourceHash{};
        XXH128_hash_t cookedHash{};
        struct TextureCookSettings
        {
            XXH128_hash_t source;
            uint8_t flip;
            uint8_t generateMips;
            uint16_t format;
        };
        TextureCookSettings cookSettings{};
        cookSettings.source = Utility::calcul_hash_streaming(sourcePath.string());
        cookSettings.flip = static_cast<uint8_t>(metadata.TextureSpec.flip);
        cookSettings.generateMips = static_cast<uint8_t>(metadata.TextureSpec.generateMips);
        cookSettings.format = static_cast<uint16_t>(metadata.TextureSpec.format);
        sourceHash = XXH3_128bits(&cookSettings, sizeof(cookSettings));
        const bool cookedIsCurrent = sourceIsCooked ||
            (std::filesystem::exists(sourcePath) &&
             (Utility::loadHashFromFile(hashPath, cookedHash) &&
              XXH128_isEqual(sourceHash, cookedHash)));

        if (std::filesystem::exists(cookedPath) && cookedIsCurrent)
            return LoadTexture2D(cookedPath, metadata.TextureSpec);

        if (sourcePath.extension() == ".png" || sourcePath.extension() == ".jpg" || sourcePath.extension() == ".jpeg")
        {
            if (metadata.TextureSpec.flip)
                stbi_set_flip_vertically_on_load(true);
            else
                stbi_set_flip_vertically_on_load(false);

            int texWidth, texHeight, texChannels;
            stbi_uc* pixels = stbi_load(sourcePath.string().c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
            if (!pixels)
            {
                NOX_CORE_ERROR("TextureImporter::ImportTexture2D - Could not load texture from filepath: {}", sourcePath.string());
                return Ref<Texture2D>(nullptr);
            }

            TextureData cpuData{};
            cpuData.Width = texWidth;
            cpuData.Height = texHeight;
            cpuData.MipLevels = metadata.TextureSpec.generateMips
                                    ? static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1
                                    : 1;
            cpuData.Data = Buffer::Copy(Buffer(pixels, static_cast<uint64_t>(texWidth) * texHeight * 4));
            cpuData.Format = metadata.TextureSpec.format;
            cpuData.MipOffsets = { 0 };

            stbi_image_free(pixels);

            GenerateRGBA8MipChain(cpuData);

            if (!std::filesystem::exists(cookedPath.parent_path()))
                std::filesystem::create_directories(cookedPath.parent_path());
            SaveNTEX(cookedPath, cpuData);
            if (!sourceIsCooked)
                Utility::saveHashToFile(hashPath, sourceHash);

            Renderer* targetRenderer = Application::Get().GetRenderer();
            Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
            cpuData.Data.Release();
            return texture;
        }

        Ref<Texture2D> texture = LoadTexture2D(sourcePath, metadata.TextureSpec);
        return texture;
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
            stbi_set_flip_vertically_on_load(true);
        else
            stbi_set_flip_vertically_on_load(false);
        
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
            stbi_set_flip_vertically_on_load(true);
        else
            stbi_set_flip_vertically_on_load(false);
        
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
        tinyddsloader::DDSFile dds;
        
        auto result = dds.Load(path.string().c_str());
        
        if (result != tinyddsloader::Result::Success) 
        {
            NOX_CORE_ASSERT("TextureImporter::LoadWithDDS - Failed to load DDS from: {} Result: {}", path.string(), std::to_string(result));
        }
        
        TextureData cpuData{};
        cpuData.Width = dds.GetWidth();
        cpuData.Height = dds.GetHeight();
        cpuData.MipLevels = dds.GetMipCount();
        
        if (dds.GetFormat() == tinyddsloader::DDSFile::DXGIFormat::BC7_UNorm)
            cpuData.Format = NRI::ImageFormat::BC7_UNorm;
        else if (dds.GetFormat() == tinyddsloader::DDSFile::DXGIFormat::BC7_UNorm_SRGB)
            cpuData.Format = NRI::ImageFormat::BC7_UNorm_SRGB;
        
        size_t currentOffset = 0;
        cpuData.MipOffsets.resize(dds.GetMipCount());
        
        for (uint32_t level = 0; level < dds.GetMipCount(); level++)
        {
            cpuData.MipOffsets[level] = currentOffset;
            
            const auto* imageData = dds.GetImageData(level, 0);
            currentOffset += imageData->m_memSlicePitch;
        }
        
        cpuData.Data = Buffer((void*)dds.GetImageData()->m_mem, currentOffset);
        
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        return texture;   
    }
    
    Ref<Texture2D> TextureImporter::LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        ktxTexture2* kTexture;
        KTX_error_code result = ktxTexture2_CreateFromNamedFile
        (
            path.string().c_str(),
            KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
            &kTexture
        );
        
        if (result != KTX_SUCCESS) NOX_CORE_ASSERT("TextureImporter::LoadWithKTX failed to load ktx texture image!");
        
        if (ktxTexture2_NeedsTranscoding(kTexture))
        {
            // Transcode to standard uncompressed RGBA8 so it works on all GPUs.
            // (If you want block compression, you can look into targeting KTX_TTF_BC7_RGBA instead!)
            result = ktxTexture2_TranscodeBasis(kTexture, KTX_TTF_BC7_RGBA, 0);
            
            if (result != KTX_SUCCESS) NOX_CORE_ASSERT("TextureImporter::LoadWithKTX failed to transcode Basis texture!");
        }
        
        if (spec.flip) NOX_CORE_ERROR("TextureImporter::LoadWithKTX doesnt support flipping pls convert with tool manually");
        
        // Get texture dimensions and data
        TextureData cpuData{};
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
        
        cpuData.Data = Buffer((void*)kTexture->pData, kTexture->dataSize);
        
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        ktxTexture2_Destroy(kTexture);
        
        return texture;
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

        char magic[5] = {};
        stream.read(magic, 4);
        if (strcmp(magic, "NTX1") != 0)
            return false;

        uint32_t format = 0;
        uint32_t directFormat = UINT32_MAX;
        uint32_t usage = 0;
        uint64_t dataSize = 0;
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

        stream.read(reinterpret_cast<char*>(&dataSize), sizeof(uint64_t));
        outData.Data.Allocate(dataSize);
        if (dataSize > 0)
            stream.read(reinterpret_cast<char*>(outData.Data.Data), dataSize);

        return !stream.fail();
    }
    
    Ref<Texture2D> TextureImporter::LoadTexture2DFromMemory(TextureData& cpuData, const TextureSpecification& spec, Renderer* renderer)
    {
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        return texture;
    }
}
