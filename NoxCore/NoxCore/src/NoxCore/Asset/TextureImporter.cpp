#include "TextureImporter.h"

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

namespace Nox
{
    Ref<Texture2D> TextureImporter::ImportTexture2D(AssetHandle handle, const AssetMetadata& metadata)
    {
        return LoadTexture2D(Project::GetActiveAssetDirectory() / metadata.FilePath);
    }

    Ref<Texture2D> TextureImporter::LoadTexture2D(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        std::filesystem::path absolutePath = std::filesystem::absolute(path);
        
        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("TextureImporter::ImportTexture2D - file not found from filepath: {}", absolutePath.string());
            return Ref<Texture2D>(nullptr);
        }
        
        if (path.extension() == ".png" || path.extension() == ".jpg" || path.extension() == ".jpeg")
            return LoadWithSTB(path, spec, renderer);
        
        if (path.extension() == ".dds")
            return LoadWithDDS(path, spec, renderer);
        
        return LoadWithKTX(path, spec, renderer);
    }
    
    Ref<Texture2D> TextureImporter::LoadWithSTB(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
        if (spec.flip)
            stbi_set_flip_vertically_on_load(true);
        
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path.string().c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        uint64_t imageSize = texWidth * texHeight * 4;
        
        uint32_t mipLevels;
        if (spec.generateMips)
            mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;
        else
            mipLevels = 1;

        if (!pixels)
        {
            NOX_CORE_ERROR("TextureImporter::LoadWithSTB - Could not load texture from filepath: {}", path.string());
        }
        
        TextureData cpuData;
        cpuData.Width = texWidth;
        cpuData.Height = texHeight;
        cpuData.MipLevels = mipLevels;
        cpuData.Data = Buffer((void*)pixels, imageSize);
        cpuData.DirectFormat = UINT32_MAX;
        
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
        
        TextureData cpuData;
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
        TextureData cpuData;
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
    
    Ref<Texture2D> TextureImporter::LoadTexture2DFromMemory(TextureData& cpuData, const TextureSpecification& spec, Renderer* renderer)
    {
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        return texture;
    }
}
