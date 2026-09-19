
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

enum SamplerIndex : uint32_t
{
    SAMPLER_LINEAR_REPEAT = 0,
    SAMPLER_NEAREST_REPEAT = 1,
    
    SAMPLER_BRDFLUT = 2,
    SAMPLER_IRRADIANCE = 3,
    SAMPLER_PREFILTER = 4,
    SAMPLER_LINEAR_CLAMP = 5,

    SamplerCount
};

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

// GPU scene (§5.5): persistent tables indexed by slot, updated only where something changed (Renderer GpuScene).
// An instance slot indexes both GpuInstance and GpuTransform; instances reference shared mesh and material slots.
struct GpuInstance
{
    uint32_t meshIndex;        // GpuMesh slot
    uint32_t materialIndex;    // GpuMaterial slot
    uint32_t boneMatrixOffset; // 0xFFFFFFFF: not skinned
    int32_t entityID;          // editor picking
};

struct GpuTransform
{
    mat4 world;
    mat4 normal;        // transpose(inverse(world))
    mat4 previousWorld; // world of the previous frame (per-object motion vectors)
};

struct GpuMaterial
{
    // Material Workflow & SpecGloss properties
    float workflow; // 0.0 = Metallic-Roughness, 1.0 = Specular-Glossiness
    vec4 diffuseFactor;
    vec4 specularFactor; // rgb: specular factor, a: glossiness factor

    vec4 baseColorFactor; // diffuseFactor for Specular-Glossiness
    uint32_t baseColorTextureIndex;
    int32_t baseColorTextureSet;

    float metallicFactor;
    float roughnessFactor;
    uint32_t metallicRoughnessTextureIndex;
    int32_t physicalDescriptorTextureSet;

    uint32_t normalTextureIndex;
    int32_t normalTextureSet;

    uint32_t occlusionTextureIndex;
    int32_t occlusionTextureSet;

    vec3 emissiveFactor;
    uint32_t emissiveTextureIndex;
    int32_t emissiveTextureSet;
    float emissiveStrength;

    // Transmission (KHR_materials_transmission)
    float transmissionFactor;
    uint32_t transmissionTextureIndex;
    int32_t transmissionTextureSet;

    // Index of Refraction (KHR_materials_ior), glTF spec default is 1.5
    float ior;

    // Volume thickness (KHR_materials_volume), glTF spec default is 0.0 (infinitely thin)
    float thickness;

    uint32_t alphaMode;   // 0 = Opaque, 1 = Mask, 2 = Blend
    float alphaMaskCutoff;
    uint32_t doubleSided;
    uint32_t unlit;
};

// Ray tracing view of an instance (same slot as GpuInstance): everything the ray tracing shaders read at candidates and
// hits, in one record. Ray queries look instances up per candidate triangle; reading instance -> material -> mesh ->
// transform from separate tables there made ReSTIR GI ~30% slower than one record (Bistro, §5.5.4).
struct GpuRayTracingInstance
{
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
    mat4 normalMatrix;              // transpose(inverse(world))
    vec4 baseColorFactor;
    vec4 emissiveFactor;            // rgb: color, a: strength
    uint32_t baseColorTextureIndex;
    float alphaCutoff;
    uint32_t alphaMode;             // 0 = Opaque, 1 = Mask, 2 = Blend
    uint32_t doubleSided;
    float metallicFactor;
    float roughnessFactor;
    uint32_t metallicRoughnessTextureIndex;
    uint32_t normalTextureIndex;
    float transmissionFactor;
    uint32_t transmissionTextureIndex;
    float workflow;                 // 0.0 = MetalRough, 1.0 = SpecGloss
    float ior;
    float thickness;
};

struct GpuMesh
{
    // Ranges inside the unified geometry streams (§5.8.2). The vertex, meshlet vertex and meshlet triangle offsets are
    // baked into this mesh's MeshletDraw records at upload, so only these two are needed per mesh.
    uint32_t drawsOffset;
    uint32_t boundsOffset;
    uint32_t meshletCount;
    // Ray tracing (the BLAS inputs): offsets into the vertex and RT index streams, 0xFFFFFFFF when the mesh has none.
    // The instance records the hit shaders read hold the resulting addresses (one flat record, §5.5.4 3b).
    uint32_t verticesOffset;
    uint32_t indicesOffset;

    uint32_t triangleCount; // of all meshlets (stats)

    // Local bounds (GPU culling)
    vec4 boundsSphere; // xyz: center, w: radius
    vec3 boundsMin;
    float padding0;
    vec3 boundsMax;
    float padding1;
};

struct UniformBufferObject 
{
    mat4 view;
    mat4 proj; // Jittered projection (for rasterization)
    mat4 invViewProj;

    // --- Previous Frame Separate Matrices ---
    mat4 prevView;
    mat4 nonJitteredProj; // Clean projection (for motion vectors)
    mat4 prevProj;        // Clean previous projection (for motion vectors)
    // ----------------------------------------

    mat4 frozenView;
    mat4 frozenProj;

    vec4 cameraWorldPos;
    vec4 frozenCameraWorldPos;

    Frustum frustum;
    Frustum frozenFrustum;

    // --- Temporal & Motion Vector Matrices ---
    vec2 jitterOffset;     // Subpixel jitter in pixels
    vec2 prevJitterOffset; // Previous frame jitter in pixels
    //

    uint samplerIndex;
    uint imageHeapIndexOffset;
    uint finalImageIndex;

    uint entityTextureIndex; // Heap index of m_entityResolveReference for edge detection outline (display resolution)
    uint entityGBufferTextureIndex; // Heap index of the render-resolution G-buffer entity ID texture (debug views only)

    // PBR IBL
    uint irradianceMapIndex;
    uint prefilteredMapIndex;
    float prefilteredCubeMipLevels;
    uint brdfLutIndex;
    float exposure;
	float gamma;
    float scaleIBLAmbient;

