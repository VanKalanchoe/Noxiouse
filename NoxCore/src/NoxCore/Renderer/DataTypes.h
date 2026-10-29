#pragma once
#include <filesystem>
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
        // Where its data starts in the cooked file: the vertex block, the cluster vertex lists and the triangles, each padded
        // to 16 bytes (PayloadBytes). Absolute once read.
        uint64_t PayloadOffset = 0;

        bool IsRoot() const { return (Flags & Root) != 0; }
        static uint64_t Align16(uint64_t bytes) { return (bytes + 15) / 16 * 16; }
        uint64_t VertexBlockBytes() const { return Align16(sizeof(shaderio::Vertex) * uint64_t(VertexCount)); }
        uint64_t MeshletVertexBlockBytes() const { return Align16(sizeof(uint32_t) * uint64_t(MeshletVertexCount)); }
        uint64_t PayloadBytes() const { return VertexBlockBytes() + MeshletVertexBlockBytes() + Align16(MeshletTriangleCount); }
    };
    static_assert(sizeof(MeshPage) == 72, "MeshPage is written to the cooked file as is");

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
        // Set when only the root pages were read: Vertices, MeshletVertices and MeshletTriangles hold just those, the other pages
        // stream from this cooked file (MeshPage::PayloadOffset).
        std::filesystem::path StreamedFrom;

        bool IsStreamed() const { return !StreamedFrom.empty() && RootPageCount < Pages.size(); }
    };
}
