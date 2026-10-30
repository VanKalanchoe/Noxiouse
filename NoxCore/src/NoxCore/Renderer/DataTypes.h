#pragma once
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "GeometryArena.h"
#include <glm/glm.hpp>
#include <glm/detail/type_quat.hpp>

namespace shaderio // Shader IO namespace -- shared layout between C++ and shaders
{
    using namespace glm; // GLSL-style types without the glm:: prefix inside the namespace
    #include "shaderIO.h"
}

namespace Nox
{
    // A submesh's morph targets as the renderer keeps them: their names and default weights (an entity's weights are by name), shared
    // by every instance of the submesh.
    struct MorphTargetInfo
    {
        std::vector<std::string> Names;
        std::vector<float> DefaultWeights;
    };

    struct MeshHandle
    {
        GeometryRange vertices;
        GeometryRange meshletDraws;
        GeometryRange meshletBounds; // one per meshlet, in draw order
        GeometryRange meshletVertices;
        GeometryRange meshletTriangles;
        GeometryRange rtIndices; // flat triangle list for the BLAS build and hit shading
        uint32_t blasId = UINT32_MAX;
        uint32_t gpuSceneMesh = UINT32_MAX; // GPU scene mesh slot
        // Geometry streaming (§5.7.2): the mesh's range of the page table, and its GeometryStreamer id (UINT32_MAX: not streamed).
        GeometryRange geometryPages;
        uint32_t streamingId = UINT32_MAX;
        // Skinned (bone weights): its bone influences in the skin stream, at the same element range as its vertices (§5.8.2).
        bool skin = false;
        // Vertex colors (MeshData::VertexColors): in the color stream at the same element range as its vertices.
        bool colors = false;
        // Morph targets: per vertex a GpuMorphRange in the morph range stream (the vertices' element range), the deltas they point
        // at in their own range of the morph delta stream; null when it has none.
        std::shared_ptr<const MorphTargetInfo> morphs;
        bool morphRanges = false;
        GeometryRange morphDeltas;
        // Local axis-aligned bounds of the vertices, worked out at upload (not saved): the editor's size readout and measuring use them.
        glm::vec3 boundsMin{ 0.0f };
        glm::vec3 boundsMax{ 0.0f };
        bool hasBounds = false;
        
        uint32_t GetFirstMeshlet() const { return meshletDraws.offset; }
        uint32_t GetMeshletCount() const { return meshletDraws.count; }
        bool IsValid() const { return vertices.count > 0; }
    };
    
    enum class AlphaMode : uint32_t
    {
        Opaque = 0,
        Mask   = 1,
        Blend  = 2
    };
    
    struct MaterialData
    {
        std::string Name;
        
        // Workflow: 0.0f = Metallic-Roughness, 1.0f = Specular-Glossiness
        float Workflow = 0.0f;
        
        // Specular-Glossiness Properties
        glm::vec4 DiffuseFactor = glm::vec4(1.0f);
        glm::vec4 SpecularFactor = glm::vec4(1.0f); // rgb: Specular Factor, a: Glossiness Factor
        
        // Base Color
        glm::vec4 BaseColorFactor = glm::vec4(1.0f);
        std::string BaseColorTexturePath;
        int32_t BaseColorTextureSet = 0;
        
        // PBR Properties (Metallic-Roughness). MeshImporter only reads a primitive's actual glTF material when one is
        // assigned (primitive.material >= 0); a primitive with none stays at these defaults. RoughnessFactor = 0.0
        // used to mean every such primitive rendered as a perfect mirror instead of a plain matte surface -- any mesh
        // exported without an explicit material (an easy thing to miss in Blender) turned into a "disco ball"
        // reflecting the environment. 1.0 (fully rough) is glTF's own spec default for roughnessFactor and the sane
        // fallback either way. MetallicFactor stays 0 (a non-metal fallback, also matching RTXPT/Donut's convention);
        // MeshImporter also corrects the vendored parser's synthetic 1/1 values for textureless transmission-only
        // materials without changing the vendor library.
        float MetallicFactor = 0.0f;
        float RoughnessFactor = 1.0f;
        std::string MetallicRoughnessTexturePath;
        int32_t PhysicalDescriptorTextureSet = 0;
        
        // Additional Maps
        std::string NormalTexturePath;
        int32_t NormalTextureSet = 0;
        