    // Lighting
    uint64_t lightDataReference;
    uint32_t lightCount;
    uint32_t enableRTShadows;

    // Ray Tracing
    uint64_t tlasDeviceAddress;
    uint32_t tlasHeapIndex;
    uint32_t enableRTReflections;
    // Unified geometry streams (§5.8.2): one buffer per stream, the mesh table and the meshlet draw records index into
    // them with offsets, so growing a stream only changes these addresses.
    uint64_t geometryVerticesReference;
    uint64_t geometryMeshletDrawsReference;
    uint64_t geometryMeshletBoundsReference;
    uint64_t geometryMeshletVerticesReference;
    uint64_t geometryMeshletTrianglesReference;

    // GPU scene tables (GpuInstance, GpuTransform, GpuMaterial, GpuMesh, GpuRayTracingInstance); 0 while the scene is empty
    uint64_t sceneInstancesReference;
    uint64_t sceneTransformsReference;
    uint64_t sceneMaterialsReference;
    uint64_t sceneMeshesReference;
    uint64_t sceneRayTracingInstancesReference; // GpuRayTracingInstance per instance slot

    // Temporal
    uint32_t frameIndex;
    uint32_t enableDDGI;
    uint32_t ddgiIrradianceTextureIndex;
    uint32_t ddgiDistanceTextureIndex;

    // DDGI (Dynamic Diffuse Global Illumination)
    vec4 ddgiGridOrigin;   // xyz: origin, w: countX
    vec4 ddgiGridSpacing;  // xyz: spacing, w: countY
    vec4 ddgiGridParams;   // x: countZ, y: raysPerProbe, z: hysteresis, w: normalBias
    vec4 ddgiAtlasParams;  // x: irrWidth, y: irrHeight, z: distWidth, w: distHeight

    // ReSTIR GI (Screen-Space Diffuse Path Resampling via RTXDI)
    uint32_t diffuseGIMode; // 0 = Off (IBL), 1 = DDGI Probes, 2 = ReSTIR GI
    uint32_t restirGIDiffuseTextureIndex;
    uint32_t restirGIReservoirBufferIndex;
    uint32_t restirGINeighborOffsetsBufferIndex;

    // ReSTIR DI (Screen-Space Resampled Direct Lighting via RTXDI) -- like ReSTIR GI above, the
    // actually-consumed copy is the PushConstantDeferredLighting one; these UBO fields exist for
    // parity/other potential consumers, matching the established pattern.
    uint32_t directLightingMode; // 0 = brute-force analytic loop, 1 = ReSTIR DI
    uint32_t restirDIDiffuseTextureIndex;  // de-modulated diffuse / specular direct lighting (NRDFrontEnd.slang)
    uint32_t restirDISpecularTextureIndex;

    // Texture streaming feedback (MipFeedback.slang): the buffer, and the frame counter that picks which pixel of each
    // 8x8 tile reports.
    uint64_t mipFeedbackReference;
    uint32_t mipFeedbackFrame;

    // Cluster LOD (§5.7): the screen-space error a cluster may have (fraction of the render height), the camera's near
    // plane for the error projection, 1 to draw only the original clusters; ClusterStats of the visibility passes.
    float lodErrorThreshold;
    float lodCameraNear;
    uint32_t lodFullDetail;
    uint64_t clusterStatsReference;
};

struct Vertex
{
    vec3 pos;
    vec3 normal;
    vec2 uv0;
	vec2 uv1;
    
    uvec4 boneIDs;
    vec4 boneWeights;
};

enum LightType : uint32_t
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct LightData
{
    vec4 position;     // xyz: World Position, w: LightType (0 = Directional, 1 = Point, 2 = Spot)
    vec4 direction;    // xyz: Normalized Direction, w: Range (0.0 = infinite)
    vec4 color;        // rgb: Color, w: Intensity
    vec4 spotParams;   // x/y: spot cone terms, z: source radius (radians for directional, meters otherwise), w: shadow samples
};

// GPU instance culling (§5.6): per view, the draw list (instance slots in bucket order) is tested against the view and
// compacted per bucket into the visible instance list and one indirect command per visible instance, without atomics:
// visibility flags in parallel, visible counts per block of entries, block offsets and bucket counts, then each block
// writes its entries (entry order, so transparent sorting survives).
STATIC_CONST uint32_t NoGeometryRange = 0xFFFFFFFF; // GpuMesh offsets: this mesh has no range in that stream

// Texture streaming feedback (§5.12): one entry per bindless image slot, the finest mip a frame's pixels asked of that
// image, biased so "finer than the image holds" stays representable; MipFeedbackNone when nothing sampled it.
STATIC_CONST uint32_t MipFeedbackSlots = 4096; // the resource heap's image capacity
STATIC_CONST uint32_t MipFeedbackBias = 16;
STATIC_CONST uint32_t MipFeedbackNone = 0xFFFFFFFF;

STATIC_CONST uint32_t CULL_BLOCK_SIZE = 256;
STATIC_CONST uint32_t CULL_BUCKET_COUNT = 10; // RenderBucket::Count
STATIC_CONST uint32_t CULL_INSTANCES = 1;     // CullView.flags: frustum test (off: every entry is visible)
STATIC_CONST uint32_t CULL_OCCLUSION = 2;     // CullView.flags: Hi-Z occlusion test (§5.6.4, two phases)

// Written by the CPU per view and frame slot.
struct CullView
{
    Frustum frustum;
    mat4 viewProj;         // this frame: phase 2 tests against the pyramid built from this frame's depth
    mat4 previousViewProj; // last frame: phase 1 tests previous transforms against last frame's pyramid
    uint32_t bucketStart[11]; // CULL_BUCKET_COUNT + 1: entry range of each bucket in the draw list
    uint32_t entryCount;
    uint32_t blockCount;
    uint32_t flags;
};

