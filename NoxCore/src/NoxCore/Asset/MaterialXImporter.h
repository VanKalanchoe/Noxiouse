#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    // One material of a .mtlx document, converted (MaterialXImporter::Import).
    struct MaterialXMaterial
    {
        std::string Name;                    // the document's surfacematerial name
        std::filesystem::path MaterialPath;  // where its .nmat goes, relative to the asset directory
        MaterialData Data;                   // texture paths relative to the asset directory
        std::vector<std::string> Warnings;   // what did not convert exactly (unsupported inputs and nodes, baked 3D procedurals)
    };

    // MaterialX (.mtlx) material import, Unreal's Interchange MaterialX way: a .mtlx document holds materials only (no meshes). Each
    // surfacematerial's shader -- Standard Surface, OpenPBR Surface, glTF PBR or UsdPreviewSurface -- is mapped onto Nox's material
    // (glTF PBR and its extensions, M1's lobes) by MaterialX's own translation graphs (assets/materialx: standard_surface_to_gltf_pbr,
    // open_pbr_to_standard_surface), evaluated here node by node. An input that is the same everywhere becomes a factor; one that
    // varies over the surface (an image, a procedural node graph, or both) is baked to a texture in glTF's channel layout (roughness
    // and metalness into one texture's G and B, ...), in the image's own texture space where it is one tiled image (the material's
    // UV channel then holds the tiling), else over the mesh's UV set 0. A baked texture that is exactly one of the source images is
    // not written: the material names the image itself (when the image is inside the project). Node graphs Nox has no equivalent
    // for at run time (noise, mixing) are baked; 3D procedurals (position) are baked with the UV coordinates in place of the
    // position, as a warning says.
    class MaterialXImporter
    {
    public:
        // `sourcePath`: the .mtlx, relative to `assetDirectory` or absolute (a document outside the project: its images are then
        // copied in, never named). `outputFolder` (relative to `assetDirectory`): where the materials' .nmat files and the baked
        // textures ("Textures") go. `definitionsDirectory`: the shipped MaterialX definitions (assets/materialx). The caller writes
        // each material's .nmat (MaterialPath) and registers it and its textures. False (with `error`) when the document cannot be read.
        static bool Import(const std::filesystem::path& assetDirectory, const std::filesystem::path& sourcePath, const std::filesystem::path& outputFolder,
                           const std::filesystem::path& definitionsDirectory, std::vector<MaterialXMaterial>& outMaterials, std::string& error);

        // Where a .mtlx's import goes by default: "<its folder>/<its name>_mtlx".
        static std::filesystem::path DefaultOutputFolder(const std::filesystem::path& sourcePath)
        {
            return sourcePath.parent_path() / (sourcePath.stem().string() + "_mtlx");
        }
    };
}
