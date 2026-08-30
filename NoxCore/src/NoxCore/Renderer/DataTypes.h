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
        glm::vec4 AlbedoColor = glm::vec4(1.0f);
        std::string AlbedoTexturePath;
        
        // Alpha properties
        AlphaMode Mode = AlphaMode::Opaque;
        
        float AlphaCutoff = 0.5f;
        bool DoubleSided = false;
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
