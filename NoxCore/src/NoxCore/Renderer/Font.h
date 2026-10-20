#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"

namespace Nox
{
    struct MSDFData;
    
    // A font's glyphs as a multi-channel signed distance field atlas (msdf-atlas-gen), for TextComponent / DrawString. An asset
    // (.ttf, .otf in the project) or the editor's default font.
    class Font : public Asset
    {
    public:
        Font(const std::filesystem::path& font);
        // From the font file's bytes (a packed or generated font); name is for the log.
        Font(const std::vector<uint8_t>& fontData, const std::string& name);
        ~Font();
        
        const MSDFData* GetMSDFData() const { return m_Data; }
        Ref<Texture2D> GetAtlasTexture() const { return m_AtlasTexture; }
        // False when the file was not a font: drawing with it draws nothing.
        bool IsValid() const { return m_AtlasTexture != nullptr; }

        static AssetType GetStaticType() { return AssetType::Font; }
        AssetType GetType() const override { return GetStaticType(); }
        
        static Ref<Font> GetDefault();
        static void ReleaseDefault();

    private:
        void Load(const std::vector<uint8_t>& fontData, const std::string& name);

        MSDFData* m_Data;
        Ref<Texture2D> m_AtlasTexture;
        static Ref<Font> s_DefaultFont;
    };
}
