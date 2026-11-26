#pragma once
// MagicaVoxel .vox files as model sources (docs/Voxel_Plan_2026-11.md §4, VX0): a .vox is a MeshSource like a .gltf, imported with
// the same dialog and cooked by the same code (MeshImporter::CookMesh / CookSplitMeshes) -- this turns it into what the glTF parser
// gives them: submeshes (each model's opaque and glass faces), their materials, the node table (a root at the pivot, one node per
// placed model) and the triangles before cluster building. So Combine / Do Not Combine / file layout work as for a glTF.
//
// Materials: one "Palette" material (and "PaletteGlass" for transparent entries) per file, reading three palette textures cooked next
// to them (<model folder>/Textures/<file>_Palette{Color,Surface,Emissive}.ntex, 256 x 1, one mip, uncompressed: each voxel face's UVs
// sit on its palette entry's texel, so the colours are exact). Color: the palette (sRGB). Surface (linear): R transparency, G
// roughness, B metallic. Emissive (sRGB): colour x emission strength / 16 (strength = _emit x 2^_flux, at most 16; the material's
// Emissive Strength is 16). The materials (.nmat) are written once like a glTF's -- edits in the Material Editor survive re-imports --
// and everything the palette decides lives in the textures, which every cook rewrites when they changed (the editor reloads them).

#include "MeshImporter.h"

#include <string>

namespace Nox
{
    class VoxImporter
    {
    public:
        static bool IsVoxFile(const std::filesystem::path& path);

        // Any thread. The .vox at `sourcePath` as the glTF parser's output (MeshImporter::ParseGltfToMeshData with outPending), and
        // its palette textures written next to the materials named after `materialBase` (relative to `assetDirectory`). Empty with a
        // logged error when the file cannot be read.
        static std::vector<MeshData> ParseToMeshData(const std::filesystem::path& sourcePath, const std::filesystem::path& assetDirectory,
                                                     const std::filesystem::path& materialBase, const MeshImportSettings& settings,
                                                     std::vector<MaterialData>& outMaterials, std::vector<MeshNodeData>& outNodes,
                                                     std::vector<PendingGeometry>& outPending);

        // What the import dialog shows: static meshes only (one per placed model), the bounds in voxels.
        static MeshImporter::GltfContent Inspect(const std::filesystem::path& sourcePath);

        // Where a palette texture of the model whose cooked mesh is `materialBase` lives (relative to the asset directory);
        // kind: "Color", "Surface" or "Emissive".
        static std::filesystem::path PaletteTexturePath(const std::filesystem::path& materialBase, const char* kind);

        // Emission strength the palette's emissive texture is scaled to (the materials' Emissive Strength).
        static constexpr float MaxEmissiveStrength = 16.0f;
    };
}
