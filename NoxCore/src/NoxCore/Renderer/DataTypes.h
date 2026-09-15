#pragma once
#include <string>
#include <glm/glm.hpp>
#include <glm/detail/type_quat.hpp>

namespace shaderio // Shader IO namespace -- shared layout between C++ and shaders
{
    using namespace glm; // GLSL-style types without the glm:: prefix inside the namespace
    #include "shaderIO.h"
}

namespace Nox
{
    struct BufferAllocation
    {
        uint32_t pageIndex = UINT32_MAX;
        uint32_t offset = 0;
        uint32_t count = 0;
        
        bool IsValid() const { return count > 0; };
    };
    
    struct MeshHandle
    {
        BufferAllocation vertices;
        BufferAllocation meshletDraws; // Also used for bounds (1:1 ratio)
        BufferAllocation meshletVertices;
        BufferAllocation meshletTriangles;
        uint32_t blasId = UINT32_MAX;
        
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
        
        // PBR Properties (Metallic-Roughness)
        float MetallicFactor = 1.0f;
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
        int32_t Parent = -1;

        glm::vec3 Translation = glm::vec3(0.0f);
        glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 Scale = glm::vec3(1.0f);

        uint32_t FirstSubmesh = UINT32_MAX;
        uint32_t SubmeshCount = 0;
    };
    
    struct MeshData
    {
        std::string Name;
        std::vector<shaderio::Vertex> Vertices;
        std::vector<shaderio::MeshletBounds> Bounds;
        std::vector<shaderio::MeshletDraw> Draws;
        std::vector<uint32_t> MeshletVertices;
        std::vector<uint8_t> MeshletTriangles;
    };
}
