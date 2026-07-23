#pragma once
#include <filesystem>

#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"

namespace Nox
{
    struct MSDFData;
    
    class Font : public RefCounted
    {
    public:
        Font(const std::filesystem::path& font);
        ~Font();
        
        const MSDFData* GetMSDFData() const { return m_Data; }
        Ref<Texture2D> GetAtlasTexture() const { return m_AtlasTexture; }
        
        static Ref<Font> GetDefault();
        static void ReleaseDefault();

    private:
        MSDFData* m_Data;
        Ref<Texture2D> m_AtlasTexture;
        static Ref<Font> s_DefaultFont;
    };
}
