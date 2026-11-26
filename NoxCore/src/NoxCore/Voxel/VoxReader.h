#pragma once
// MagicaVoxel .vox files (docs/Voxel_Plan_2026-11.md §4, VX0): the models, the palette, the materials and the scene graph, read
// into plain data. No engine types: the importer (MeshImporter, VoxMesher) turns it into meshes. Every size is checked against the
// file -- a broken or truncated file is an error with a message, never a crash or a read past the end.
//
// The format: "VOX " + version (150 / 200), then a MAIN chunk whose children are the content. A chunk is a 4-byte id, its content
// size, its children's size, the content, the children. Content (all little endian):
//   PACK  model count (old files; ignored, SIZE/XYZI pairs are counted)
//   SIZE  x, y, z                                  -- a model's size, followed by its XYZI
//   XYZI  count, then count x (x, y, z, colour index) bytes
//   RGBA  256 x RGBA: entry k is colour index k + 1 (the last entry is unused); absent = the default palette
//   MATL  id (the colour index), DICT (_type _diffuse/_metal/_glass/_emit/_blend/_media/_cloud, _rough, _metal, _ior, _ri,
//         _trans, _alpha, _emit, _flux, ...)
//   MATT  the 0.98 materials: id, type, weight, property bits + values
//   nTRN  node id, DICT (_name, _hidden), child id, reserved, layer id, frame count, frames (DICT: _r rotation byte, _t "x y z")
//   nGRP  node id, DICT, child count, child ids
//   nSHP  node id, DICT, model count, (model id, DICT _f frame) -- several models are an animation's frames (frame 0's is placed)
//   LAYR  layer id, DICT (_name, _hidden), reserved
// Unknown chunks (rOBJ, rCAM, NOTE, IMAP, ...) are skipped. MagicaVoxel is Z-up; this reader keeps its axes (VoxMesher converts).

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Nox::Vox
{
    struct Model
    {
        int32_t SizeX = 0, SizeY = 0, SizeZ = 0;
        // SizeX * SizeY * SizeZ colour indices, x fastest then y then z; 0 = empty.
        std::vector<uint8_t> Voxels;
        uint32_t VoxelCount = 0; // solid voxels

        uint8_t At(int32_t x, int32_t y, int32_t z) const
        {
            if (x < 0 || y < 0 || z < 0 || x >= SizeX || y >= SizeY || z >= SizeZ)
                return 0;
            return Voxels[static_cast<size_t>(x) + static_cast<size_t>(SizeX) * (static_cast<size_t>(y) + static_cast<size_t>(SizeY) * static_cast<size_t>(z))];
        }
    };

    enum class MaterialType : uint8_t
    {
        Diffuse,
        Metal,
        Glass,
        Emit,
        Blend,
        Media,
        Cloud
    };

    // One palette entry's material, as MagicaVoxel's Render tab sets it (defaults: a plain diffuse surface).
    struct Material
    {
        MaterialType Type = MaterialType::Diffuse;
        float Roughness = 1.0f;     // _rough (0 mirror .. 1 matte)
        float Metallic = 0.0f;      // _metal
        float Transparency = 0.0f;  // _trans (glass, blend, media, cloud): 0 opaque .. 1 clear; _alpha for blend when _trans is absent
        float IOR = 1.5f;           // _ri, or 1 + _ior (older files store the IOR minus one)
        float Emission = 0.0f;      // _emit (0 .. 1)
        float Flux = 0.0f;          // _flux, MagicaVoxel's emission "Power" (0 .. 4)

        // Light passes through it (the importer's glass submesh).
        bool IsTransparent() const
        {
            return Type == MaterialType::Glass || Type == MaterialType::Blend || Type == MaterialType::Media || Type == MaterialType::Cloud;
        }
    };

    // A rotation as MagicaVoxel stores it: a signed permutation matrix (every row one +1 / -1), packed into a byte.
    struct Rotation
    {
        // Rows of the matrix; Column[r] is where row r's non-zero entry sits, Sign[r] its sign.
        std::array<int8_t, 3> Column = { 0, 1, 2 };
        std::array<int8_t, 3> Sign = { 1, 1, 1 };

        static Rotation Identity() { return {}; }
        // bits 0-1: row 0's column, bits 2-3: row 1's column (row 2 takes the remaining one), bits 4 / 5 / 6: rows 0 / 1 / 2 negative.
        // False when the byte names one column twice or a column past 2.
        static bool FromByte(uint8_t packed, Rotation& out);
        uint8_t ToByte() const;

        int32_t Entry(int row, int column) const { return Column[row] == column ? Sign[row] : 0; }
        int32_t Determinant() const;
        // this * other
        Rotation operator*(const Rotation& other) const;
        std::array<int32_t, 3> Apply(const std::array<int32_t, 3>& v) const;
        std::array<double, 3> Apply(const std::array<double, 3>& v) const;
    };

    // One placed model of the scene graph, flattened: every transform node above it applied (parents first).
    struct Instance
    {
        std::string Name;   // the nearest named transform node (_name), or empty
        uint32_t ModelIndex = 0;
        Rotation Rotate;
        std::array<int32_t, 3> Translation = { 0, 0, 0 };
        int32_t Layer = -1;
    };

    struct Scene
    {
        uint32_t Version = 0;
        std::vector<Model> Models;
        // Colour index -> 0xAABBGGRR (index 0 unused). The file's RGBA chunk, or the default palette.
        std::array<uint32_t, 256> Palette{};
        bool HasPaletteChunk = false;
        std::array<Material, 256> Materials{}; // by colour index
        bool HasMaterials = false;             // a MATL or MATT chunk was read
        // Visible placed models: hidden transform nodes and hidden layers are left out; of an animation (a shape's frames, a transform's
        // frames) frame 0 only. Files without a scene graph (before MagicaVoxel 0.99), whose several models are an animation's frames,
        // get one instance of the first model at the origin.
        std::vector<Instance> Instances;
        bool HasSceneGraph = false;
    };

    // MagicaVoxel's default palette (colour index -> 0xAABBGGRR), used when a file has no RGBA chunk.
    const std::array<uint32_t, 256>& DefaultPalette();

    // False with `error` set when the data is not a readable .vox (`scene` is then unspecified).
    bool ReadVox(const uint8_t* data, size_t size, Scene& scene, std::string& error);
    bool ReadVoxFile(const std::filesystem::path& path, Scene& scene, std::string& error);
}