        std::string OcclusionTexturePath;
        int32_t OcclusionTextureSet = 0;
        
        // Emission
        glm::vec3 EmissiveFactor = glm::vec3(0.0f);
        std::string EmissiveTexturePath;
        int32_t EmissiveTextureSet = 0;
        float emissiveStrength = 1.0f;
        
        // Transmission (KHR_materials_transmission)
        float TransmissionFactor = 0.0f;
        std::string TransmissionTexturePath;
        int32_t TransmissionTextureSet = 0;

        // Index of Refraction (KHR_materials_ior), glTF spec default is 1.5
        float IOR = 1.5f;

        // Volume thickness (KHR_materials_volume), glTF spec default is 0 (infinitely thin).
        // Used by the hybrid RT transmission path to model this material as a thin slab of this
        // depth along the refracted ray instead of ray-tracing the mesh's own real back-face/bevel
        // geometry as the exit surface - the real geometry is fine for a plain box's flat faces but
        // breaks down near beveled/rounded edges, where the local surface isn't parallel to the
        // entry face and can bend or total-internal-reflect the ray in unintended directions.
        float Thickness = 0.0f;
        // Volume absorption (KHR_materials_volume): light crossing AttenuationDistance (world units) of the volume keeps
        // AttenuationColor of itself (Beer-Lambert). Distance 0 = infinite: no absorption, the glTF default.
        glm::vec3 AttenuationColor = glm::vec3(1.0f);
        float AttenuationDistance = 0.0f;
        // Diffuse transmission (KHR_materials_diffuse_transmission; RTXPT's DiffuseTransmissionFactor): how much of the diffuse
        // light goes through a thin surface to its other side -- leaves and cloth lit from behind. 0: none.
        float DiffuseTransmission = 0.0f;
        // shaderio::PathTracer* flags (thin surface, excluded from shadow rays, from an RTXPT material file).
        uint32_t PathTracerFlags = 0;
        // Depth bias (as HDRP's Mesh Decal View Bias, the decal materials' depth offset of the engine the Bistro came from): the
        // rasterizer draws the surface this much closer to the camera along the view ray, in world units, so geometry modelled onto
        // another surface (decals, trims, overlay layers lying in its plane) shows over it instead of z-fighting. Where it lands on
        // screen does not change. Tiny: just past the rounding between the two (~0.05 mm) -- the Bistro's edge decals have other
        // details 1.7 mm in front of them, which a millimetre of bias jumped over. 0: none.
        float DepthBias = 0.0f;

        // Alpha properties
        AlphaMode Mode = AlphaMode::Opaque;
        float AlphaMaskCutoff = 0.5f;
        bool DoubleSided = false;
        bool Unlit = false;
    };
    
