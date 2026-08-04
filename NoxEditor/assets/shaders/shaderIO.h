
#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

#ifdef __SLANG__
typealias vec2 = float2;
typealias vec3 = float3;
typealias vec4 = float4;
typealias uvec4 = uint4;
typealias mat4 = float4x4;
#define STATIC_CONST static const
#else
#define STATIC_CONST const
#endif

STATIC_CONST uint32_t TASK_SHADER_DISPATCH_X = 64;
STATIC_CONST uint32_t MESH_SHADER_DISPATCH_X = 32;
STATIC_CONST uint32_t MAX_VERTICES            = 64;
STATIC_CONST uint32_t MAX_PRIMITIVES          = 64;

// Per https://developer.nvidia.com/blog/using-mesh-shaders-for-professional-graphics/
// Task shader output should ideally be below 236/108. We can hit the 108 if we turn uint32_t to uint8_t and store offsets in the payload.
// 4 + 64 = 68
struct MeshletPayload
{
    uint32_t drawID;
    uint32_t groupMeshletOffset;
    uint8_t  meshletIndices[TASK_SHADER_DISPATCH_X];
};

struct Frustum
{
    vec4 planes[6];

#ifndef __SLANG__
    Frustum() = default;

    explicit Frustum(const mat4& viewProj)
    {
        planes[0] = glm::vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]
    );

    planes[1] = glm::vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]
    );

    planes[2] = glm::vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]
    );

    planes[3] = glm::vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]
    );

    planes[4] = glm::vec4(
        viewProj[0][3] + viewProj[0][2],
        viewProj[1][3] + viewProj[1][2],
        viewProj[2][3] + viewProj[2][2],
        viewProj[3][3] + viewProj[3][2]
    );

    planes[5] = glm::vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]
    );

    for (auto& plane : planes) {
        float length = glm::length(glm::vec3(plane));
        plane /= length;
    }
    }
#endif
};

struct UniformBufferObject 
{
    mat4 view;
    mat4 proj;

    mat4 frozenView;
    mat4 frozenProj;

    vec4 cameraWorldPos;
    vec4 frozenCameraWorldPos;

    Frustum frustum;
    Frustum frozenFrustum;

    uint samplerIndex;
    uint imageHeapIndexOffset;
    uint finalImageIndex;
};

struct Vertex
{
    vec3 pos;
    vec2 texCoord;
    
    uvec4 boneIDs;
    vec4 boneWeights;
};

struct InstanceData
{
    // Mesh
    mat4 modelMatrix;

    // --- NEW: Page Information ---
    uint32_t drawsPageIndex;            // Same index used for meshletBounds (1:1 allocation)
    uint32_t drawsOffset;               // Where this model's meshlets start in the page
    uint32_t meshletCount;              // How many meshlets this model has
    
    uint32_t verticesPageIndex;
    uint32_t meshletVerticesPageIndex;
    uint32_t meshletTrianglesPageIndex;
    // -----------------------------
    
    // Material
    vec4 albedoColor;
    uint32_t albedoTextureIndex;
    
    // MeshAnimation
    uint32_t boneMatrixOffset = 0xFFFFFFFF;

    // Editor-only
    int entityID;
};

struct PushConstantMeshlets
{
    uint64_t matrixReference;
    uint64_t instanceReference;
    uint64_t boneMatrixReference;
    // These now point to the Page Table buffers (array of uint64_t BDAs)
    uint64_t vertexPageTableReference;
    uint64_t meshletBoundsPageTableReference;
    uint64_t meshletDrawsPageTableReference;
    uint64_t meshletVerticesPageTableReference;
    uint64_t meshletTrianglesPageTableReference;
};

// Meshlet Global stores all meshes
// Buffer 1: Read ONLY by Task Shader (32 Bytes -> 2 fit in 1 cache line!)
struct MeshletBounds
{
    vec3 center;
    float radius;
    vec3 coneApex;
    float coneCutoff;
    vec3 coneAxis;
};

// Buffer 2: Read ONLY by Mesh Shader (24 Bytes)
struct MeshletDraw
{
    uint32_t vertexOffset;          // Global offset into meshletVertices
    uint32_t triangleOffset;        // Global offset into meshletTriangles
    uint32_t vertexCount;           // Vertices in this meshlet (max 64)
    uint32_t triangleCount;         // Triangles in this meshlet (max 124)
    uint32_t globalVertexOffset;    // Base vertex offset in primary vertex buffer
};


struct PushConstantQuad
{
    uint64_t matrixReference;
    uint64_t quadDataReference;
    uint numOfElements;
};
struct QuadData
{
    mat4 modelMatrix;
    vec4 color;
    uint32_t materialIndex;

    // Editor-only
    int entityID;
};

struct PushConstantCircle
{
    uint64_t matrixReference;
    uint64_t circleDataReference;
    uint numOfElements;
};
struct CircleData
{
    mat4 worldPosition;
    vec4 color;
    float thickness;
    float fade;
    
    // Editor-only
    int entityID;
};

struct PushConstantText
{
    uint64_t matrixReference;
    uint64_t textDataReference;
    uint numOfElements;
};
struct TextData
{
    mat4 transform;
    vec2 quadMin;
    vec2 quadMax;
    vec2 texMin;
    vec2 texMax;
    vec4 color;
    uint32_t materialIndex;

    // TODO: bg color for outline/bg
    
    // Editor-only
    int entityID;
};

struct PushConstantLine
{
    uint64_t matrixReference;
    uint64_t lineDataReference;
    uint numOfElements;
};
struct LineData
{
    vec3 p0;
    vec3 p1;
    vec4 color;
    
    // Editor-only
    int entityID;
};


#endif  // HOST_DEVICE_H