// VkDrawIndirectCommand layout of drawMeshTasksIndirect(Count).
struct MeshTasksIndirectCommand
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};

// Per block of entries (GPU-written).
struct CullBlock
{
    uint32_t visibleCount;
    uint32_t visibleBefore;  // visible entries in all earlier blocks
    uint32_t occludedCount;  // entries that passed the frustum but failed the occlusion test (phase 2 re-tests them)
    uint32_t occludedBefore;
    uint32_t visibleTriangles;
};

// Draw counts per bucket (the count buffer of drawMeshTasksIndirectCount) followed by the visible entries before each
// bucket start (GPU-written).
struct CullCounts
{
    uint32_t drawCount[10];       // CULL_BUCKET_COUNT: phase 1 draws
    uint32_t visibleBefore[11];   // CULL_BUCKET_COUNT + 1
    uint32_t lateDrawCount[10];   // phase 2 candidates per bucket (occluded in phase 1)
    uint32_t occludedBefore[11];
    uint32_t visibleTriangles;    // of the instances phase 1 draws (before meshlet and back face culling)
    uint32_t lateDrawn;           // instances phase 2 draws after re-testing them against this frame's pyramid
    uint32_t lateTriangles;       // of those instances
};

struct PushConstantInstanceCulling
{
    uint64_t matrixReference;           // UniformBufferObject (GPU scene tables)
    uint64_t viewReference;             // CullView
    uint64_t drawListReference;         // uint32_t instance slot per entry
    uint64_t flagsReference;            // uint32_t visible (0/1) per entry
    uint64_t blocksReference;           // CullBlock per block
    uint64_t countsReference;           // CullCounts
    uint64_t visibleInstancesReference; // uint32_t instance slot per visible entry (bucket start + rank)
    uint64_t commandsReference;         // MeshTasksIndirectCommand per visible entry
    uint64_t lateInstancesReference;    // phase 1: the occluded candidates; phase 2 draws them
    uint64_t lateCommandsReference;     // phase 2: their commands (group count 0 while still occluded)
    uint32_t hiZTextureIndex;           // depth pyramid (level 0 is half the render size), 0xFFFFFFFF while there is none
    uint32_t hiZWidth;                  // of level 0
    uint32_t hiZHeight;
    uint32_t hiZMipCount;
};

// Depth pyramid level build (§5.6.4): level 0 reduces the depth buffer, every next level its previous one. Reverse-Z, so
// a level keeps the FARTHEST (smallest) depth of the texels it covers: an instance is occluded only when it is behind
// everything drawn in its screen area.
// DLSS Ray Reconstruction guides (RRGuides.slang): the diffuse and specular albedo the lit image was shaded with.
struct PushConstantRRGuides
{
    uint64_t matrixReference; // UniformBufferObject
    uint32_t depthTextureIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t diffuseAlbedoStorageIndex;
    uint32_t specularAlbedoStorageIndex;
    uint32_t rawReflectionIndex;      // RT reflections (alpha: hit distance), 0xFFFFFFFF when they did not run
    uint32_t hitDistanceStorageIndex; // specular hit distance guide, written when rawReflectionIndex is valid
    uint32_t width;
    uint32_t height;
};

struct PushConstantHiZBuild
{
    uint32_t sourceTextureIndex;
    uint32_t outputStorageIndex;
    uint32_t width;       // of the level being written
    uint32_t height;
    uint32_t sourceMip;
    uint32_t sourceIsDepth; // 1: level 0 from the depth buffer
};

STATIC_CONST uint32_t MESHLET_CULL_FRUSTUM = 1;     // PushConstantMeshlets.meshletCulling
STATIC_CONST uint32_t MESHLET_COUNT_TRIANGLES = 2;  // PushConstantMeshlets.meshletCulling: add the drawn clusters to ClusterStats

struct PushConstantMeshlets
{
    uint64_t matrixReference;
    uint64_t drawInstancesReference; // visible instance slots of this view (instanceBaseIndex + SV_DrawIndex)
    uint64_t boneMatrixReference;
    uint32_t instanceBaseIndex;
    uint32_t meshletCulling; // MESHLET_* flags (MESHLET_CULL_FRUSTUM off: every cluster of the LOD cut is drawn)
};

// What the visibility passes drew after the LOD cut (cleared every frame, read back per frame slot).
struct ClusterStats
{
    uint32_t drawnClusters;
    uint32_t drawnTriangles;
};

STATIC_CONST float ClusterTerminalError = 3.402823466e+38f; // a cluster with no coarser version (FLT_MAX)

// Per cluster (meshlet), read by the task shader. Culling bounds, and the cluster LOD DAG (§5.7, meshoptimizer
// clusterlod): a cluster is drawn when its parent (the coarser version of its group) is too coarse and the cluster
// itself is fine enough -- both errors projected to the screen from the group bounds they were measured on.
struct MeshletBounds
{
    vec3 center;
    float radius;
    vec3 coneApex;
    float coneCutoff;
    vec3 coneAxis;

    vec3 lodCenter;        // bounds of the group this cluster was simplified from (the original clusters: error 0)
    float lodRadius;
    float lodError;
    vec3 parentCenter;     // bounds of the group this cluster was merged into and simplified (ClusterTerminalError: none)
    float parentRadius;
    float parentError;
    uint32_t lodLevel;     // 0: original geometry, n: simplified n times
    uint32_t triangleCount;
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

struct PushConstantEquirect
{
    uint32_t hdrTextureIndex;
    uint32_t cubemapStorageIndex;
    uint32_t cubemapSize;
};

struct PushConstantSkybox
{
    uint64_t matrixReference; // BDA to UniformBufferObject
    uint32_t cubemapIndex;     // Index in descriptor heap
};

struct PushConstantVisibilityDebug
{
    uint64_t matrixReference;
    uint64_t boneMatrixReference;
    uint32_t visibilityTextureIndex;
    uint32_t debugMode; // 0 = Albedo, 1 = Normal, 2 = Roughness, 3 = Metallic, 4 = Emission, 5 = Occlusion, 6 = Colored Meshlets
    vec2 viewportSize;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t gbufferEmissionIndex;
};

// NRD's view Z and packed normal/roughness guides (NRDGuides.slang).
struct PushConstantNRDGuides
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    vec2 viewportSize;
};