    enum class GltfLightType : uint32_t
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    struct LightNodeData
    {
        std::string Name;
        GltfLightType Type = GltfLightType::Point;
        int32_t NodeIndex = -1;
        glm::vec3 Color = glm::vec3(1.0f);
        float Intensity = 1.0f;
        float Range = 10.0f;
        float InnerConeAngle = 0.0f;  // in degrees
        float OuterConeAngle = 45.0f; // in degrees

        glm::vec3 Translation = glm::vec3(0.0f);
        glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);
    };

    enum class GltfCameraType : uint32_t
    {
        Perspective = 0,
        Orthographic = 1
    };

    // glTF camera attached to a node. The aspect ratio is not stored: scene cameras follow the viewport aspect.
    struct CameraNodeData
    {
        std::string Name;
        GltfCameraType Type = GltfCameraType::Perspective;
        int32_t NodeIndex = -1;
        float VerticalFov = 0.785398f;  // radians (glTF perspective.yfov)
        float OrthographicSize = 10.0f; // full height (2 * glTF orthographic.ymag)
        float NearClip = 0.1f;
        float FarClip = 1000.0f;        // glTF zfar; infinite perspective cameras keep this default

        glm::vec3 Translation = glm::vec3(0.0f);
        glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);
    };

    struct MeshNodeData
    {
        std::string Name;
        // -1: a root. FileLayoutParent: a per-mesh asset's "where the file put this mesh" instance (world transform); those
        // nodes only spawn when the model instance asks for the file layout (ModelInstanceComponent::AtFileLayout).
        static constexpr int32_t FileLayoutParent = -2;
        int32_t Parent = -1;

        glm::vec3 Translation = glm::vec3(0.0f);
        glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);

        uint32_t FirstSubmesh = UINT32_MAX;
        uint32_t SubmeshCount = 0;
        bool Skinned = false; // import time only (not cooked): the node's mesh is skinned by a glTF skin
        int32_t MeshIndex = -1;  // import time only: the glTF mesh this node instances
        std::string MeshName;    // import time only: that mesh's name
    };
    
    // A streaming page of a mesh's cluster DAG (§5.7.2, Nanite's pages): whole groups of clusters with their own vertices, the
    // unit geometry is loaded and evicted in. A page's clusters, vertex block and cluster vertex/triangle lists are contiguous
    // ranges of the MeshData arrays, in page order; its clusters index its own vertex block (MeshletDraw::globalVertexOffset is
    // the block's first vertex). Saved as is in the cooked file (MeshSerializer).
    struct MeshPage
    {
        static constexpr uint32_t Root = 1u << 0; // always resident: holds groups nothing coarser replaces
        static constexpr uint32_t Colors = 1u << 1; // its mesh has vertex colors: a color block follows the vertex block

        uint32_t FirstCluster = 0;
        uint32_t ClusterCount = 0;
        uint32_t FirstVertex = 0;
        uint32_t VertexCount = 0;
        uint32_t FirstMeshletVertex = 0;
        uint32_t MeshletVertexCount = 0;
        uint32_t FirstMeshletTriangle = 0; // bytes
        uint32_t MeshletTriangleCount = 0;
        // Pages that must be resident while this one is: those holding the coarser clusters its groups were simplified into.
        uint32_t FirstDependency = 0;
        uint32_t DependencyCount = 0;
        // Conservative LOD bounds of its groups: a sphere around each group's simplified bounds and the largest error. The page
        // is wanted wherever that error projects above the LOD threshold (it can only over-ask).
        glm::vec3 LodCenter{ 0.0f };
        float LodRadius = 0.0f;
        float LodError = 0.0f;
        uint32_t Flags = 0;
        // Where its data starts in the cooked file: the vertex block (shaderio::PackedVertex), the color block (one packed RGBA8 per
        // vertex, only with Colors), the cluster vertex lists and the triangles, each padded to 16 bytes (PayloadBytes). Absolute once
        // read.
        uint64_t PayloadOffset = 0;

        bool IsRoot() const { return (Flags & Root) != 0; }
        bool HasColors() const { return (Flags & Colors) != 0; }
        static uint64_t Align16(uint64_t bytes) { return (bytes + 15) / 16 * 16; }
        uint64_t VertexBlockBytes() const { return Align16(sizeof(shaderio::PackedVertex) * uint64_t(VertexCount)); }
        uint64_t ColorBlockBytes() const { return HasColors() ? Align16(sizeof(uint32_t) * uint64_t(VertexCount)) : 0; }
        uint64_t MeshletVertexBlockBytes() const { return Align16(sizeof(uint32_t) * uint64_t(MeshletVertexCount)); }
        // Where each block starts in the payload.
        uint64_t ColorBlockOffset() const { return VertexBlockBytes(); }
        uint64_t MeshletVertexBlockOffset() const { return VertexBlockBytes() + ColorBlockBytes(); }
        uint64_t TriangleBlockOffset() const { return MeshletVertexBlockOffset() + MeshletVertexBlockBytes(); }
        uint64_t PayloadBytes() const { return TriangleBlockOffset() + Align16(MeshletTriangleCount); }
    };
    static_assert(sizeof(MeshPage) == 72, "MeshPage is written to the cooked file as is");

    // A morph target (glTF's primitive targets; Unreal's morph targets, Blender's shape keys): how far each vertex's position and
    // normal move at weight 1. The weighted targets are added to the vertices before skinning (ComputeSkinning.slang).
    struct MorphTarget
    {
        std::string Name;
        float DefaultWeight = 0.0f;       // the glTF mesh's weights: the shape it is drawn with when nothing sets the weight
        std::vector<glm::vec3> Positions; // per vertex of the mesh (MeshData::Vertices, same order), zero for an unmoved one
        std::vector<glm::vec3> Normals;
    };

    struct MeshData
    {
        std::string Name;
        std::vector<shaderio::Vertex> Vertices;
        std::vector<shaderio::MeshletBounds> Bounds;
        std::vector<shaderio::MeshletDraw> Draws;
        std::vector<uint32_t> MeshletVertices;
        std::vector<uint8_t> MeshletTriangles;

        // Streaming pages (§5.7.2), empty for geometry without a cluster DAG (one level, every cluster always resident). The
        // root pages come first.
        std::vector<MeshPage> Pages;
        std::vector<uint32_t> PageDependencies;
        uint32_t RootPageCount = 0;
        // Local bounds of all the vertices (paged geometry: the vertex arrays may hold only the root pages).
        glm::vec3 BoundsMin{ 0.0f };
        glm::vec3 BoundsMax{ 0.0f };
        // Vertex colors (glTF COLOR_0, Vertex::color): kept, in the color stream, only when set; the vertices' colors are ignored
        // (white) otherwise.
        bool VertexColors = false;
        // Morph targets: a mesh with them is one level (no cluster LOD, as skinned geometry: a simplified surface would move wrongly).
        std::vector<MorphTarget> Morphs;
        // Set when only the root pages were read: Vertices, MeshletVertices and MeshletTriangles hold just those, the other pages
        // stream from this cooked file (MeshPage::PayloadOffset).
        std::filesystem::path StreamedFrom;

        bool IsStreamed() const { return !StreamedFrom.empty() && RootPageCount < Pages.size(); }
    };

    // The geometry stream's vertex format (shaderio::PackedVertex, §5.8.2): packed when a mesh is cooked (version 4) or uploaded,
    // unpacked when a cook is read (the importer, HLOD bake and editor tools work on shaderio::Vertex). Mirrors unpackVertex in
    // shaderIO.h.
    namespace VertexPacking
    {
        // Octahedral: the unit vector onto |x| + |y| + |z| = 1, the lower half folded over the upper (each component in [-1, 1]).
        inline glm::vec2 OctEncode(glm::vec3 n)
        {
            const float sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
            if (sum <= 0.0f)
                return glm::vec2(0.0f, 0.0f); // a zero vector: decodes as +Z
            n /= sum;
            if (n.z < 0.0f)
            {
                const glm::vec2 folded((1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
                return folded;
            }
            return glm::vec2(n.x, n.y);
        }
        inline glm::vec3 OctDecode(glm::vec2 e)
        {
            glm::vec3 n(e.x, e.y, 1.0f - std::abs(e.x) - std::abs(e.y));
            const float t = std::clamp(-n.z, 0.0f, 1.0f);
            n.x += n.x >= 0.0f ? -t : t;
            n.y += n.y >= 0.0f ? -t : t;
            return glm::normalize(n);
        }
        // [-1, 1] to an unsigned integer of `bits` bits, rounded.
        inline uint32_t Quantize(float value, uint32_t bits)
        {
            const float scale = float((1u << bits) - 1u);
            return static_cast<uint32_t>(std::lround(std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f) * scale));
        }
        inline float Dequantize(uint32_t value, uint32_t bits) { return float(value) * (2.0f / float((1u << bits) - 1u)) - 1.0f; }

        inline shaderio::PackedVertex Pack(const shaderio::Vertex& vertex)
        {
            shaderio::PackedVertex packed{};
            packed.pos = vertex.pos;
            const glm::vec2 normal = OctEncode(vertex.normal);
            packed.normal = Quantize(normal.x, 16) | (Quantize(normal.y, 16) << 16);
            packed.uv0 = vertex.uv0;
            packed.uv1 = vertex.uv1;
            const glm::vec2 tangent = OctEncode(glm::vec3(vertex.tangent));
            packed.tangent = Quantize(tangent.x, 15) | (Quantize(tangent.y, 15) << 15) | (vertex.tangent.w < 0.0f ? 0x80000000u : 0u);
            return packed;
        }
        inline shaderio::Vertex Unpack(const shaderio::PackedVertex& packed)
        {
            shaderio::Vertex vertex{};
            vertex.pos = packed.pos;
            vertex.normal = OctDecode(glm::vec2(Dequantize(packed.normal & 0xFFFFu, 16), Dequantize(packed.normal >> 16, 16)));
            vertex.uv0 = packed.uv0;
            vertex.uv1 = packed.uv1;
            vertex.tangent = glm::vec4(OctDecode(glm::vec2(Dequantize(packed.tangent & 0x7FFFu, 15), Dequantize((packed.tangent >> 15) & 0x7FFFu, 15))),
                                       (packed.tangent & 0x80000000u) != 0 ? -1.0f : 1.0f);
            return vertex;
        }

        // Bone influences: 16-bit indices, 16-bit unorm weights (renormalized so they still sum to 1 after rounding).
        inline shaderio::SkinVertex PackSkin(const shaderio::Vertex& vertex)
        {
            const glm::vec4 weights = vertex.boneWeights;
            const float sum = weights.x + weights.y + weights.z + weights.w;
            const glm::vec4 normalized = sum > 0.0f ? weights / sum : glm::vec4(0.0f);
            uint32_t q[4];
            uint32_t total = 0;
            for (int i = 0; i < 4; ++i)
            {
                q[i] = static_cast<uint32_t>(std::lround(std::clamp(normalized[i], 0.0f, 1.0f) * 65535.0f));
                total += q[i];
            }
            if (sum > 0.0f && total != 65535u)
            {
                // The rounding error onto the largest weight.
                int largest = 0;
                for (int i = 1; i < 4; ++i)
                    largest = q[i] > q[largest] ? i : largest;
                q[largest] = static_cast<uint32_t>(std::clamp<int64_t>(int64_t(q[largest]) + 65535 - int64_t(total), 0, 65535));
            }
            shaderio::SkinVertex skin{};
            skin.boneIDs01 = (vertex.boneIDs.x & 0xFFFFu) | ((vertex.boneIDs.y & 0xFFFFu) << 16);
            skin.boneIDs23 = (vertex.boneIDs.z & 0xFFFFu) | ((vertex.boneIDs.w & 0xFFFFu) << 16);
            skin.boneWeights01 = q[0] | (q[1] << 16);
            skin.boneWeights23 = q[2] | (q[3] << 16);
            return skin;
        }
        inline void UnpackSkin(const shaderio::SkinVertex& skin, shaderio::Vertex& vertex)
        {
            vertex.boneIDs = glm::uvec4(skin.boneIDs01 & 0xFFFFu, skin.boneIDs01 >> 16, skin.boneIDs23 & 0xFFFFu, skin.boneIDs23 >> 16);
            vertex.boneWeights = glm::vec4(float(skin.boneWeights01 & 0xFFFFu), float(skin.boneWeights01 >> 16), float(skin.boneWeights23 & 0xFFFFu),
                                           float(skin.boneWeights23 >> 16)) / 65535.0f;
        }
        // Vertex colors as the color stream holds them: RGB sRGB-encoded 8 bits each (precision where the eye sees it, as a texture's),
        // A linear 8 bits. Mirrors unpackVertexColor in shaderIO.h.
        inline uint32_t PackColor(const glm::vec4& color)
        {
            auto encode = [](float linear)
            {
                linear = std::clamp(linear, 0.0f, 1.0f);
                const float srgb = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                return static_cast<uint32_t>(std::lround(srgb * 255.0f));
            };
            const uint32_t alpha = static_cast<uint32_t>(std::lround(std::clamp(color.a, 0.0f, 1.0f) * 255.0f));
            return encode(color.r) | (encode(color.g) << 8) | (encode(color.b) << 16) | (alpha << 24);
        }
        inline glm::vec4 UnpackColor(uint32_t packed)
        {
            auto decode = [](uint32_t value)
            {
                const float srgb = float(value) / 255.0f;
                return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
            };
            return glm::vec4(decode(packed & 0xFFu), decode((packed >> 8) & 0xFFu), decode((packed >> 16) & 0xFFu), float(packed >> 24) / 255.0f);
        }

        // Whether any vertex has a bone influence (a skinned mesh: its influences go to the skin stream).
        inline bool HasSkin(const std::vector<shaderio::Vertex>& vertices)
        {
            for (const shaderio::Vertex& vertex : vertices)
            {
                if (vertex.boneWeights.x + vertex.boneWeights.y + vertex.boneWeights.z + vertex.boneWeights.w > 0.0f)
                    return true;
            }
            return false;
        }
    }
}
