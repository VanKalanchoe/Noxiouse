#pragma once
#include <filesystem>

#include "Asset.h"
#include "NRI/Texture.h"

namespace Nox
{
    struct TextureSpecification
    {
        bool flip = false;
        bool generateMips = true;
        NRI::ImageFormat format = NRI::ImageFormat::SRGBA8; // Default to sRGB
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
        MeshCombineMode StaticCombine = MeshCombineMode::DoNotCombine;   // static meshes: merged with their node transforms baked in
        MeshCombineMode SkeletalCombine = MeshCombineMode::DoNotCombine; // skinned meshes (same skeleton): merged in bind pose
        // One glTF mesh of the file as its own static mesh (Do Not Combine gives every mesh an asset); -1 = the whole file.
        // Unreal's Import Uniform Scale: glTF is defined in meters, but a model authored in centimeters (the Khronos Fox)
        // claims meters too and comes in 100x too big -- nothing in the file says so, you tell the importer. Applied as the
        // scale of the entity a drag-in creates (the model itself is untouched).
        float ImportScale = 1.0f;
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

        operator bool () const { return Type != AssetType::None; }
    };
}