struct PushConstantShadowMask
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    vec2 viewportSize;
    uint32_t frameIndex;
};

struct PushConstantReflection
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t visibilityTextureIndex;
    vec2 viewportSize;
    uint32_t frameIndex;
    uint32_t denoiserMode; // 0 = Off, 1 = REBLUR (YCoCg), 2 = RELAX (RGB)
};

struct PushConstantDeferredLighting
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t gbufferEmissionIndex;
    uint32_t depthTextureIndex;
    uint32_t visibilityTextureIndex;
    vec2 viewportSize;
    uint32_t debugMode;
    // 0 = Full PBR, 1 = Direct Lights, 2 = IBL, 3 = World Pos, 4 = Albedo, 5 = Normal, 6 = Roughness, 
    // 7 = Metallic, 8 = Occlusion, 9 = Emission, 10 = World Pos, 11 = Entity ID, 12 = Depth Buffer
    uint32_t gbufferVelocityIndex;

    // Temporal & RT Denoised Buffers
    uint32_t frameIndex; // RR
    uint32_t shadowMaskTextureIndex; // NRD
    uint32_t nrdShadowsEnabled;
    uint32_t reflectionTextureIndex; // NRD REBLUR / RELAX or Raw 1-SPP

    // ReSTIR GI (passed via push constant instead of the UBO -- push-constant-sourced bindless
    // indices are proven reliable elsewhere in this shader; the UBO-sourced restirGIDiffuseTextureIndex
    // read was empirically returning stale/wrong data despite correct C++-side values)
    uint32_t diffuseGIMode;
    uint32_t restirGIDiffuseTextureIndex;
    uint32_t restirGIDenoiserMode; // 0 = Off, 1 = REBLUR (output is YCoCg, needs decoding), 2 = RELAX (plain RGB)

    // ReSTIR DI (screen-space resampled direct lighting, replaces the brute-force light loop below
    // when active -- see directLightingMode)
    uint32_t directLightingMode; // 0 = brute-force analytic loop (existing), 1 = ReSTIR DI
    uint32_t restirDIDiffuseTextureIndex;  // de-modulated diffuse / specular direct lighting (NRDFrontEnd.slang)
    uint32_t restirDISpecularTextureIndex;
    uint32_t restirDIDenoiserMode; // how both were packed: NRD_SIGNAL_RAW / REBLUR / RELAX
};

struct PushConstantPathTracer
{
    mat4 invViewProj;
    uint64_t matrixReference;
    vec2 viewportSize;
    uint32_t frameIndex;
    uint32_t sampleCount;              // Progressive sample counter (1, 2, 3...)
    uint32_t maxBounces;               // Default: 3
    uint32_t accumulationTextureIndex; // Texture slot for previous accumulation
    uint32_t debugMode;                // 16 = 1-SPP, 17 = Progressive Accumulation
    uint32_t skyboxTextureIndex;       // Environment cubemap slot
    uint32_t denoiserMode;              // 0 = Off (plain RGB), 1 = NRD REBLUR (needs YCoCg encode), 2 = NRD RELAX (plain RGB)
    uint32_t restirGIDiffuseTextureIndex;
    uint32_t restirGIDenoiserMode;      // 0 = Off/plain RGB, 1 = REBLUR YCoCg, 2 = RELAX RGB
    uint32_t restirDIDiffuseTextureIndex;  // de-modulated diffuse / specular direct lighting (NRDFrontEnd.slang)
    uint32_t restirDISpecularTextureIndex;
    uint32_t restirDIDenoiserMode;      // how both were packed: NRD_SIGNAL_RAW / REBLUR / RELAX
    // The G-buffer surface the NRD signals are de-modulated by (PathTracerSignals.slang).
    uint32_t depthTextureIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
};

// The path tracer's NRD composite (PTComposite.slang): emission + denoised diffuse / specular, modulated back.
struct PushConstantPTComposite
{
    uint64_t matrixReference;
    uint32_t emissionTextureIndex;
    uint32_t diffuseTextureIndex;
    uint32_t specularTextureIndex;
    uint32_t depthTextureIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t outputStorageIndex;
    uint32_t denoiserMode;
    uint32_t width;
    uint32_t height;
};

// Render graph texture inspection (TextureInspect.slang): how the source texture is interpreted.
STATIC_CONST uint32_t TEXTURE_INSPECT_FLOAT         = 0; // color / data, exposure + channel mask
STATIC_CONST uint32_t TEXTURE_INSPECT_SIGNED_ID     = 1; // R32_SINT ids (entity)
STATIC_CONST uint32_t TEXTURE_INSPECT_UNSIGNED_PAIR = 2; // R32G32_UINT (visibility buffer)
STATIC_CONST uint32_t TEXTURE_INSPECT_DEPTH         = 3; // reverse-Z depth

struct PushConstantTextureInspect
{
    uint32_t sourceTextureIndex; // bindless sampled slot
    uint32_t outputStorageIndex; // RGBA8 display image, storage slot
    uint32_t probeStorageIndex;  // 1x1 RGBA32F, raw value of the probed texel
    uint32_t mode;               // TEXTURE_INSPECT_*
    uint32_t mip;
    uint32_t channelCount;       // float formats: 1 = gray, 2 = RG, 3/4 = RGB
    uint32_t channelMask;        // bit 0..3 = R, G, B, A shown
    float exposure;              // multiplier
    int32_t probeX;              // -1: no probe
    int32_t probeY;
    uint32_t width;              // mip size
    uint32_t height;
};

