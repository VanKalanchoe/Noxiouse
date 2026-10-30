#pragma once
#include <filesystem>
#include <vector>

#include "Asset.h"
#include "NRI/Texture.h"

namespace Nox
{
    // How a PNG / JPG source is block-compressed when cooked (Unreal's texture Compression Settings). DDS and KTX2 keep the
    // compression they ship with.
    enum class TextureCompression : uint8_t
    {
        Default = 0, // BC7: color (sRGB) or packed data (linear), 4 channels
        Normalmap,   // BC5: the X and Y of a tangent-space normal map, each at a single channel's full precision; Z is rebuilt
                     // in the shaders (tangent_normal_from_map), as every engine stores normal maps
        Grayscale    // BC4: one channel (red), for masks and height maps
    };

    // Whether a texture is cooked as a virtual texture (§5.12.2, Unreal's Virtual Texture Streaming flag): its mips in tiles that
    // stream as the view samples them, instead of whole mips.
    enum class VirtualTextureMode : uint8_t
    {
        Auto = 0, // virtual from VirtualTextureAutoSize² texels up (Unreal's Auto Virtual Texturing Size)
        Off,
        On
    };
    inline constexpr uint32_t VirtualTextureAutoSize = 4096;

    struct TextureSpecification
    {
        bool flip = false;
        bool generateMips = true;
        NRI::ImageFormat format = NRI::ImageFormat::SRGBA8; // Default to sRGB
        TextureCompression compression = TextureCompression::Default;
        VirtualTextureMode virtualTexture = VirtualTextureMode::Auto;
    };
    
    // Unreal Interchange: merge the imported meshes of a kind into fewer submeshes (one per material). glTF has no
    // visibility flag, so CombineVisible and CombineAll do the same for now.
    enum class MeshCombineMode : uint8_t
    {
        DoNotCombine = 0,
        CombineVisible,
        CombineAll
    };

    // What a glTF import brings in (Unreal Interchange's Import Static Meshes / Skeletal Meshes / Animations).
    struct MeshImportSettings
    {
        bool ImportStaticMeshes = true;   // meshes not skinned by a glTF skin
        bool ImportSkeletalMeshes = true; // skinned meshes, plus the skeleton
        bool ImportAnimations = true;     // animation clips
        // Unreal's content type "Geometry Only": off, skinned meshes are imported as plain geometry (static meshes, no skeleton,
        // no skinning). ("Skin Weights Only" needs an existing mesh to apply them to; there is no such import.)
        bool ImportSkinWeights = true;
        MeshCombineMode StaticCombine = MeshCombineMode::DoNotCombine;   // static meshes: merged with their node transforms baked in
        MeshCombineMode SkeletalCombine = MeshCombineMode::DoNotCombine; // skinned meshes (same skeleton): merged in bind pose
        // One glTF mesh of the file as its own static mesh (Do Not Combine gives every mesh an asset); -1 = the whole file.
        // Unreal's Import Uniform Scale: glTF is defined in meters, but a model authored in centimeters (the Khronos Fox)
        // claims meters too and comes in 100x too big -- nothing in the file says so, you tell the importer. Applied as the
        // scale of the entity a drag-in creates (the model itself is untouched).
        float ImportScale = 1.0f;
        // Unreal's Import into Level: also place the file's scene (node hierarchy, lights, cameras) in the level. An action of
        // this import, not a property of the assets, so it is not saved with them.
        bool ImportIntoLevel = false;
        int32_t SourceMeshIndex = -1;
        // file's materials): the whole-file mesh path; empty = this asset's own path.
        std::filesystem::path MaterialBasePath;
    };

    struct AssetMetadata
    {
        AssetType Type = AssetType::None;
        std::filesystem::path FilePath;
        std::filesystem::path SourceFilePath;
        TextureSpecification TextureSpec; // Import settings for Texture2D assets
        MeshImportSettings MeshSettings;  // Import settings for Mesh / StaticMesh assets
        // Where the asset was before it was renamed or moved (EditorAssetManager::MoveAsset): still found by those paths, which
        // materials (their textures) and meshes (their materials, derived from the mesh's name) name it by.
        std::vector<std::filesystem::path> FormerPaths;

        operator bool () const { return Type != AssetType::None; }
    };
}
