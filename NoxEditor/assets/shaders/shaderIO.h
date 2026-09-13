
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

struct InstanceLUT
{
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
    mat4 normalMatrix;              // Transforms local vertex normals to world space
    vec4 baseColorFactor;
    vec4 emissiveFactor;            // rgb: color, a: strength
    uint32_t baseColorTextureIndex;
    float alphaCutoff;
    uint32_t alphaMode;             // 0 = Opaque, 1 = Mask, 2 = Blend
    uint32_t doubleSided;           // 0 = Single-sided, 1 = Double-sided

    // PBR Material & Textures
    float metallicFactor;
    float roughnessFactor;
    uint32_t metallicRoughnessTextureIndex;
    uint32_t normalTextureIndex;

    // Transmission & Workflow
    float transmissionFactor;
    uint32_t transmissionTextureIndex;
    float workflow;                 // 0.0 = MetalRough, 1.0 = SpecGloss
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
    uint64_t instanceLUTReference;

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
    uint32_t restirDIDirectLightingTextureIndex;
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

struct InstanceData
{
    // Mesh
    mat4 modelMatrix;
    mat4 normalMatrix;

    // --- NEW: Page Information ---
    uint32_t drawsPageIndex;            // Same index used for meshletBounds (1:1 allocation)
    uint32_t drawsOffset;               // Where this model's meshlets start in the page
    uint32_t meshletCount;              // How many meshlets this model has
    
    uint32_t verticesPageIndex;
    uint32_t meshletVerticesPageIndex;
    uint32_t meshletTrianglesPageIndex;
    // -----------------------------
    
    // Material Workflow & SpecGloss properties
    float workflow; // 0.0 = Metallic-Roughness, 1.0 = Specular-Glossiness
    vec4 diffuseFactor;
    vec4 specularFactor; // rgb: specular factor, a: glossiness factor

    // Material
    vec4 baseColorFactor;
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
    
    uint32_t alphaMode;   // 0 = Opaque, 1 = Mask, 2 = Blend
    float alphaMaskCutoff;
    uint32_t doubleSided; // Use uint32_t instead of bool for GPU alignment
    uint32_t unlit; 
    
    // MeshAnimation
    uint32_t boneMatrixOffset = 0xFFFFFFFF;

    // Editor-only
    int entityID;
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
    vec4 spotParams;   // x: cos(innerAngle), y: cos(outerAngle), z: 0.0, w: 0.0
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
    uint32_t instanceBaseIndex;
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
    uint64_t instanceReference;
    uint64_t boneMatrixReference;
    uint64_t vertexPageTableReference;
    uint64_t meshletDrawsPageTableReference;
    uint64_t meshletVerticesPageTableReference;
    uint64_t meshletTrianglesPageTableReference;
    uint32_t visibilityTextureIndex;
    uint32_t debugMode; // 0 = Albedo, 1 = Normal, 2 = Roughness, 3 = Metallic, 4 = Emission, 5 = Occlusion, 6 = Colored Meshlets
    vec2 viewportSize;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t gbufferEmissionIndex;
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
    uint32_t restirDIDirectLightingTextureIndex;
    uint32_t restirDIDenoiserMode; // 0 = Off, 1 = REBLUR (output is YCoCg, needs decoding), 2 = RELAX (plain RGB)
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
    uint64_t lightDataReference;
    uint64_t reservoirBufferReference; // writes to buffer A
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferMaterialIndex;
    vec2 viewportSize; // offset 120, multiple of 8
    uint32_t frameIndex;
    uint32_t reservoirBlockRowPitch;
    uint32_t reservoirArrayPitch;
    uint32_t firstLocalLightIndex;
    uint32_t numLocalLights;
    uint32_t firstInfiniteLightIndex;
    uint32_t numInfiniteLights;
    uint32_t numLocalLightSamples;
    uint32_t numInfiniteLightSamples;

    // RIS + ReGIR (phase 2: power-weighted local-light candidates instead of pure uniform selection)
    uint64_t risBufferReference;   // shared uint2-per-element buffer: [0, risBufferOffset) = plain RIS
                                    // tiles (fallback / out-of-grid), [risBufferOffset, end) = ReGIR cells
    uint32_t risBufferOffset;
    uint32_t risTileSize;
    uint32_t risTileCount;
    uint32_t regirEnabled; // 0 = plain RIS-tile sampling only, 1 = try a ReGIR cell first
    uint32_t cellsX;
    uint32_t cellsY;
    uint32_t cellsZ;
    uint32_t lightsPerCell;
    vec4 gridCenterAndCellSize; // xyz: world-space grid center, w: cell size (meters)
    float regirSamplingJitter; // 0 = no jitter (fully static cell assignment, stable but hard cell-boundary
                                // edges), 1 = full +/-0.5 cell jitter (RTXPT's default, diffuses cell
                                // discretization error across frames/pixels at the cost of visible edge
                                // instability with only a handful of lights and no heavy temporal accumulation)
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
    uint32_t denoiserMode; // 0 = Off (plain RGB), 1 = NRD REBLUR (needs YCoCg encode), 2 = NRD RELAX (plain RGB)
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

#endif  // HOST_DEVICE_H