// Decodes an NRD REBLUR-denoised texture's YCoCg color back to linear RGB in place -- see
// YCoCgDecodeInPlace.slang. Only needed when denoiserMode == 1 (REBLUR); RELAX never encodes YCoCg.
struct PushConstantYCoCgDecode
{
    uint32_t readTextureIndex;  // bindless ShaderResource slot
    uint32_t writeTextureIndex; // bindless Storage slot (same underlying texture, mip 0)
    uint32_t width;
    uint32_t height;
};

struct PushConstantPostProcess
{
    uint64_t matrixReference;
    uint32_t hdrTextureIndex;
    uint32_t debugMode;
    uint32_t tonemapMode; // 0 = None, 1 = ACES Narkowicz, 2 = ACES Hill, 3 = ACES Hill Exp, 4 = Khronos PBR Neutral
};

struct PushConstantDDGIRadiance
{
    uint64_t matrixReference;
    uint32_t raysPerProbe;
    uint32_t probeCountTotal;
    uint32_t frameIndex;
};

struct PushConstantDDGIBlend
{
    uint64_t matrixReference;
    uint32_t rayDataTextureIndex;
    uint32_t prevAtlasTextureIndex;
    uint32_t probesPerRow;
    uint32_t raysPerProbe;
    uint32_t probeCountTotal;
    float hysteresis;
    uint32_t firstFrame;
    uint32_t frameIndex;
};

struct PushConstantDDGIDebug
{
    uint64_t matrixReference;
    uint32_t probeCountTotal;
    float sphereRadius;
    uint32_t irradianceAtlasIndex;
};

// RTXDI light sampling of an arbitrary surface (RTXDI/RTXDILightSampling.slang): this frame's presampled RIS tiles and
// ReGIR cells over the light buffer. ReSTIR DI's initial pass and ReSTIR GI's path vertices sample with it.
struct RTXDILightSamplingParams
{
    uint64_t lightDataReference;
    uint64_t risBufferReference;   // uint2 per element: [0, risBufferOffset) = plain RIS tiles (fallback / out-of-grid),
                                   // [risBufferOffset, end) = ReGIR cells
    vec4 gridCenterAndCellSize;    // xyz: world-space ReGIR grid center, w: cell size (meters)
    uint32_t firstLocalLightIndex;
    uint32_t numLocalLights;
    uint32_t firstInfiniteLightIndex;
    uint32_t numInfiniteLights;
    uint32_t numLocalLightSamples;
    uint32_t numInfiniteLightSamples;
    uint32_t risBufferOffset;
    uint32_t risTileSize;
    uint32_t risTileCount;
    uint32_t regirEnabled;         // 0 = plain RIS-tile sampling only, 1 = try a ReGIR cell first
    uint32_t cellsX;
    uint32_t cellsY;
    uint32_t cellsZ;
    uint32_t lightsPerCell;
    float regirSamplingJitter;     // 0 = static cell assignment, 1 = full +/-0.5 cell jitter (RTXPT's default)
};

struct PushConstantReSTIRGIInitial
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint64_t reservoirBufferReference;
    // viewportSize placed right after the uint64_t fields (offset 80, a multiple of 16) rather than
    // after 5 uint32_t fields (offset 100, not a multiple of 8) -- a vec2 at a non-8-aligned offset
    // is ambiguous between C++'s natural/tight struct packing and SPIR-V's std430-like push-constant
    // layout rules, which can silently insert 4 bytes of padding GPU-side that C++ never writes,
    // shifting every field after it (including reservoirBlockRowPitch/reservoirArrayPitch, which the
    // reservoir pointer math directly depends on) by 4 bytes between what C++ sends and what the
    // shader reads.
    vec2 viewportSize;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t visibilityTextureIndex;
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    // Lighting of the path vertices (RTXDI/RTXDILightSampling.slang); 8-aligned after the 8 uint32_t above.
    RTXDILightSamplingParams lightSampling;
};

// ReSTIR GI is now a proper two-pass pipeline (Temporal, then Spatial), matching RTXPT's actual
// architecture (rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial, using the SDK's separate
// Rtxdi/GI/TemporalResampling.hlsli + Rtxdi/GI/SpatialResampling.hlsli) instead of the fused
// Rtxdi/GI/SpatioTemporalResampling.hlsli. The fused function's "spatial" phase reads neighbor
// reservoirs from the SAME previous-frame history buffer as its temporal phase, so during camera
// motion -- when many neighboring pixels are simultaneously disoccluded/noisy from their own failed
// temporal reprojection -- spatial reuse was compounding that noise instead of stabilizing it. The
// two-pass split makes the spatial pass read THIS frame's already-temporally-resampled neighbors.
//
// Every vec2/vec4 field below is placed at an offset that's already a multiple of 8 (viewportSize)
// under BOTH natural/tight packing and SPIR-V's std430-like push-constant rules, to avoid the exact
// byte-offset ambiguity that caused the original ReSTIR GI reservoir hand-off bug.
struct PushConstantReSTIRGITemporal
{
    mat4 invViewProj;
    mat4 prevInvViewProj; // for reconstructing world position from the PREVIOUS frame's depth/normal
                          // textures -- unprojecting last frame's depth with THIS frame's matrix
                          // gives a wrong world position whenever the camera has moved, which
                          // silently corrupts the reprojection validity check and jacobian.
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t currentReservoirReference;  // Pass 1's initial candidate (read as inputReservoir); overwritten in place with the temporal result
    uint64_t previousReservoirReference; // history buffer, read for temporal candidates
    uint32_t depthTextureIndex;
    uint32_t prevDepthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t prevNormalTextureIndex;
    uint32_t gbufferVelocityIndex;
    uint32_t gbufferMaterialIndex;
    vec2 viewportSize; // offset 192, multiple of 8 -- safe under both packing conventions
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t maxHistoryLength;
    float normalThreshold;
    float depthThreshold;
    uint32_t enablePermutationSampling;
    uint32_t maxReservoirAge;
};

