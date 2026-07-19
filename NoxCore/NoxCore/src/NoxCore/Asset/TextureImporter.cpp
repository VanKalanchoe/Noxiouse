#include "TextureImporter.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define TINYDDSLOADER_IMPLEMENTATION
#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Renderer/tinyddsloader.h"
using namespace tinyddsloader;

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
            return LoadWithDDS(path, spec);
        
        return LoadWithKTX(path, spec);
    }
    
    Ref<Texture2D> TextureImporter::LoadWithSTB(const std::filesystem::path& path, const TextureSpecification& spec, Renderer* renderer)
    {
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
        cpuData.ImageSize = imageSize;
        cpuData.Pixels = pixels;
        
        Renderer* targetRenderer = renderer ? renderer : Application::Get().GetRenderer();
        
        Ref<Texture2D> texture = targetRenderer->UploadTexture(cpuData);
        
        stbi_image_free(pixels);
        
        return texture;
    }
    
    Ref<Texture2D> TextureImporter::LoadWithDDS(const std::filesystem::path& path, const TextureSpecification& spec)
    {
        
    }
    
    Ref<Texture2D> TextureImporter::LoadWithKTX(const std::filesystem::path& path, const TextureSpecification& spec)
    {
        
    }
}