struct PushConstantReSTIRGISpatial
{
    mat4 invViewProj;
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t inputReservoirReference;  // this frame's temporally-resampled buffer; READ ONLY, used for both the own-pixel input and every spatial neighbor -- must stay untouched for the whole pass so concurrently-running neighbor pixels never race a write against this read.
    uint64_t outputReservoirReference; // separate physical buffer; WRITE ONLY, receives the final resampled result and becomes next frame's temporal history.
    uint64_t neighborOffsetsReference;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t frameIndex;
    vec2 viewportSize; // offset 128, multiple of 8 -- safe under both packing conventions
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    float samplingRadius;
    uint32_t numSamples;
    float normalThreshold;
    float depthThreshold;
    uint32_t neighborOffsetMask;
    uint32_t enableBoilingFilter;
    float boilingFilterStrength;
    uint32_t denoiserMode; // 0 = Off, 1 = REBLUR (needs YCoCg-encoded radiance), 2 = RELAX (plain RGB)
};

// ==========================================================================================
// RESTIR DI (SCREEN-SPACE RESAMPLED DIRECT LIGHTING)
// ==========================================================================================
// v1: analytic point/spot/directional lights only (our LightData), uniform light selection --
// no RIS/ReGIR buffer yet, that's a follow-up quality/perf layer once this is verified working.
// Reservoir buffer rotation is NOT the fixed scratch/persistent trick used for GI -- RTXDI's own
// ReSTIRDIContext::UpdateBufferIndices (NoxCore/vendors/RTXDI/Source/ReSTIRDI.cpp) rotates through
// 3 physical buffers every frame (c_NumReSTIRDIReservoirBuffers = 3):
//   A = (lastFrameOutput + 1) % 3   -- Initial writes here, Temporal reads+overwrites here in place
//   C = lastFrameOutput             -- Temporal's history read (read-only)
//   B = (A + 1) % 3                 -- Spatial reads A (own input + all neighbors), writes here
// FinalShading reads B; next frame lastFrameOutput = B. Using only 2 buffers here would reintroduce
// the exact read/write race already hit and fixed once for GI's spatial pass.

// RIS presample pass: builds risTileCount tiles of risTileSize power-weighted local-light candidates
// each (one compute thread per RIS buffer slot). Independent of ReGIR -- also serves as the fallback
// for pixels whose world position falls outside the ReGIR grid.
// Writes mip 0 of the light PDF texture: one thread per texel, Z-curve indexed (RTXDI_LinearIndexToZCurve)
// so RTXDI_ZCurveToLinearIndex can recover the light index later during RTXDI_SamplePdfMipmap's
// hierarchical descent. Texels beyond numLocalLights are written 0 (unused).
struct PushConstantReSTIRDIWriteLightPDF
{
    uint64_t lightDataReference;
    uint32_t firstLocalLightIndex;
    uint32_t numLocalLights;
    uint32_t pdfTextureStorageIndex; // mip-0 storage (UAV) descriptor slot
    uint32_t pdfTextureSize;
};

// Sum-reduces one mip level of the light PDF texture from the previous (finer) mip -- NOT an average:
// RTXDI_SamplePdfMipmap's hierarchical descent needs each coarser texel to hold the total probability
// mass of the 4 finer texels it covers, so a plain box-filter average would silently bias the sampling
// distribution.
struct PushConstantReSTIRDIReduceLightPDFMip
{
    uint32_t srcTextureIndex;  // bindless ShaderResource slot, read via Texture2D<float>.Load(pos, srcMip)
    uint32_t dstStorageIndex;  // destination mip's storage (UAV) descriptor slot
    uint32_t srcMip;
    uint32_t dstSize;
};

struct PushConstantReSTIRDIPresample
{
    uint64_t lightDataReference;
    uint64_t risBufferReference;
    uint32_t firstLocalLightIndex;
    uint32_t numLocalLights;
    uint32_t risTileSize;
    uint32_t risTileCount;
    uint32_t frameIndex;
    uint32_t pdfTextureIndex; // bindless ShaderResource slot for the light PDF mip chain
    uint32_t pdfTextureSize;
};

// ReGIR presample pass: one compute thread per (cell, slot-within-cell), each doing its own
// power-weighted draw over every local light, weighted by importance to that cell's center rather
// than to a specific surface point. Writes into a SEPARATE segment of the same RIS buffer, offset by
// risBufferOffset (grid-mode only -- our scenes are bounded interiors, no need for ReGIR's "Onion"
// mode meant for open/unbounded worlds).
struct PushConstantReSTIRDIPresampleReGIR
{
    uint64_t lightDataReference;
    uint64_t risBufferReference;
    vec4 gridCenterAndCellSize; // xyz: center, w: cell size (meters)
    uint32_t firstLocalLightIndex;
    uint32_t numLocalLights;
    uint32_t risBufferOffset;
    uint32_t lightsPerCell;
    uint32_t cellsX;
    uint32_t cellsY;
    uint32_t cellsZ;
    uint32_t frameIndex;
    float regirSamplingJitter; // 0 = no jitter (fully static cell assignment), 1 = full +/-0.5 cell
                                // (RTXPT's default); must match ReSTIRDIInitial.slang's lookup jitter
                                // so the cell's precomputed weighting radius matches what's actually sampled
    uint32_t risTileSize;   // plain RIS tile segment params (fallback candidate pool for
    uint32_t risTileCount;  // RTXDI_PresampleLocalLightsForReGIR's Power_RIS presampling mode)
    uint32_t numRegirBuildSamples; // RTXPT's default: 8 weighted draws combined per cell slot via
                                    // streaming RIS, not a single full-light-list pass
};


struct PushConstantReSTIRDIInitial
{
    mat4 invViewProj;
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t reservoirBufferReference; // writes to buffer A
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferMaterialIndex;
    vec2 viewportSize; // offset 112, multiple of 8
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t pad0; // the struct below starts 8-aligned
    RTXDILightSamplingParams lightSampling;
};

struct PushConstantReSTIRDITemporal
{
    mat4 invViewProj;
    mat4 prevInvViewProj; // MUST use the actual previous-frame matrix for previous-frame G-buffer
                          // reconstruction -- using this frame's matrix instead is the exact bug
                          // that caused ReSTIR GI's camera-motion light leaks, fixed once already.
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t lightDataReference;
    uint64_t currentReservoirReference;  // buffer A: this frame's initial candidate; overwritten
                                          // in place with the temporal result (safe: own-pixel only)
    uint64_t previousReservoirReference; // buffer C: history, read-only
    uint32_t depthTextureIndex;
    uint32_t prevDepthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t prevNormalTextureIndex;
    uint32_t gbufferVelocityIndex;
    uint32_t gbufferMaterialIndex;
    vec2 viewportSize; // offset 200, multiple of 8 -- confirmed via slangc -reflection-json
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t maxHistoryLength;
    float normalThreshold;
    float depthThreshold;
    uint32_t enablePermutationSampling;
};

struct PushConstantReSTIRDISpatial
{
    mat4 invViewProj;
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t lightDataReference;
    uint64_t inputReservoirReference;  // buffer A: read only, own-pixel input + every neighbor
    uint64_t outputReservoirReference; // buffer B: write only, final result
    uint64_t neighborOffsetsReference; // reuses the same neighbor-offsets buffer/format as GI
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t frameIndex;
    vec2 viewportSize; // offset 136, multiple of 8 -- confirmed via slangc -reflection-json
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    float samplingRadius;
    uint32_t numSamples;
    float normalThreshold;
    float depthThreshold;
    uint32_t neighborOffsetMask;
};

struct PushConstantReSTIRDIFinalShading
{
    mat4 invViewProj;
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t lightDataReference;
    uint64_t reservoirReference; // buffer B, read only
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferMaterialIndex;
    vec2 viewportSize; // offset 120, multiple of 8
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t denoiserMode; // NRD_SIGNAL_RAW / REBLUR / RELAX: how the diffuse and specular outputs are packed
    uint32_t firstInfiniteLightIndex;
    uint32_t numInfiniteLights;
    uint32_t pad0;
    uint32_t pad1;
};

struct PushConstantOutline
{
    uint64_t matrixReference; // BDA to UniformBufferObject
    uint64_t selectedEntityIDsReference;
    uint32_t selectedEntityCount;
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

// ReSTIR PT (Screen-Space Path Resampling via RTXDI) -- RIS across numInitialSamples full paths per
// pixel, optional temporal reuse, RTXDI's default footprint-based reconnection mode, and uniform
// (not RIS/ReGIR) NEE light sampling -- matches ReSTIR DI's own v1 staging before RIS/ReGIR were added.
struct PushConstantReSTIRPTInitial
{
    mat4 invViewProj;
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t lightDataReference;
    uint64_t reservoirBufferReference; // writes to buffer index 0 (ResamplingMode::None)
    uint64_t preservedReservoirReference; // optional copy of the unresampled initial reservoir for SDK final-shading decorrelation
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferMaterialIndex;
    vec2 viewportSize; // offset 128, multiple of 8
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t firstLocalLightIndex;
    uint32_t numLocalLights;
    uint32_t firstInfiniteLightIndex;
    uint32_t numInfiniteLights;
    uint32_t numInitialSamples;  // RTXDI_PTInitialSamplingParameters.numInitialSamples
    uint32_t maxBounceDepth;
    uint32_t maxRcVertexLength;
    uint32_t numNeeSamples;      // NEE draws per bounce (0 disables NEE -> emissive-only sampling)
    float roughnessThreshold;    // legacy FixedThreshold UI field; Footprint mode uses SDK defaults
    float distanceThreshold;
    uint32_t skyboxTextureIndex;
};

struct PushConstantReSTIRPTFinalShading
{
    mat4 invViewProj;
    vec4 cameraWorldPos;
    uint64_t matrixReference;
    uint64_t lightDataReference;
    uint64_t reservoirReference; // buffer index 0 (ResamplingMode::None), read only
    uint64_t preservedReservoirReference; // preserved initial-sampling reservoir used by SDK final-shading decorrelation
    // viewportSize placed right after the uint64_t block (offset 112, already a multiple of 8) rather
    // than after an odd count of uint32_t fields -- see PushConstantReSTIRGIInitial's note above on why
    // a vec2 at a non-8-aligned offset is dangerous (SPIR-V silently pads it, C++ doesn't).
    vec2 viewportSize;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t primaryDirectTextureIndex; // bounce-1 direct lighting from the Initial pass, added to the resampled indirect result
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t denoiserMode; // 0 = Off, 1 = NRD REBLUR (YCoCg encode), 2 = NRD RELAX (plain RGB)
    float decorrelationFactor; // SDK default decorrelation probability; 0 disables preserved-initial replacement
    uint32_t decorrelationMode; // RTXDI_PT_DECORRELATION_MODE_*; Uniform is usable without the duplication-map pass
};

struct PushConstantReSTIRPTTemporal
{
    // invViewProj deliberately NOT duplicated here (unlike ReSTIRPTInitial/FinalShading) -- this struct
    // already needed prevInvViewProj + prevCameraWorldPos on top of everything Initial/FinalShading
    // carry, which pushed it past the device's 256-byte push-constant limit (hit as a real crash:
    // vkCmdPushDataEXT: data.size 288 > maxPushDataSize 256). g_UBO->invViewProj (read via
    // matrixReference, already below) is exactly the same value -- computed identically as
    // glm::inverse(uniformData.proj * uniformData.view) on the C++ side -- so there's nothing lost by
    // reading it from there instead of duplicating it in every push constant.
    mat4 prevInvViewProj; // unprojects the PREVIOUS frame's depth/normal textures with the matching matrix
    vec4 cameraWorldPos;
    vec4 prevCameraWorldPos;
    vec4 prevPrevCameraWorldPos;
    uint64_t matrixReference;
    uint64_t lightDataReference;
    uint64_t currentReservoirReference; // this frame's Initial-sampling candidate; overwritten in place with the temporal result
    uint64_t historyReservoirReference; // previous frame's finalized reservoir, read only
    vec2 viewportSize; // placed right after the uint64_t block (offset 144, multiple of 8) -- see the
                        // ReSTIRGIInitial/ReSTIRPTFinalShading structs above for why a vec2 needs an
                        // 8-aligned offset to avoid a silent C++/SPIR-V padding mismatch.
    uint32_t depthTextureIndex;
    uint32_t prevDepthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t prevNormalTextureIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t prevAlbedoTextureIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t prevMaterialTextureIndex;
    uint32_t gbufferVelocityIndex;
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t maxBounceDepth;
    uint32_t maxRcVertexLength;
    float roughnessThreshold;    // legacy FixedThreshold UI field; Footprint mode uses SDK defaults
    float distanceThreshold;
    float depthThreshold;        // temporal reprojection validity check
    float normalThreshold;
    uint32_t maxHistoryLength;
    uint32_t maxReservoirAge;
    uint32_t enablePermutationSampling;
    uint32_t skyboxTextureIndex;
};

#ifdef __SLANG__
// Raster view of one GPU scene instance: the instance record joined with its transform, material and mesh (ray tracing
// shaders read GpuRayTracingInstance instead).
struct InstanceData
{
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float4x4 previousModelMatrix;

    uint32_t drawsOffset;
    uint32_t boundsOffset;
    uint32_t meshletCount;

    float workflow;
    float4 diffuseFactor;
    float4 specularFactor;
    float4 baseColorFactor;
    uint32_t baseColorTextureIndex;
    int32_t baseColorTextureSet;
    float metallicFactor;
    float roughnessFactor;
    uint32_t metallicRoughnessTextureIndex;
    int32_t physicalDescriptorTextureSet;
    uint32_t normalTextureIndex;
    int32_t normalTextureSet;
    uint32_t occlusionTextureIndex;
    int32_t occlusionTextureSet;
    float3 emissiveFactor;
    uint32_t emissiveTextureIndex;
    int32_t emissiveTextureSet;
    float emissiveStrength;
    float transmissionFactor;
    uint32_t transmissionTextureIndex;
    int32_t transmissionTextureSet;
    float ior;
    float thickness;
    uint32_t alphaMode;
    float alphaMaskCutoff;
    uint32_t doubleSided;
    uint32_t unlit;

    uint32_t boneMatrixOffset;
    int entityID;
};

struct SceneInstances
{
    GpuInstance* instances;
    GpuTransform* transforms;
    GpuMaterial* materials;
    GpuMesh* meshes;

    __init(UniformBufferObject* ubo)
    {
        instances = (GpuInstance*)ubo->sceneInstancesReference;
        transforms = (GpuTransform*)ubo->sceneTransformsReference;
        materials = (GpuMaterial*)ubo->sceneMaterialsReference;
        meshes = (GpuMesh*)ubo->sceneMeshesReference;
    }

    // Field by field through the pointers: a lookup loads only the fields its caller uses (whole-record loads would
    // copy transforms, materials and meshes for every candidate).
    __subscript(uint slot) -> InstanceData
    {
        get
        {
            GpuInstance* instance = &instances[slot];
            GpuTransform* transform = &transforms[slot];
            GpuMaterial* material = &materials[instance.materialIndex];
            GpuMesh* mesh = &meshes[instance.meshIndex];

            InstanceData data;
            data.modelMatrix = transform.world;
            data.normalMatrix = transform.normal;
            data.previousModelMatrix = transform.previousWorld;

            data.drawsOffset = mesh.drawsOffset;
            data.boundsOffset = mesh.boundsOffset;
            data.meshletCount = mesh.meshletCount;

            data.workflow = material.workflow;
            data.diffuseFactor = material.diffuseFactor;
            data.specularFactor = material.specularFactor;
            data.baseColorFactor = material.baseColorFactor;
            data.baseColorTextureIndex = material.baseColorTextureIndex;
            data.baseColorTextureSet = material.baseColorTextureSet;
            data.metallicFactor = material.metallicFactor;
            data.roughnessFactor = material.roughnessFactor;
            data.metallicRoughnessTextureIndex = material.metallicRoughnessTextureIndex;
            data.physicalDescriptorTextureSet = material.physicalDescriptorTextureSet;
            data.normalTextureIndex = material.normalTextureIndex;
            data.normalTextureSet = material.normalTextureSet;
            data.occlusionTextureIndex = material.occlusionTextureIndex;
            data.occlusionTextureSet = material.occlusionTextureSet;
            data.emissiveFactor = material.emissiveFactor;
            data.emissiveTextureIndex = material.emissiveTextureIndex;
            data.emissiveTextureSet = material.emissiveTextureSet;
            data.emissiveStrength = material.emissiveStrength;
            data.transmissionFactor = material.transmissionFactor;
            data.transmissionTextureIndex = material.transmissionTextureIndex;
            data.transmissionTextureSet = material.transmissionTextureSet;
            data.ior = material.ior;
            data.thickness = material.thickness;
            data.alphaMode = material.alphaMode;
            data.alphaMaskCutoff = material.alphaMaskCutoff;
            data.doubleSided = material.doubleSided;
            data.unlit = material.unlit;

            data.boneMatrixOffset = instance.boneMatrixOffset;
            data.entityID = instance.entityID;
            return data;
        }
    }
};

#endif

#endif  // HOST_DEVICE_H
