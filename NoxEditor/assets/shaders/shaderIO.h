
#ifndef HOST_DEVICE_H
#define HOST_DEVICE_H

#ifdef __SLANG__
#include "NoxGpu.slang"
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
    SAMPLER_NEAREST_CLAMP = 6,

    SamplerCount
};

STATIC_CONST uint32_t TASK_SHADER_DISPATCH_X = 64;
STATIC_CONST uint32_t MESH_SHADER_DISPATCH_X = 32;
STATIC_CONST uint32_t MAX_VERTICES            = 64;
STATIC_CONST uint32_t MAX_PRIMITIVES          = 64;

// Per https://developer.nvidia.com/blog/using-mesh-shaders-for-professional-graphics/
// Task shader output should ideally be below 236/108. 4 x 8-bit offsets packed per uint32_t (no uint8_t, legal on both Vulkan & DXIL).
// 8 + 64 = 72 bytes.
struct MeshletPayload
{
    uint32_t drawID;
    uint32_t groupMeshletOffset;
    uint32_t meshletIndices[TASK_SHADER_DISPATCH_X / 4];
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
    uint32_t skinnedVertexOffset; // 0xFFFFFFFF: no compute-skinned vertex cache
    int32_t entityID;          // editor picking
};

struct GpuTransform
{
    mat4 world;
    mat4 normal;        // transpose(inverse(world))
    mat4 previousWorld; // world of the previous frame (per-object motion vectors)
};

// A material's UV channels (MaterialData::UVChannels, MaxMaterialUVChannels).
STATIC_CONST uint32_t MaterialUVChannels = 4u;

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
    // Volume absorption (KHR_materials_volume): what crossing attenuationDistance of the volume leaves of the light (0: none)
    vec3 attenuationColor;
    float attenuationDistance;

    uint32_t alphaMode;   // 0 = Opaque, 1 = Mask, 2 = Blend
    float alphaMaskCutoff;
    uint32_t doubleSided;
    uint32_t shadingModel; // ShadingModel* (MaterialData::ShadingModel): Default Lit, Unlit, Toon
    float diffuseTransmission; // through a thin surface to its back (leaves, cloth); 0: none
    uint32_t pathTracerFlags;  // PathTracerThin... (above)
    float depthBias; // MaterialData::DepthBias: drawn this much closer to the camera along the view ray, world units (depth_bias_clip)

    // The lobes past the core model (MaterialData, one slab as UE5 Substrate's): MaterialLobe* bits say which are on.
    uint32_t lobes;
    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    uint32_t clearcoatTextureIndex;          // R
    int32_t clearcoatTextureSet;
    uint32_t clearcoatRoughnessTextureIndex; // G
    int32_t clearcoatRoughnessTextureSet;
    uint32_t clearcoatNormalTextureIndex;    // 0xFFFFFFFF: the coat is flat
    int32_t clearcoatNormalTextureSet;
    vec3 sheenColorFactor;
    float sheenRoughnessFactor;
    uint32_t sheenColorTextureIndex;         // RGB (sRGB)
    int32_t sheenColorTextureSet;
    uint32_t sheenRoughnessTextureIndex;     // A
    int32_t sheenRoughnessTextureSet;
    vec3 specularColorFactor;
    float specularStrength;
    uint32_t specularColorTextureIndex;      // RGB (sRGB)
    int32_t specularColorTextureSet;
    uint32_t specularStrengthTextureIndex;   // A
    int32_t specularStrengthTextureSet;
    float iridescenceFactor;
    float iridescenceIor;
    float iridescenceThicknessMinimum;       // nanometers
    float iridescenceThicknessMaximum;
    uint32_t iridescenceTextureIndex;        // R
    int32_t iridescenceTextureSet;
    uint32_t iridescenceThicknessTextureIndex; // G
    int32_t iridescenceThicknessTextureSet;
    vec2 anisotropyDirection;                // (cos, sin) of the rotation from the tangent
    float anisotropyStrength;
    uint32_t anisotropyTextureIndex;         // RG direction, B strength
    int32_t anisotropyTextureSet;
    float dispersion;                        // 20 / Abbe number

    // The UV channels a texture's set names (MaterialData::UVChannels: KHR_texture_transform): channel k reads mesh UV set
    // (uvSources >> k) & 1 through the rows uvRow0[k] / uvRow1[k] (uv_transform; material_texture_uv).
    vec3 uvRow0[MaterialUVChannels];
    vec3 uvRow1[MaterialUVChannels];
    uint32_t uvSources;
    float normalScale;          // glTF normalTextureInfo.scale
    float occlusionStrength;    // glTF occlusionTextureInfo.strength
    float clearcoatNormalScale;

    // The Toon shading model (MaterialData::Toon*; ToonShading.slang). Bands 1..8 (1: two tones); the ramp (0xFFFFFFFF: none)
    // replaces the bands; highlight size 0 and rim intensity 0 turn those off.
    uint32_t toonRampTextureIndex;
    uint32_t toonBands;
    float toonBandSoftness;
    float toonSpecularSize;
    float toonSpecularSoftness;
    float toonSpecularIntensity;
    vec3 toonRimColor;
    float toonRimWidth;
    float toonRimIntensity;

    // The Custom Lit shading model (MaterialData::Custom*; CustomLit.slang): which Material Shader lights it (the renderer's slot for
    // it, CustomLitShaders.h; a material whose shader is not ready packs as Default Lit instead), its four parameters and two textures
    // (0xFFFFFFFF: none; the first sRGB, the second linear).
    uint32_t customLitSlot;
    uint32_t customTextureIndex[2];
    vec4 customParameters[4];

    // A material graph (MaterialData::SurfaceGraph; MaterialGraph.slang, Renderer/MaterialGraph.h): the renderer's slot for its graph
    // (0xFFFFFFFF: none, or not built yet -- the material's own values), which Material Output pins it connects (MaterialGraphOutput
    // bits), its parameters (the graph's layout: the material's overrides, else the graph's defaults) and textures (0xFFFFFFFF: none).
    uint32_t graphSlot;
    uint32_t graphOutputs;
    uint32_t graphOutputs2; // MaterialGraphOutput2* bits (step R1+: Subsurface Color, etc.)
    uint32_t graphTextureIndex[8];
    vec4 graphParameters[16];
    // Its World Position Offset's Max Displacement (MaterialGraphProgram::MaxDisplacement, world units; 0: no clamp, no larger bounds).
    float graphMaxDisplacement;

    // The Subsurface shading model (MaterialData::Subsurface*; SubsurfaceShading.slang, step R1).
    vec3 subsurfaceColor;
    float subsurfaceOpacity;
    float subsurfaceRadius;
    uint32_t subsurfaceColorTextureIndex;
    int32_t subsurfaceColorTextureSet;
};

// GpuMaterial::graphOutputs (MaterialGraphOutput, Renderer/MaterialGraph.h).
STATIC_CONST uint32_t MaterialGraphOutputBaseColor = 1u << 0;
STATIC_CONST uint32_t MaterialGraphOutputMetallic = 1u << 1;
STATIC_CONST uint32_t MaterialGraphOutputRoughness = 1u << 2;
STATIC_CONST uint32_t MaterialGraphOutputNormal = 1u << 3;
STATIC_CONST uint32_t MaterialGraphOutputEmissive = 1u << 4;
STATIC_CONST uint32_t MaterialGraphOutputOcclusion = 1u << 5;
STATIC_CONST uint32_t MaterialGraphOutputOpacity = 1u << 6;
STATIC_CONST uint32_t MaterialGraphOutputOpacityMask = 1u << 7; // its own function (material_graph_opacity_mask): a Masked material's alpha test
STATIC_CONST uint32_t MaterialGraphOutputWorldPositionOffset = 1u << 8; // its own function (material_graph_world_position_offset): per vertex
STATIC_CONST uint32_t MaterialGraphOutputVertexNormal = 1u << 11; // per-vertex normal override (step 4f)
STATIC_CONST uint32_t MaterialGraphOutputVertexColor = 1u << 12; // per-vertex colour override (step 4f)
STATIC_CONST uint32_t MaterialGraphOutputVertexInterpolators = 1u << 13; // Vertex Interpolator slots 0..3 (step 4f)
STATIC_CONST uint32_t MaterialGraphOutputSpecular = 1u << 14;
STATIC_CONST uint32_t MaterialGraphOutputSpecularColor = 1u << 15;
STATIC_CONST uint32_t MaterialGraphOutputClearCoat = 1u << 16;
STATIC_CONST uint32_t MaterialGraphOutputClearCoatRoughness = 1u << 17;
STATIC_CONST uint32_t MaterialGraphOutputClearCoatNormal = 1u << 18;
STATIC_CONST uint32_t MaterialGraphOutputSheenColor = 1u << 19;
STATIC_CONST uint32_t MaterialGraphOutputSheenRoughness = 1u << 20;
STATIC_CONST uint32_t MaterialGraphOutputAnisotropy = 1u << 21;
STATIC_CONST uint32_t MaterialGraphOutputAnisotropyDirection = 1u << 22;
STATIC_CONST uint32_t MaterialGraphOutputTransmission = 1u << 23;
STATIC_CONST uint32_t MaterialGraphOutputIOR = 1u << 24;
STATIC_CONST uint32_t MaterialGraphOutputThickness = 1u << 25;
STATIC_CONST uint32_t MaterialGraphOutputAttenuationColor = 1u << 26;
STATIC_CONST uint32_t MaterialGraphOutputAttenuationDistance = 1u << 27;
STATIC_CONST uint32_t MaterialGraphOutputDiffuseTransmission = 1u << 28;
STATIC_CONST uint32_t MaterialGraphOutputIridescence = 1u << 29;
STATIC_CONST uint32_t MaterialGraphOutputIridescenceThickness = 1u << 30;
STATIC_CONST uint32_t MaterialGraphOutputDispersion = 1u << 31;
STATIC_CONST uint32_t MaterialGraphOutputAllLobes =
    MaterialGraphOutputSpecular | MaterialGraphOutputSpecularColor |
    MaterialGraphOutputClearCoat | MaterialGraphOutputClearCoatRoughness | MaterialGraphOutputClearCoatNormal |
    MaterialGraphOutputSheenColor | MaterialGraphOutputSheenRoughness |
    MaterialGraphOutputAnisotropy | MaterialGraphOutputAnisotropyDirection |
    MaterialGraphOutputTransmission | MaterialGraphOutputIOR | MaterialGraphOutputThickness |
    MaterialGraphOutputAttenuationColor | MaterialGraphOutputAttenuationDistance |
    MaterialGraphOutputDiffuseTransmission |
    MaterialGraphOutputIridescence | MaterialGraphOutputIridescenceThickness |
    MaterialGraphOutputDispersion;

// GpuMaterial::graphOutputs2 (MaterialGraphOutput2, Renderer/MaterialGraph.h; step R1+).
STATIC_CONST uint32_t MaterialGraphOutput2SubsurfaceColor = 1u << 0;
STATIC_CONST uint32_t MaterialGraphOutput2AOLightAffect = 1u << 1;
STATIC_CONST uint32_t MaterialGraphOutput2PixelDepthOffset = 1u << 2;

// GpuMaterial::shadingModel (MaterialShadingModel, DataTypes.h).
STATIC_CONST uint32_t ShadingModelDefaultLit = 0u;
STATIC_CONST uint32_t ShadingModelUnlit = 1u;
STATIC_CONST uint32_t ShadingModelToon = 2u;
STATIC_CONST uint32_t ShadingModelCustomLit = 3u;
STATIC_CONST uint32_t ShadingModelSubsurface = 4u;

// GpuMaterial::lobes.
STATIC_CONST uint32_t MaterialLobeClearcoat = 1u << 0;
STATIC_CONST uint32_t MaterialLobeSheen = 1u << 1;
STATIC_CONST uint32_t MaterialLobeSpecular = 1u << 2; // F0 other than IOR 1.5's 4% white, or a reflection weaker than full
STATIC_CONST uint32_t MaterialLobeIridescence = 1u << 3;
STATIC_CONST uint32_t MaterialLobeAnisotropy = 1u << 4;
STATIC_CONST uint32_t MaterialLobeDispersion = 1u << 5;

// Ray tracing view of an instance (same slot as GpuInstance): everything the ray tracing shaders read at candidates and
// hits, in one record. Ray queries look instances up per candidate triangle; reading instance -> material -> mesh ->
// transform from separate tables there made ReSTIR GI ~30% slower than one record (Bistro, §5.5.4).
struct GpuRayTracingInstance
{
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
    uint64_t skinnedVertexBufferAddress; // 0 for rigid instances; local-space SkinnedVertex stream otherwise
    uint64_t colorBufferAddress;    // packed vertex colors at vertexBufferAddress's indices; 0 when the mesh has none
    mat4 normalMatrix;              // transpose(inverse(world))
    vec4 baseColorFactor;
    vec4 diffuseFactor;             // KHR_materials_pbrSpecularGlossiness
    vec4 specularFactor;            // rgb: F0, a: glossiness
    vec4 emissiveFactor;            // rgb: color, a: strength
    uint32_t emissiveTextureIndex;
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
    vec3 attenuationColor;          // volume absorption (KHR_materials_volume; attenuationDistance 0: none)
    float attenuationDistance;
    float diffuseTransmission;      // through a thin surface to its back (leaves, cloth); 0: none
    uint32_t pathTracerFlags;       // PathTracerThin...
    uint32_t materialIndex;         // GpuMaterial slot: the lobes (when lobes != 0) are read from there
    uint32_t lobes;                 // GpuMaterial::lobes
    vec3 uvRow0;                    // UV channel 0's transform (ray_hit_uv: hits read every texture through channel 0)
    vec3 uvRow1;
    float normalScale;              // GpuMaterial::normalScale
    uint32_t instanceSlot;          // the GPU scene's instance slot (its transform: a material graph's Opacity Mask at a ray hit reads
                                    // World Position); also keeps the record a multiple of 8 bytes (its addresses)
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
    uint32_t vertexCount;
    uint32_t indicesOffset;

    uint32_t triangleCount; // of all meshlets (stats)
    // Geometry streaming (§5.7.2): the mesh's pages in the page table (GpuGeometryPage), NoGeometryRange when every cluster is
    // resident (a mesh that does not stream).
    uint32_t pageOffset;
    uint32_t pageCount;

    // Local bounds (GPU culling)
    vec4 boundsSphere; // xyz: center, w: radius
    vec3 boundsMin;
    uint32_t flags;    // GpuMeshVertexColors
    vec3 boundsMax;
    float padding1;
};
STATIC_CONST uint32_t GpuMeshVertexColors = 1u << 0; // its vertices' colors are in the color stream (geometryColorsReference)

// One DDGI cascade (UniformBufferObject::ddgiCascades). Plain 4-byte members (read through the uniforms' pointer). A probe's atlas slot
// is its grid coordinate in the world modulo the probe counts (toroidal addressing), so a cascade that follows the camera keeps every
// probe's history where it is while it scrolls.
struct DDGICascade
{
    float originX;     // world position of the cascade's first probe
    float originY;
    float originZ;
    float spacingX;    // world units between probes
    float spacingY;
    float spacingZ;
    uint32_t scrollX;  // atlas slot (within the cascade) of the first probe
    uint32_t scrollY;
    uint32_t scrollZ;
    int32_t shiftX;    // probes the cascade moved this frame: the ones that came in on that side start over
    int32_t shiftY;
    int32_t shiftZ;
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
    vec2 jitterOffset;     // Subpixel jitter in pixels (render resolution, x right, y down)
    vec2 prevJitterOffset; // Previous frame jitter in pixels: last frame's image was drawn with it (prev_clip_to_uv)
    //

    uint samplerIndex;
    uint imageHeapIndexOffset;
    uint finalImageIndex;

    uint entityTextureIndex; // Heap index of m_entityResolveReference for edge detection outline (display resolution)
    uint entityGBufferTextureIndex; // Heap index of the render-resolution G-buffer entity ID texture (debug views only)
    // The G-buffer's lobe targets (GBufferMaterial.slang; gbuffer_decode_lobes): 0xFFFFFFFF when not read this frame.
    uint gbufferLobesTextureIndex;    // RGBA32_UINT: clear coat, sheen, anisotropy, iridescence
    uint gbufferSpecularTextureIndex; // RGBA8: dielectric F0 (square root) and the specular weight

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
    uint64_t geometryPagesReference; // GpuGeometryPage table (geometry streaming, §5.7.2)
    // SkinVertex per vertex of a skinned mesh, at the vertex's own index (the stream mirrors the vertex stream; only skinned meshes'
    // ranges hold data, §5.8.2).
    uint64_t geometrySkinReference;
    // A packed vertex color (unpackVertexColor) per vertex of a mesh with vertex colors (GpuMesh::flags), mirroring the vertex
    // stream the same way.
    uint64_t geometryColorsReference;

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

    // World units per meter (docs/Units_And_World_Tools_Plan_2026.md, U6): 1 while the project is in meters, 100 for a centimeter
    // project. Every shader-side distance that is a physical length in meters (self-shadowing ray offsets, TMin epsilons, ...)
    // multiplies its literal by this instead of assuming meters.
    float unitsPerMeter;

    // Exponential height fog (the Environment Light's Height Fog; HeightFog.slang), applied by deferred lighting and the
    // skybox: density(y) = heightFogDensity * exp(-heightFogFalloff * (y - heightFogBaseY)), lengths in world units.
    // Read through the pointer (natural layout, like C++): plain 4-byte members, no padding rules to meet.
    float heightFogDensity;          // extinction per world unit at heightFogBaseY; 0 = no fog
    float heightFogFalloff;          // per world unit
    float heightFogBaseY;
    float heightFogStartDistance;
    float heightFogMaxOpacity;
    uint32_t heightFogFromEnvironment; // 1: heightFogColor multiplies the environment's light (its irradiance cubemap)
    float heightFogSunExponent;
    vec3 heightFogColor;
    vec3 heightFogSunDirection;      // towards the sun
    vec3 heightFogSunColor;          // directional inscattering: the sun's light scattered by the fog towards the camera

    // DDGI cascades (DDGICommon.slang): ddgiCascadeCount nested probe volumes around the camera, each with the probe counts of
    // ddgiGridOrigin.w / ddgiGridSpacing.w / ddgiGridParams.x and a wider spacing than the one inside it. Probes are stored cascade
    // after cascade. ddgiUpdateStride: 1 updates every probe of the first cascade each frame, 2 every other one in turn, 4 a quarter
    // (each further cascade half as often again).
    DDGICascade ddgiCascades[4];
    uint32_t ddgiCascadeCount;
    uint32_t ddgiUpdateStride;

    // Screen-probe GI's per-pixel result for the passes that shade something else than the G-buffer's pixels (SceneGI.slang):
    // irradiance / pi, and the distance from the camera of the surface each pixel is for (0: sky). Render resolution, this
    // camera's (jittered) pixels; 0xFFFFFFFF when it did not run.
    uint32_t screenGITextureIndex;
    uint32_t screenGIDistanceTextureIndex;
    // The light the RT shadow mask is for (the first directional light, the sun); 0xFFFFFFFF: none, every light traces its own.
    uint32_t shadowMaskLightIndex;
    // Ray tracing resolution (Settings > Ray Tracing Resolution, independent of DLSS): RT reflections, RT shadows and RTAO trace one
    // pixel of every rtSubsample x rtSubsample block (1: every pixel), which one moving every frame (rtSubsampleOffset: x | y << 16);
    // RTUpsample.slang brings their results back to the render resolution.
    uint32_t rtSubsample;
    uint32_t rtSubsampleOffset;
    // The clouds' shadow (procedural sky with clouds; SkyAtmosphere.slang cloudShadowMain): a map of the cloud layer's
    // transmittance towards the shadow mask's light (the sun, the moon at night), over 2 x cloudShadowExtentKm around the camera
    // (cloudShadowCenterKm, world x / z in km). cloud_shadow() looks it up. 0xFFFFFFFF: none.
    uint32_t cloudShadowTextureIndex;
    float cloudShadowExtentKm;
    vec2 cloudShadowCenterKm;
    // The aerial perspective volume of this frame (over a procedural sky, while deferred lighting runs), for the forward passes'
    // fog (HeightFog.slang sceneFogTerms); 0xFFFFFFFF: none. Km of air per world unit of distance (its distance scale included).
    uint32_t aerialPerspectiveTextureIndex;
    float aerialPerspectiveKmPerUnit;
    // 1: the sky is the procedural one, whose suns and moons are not in the environment map: the directional lights' discs are
    // drawn where rays see them (Skybox.slang for the raster view, directional_light_discs for the path tracer). An HDR map has
    // its own sun in the image (0).
    uint32_t skyDiscs;
    // The shadow-ray budget of the deferred lighting's light loop: per pixel, only the N lights that light it the most trace their
    // shadow rays (the sun through its shadow mask aside); the weaker ones light it unshadowed. 0: every light in range traces.
    uint32_t shadowRayLightLimit;
    // DDGI probe relocation (DDGIRelocation.slang): a texture of each probe's offset from its grid point in cells of its cascade
    // (one texel per probe, DDGI_PROBES_PER_ROW a row), matching the atlases of this frame. 0xFFFFFFFF: probes on the grid.
    uint32_t ddgiProbeOffsetsTextureIndex;
    // The emissive mesh instances the path tracer samples as lights (PathTracer/EmissiveLights.slang): EmissiveLightInstance
    // [emissiveLightCount], sorted by instance slot, with the running sum of their weights (emissiveLightTotalWeight at the end).
    // 0 / 0: none (or the path tracer is off).
    uint32_t emissiveLightCount;
    float emissiveLightTotalWeight;
    uint64_t emissiveLightsReference;
    // The emissive lights that matter where (EmissiveLightGrid.slang): EmissiveGridEntries list entries per cell of a grid of
    // EmissiveGridSizeX/Y/Z cells of emissiveGridCellSize world units from emissiveGridOrigin, around the camera. 0: none.
    uint64_t emissiveGridReference;
    vec3 emissiveGridOrigin;
    float emissiveGridCellSize;
    // The path tracer's emissive triangle lights (EmissiveLightGrid.slang): EmissiveTriangle[emissiveTriangleCount], one per
    // triangle of the emissive instances in list order (EmissiveLightInstance::triangleOffset), and the running sum of their power
    // (float[emissiveTriangleCount]). 0: none.
    uint64_t emissiveTrianglesReference;
    uint64_t emissiveTriangleCdfReference;
    uint32_t emissiveTriangleCount;
    uint32_t emissiveTrianglePadding;
    // Texture LOD biases (negative: sharper). An upscaled image is rendered at fewer pixels than it is shown: its textures are
    // sampled as for the shown resolution (log2(render / display), NVIDIA's DLSS guidance) -- the raster passes'. The path
    // tracer's adds RTXPT's -1. The mips texture streaming is asked for follow the finest of the two that sample this frame.
    float textureLodBias;
    float pathTracerTexLodBias;
    float mipFeedbackLodBias;
    // 1: DLSS Ray Reconstruction denoises this frame's image (no NRD before it): what is drawn into it must keep its noise where
    // RR's guides are (Material_TransparentLit_Mesh's refraction).
    uint32_t rayReconstructionActive;
    // Streaming virtual textures (§5.12.2): GpuVirtualTexture per material texture slot, the page table (uint per entry), the tile
    // requests (VirtualTextureFeedback, in the mip feedback buffer) and the frame each tile was last requested (uint per entry,
    // frame + 1). 0 while no virtual texture is registered.
    uint64_t virtualTexturesReference;
    uint64_t virtualPageTableReference;
    uint64_t virtualFeedbackReference;
    uint64_t virtualTileStampsReference;
    // The sun's cascaded shadow maps (Settings > Sun Shadows: Shadow Maps; Renderer::addShadowMapPasses): GpuShadowCascades of this
    // frame for the passes that shade in their own pixels (the forward and particle passes; ShadowMaps.slang). 0: the sun is
    // shadowed by ray tracing, or not at all. The deferred lighting reads the shadow mask the cascades are resolved into instead.
    uint64_t sunShadowCascadesReference;
    // Local lights' shadow maps (Settings > Local Light Shadows: Shadow Maps; Renderer::addLocalShadowPasses, ShadowMaps.slang):
    // GpuLocalShadows of this frame, and per light (its index in the light buffer) (first view | view count << 16) or
    // LOCAL_SHADOW_NONE -- a light without a map keeps its RT shadow rays. 0: no local light has a shadow map.
    uint64_t localShadowsReference;
    uint64_t localShadowLightViewsReference;
    // Contact shadows (RS2, Settings > Contact Shadows; ShadowMaps.slang contact_shadow): a ray this long (world units) marched
    // towards the light through the depth buffer in contactShadowSamples steps, for the lights shadowed by shadow maps (the detail
    // their texels miss). 0: off.
    float contactShadowLength;
    uint32_t contactShadowSamples;
    // Reflection probes (RS4b; Renderer::addReflectionProbePasses, reflection_probe_radiance in PBRLighting.slang): GpuReflectionProbe
    // [reflectionProbeCount], the smallest boxes first, each with a captured prefiltered cubemap. 0: none (the sky's map alone).
    uint64_t reflectionProbesReference;
    uint32_t reflectionProbeCount;
    uint32_t reflectionProbePadding;
    // The irradiance volume (RS5a, Diffuse GI: Raster Probe Volume; IrradianceVolumeCommon.slang): GpuIrradianceVolume, 0 without one.
    uint64_t irradianceVolumeReference;
    // Volumetric fog (RS6, Settings > Volumetric Fog; VolumetricFog.slang): the integrated froxel volume (Texture2DArray: rgb the
    // light scattered towards the camera up to each slice, a the transmittance), its slices (slice z ends at
    // volumetricFogDistance x ((z + 1) / slices)^2 along the view ray, as the aerial perspective's). 0xFFFFFFFF: none -- the height
    // fog alone (with it, the height fog starts where the volume ends).
    uint32_t volumetricFogTextureIndex;
    uint32_t volumetricFogSlices;
    float volumetricFogDistance;
    // Screen-space GI (Settings > Screen-Space GI; SSGI.slang), for the forward passes (a decal takes its wall's): the denoised result
    // (rgb irradiance / pi x the hit share, a the hit share) and its view depth (x) per pixel. 0xFFFFFFFF: none.
    uint32_t ssgiTextureIndex;
    uint32_t ssgiDepthTextureIndex;
    uint32_t ssgiPadding[3];
    // Order-independent transparency (Settings > Order-Independent Transparency; OITShared.slang): OIT_WEIGHTED_BLENDED, or OIT_AVBOIT
    // (adaptive voxel-based OIT, Drobot, SIGGRAPH 2025): each transparent fragment weighted by the transmittance of what is in front
    // of it, read from a low-resolution volume built from every transparent surface first (OITShared.slang, Renderer::addAVBOITPasses).
    uint32_t oitMode;
    uint32_t avboitGridWidth;   // the volume's columns: the render size / AVBOIT_TILE_SIZE
    uint32_t avboitGridHeight;
    uint32_t avboitSlices;      // physical depth slices
    float avboitDistance;       // world units: the slices end here, along the view ray
    float avboitLinearDistance; // world units: the slices' log curve is nearly linear closer than this
    uint32_t avboitTransmittanceTextureIndex; // Texture2DArray of the transmittance in front of each slice (rgb, a mono); 0xFFFFFFFF none
    uint32_t avboitDepthTextureIndex;         // the scene depth, for the prepasses' own test (they draw at the volume's resolution)
    uint64_t avboitOccupancyReference;  // uint[AVBOIT_VIRTUAL_SLICES / 32]: the virtual slices any transparent surface is in
    uint64_t avboitLutReference;        // AvboitDepthWarp: virtual slice -> physical slice (empty depth ranges skipped)
    uint64_t avboitExtinctionReference; // uint per froxel: the extinction splatted there, 3 x (8 + 2 overflow) bits
    uint64_t avboitOverflowReference;   // uint3 per column: the first slice each channel's extinction overflowed in

    // Screen-space reflections of the transparent surfaces in raster (Renderer::addTransparentReflectionInputs; SSRCommon.slang):
    // this frame's opaque depth and normals to march through, the lit scene behind them is the refraction source (push constants).
    // transparentSSRDepthTextureIndex 0xFFFFFFFF: none this frame (RT reflections run, SSR off, no refraction source).
    uint32_t transparentSSRDepthTextureIndex;
    uint32_t transparentSSRNormalTextureIndex;
    uint32_t transparentSSRHiZTextureIndex; // the closest-depth pyramid of that depth (HiZBuild.slang keepClosest)
    uint32_t transparentSSRHiZLevels;
    uint32_t ssrMaxSteps;
    float ssrMaxDistance;   // world units
    float ssrThickness;     // world units
    float ssrMaxRoughness;
    // Opaque scene depth copy readable by transparent material graphs (Scene Depth / Depth Fade, step 4h); 0xFFFFFFFF when unavailable.
    uint32_t transparentSceneDepthTextureIndex;

    // Seconds since the renderer started, wrapped every hour (float precision stays sub-millisecond): what animated Custom Lit shaders
    // read (CustomLitSurface::time).
    float timeSeconds;
    // Large world coordinates: the world position of the GPU's origin (Renderer::GetRenderOrigin; positions here are relative to it) --
    // a material graph's World Position node adds it, so world-aligned patterns stay put when the origin moves.
    vec3 renderOrigin;
    // timeSeconds of the previous frame (equal to it on the first frame and when the hour wrapped): a material graph's World Position
    // Offset of the previous frame (the G-buffer resolve's motion vectors) reads it.
    float previousTimeSeconds;
};

STATIC_CONST uint32_t OIT_WEIGHTED_BLENDED = 0;
STATIC_CONST uint32_t OIT_AVBOIT = 1;
STATIC_CONST uint32_t AVBOIT_TILE_SIZE = 8;        // pixels per volume column side (the slides' 1/8 resolution)
STATIC_CONST uint32_t AVBOIT_VIRTUAL_SLICES = 1024; // the finest depth resolution; halved until the occupied slices fit the physical ones

// The adaptive depth warp (AVBOIT.slang): built each frame from the occupancy (AVBOITBuild.slang lutMain).
struct AvboitDepthWarp
{
    uint32_t virtualShift; // the virtual resolution used: AVBOIT_VIRTUAL_SLICES >> virtualShift
    uint32_t virtualCount;
    uint32_t usedSlices;   // physical slices holding something
    uint32_t padding;
    uint32_t entries[AVBOIT_VIRTUAL_SLICES]; // physical slice (bits 0-15), occupied (bit 31)
};

struct PushConstantAVBOIT
{
    uint64_t matrixReference;
    uint32_t transmittanceStorageIndex; // RWTexture2DArray<float4> (RGBA8) the integration writes
    uint32_t padding;
};

// An emissive mesh instance as a light for the path tracer's direct light sampling: picked by weight (its emission's luminance
// times its approximate area), then one of its triangles uniformly, then a point on it uniformly.
struct EmissiveLightInstance
{
    uint32_t instanceSlot;  // GPU scene instance slot (the TLAS instanceCustomIndex)
    uint32_t triangleCount; // its ray tracing triangles
    float cdf;              // the weights up to and including this one
    float weight;
    vec3 center;            // its world bounds' center and radius
    float radius;
    uint32_t triangleOffset; // its first triangle in the emissive triangle list (UniformBufferObject::emissiveTrianglesReference)
};

// One emissive triangle as a light (EmissiveLightGrid.slang trianglesMain): its instance and triangle, its area and power (the
// luminance of its average emission -- the emissive texture over the triangle -- times its area), written every frame.
struct EmissiveTriangle
{
    uint32_t instanceSlot;
    uint32_t primitive;
    float power;
    float area;
    vec3 centroid;
    float emissiveTrianglePadding;
};

// The local light grid of the emissive lights (EmissiveLightGrid.slang, PathTracer/EmissiveLights.slang): around the camera, each
// cell holds EmissiveGridEntries picks from the emissive light list, drawn by each light's power over its distance squared from
// the cell. The global list alone (by power) almost never picked the lamp next to a point: the street's string lights and
// signs outweigh it, and rooms lit by their lamps stayed black.
STATIC_CONST uint32_t EmissiveGridSizeX = 32;
STATIC_CONST uint32_t EmissiveGridSizeY = 16;
STATIC_CONST uint32_t EmissiveGridSizeZ = 32;
STATIC_CONST uint32_t EmissiveGridEntries = 16;
STATIC_CONST uint32_t EmissiveGridCandidates = 64; // triangles drawn by power per cell, of which EmissiveGridEntries are kept
STATIC_CONST float EmissiveGridCellMeters = 2.0;

struct PushConstantEmissiveGrid
{
    uint64_t matrixReference;
    uint32_t frameIndex;
    uint32_t emissiveGridPadding;
};

STATIC_CONST uint32_t EmissiveScanGroupSize = 1024; // the power sum's one workgroup (EmissiveLightGrid.slang scanMain)

struct Vertex
{
    vec3 pos;
    vec3 normal;
    vec2 uv0;
	vec2 uv1;
    // xyz: tangent direction, w: bitangent handedness sign (+1/-1); bitangent = cross(normal, tangent.xyz) * tangent.w.
    // Either the glTF file's own authored TANGENT attribute, or meshopt_generateTangents (MikkTSpace-compatible) when a
    // primitive doesn't have one -- see MeshImporter::GenerateMissingTangents. Every cooked mesh has one of the two:
    // GBufferMaterial.slang uses it directly instead of reconstructing a tangent frame per pixel from UV/position
    // derivatives, which -- lacking the handedness this carries -- picked the wrong sign on some UV-mirrored geometry
    // (a symmetric moulding profile, mirrored down the middle to save the artist re-authoring half of it, being the
    // case that actually surfaced it: correct on one half, flipped on the other, a visible seam in specular only).
    vec4 tangent;

    uvec4 boneIDs;
    vec4 boneWeights;

    // glTF COLOR_0 (linear RGBA), multiplying the material's base color. White where a mesh has none: only meshes with vertex colors
    // (MeshData::VertexColors) keep them, in the color stream (geometryColorsReference, §5.8.2).
#ifndef __SLANG__
    vec4 color = vec4(1.0f);
#else
    vec4 color;
#endif
};

// The vertex as the geometry stream stores it (§5.8.2, Nanite-style packing): 36 bytes instead of 88. The position stays a float3 at
// offset 0 (BLAS builds read it as R32G32B32_SFLOAT with this stride) and the UVs stay floats (tiling UVs go far past 0..1); the
// normal and tangent are octahedral (16:16 unorm; the tangent 15:15 plus the bitangent sign in bit 31). Bone influences live in a
// stream of their own (SkinVertex), only for skinned meshes. Shaders read it through unpackVertex / loadMeshVertex.
struct PackedVertex
{
    vec3 pos;
    uint32_t normal;
    vec2 uv0;
    vec2 uv1;
    uint32_t tangent;
};

// A skinned vertex's four bone influences: 16-bit bone indices, 16-bit unorm weights (UniformBufferObject::geometrySkinReference).
struct SkinVertex
{
    uint32_t boneIDs01;
    uint32_t boneIDs23;
    uint32_t boneWeights01;
    uint32_t boneWeights23;
};

// Per-instance output of the compute skinning pass. Position and normal stay in mesh-local space so every raster
// consumer can apply the instance transform, and the previous frame remains usable when only that transform changes.
struct SkinnedVertex
{
    vec4 position;
    vec4 normal;
    vec4 tangent; // xyz skinned like normal (bone matrix, no translation); w (handedness) carried through unchanged.
};

// One compute workgroup owns one skinned mesh instance and walks all of its vertices.
struct SkinningWorkItem
{
    uint32_t sourceVertexOffset;
    uint32_t destinationVertexOffset;
    uint32_t vertexCount;
    uint32_t boneMatrixOffset; // NoSkinningBones: not skinned (morph targets only)
    uint32_t morphWeightOffset; // its weights in the frame's morph weights (one per target of its mesh); NoMorphWeights: none
};
STATIC_CONST uint32_t NoSkinningBones = 0xFFFFFFFF;
STATIC_CONST uint32_t NoMorphWeights = 0xFFFFFFFF;

// Morph targets (§5.8.2): per vertex of a mesh with them, the run of its deltas (PushConstantSkinning::morphRangesReference, mirroring
// the vertex stream); each delta, the target it belongs to and how far that target moves the vertex at weight 1.
struct GpuMorphRange
{
    uint32_t first; // into the morph delta stream
    uint32_t count;
};
struct GpuMorphDelta
{
    uint32_t target;
    vec3 position;
    vec3 normal;
};
STATIC_CONST uint32_t SKINNING_VERTICES_PER_WORK_ITEM = 256;

enum LightType : uint32_t
{
    Directional = 0,
    Point = 1,
    Spot = 2,
    Rect = 3,
    Disc = 4
};

// Filled by Renderer::SubmitLight. Point and spot lights: Intensity arrives multiplied by (world units per meter)^2, so the inverse
// square law in world units is the one in meters whatever the project's unit; Range and the source radius are world units.
// Rect lights: position = the centre, direction.xyz = the side they shine towards, spotParams.xyz = the half-width axis and
// spotParams.w = the half height (world units); Intensity is the radiance of their surface (one-sided, Lambertian). Disc lights
// the same, a circle: both half axes are its radius (spotParams.xyz = an axis of its plane * radius, spotParams.w = radius).
struct LightData
{
    vec4 position;     // xyz: World Position, w: LightType (0 = Directional, 1 = Point, 2 = Spot, 3 = Rect, 4 = Disc)
    vec4 direction;    // xyz: Normalized Direction (directional: towards the light; spot: where it points; rect: its front), w: Range (0.0 = infinite)
    vec4 color;        // rgb: Color, w: Intensity
    vec4 spotParams;   // x/y: spot cone terms, z: source radius (radians for directional, world units otherwise), w: shadow samples
                       // (rect / disc: xyz the half-width axis, w the half height)
};

// GPU instance culling (§5.6): per view, the draw list (instance slots in bucket order) is tested against the view and
// compacted per bucket into the visible instance list and one indirect command per visible instance, without atomics:
// visibility flags in parallel, visible counts per block of entries, block offsets and bucket counts, then each block
// writes its entries (entry order, so transparent sorting survives).
STATIC_CONST uint32_t NoGeometryRange = 0xFFFFFFFF; // GpuMesh offsets: this mesh has no range in that stream
STATIC_CONST uint32_t NoSkinnedVertices = 0xFFFFFFFF;

// Texture streaming feedback (§5.12): one entry per bindless image slot, the finest mip a frame's pixels asked of that
// image, biased so "finer than the image holds" stays representable; MipFeedbackNone when nothing sampled it.
STATIC_CONST uint32_t MipFeedbackSlots = 4096; // the resource heap's image capacity
STATIC_CONST uint32_t MipFeedbackBias = 16;
STATIC_CONST uint32_t MipFeedbackNone = 0xFFFFFFFF;

// Streaming virtual textures (§5.12.2, UE5's SVT). A material texture index with this flag names a virtual texture by its tail
// image's slot (the mips no larger than a tile, always resident). Its mips larger than a tile are tiles in a physical pool of its
// format, found through its page table: one entry per tile of each tiled mip (pageTableOffset + firstTile[mip] + row * tilesX +
// column), always mapping the finest resident tile at or above it -- its own when resident, else an ancestor's -- so a sample takes
// one lookup and never finds a hole. The tail stands in when no ancestor is resident.
STATIC_CONST uint32_t MaterialTextureVirtual = 0x40000000;
STATIC_CONST uint32_t VirtualTileSize = 128;    // texels of a tile, without borders (VirtualTextureLayout::TileSize)
STATIC_CONST uint32_t VirtualTileBorder = 4;    // texels on each side, copied from the neighbors (filtering stays in the tile)
STATIC_CONST uint32_t VirtualTileStride = 136;  // VirtualTileSize + 2 * VirtualTileBorder: a pool slot's texels per side
STATIC_CONST uint32_t VirtualTextureMaxMips = 12; // tiled mips (a 256K texture)
// Anisotropic filtering inside a tile reaches at most a border on each side: footprints longer than this many texels are shortened.
STATIC_CONST uint32_t VirtualTextureMaxAnisotropy = 8;
// A page table entry: the pool slot and the mip of the tile it maps; unmapped (the tail) when no tile at or above it is resident.
STATIC_CONST uint32_t VirtualPageUnmapped = 0xFFFFFFFF;
STATIC_CONST uint32_t VirtualPageSlotMask = 0x00FFFFFF;
STATIC_CONST uint32_t VirtualPageMipShift = 24;
STATIC_CONST uint32_t VirtualTileCapacity = 1u << 20; // page table entries of all virtual textures (4 MB)
// Tile requests per frame (MipFeedback.slang reportVirtualTextureFeedback): one per tile and frame; a missing tile asks every
// frame it is sampled, a resident one once per VirtualTileTouchInterval frames (the streamer evicts what stopped asking first).
STATIC_CONST uint32_t VirtualTextureRequestCapacity = 16384;
STATIC_CONST uint32_t VirtualTileTouchInterval = 16;
STATIC_CONST uint32_t VirtualRequestResident = 0x80000000; // request flag: the tile is resident (a touch, not a load)

struct GpuVirtualTexture // per material texture slot (its tail image's), UniformBufferObject::virtualTexturesReference
{
    uint32_t width;           // texels of mip 0
    uint32_t height;
    uint32_t mipLevels;       // the whole chain, tail included
    uint32_t tiledMips;       // mips [0, tiledMips) are tiles; the tail image's mip 0 is mip tiledMips
    uint32_t pageTableOffset; // its first page table entry
    uint32_t poolTextureIndex; // its physical pool's image slot
    uint32_t poolTilesPerRow;
    float poolInvSize;        // 1 / the pool's texels per side
    uint32_t firstTile[VirtualTextureMaxMips]; // per tiled mip, from pageTableOffset
};

// Appended to the mip feedback buffer (after MipFeedbackSlots entries): cleared and read back with it.
struct VirtualTextureFeedback
{
    uint32_t count;
    uint32_t padding[3];
    uint32_t requests[VirtualTextureRequestCapacity]; // page table index | VirtualRequestResident
};

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

// Indirect mesh draw record for drawMeshTasksIndirectCountWithDrawId (16 bytes):
// on D3D12, ExecuteIndirect writes drawID to root constant b1 before DispatchMesh;
// on Vulkan, vkCmdDrawMeshTasksIndirectCountEXT reads groupCountX/Y/Z at offset + 4 while shaders use SV_DrawIndex.
struct MeshTasksIndirectCommand
{
    uint32_t drawID;
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
    uint32_t rawReflectionIndex;         // RT reflections (alpha: hit distance), 0xFFFFFFFF when they did not run
    uint32_t velocityTextureIndex;       // the G-buffer's motion vectors
    uint32_t specularMotionStorageIndex; // DLSS-RR's specular motion vectors (RTXPT's), written every frame
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
    uint32_t keepClosest;   // 0: the farthest depth of the texels covered (occlusion culling), 1: the closest (SSR's Hi-Z tracing)
    uint32_t padding;
};

STATIC_CONST uint32_t MESHLET_CULL_FRUSTUM = 1;     // PushConstantMeshlets.meshletCulling
STATIC_CONST uint32_t MESHLET_COUNT_TRIANGLES = 2;  // PushConstantMeshlets.meshletCulling: add the drawn clusters to ClusterStats
STATIC_CONST uint32_t MESHLET_SHADE_OPAQUE = 4;     // PushConstantMeshlets.meshletCulling: Material_TransparentLit_Mesh shades an opaque / masked surface (alpha 1, no transmission; the editor's extra views)
// A material is see-through (glass: the raster transparent path, rays pass it) from this much transmission on. Less is a thin
// solid surface that lets some light through -- RTXPT's leaves (0.36-0.55, alpha tested), fabrics, curtains: rendered solid
// (cut out where masked), its transmission only for the path tracer and its diffuse transmission for the back light. Routing those
// through the glass path drew every leaf as glass: no depth, sorted per object (see-through foliage), ~60 ms looking at a tree.
STATIC_CONST float SeeThroughTransmission = 0.5;

// How much of the stops a dark scene is under mid-grey auto exposure makes up (1: all; the eye keeps a night darker).
STATIC_CONST float AutoExposureDarkAdaptation = 0.6;
// What auto exposure maps the image's average luminance to: Unreal's calibration, a reflected-light meter's EV100 = log2(L x 100 /
// 12.5) shown at 1 / (1.2 x 2^EV100), so L becomes 1 / (1.2 x 8). (RTXPT's key, 0.042, put the average 2.3 stops lower: daylight
// looked dim, a frame of sky alone dark.)
STATIC_CONST float AutoExposureKey = 0.104167;

// Path tracer material flags (GpuMaterial / GpuRayTracingInstance::pathTracerFlags), from RTXPT's material files when present.
STATIC_CONST uint32_t PathTracerThinSet = 1u;         // the material says whether it is thin (else: thin when it has no thickness)
STATIC_CONST uint32_t PathTracerThin = 2u;            // thin-walled: no refraction, 1 / IoR on both faces (RTXPT ThinSurface)
STATIC_CONST uint32_t PathTracerExcludeFromNEE = 4u;  // shadow rays pass it, by its color only (RTXPT ExcludeFromNEE: window and lantern glass)
STATIC_CONST uint32_t PathTracerRtxptMaterial = 8u;   // from an RTXPT material file: shadow rays follow RTXPT's rule (block
                                                      // unless ExcludeFromNEE or cut out), not the tinted pass-through
// RTXPT's PSDDominantDeltaLobe (which of a smooth pane's lobes DLSS-RR's guides follow): neither flag the transmission (0, the
// default), PathTracerPSDDominantReflection the reflection (1), PathTracerPSDDominantSurface the pane itself (-1).
STATIC_CONST uint32_t PathTracerPSDDominantReflection = 16u;
STATIC_CONST uint32_t PathTracerPSDDominantSurface = 32u;

// The mip the hybrid passes' rays read an alpha-masked surface's opacity at (shadows, AO, GI probes, glass). Full resolution: thin
// cut-outs (needles, small leaves) average below their cutoff in coarser mips, the rays then passed through the canopy and leaves
// inside trees lit up in sunlit speckles.
STATIC_CONST float RayAlphaTestLod = 0.0;
STATIC_CONST uint32_t MESHLET_RT_REFRACTION = 8;    // PushConstantMeshlets.meshletCulling: Material_TransparentLit_Mesh traces refraction rays for smooth glass (Hybrid_Renderer_Plan H3)
// PushConstantMeshlets.meshletCulling, the task stage: only the instances that are see-through (ray_see_through: glass), or all but
// them (the forward transparent pass draws the glass in passes of its own, MESHLET_GLASS_MULTIPLY / _ADD).
STATIC_CONST uint32_t MESHLET_SEE_THROUGH_ONLY = 32;
STATIC_CONST uint32_t MESHLET_SKIP_SEE_THROUGH = 64;
// PushConstantMeshlets.meshletCulling, Material_TransparentLit_Mesh: the glass in two passes over the scene, in any order
// (Renderer::addForward3DPass): first what each pane lets through, multiplied into the scene (MESHLET_GLASS_MULTIPLY), then the light
// each gives itself, added (MESHLET_GLASS_ADD) -- panes behind panes (a lantern's cylinder: in at the front, out at the back) all
// tint what is behind them, as in the path tracer. Thick or rough glass, whose view behind is bent or blurred (the color pyramid),
// blocks what is behind in the first pass and adds it, with its own light, in the second.
STATIC_CONST uint32_t MESHLET_GLASS_MULTIPLY = 128;
STATIC_CONST uint32_t MESHLET_GLASS_ADD = 256;

struct PushConstantMeshlets
{
    uint64_t matrixReference;
    uint64_t drawInstancesReference; // visible instance slots of this view (instanceBaseIndex + SV_DrawIndex)
    uint64_t boneMatrixReference;
    uint64_t skinnedVertexReference;
    uint32_t instanceBaseIndex;
    uint32_t meshletCulling; // MESHLET_* flags (MESHLET_CULL_FRUSTUM off: every cluster of the LOD cut is drawn)
    // Only meaningful to Material_TransparentLit_Mesh.slang's fragment stage (the forward pass has no G-buffer for
    // DeferredLighting.slang to read this from instead): set once per pass, after SSAO/RTAO have run, by
    // Renderer::addForward3DPass -- every other consumer of this struct (task/mesh stages, the opaque G-buffer
    // resolve) leaves it unread. 0xFFFFFFFF when neither SSAO nor RTAO ran this frame.
    uint32_t ssaoTextureIndex;
    // Material_TransparentLit_Mesh.slang only: the lit scene behind transparent surfaces with its blur mips (ColorPyramid.slang),
    // which transmissive materials refract. 0xFFFFFFFF: none this pass (transmission then only lets the background show through).
    uint32_t refractionTextureIndex;
};

// World Position Offset in the ray tracing scene (material graph step 4c part 4; RayTracingOffset.slang through its generated graph
// variant): an instance whose material graph offsets its vertices gets its moved vertices written here each frame, its own BLAS refit
// from them (Renderer::updateAnimatedBLAS) and its hits read them (GpuRayTracingInstance::skinnedVertexBufferAddress). One work item per
// chunk of SKINNING_VERTICES_PER_WORK_ITEM vertices.
struct RayTracingOffsetWorkItem
{
    uint32_t instanceSlot;
    uint32_t firstVertex;             // of the mesh
    uint32_t vertexCount;
    uint32_t destinationVertexOffset; // the instance's first vertex in the output (SkinnedVertex)
    uint32_t skinnedVertexOffset;     // its skinned vertices this frame (compute skinning's output); NoSkinnedVertices: the mesh's own
};

struct PushConstantRayTracingOffset
{
    uint64_t uniformsReference;       // UniformBufferObject
    uint64_t workItemsReference;      // RayTracingOffsetWorkItem
    uint64_t skinnedVerticesReference; // compute skinning's output this frame (0: none)
    uint64_t outputReference;         // SkinnedVertex
};

struct PushConstantSkinning
{
    uint64_t sourceVerticesReference; // PackedVertex
    uint64_t sourceSkinReference;     // SkinVertex, same indices
    uint64_t skinnedVerticesReference;
    uint64_t boneMatricesReference;
    uint64_t workItemsReference;
    uint64_t morphRangesReference;    // GpuMorphRange, same indices as the vertices
    uint64_t morphDeltasReference;    // GpuMorphDelta
    uint64_t morphWeightsReference;   // float, SkinningWorkItem::morphWeightOffset
};

// Geometry streaming (§5.7.2, Nanite's page streaming). A streamed mesh's clusters live in pages; its records (MeshletDraw,
// MeshletBounds) always stay, the pages' vertices and triangles come and go. One record per page of every streamed mesh
// (GpuMesh::pageOffset + page): what the feedback pass needs to ask for it, and whether it is resident -- written by the frame
// that activates or evicts it (Renderer::addGeometryPagesUpdatePass), so every pass of a frame sees one residency.
struct GpuGeometryPage
{
    vec3 lodCenter;        // conservative LOD bounds of its groups, mesh space (MeshPage::LodCenter)
    float lodRadius;
    float lodError;        // ClusterTerminalError: a root page, always resident, never asked for
    uint32_t resident;     // 1 while its clusters may be drawn
    uint32_t requestFrame; // GPU only: the last feedback frame that reported it (one report per page and frame)
    uint32_t padding;
};

// The feedback pass (GeometryStreamingFeedback.slang): every streamed instance's pages whose LOD error at its distance is above the
// threshold are wanted. Missing pages are requested every frame, resident ones report every GeometryTouchInterval frames so the
// streamer knows what is still in use (its LRU). Read back per frame slot.
STATIC_CONST uint32_t GeometryRequestCapacity = 16384;
// Pages are asked for once their error passes this fraction of the LOD threshold: resident (and in the ray tracing leaf cut) before
// the raster cut needs them, so a newly refined surface is never finer in the raster than in the BLAS (raster_surface_bias).
STATIC_CONST float GeometryPrefetchFactor = 0.5;
STATIC_CONST uint32_t GeometryTouchInterval = 16;
STATIC_CONST uint32_t GeometryRequestResident = 0x80000000; // GeometryPageRequest::page: a resident page reporting it is in use
struct GeometryPageRequest
{
    uint32_t page;     // global page (the page table's index), | GeometryRequestResident
    float priority;    // how far above the threshold its error projects (the largest first)
};
struct GeometryStreamingFeedback
{
    uint32_t count;    // requests written (may pass the capacity: the rest were dropped)
    uint32_t padding0;
    uint32_t padding1;
    uint32_t padding2;
    GeometryPageRequest requests[GeometryRequestCapacity];
};

struct PushConstantGeometryFeedback
{
    uint64_t matrixReference;    // the main view's uniforms (its culling camera and LOD threshold)
    uint64_t drawListReference;  // instance slots of the draw list
    uint64_t feedbackReference;  // GeometryStreamingFeedback
    uint32_t drawCount;
    uint32_t groupCount;         // workgroups dispatched (each walks entries groupCount apart)
    uint32_t frame;
};

// A streamed mesh's BLAS is built from its finest resident clusters (the leaf cut); ClusterIndices.slang writes their triangles as
// vertex stream indices into the RT index stream, one job per cluster.
struct ClusterIndexJob
{
    uint32_t draw;       // MeshletDraw index in the draw stream
    uint32_t firstIndex; // where its triangles go in the RT index stream
};
struct PushConstantClusterIndices
{
    uint64_t matrixReference;  // uniforms: the geometry stream addresses
    uint64_t jobsReference;
    uint64_t indicesReference; // the RT index stream
    uint32_t jobCount;
    uint32_t groupCount;
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
    // Geometry streaming (§5.7.2): the page this cluster's data lives in, and the page holding the group it was simplified from
    // (its finer version; ClusterNoPage for the original clusters). Pages of the mesh, numbered from 0 (GpuMesh::pageOffset).
    uint32_t page;
    uint32_t refinedPage;
};
STATIC_CONST uint32_t ClusterNoPage = 0xFFFFFFFF;

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
    float rotation; // EquirectToCubemap / CubemapResample: the environment's turn around world +Y (radians), baked in
};

// SkyAtmosphere.slang, the procedural sky (Hillaire 2020, UE's SkyAtmosphere): one struct for its three compute passes
// (transmittance LUT, multiple-scattering LUT, environment cubemap). Lengths in km inside the shader.
struct PushConstantSkyAtmosphere
{
    // Push constants compile with std430 rules (GLSLForceScalarLayout does not reach them): a vec3 sits on a 16-byte
    // boundary. Each vec3 here is placed there and followed by a 4-byte member, so std430 and the C++ struct agree.
    uint32_t outputStorageIndex;          // what this pass writes (storage slot): a LUT, or the environment cubemap
    uint32_t transmittanceTextureIndex;   // sampled slot of the transmittance LUT (read by the two later passes)
    uint32_t multiScatteringTextureIndex; // sampled slot of the multiple-scattering LUT (read by the cubemap pass)
    uint32_t outputSize;                  // width of what is written (LUT width, or the cubemap face size)
    vec3 sunDirection;                    // world space, toward the sun
    uint32_t outputHeight;                // LUT height (unused by the cubemap pass)
    vec3 sunIlluminance;                  // the sun's color * intensity (the scene's directional light)
    float rayleighScale;                  // 1 = Earth: the blue of the sky
    vec3 groundAlbedo;
    float mieScale;                       // 1 = Earth: haze
    float ozoneScale;                     // 1 = Earth: the deep blue at the zenith / purple at dusk
    float mieAnisotropy;                  // Mie phase g (0.8: forward-scattering glow around the sun)
    float cameraAltitudeKm;               // where the sky is seen from, above the ground
    float padding;
    // Volumetric clouds (CloudNoise.slang textures; cloudShapeTextureIndex ~0 = no clouds): a layer cloudBottomKm above
    // the ground, cloudThicknessKm thick.
    uint32_t cloudShapeTextureIndex;      // 2D array standing in for a 3D texture: Perlin-Worley (r) + Worley FBMs (gba)
    uint32_t cloudDetailTextureIndex;     // 2D array: Worley FBMs eroding the edges
    uint32_t cloudWeatherTextureIndex;    // 2D: where clouds are (coverage variation)
    float cloudCoverage;                  // 0 = clear, 1 = overcast
    float cloudDensity;                   // scales the extinction (1 = cumulus, ~40 / km)
    float cloudBottomKm;
    float cloudThicknessKm;
    float cloudScaleKm;                   // size of one tile of the shape noise
    float cloudOffsetXKm;                 // moves the clouds (wind)
    float cloudOffsetZKm;
    // downsampleCubeMain: the cubemap read (sampled slot) and the mip read from it.
    uint32_t sourceTextureIndex;
    float sourceLod;
    // The moon (at night): the air and the clouds scatter its light as they do the sun's. moonIlluminance 0: none (by day).
    vec3 moonDirection;                   // world space, towards the moon
    uint32_t firstFace;                   // skyCubemapMain: the cube face of dispatch z = 0 (one face per frame while the wind blows);
                                          // skyHorizonBandMain: the first column written
    vec3 moonIlluminance;                 // its color * intensity * the lit fraction of its disc, above the atmosphere
    // cloudShadowMain: the square it covers (half size, centre: world x / z in km) towards sunDirection (the shadow mask's light).
    float cloudShadowExtentKm;
    vec2 cloudShadowCenterKm;
    float cloudShadowPadding0;
    float cloudShadowPadding1;
};

// LightShapes.slang: the rect lights' faces drawn into the lit scene.
struct PushConstantLightShapes
{
    uint64_t matrixReference; // BDA to UniformBufferObject
};

// EditorRaycast.slang: rays the editor asks about (Renderer::RequestRaycast), one thread each, against the scene's TLAS. The buffer
// (host visible, one per frame in flight) holds the rays and receives the distance of each one's first opaque hit, -1 for none.
STATIC_CONST uint32_t EDITOR_RAYCAST_MAX = 64;
struct EditorRay
{
    vec3 origin;
    float maxDistance;
    vec3 direction;
    float hitDistance; // written by the shader
};

struct PushConstantEditorRaycast
{
    uint64_t matrixReference; // BDA to UniformBufferObject (its TLAS)
    uint64_t raysReference;   // BDA to EditorRay[rayCount]
    uint32_t rayCount;
    uint32_t padding;
};

// OITComposite.slang: the order-independent transparency's targets, resolved over the HDR scene (descriptor heap slots, the
// image heap's offset already added).
struct PushConstantOITComposite
{
    uint32_t accumIndex;
    uint32_t revealIndex;
    uint32_t mode;    // OIT_COMPOSITE_MULTIPLY: the scene times what the layers let through; OIT_COMPOSITE_ADD: the layers' light added
    uint32_t padding;
};
STATIC_CONST uint32_t OIT_COMPOSITE_MULTIPLY = 0;
STATIC_CONST uint32_t OIT_COMPOSITE_ADD = 1;

struct PushConstantSkybox
{
    uint64_t matrixReference; // BDA to UniformBufferObject
    uint32_t cubemapIndex;     // Index in descriptor heap
    // The sun's disc over a procedural sky (SkyAtmosphere.slang); skyTransmittanceIndex is ~0 for an HDR environment, whose
    // image has its own sun. Each vec3 on a 16-byte boundary followed by a float (std430 push constants, see above).
    uint32_t skyTransmittanceIndex; // sampled slot of the sky's transmittance LUT
    vec3 sunDirection;              // world space, towards the sun
    float sunCosAngularRadius;
    vec3 sunDiscRadiance;           // at the disc's center above the atmosphere: illuminance / solid angle (capped)
    float skyAltitudeKm;            // the viewer's height in the sky model
    // The moon's disc (lit by the sun: its phase), and the stars; the cubemap's alpha (the clouds' transmittance) hides them.
    vec3 moonDirection;             // world space, towards the moon
    float moonCosAngularRadius;
    vec3 moonDiscRadiance;          // a full moon's, at the disc's centre above the atmosphere (0: no moon)
    float starIntensity;            // 0: no stars (daylight)
    uint32_t horizonBandIndex;      // sampled slot of the sky's horizon band (SkyHorizonBand*), ~0: the cubemap everywhere
    uint32_t skyboxPad0;
    uint32_t skyboxPad1;
    uint32_t skyboxPad2;
};

// The procedural sky's horizon band: the sky and its clouds near the horizon at about the screen's resolution, where the
// environment cubemap's texels (0.35 degrees) are hundreds of meters wide on clouds tens of km away. A panorama: x the
// azimuth (atan2(z, x) over the full circle), y the elevation from SkyHorizonBandMinDegrees (bottom row) to ...MaxDegrees.
// The skybox draws it in place of the cubemap there (fading into it at the top); lighting and reflections keep the cubemap.
STATIC_CONST uint32_t SkyHorizonBandWidth = 4096;  // 0.088 degrees a texel
STATIC_CONST uint32_t SkyHorizonBandHeight = 192;  // 0.073 degrees a texel
STATIC_CONST float SkyHorizonBandMinDegrees = -2.0;
STATIC_CONST float SkyHorizonBandMaxDegrees = 12.0;
STATIC_CONST uint32_t SkyHorizonBandSlices = 8;    // rendered a slice of columns per frame while the wind blows

// Aerial perspective (SkyAtmosphere.slang aerialPerspectiveMain): the atmosphere between the camera and the scene, as a
// camera-aligned volume of AerialPerspectiveSize x AerialPerspectiveSize froxels and AerialPerspectiveSlices depth slices
// (a 2D array texture: rgb = light scattered towards the camera up to the slice, a = mean transmittance). Slice z ends at
// maxDistanceKm * ((z + 1) / slices)^2: finer near the camera. Deferred lighting applies it to the lit scene.
// NRD's denoising range (CommonSettings::denoisingRange), in meters: farther pixels (the sky) are left alone.
STATIC_CONST float NRDDenoisingRangeMeters = 100000.0;

STATIC_CONST uint32_t AerialPerspectiveSize = 32;
STATIC_CONST uint32_t AerialPerspectiveSlices = 32;
STATIC_CONST float AerialPerspectiveMaxDistanceKm = 32.0;

struct PushConstantAerialPerspective
{
    uint64_t matrixReference;           // BDA to UniformBufferObject (camera)
    uint32_t outputStorageIndex;        // storage slot of the volume
    uint32_t transmittanceTextureIndex;
    vec3 sunDirection;                  // world space, towards the sun
    uint32_t multiScatteringTextureIndex;
    vec3 sunIlluminance;
    float rayleighScale;
    float mieScale;
    float ozoneScale;
    float mieAnisotropy;
    float kmPerWorldUnit;               // the camera's position in the atmosphere
    float altitudeKm;                   // the world origin's height above the ground
    float padding0;
    float padding1;
    float padding2;
    vec3 moonDirection;                 // the moon at night (moonIlluminance 0: none)
    float moonPadding0;
    vec3 moonIlluminance;
    float moonPadding1;
};

#ifndef __SLANG__
// The std430 offsets the shaders read (checked against the SPIR-V of the engine's own compile path).
static_assert(offsetof(PushConstantSkyAtmosphere, sunDirection) == 16 && offsetof(PushConstantSkyAtmosphere, sunIlluminance) == 32 &&
              offsetof(PushConstantSkyAtmosphere, groundAlbedo) == 48 && offsetof(PushConstantSkyAtmosphere, cameraAltitudeKm) == 72 &&
              offsetof(PushConstantSkyAtmosphere, cloudShapeTextureIndex) == 80 && offsetof(PushConstantSkyAtmosphere, sourceLod) == 124 &&
              offsetof(PushConstantSkyAtmosphere, moonDirection) == 128 && offsetof(PushConstantSkyAtmosphere, moonIlluminance) == 144 &&
              offsetof(PushConstantSkyAtmosphere, cloudShadowCenterKm) == 160 &&
              sizeof(PushConstantSkyAtmosphere) == 176, "PushConstantSkyAtmosphere must match its std430 layout");
static_assert(offsetof(PushConstantSkybox, sunDirection) == 16 && offsetof(PushConstantSkybox, sunDiscRadiance) == 32 &&
              offsetof(PushConstantSkybox, skyAltitudeKm) == 44 && offsetof(PushConstantSkybox, moonDirection) == 48 &&
              offsetof(PushConstantSkybox, moonDiscRadiance) == 64 && offsetof(PushConstantSkybox, starIntensity) == 76 &&
              offsetof(PushConstantSkybox, horizonBandIndex) == 80 && sizeof(PushConstantSkybox) == 96, "PushConstantSkybox must match its std430 layout");
static_assert(offsetof(PushConstantAerialPerspective, sunDirection) == 16 && offsetof(PushConstantAerialPerspective, sunIlluminance) == 32 &&
              offsetof(PushConstantAerialPerspective, altitudeKm) == 64 && offsetof(PushConstantAerialPerspective, moonDirection) == 80 &&
              offsetof(PushConstantAerialPerspective, moonIlluminance) == 96 && sizeof(PushConstantAerialPerspective) == 112,
              "PushConstantAerialPerspective must match its std430 layout");
#endif

struct PushConstantVisibilityDebug
{
    uint64_t matrixReference;
    uint64_t boneMatrixReference;
    uint64_t skinnedVertexReference;
    uint64_t previousSkinnedVertexReference;
    uint32_t visibilityTextureIndex;
    uint32_t debugMode; // 0 = Albedo, 1 = Normal, 2 = Roughness, 3 = Metallic, 4 = Emission, 5 = Occlusion, 6 = Colored Meshlets
    vec2 viewportSize;
    uint32_t skinnedHistoryValid;
    uint32_t gbufferAlbedoIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t gbufferEmissionIndex;
    // This pixel's own depth, already resolved by hardware rasterization in the Visibility pass: the G-Buffer resolve
    // reconstructs its world position (and so its barycentric weights) from it, not a world-space ray/triangle-plane
    // intersection (numerically unstable at grazing angles -- see GBufferMaterial.slang).
    uint32_t depthTextureIndex;
};

// NRD's view Z and packed normal/roughness guides (NRDGuides.slang).
struct PushConstantNRDGuides
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex; // the lobe bits (gbuffer_load_lobes: the reflection layer)
    uint32_t padding;              // (viewportSize at an 8-byte offset)
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
    uint32_t gbufferMaterialIndex; // its alpha: double-sided (foliage, cloth), shadowed from the side the light is on
};

// The sun's cascaded shadow maps (RS1; Renderer::addShadowMapPasses, ShadowMaps.slang), written by the CPU per frame slot. Stable
// cascades (UE5's directional light CSM): each covers a bounding sphere of its slice of the camera's view, its texels snapped to the
// world so the shadow edges stand still as the camera moves. Orthographic, reverse Z (1 towards the sun); casters in front of the
// near plane are flattened onto it (depth clamp).
STATIC_CONST uint32_t MaxShadowCascades = 4;
STATIC_CONST uint32_t SHADOW_FILTER_PCF = 0;  // a fixed kernel of shadowFilterTexels
STATIC_CONST uint32_t SHADOW_FILTER_PCSS = 1; // contact hardening: the kernel grows with the distance to the caster by the sun's angular radius

struct GpuShadowCascades
{
    mat4 viewProj[4];       // MaxShadowCascades: render space -> the cascade's clip space
    vec4 splitFar;          // the view depth (world units along the camera's forward axis) where each cascade ends
    vec4 texelWorldSize;    // world units per texel of each cascade
    vec4 depthRangeWorld;   // world units its depth range [0, 1] spans
    vec4 lightDirection;    // xyz: towards the sun; w: tan of its angular radius (PCSS)
    uint32_t textureIndex[4]; // MaxShadowCascades: bindless slots of the depth maps
    uint32_t count;
    uint32_t filter;        // SHADOW_FILTER_*
    uint32_t resolution;    // texels per side
    float normalBias;       // texels the receiver moves along its normal before the lookup
    float depthBias;        // texels (of world size) the receiver moves towards the sun
    float blendFraction;    // the last part of each cascade's depth range that dithers into the next one; the last cascade fades out
    float filterTexels;     // PCF kernel radius / PCSS minimum
    float maxFilterTexels;  // PCSS kernel radius limit
    uint32_t padding[3];
};

// A reflection probe (RS4b, UE5's box reflection capture / Godot's ReflectionProbe): the scene seen from its position, captured into a
// cubemap and prefiltered like the sky's; inside its box (the entity's transform, Extents) the specular environment comes from it,
// box-projected (the reflection ray met with the box, looked up from the capture point), fading out over the blend distance.
STATIC_CONST uint32_t MaxReflectionProbes = 32;

struct GpuReflectionProbe
{
    mat4 worldToLocal;      // render space -> the probe's box space (the entity's transform inverted)
    mat4 localToWorld;
    vec4 extents;           // xyz: the box's half size (local units), w: intensity
    vec4 blend;             // xyz: the blend distance per axis (local units), w: the cubemap's mip count
    vec4 center;            // xyz: where it was captured (render space)
    uint32_t cubeTextureIndex;
    uint32_t padding[3];
};

// An irradiance volume (RS5a, GI without rays; Renderer::addIrradianceVolumePasses): a grid of probes over the entity's box. Each
// probe's surroundings are rasterized once into six small faces as a G-buffer (diffuse albedo, emission, normal, distance: the
// probe's surface cache, IrradianceProbeSH.slang packMain), captured again only where the scene changed; every frame a share of the
// probes is lit again from it (relightMain: the sun through the volume's own shadow map, the local lights, the sky, emission, and
// the volume itself for further bounces) and projected onto L1 spherical harmonics -- the light changes without drawing the scene
// (Enlighten's / Frostbite's precomputed-visibility relighting; UE's volumetric lightmap, lit at runtime). probeSH: per probe 3
// float4 (R, G, B: L00, L1-1, L10, L11 of the radiance), a fourth: x 1 once lit (0: not yet -- the lookup leaves it out), then L1
// SH of the distance to what the probe sees and of its square (visibility: DDGI's Chebyshev test against walls between a probe and
// the point).
STATIC_CONST uint32_t IrradianceProbeFaceSize = 16;          // texels per side of a capture face
STATIC_CONST uint32_t IrradianceProbeSHStride = 7;           // float4s per probe in GpuIrradianceVolume::probeSHReference
STATIC_CONST uint32_t MaxIrradianceProbesPerFrame = 16;      // captures a frame
STATIC_CONST uint32_t MaxIrradianceRelightsPerFrame = 4096;  // probes lit again a frame

struct GpuIrradianceVolume
{
    mat4 worldToLocal;      // render space -> the volume's box space
    mat4 localToWorld;
    mat4 sunViewProj;       // the volume's sun shadow map (orthographic over the whole box, reverse Z)
    vec4 extents;           // xyz: the box's half size (local units), w: intensity
    uint32_t countX;
    uint32_t countY;
    uint32_t countZ;
    uint32_t probeCount;
    uint64_t probeSHReference; // float4[IrradianceProbeSHStride] per probe (R, G, B SH, x = lit, distance SH, distance^2 SH, sky visibility SH), x fastest, then y, then z
    // uint4[6 x cacheFaceSize^2] per probe, face by face (+X, -X, +Y, -Y, +Z, -Z), rows down: x albedo rgb + sky fraction (unorm8 x4),
    // y emission rg (half x2), z emission b + distance (half x2), w the normal (octahedral, snorm16 x2).
    uint64_t surfaceCacheReference;
    float normalBias;       // local units the lookup moves along the normal (away from the surface's own probe gaps)
    uint32_t cacheFaceSize; // texels per side of a face in the surface cache
    uint32_t sunShadowTextureIndex; // sampled slot of the sun shadow map (~0: none -- unshadowed)
    uint32_t sunShadowResolution;
    float sunTexelWorld;    // world units per texel of the sun shadow map
    float sunDepthRange;    // world units its depth spans
    uint32_t padding0;
    uint32_t padding1;
};

// A frame's captured faces into the probes' surface caches and distance SH (IrradianceProbeSH.slang packMain): row k of the atlases
// holds probe k's six faces (+X, -X, +Y, -Y, +Z, -Z).
struct PushConstantIrradianceProbePack
{
    uint64_t probeIndicesReference; // uint[probesThisFrame]: each row's probe in the volume
    uint64_t volumeReference;       // GpuIrradianceVolume
    uint32_t albedoTextureIndex;    // rgb diffuse albedo, a the normal's octahedral x
    uint32_t emissionTextureIndex;  // rgb emission, a the normal's octahedral y
    uint32_t depthTextureIndex;     // the faces' depth (reverse Z, infinite: view depth = nearPlane / depth; 0 the sky)
    uint32_t probesThisFrame;
    float nearPlane;
    float maxDistance;              // distances are clamped here (the sky: this far)
};

// Probes lit again from their surface caches (IrradianceProbeSH.slang relightMain): one group per probe.
struct PushConstantIrradianceRelight
{
    uint64_t matrixReference;       // the frame's uniforms: lights, shadow maps, the volume itself (the bounce)
    uint64_t probeIndicesReference; // uint[probeCount]
    uint64_t volumeReference;       // GpuIrradianceVolume
    uint32_t probeCount;
    uint32_t frameIndex;
};

// The capture's six rendered faces into a probe's cubemap (ReflectionProbe.slang assembleMain): per cube texel, its direction, the
// face rendered towards it and that face's pixel.
struct PushConstantProbeAssemble
{
    uint64_t facesReference;   // mat4 viewProj[6] (+X, -X, +Y, -Y, +Z, -Z), then vec4 center (render space)
    uint32_t faceTextureIndex0;
    uint32_t faceTextureIndex1;
    uint32_t faceTextureIndex2;
    uint32_t faceTextureIndex3;
    uint32_t faceTextureIndex4;
    uint32_t faceTextureIndex5;
    uint32_t outputStorageIndex; // the cubemap's mip being written (RWTexture2DArray)
    uint32_t outputSize;
    uint32_t sourceTextureIndex; // downsampleMain: the cubemap (sampled), its mip outputMip - 1
    uint32_t outputMip;
};

// Local lights' shadow maps (RS1b; Renderer::addLocalShadowPasses): perspective views in one depth atlas, UE5's local light shadows
// without virtual shadow maps. A spot light has one view (its cone); point, rect and disc lights a cube of six (+X, -X, +Y, -Y, +Z,
// -Z), a face behind a rect / disc light left out (an empty atlasRect: lit). Each view's field of view is widened by a border of
// texels, so a filter kernel at the face's edge stays in its own tile. Infinite reverse-Z projection: depth = nearPlane / view depth.
STATIC_CONST uint32_t MaxLocalShadowViews = 96;
STATIC_CONST uint32_t LOCAL_SHADOW_NONE = 0xFFFFFFFF;

struct GpuShadowView
{
    mat4 viewProj;        // render space -> the view's clip space (w = the depth along the view)
    vec4 atlasRect;       // xy: the tile's corner in atlas uv, zw: its size (0: no tile, lit)
    float nearPlane;      // the view depth at depth 1
    float texelPerDepth;  // world size of a texel per unit of view depth (2 tan(half fov) / tile texels)
    uint32_t tileTexels;
    uint32_t borderTexels; // the widened field of view's border (the filter's reach)
};

struct GpuLocalShadows
{
    uint32_t atlasTextureIndex;
    uint32_t atlasSize;      // texels per side
    uint32_t viewCount;
    uint32_t filter;         // SHADOW_FILTER_* (the sun's setting)
    float normalBias;        // texels, as GpuShadowCascades
    float depthBias;
    float filterTexels;
    float maxFilterTexels;
    GpuShadowView views[96]; // MaxLocalShadowViews
};

// The shadow maps resolved into the screen's shadow mask (ShadowMapResolve.slang), the RT shadow mask's format.
struct PushConstantShadowMapResolve
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint64_t cascadesReference; // GpuShadowCascades
    uint32_t depthTextureIndex;
    uint32_t gbufferNormalIndex;
    uint32_t gbufferMaterialIndex;
    uint32_t frameIndex;
    vec2 viewportSize;
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
    // > 0: the full-resolution pass after a lower ray tracing resolution's upsample: every pixel traces its own ray, but only where
    // the surface is smoother than this roughness (sharp reflections show the reduced resolution); the others keep the upsampled one.
    float smoothFillRoughness;
    uint32_t gbufferAlbedoIndex; // the specular color for the emissive light sampling (Reflection.slang); ~0: none
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

    uint32_t ssaoTextureIndex; // 0xFFFFFFFF when SSAO didn't run this frame (disabled, or RTAO owns occlusion instead)

    // Aerial perspective over a procedural sky (0xFFFFFFFF otherwise): sampled slot of the volume, and world units -> km of
    // the view distance it is looked up with (times the component's distance scale).
    uint32_t aerialPerspectiveTextureIndex;
    float aerialPerspectiveKmPerUnit;
    // Screen-probe GI's result (ScreenProbeIntegrate.slang: irradiance / pi), 0xFFFFFFFF when it did not run (diffuseGIMode 3).
    uint32_t screenProbeGITextureIndex;
    // Screen-space reflections (SSR.slang: rgb the reflected radiance, a its confidence), 0xFFFFFFFF when they did not run (RT
    // reflections take precedence).
    uint32_t ssrTextureIndex;
    // Screen-space GI (SSGI.slang, denoised: rgb the irradiance / pi its rays found on screen, a the share of rays that found
    // something), 0xFFFFFFFF when it did not run. The rest keeps the frame's other diffuse ambient.
    uint32_t ssgiTextureIndex;
};

// Raster-only ambient occlusion (SSAO.slang): hemisphere-kernel, world-space reprojection, one fullscreen pass.
// Deliberately not RTAO -- both are meant to feed the SAME ssaoTextureIndex slot in PushConstantDeferredLighting and
// never run in the same frame (Renderer::m_ssaoEnabled / a future m_rtaoEnabled are mutually exclusive), so adding
// RTAO later is a second producer of that slot, not a second, separately-consumed occlusion term to reconcile.
struct PushConstantSSAO
{
    mat4 invViewProj;
    mat4 view;
    mat4 proj; // Jittered: must match the projection the depth buffer being sampled was rasterized with.
    uint64_t matrixReference;
    uint64_t kernelReference; // BDA to float4[kernelSize] hemisphere sample directions (tangent space, +Z up)
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t kernelSize;
    float radius;   // world units
    float bias;     // world units: shrugs off self-occlusion from the surface's own micro-slope / depth precision
    float power;    // contrast (pow(ao, power) after averaging)
    vec2 viewportSize;
    float shortRangePixels; // > 0: the radius at most this many pixels' width at the surface (ambient_occlusion_radius)
};

// Ground-truth ambient occlusion (RS3; GTAO.slang, Jimenez et al. 2016 / Intel XeGTAO): per pixel a few screen-space slices around
// the view direction, the horizon found on both sides of each, the visible arc integrated cosine-weighted against the normal. Writes
// resources.SSAORaw like SSAO / RTAO (SSAOBlur.slang denoises it). The camera's matrices come from the uniforms.
struct PushConstantGTAO
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t sliceCount;   // directions per pixel (turned per pixel and frame)
    uint32_t stepsPerSide; // depth samples along each side of a slice
    float radius;          // world units
    float power;           // contrast (pow(ao, power))
    vec2 viewportSize;
    float shortRangePixels; // > 0: the radius at most this many pixels' width at the surface (ambient_occlusion_radius)
    uint32_t frameIndex;
};

// Screen-space reflections (RS4; SSR.slang): the mirror ray marched through this frame's depth buffer, the radiance at its hit taken
// from last frame's lit scene (reprojected) at a blur mip for the roughness' cone.
struct PushConstantSSR
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t materialTextureIndex;
    uint32_t historyTextureIndex; // last frame's lit scene with blur mips
    uint32_t historyMipCount;
    uint32_t maxSteps;
    vec2 viewportSize;
    float maxDistance;  // world units
    float thickness;    // world units: how far behind a surface the ray may be and still hit it
    float maxRoughness; // rougher surfaces keep the environment / probes
    uint32_t frameIndex;
    uint32_t hizTextureIndex; // the closest-depth pyramid (HiZBuild.slang keepClosest): Hi-Z levels 1.. (level 0: the depth)
    uint32_t hizLevels;       // its mips
};

// SSR's roughness filter (SSR.slang filterMain; Godot's SSR filter): the trace blurred on its own surface by roughness, this frame
// only. What the deferred lighting reads.
struct PushConstantSSRFilter
{
    uint64_t matrixReference;
    uint32_t rawTextureIndex;      // the trace (premultiplied: rgb light x confidence, a confidence)
    uint32_t depthTextureIndex;
    uint32_t materialTextureIndex; // r: roughness
    uint32_t padding;
    vec2 viewportSize;
};

// Screen-space GI (RS5b; SSGI.slang traceMain): a few cosine-distributed rays per pixel marched through the depth buffer, the light
// at their hits from last frame's lit scene (the SSR history).
struct PushConstantSSGI
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t historyTextureIndex; // last frame's lit scene with blur mips (as SSR's)
    uint32_t historyMipCount;
    uint32_t rayCount;
    uint32_t steps;
    vec2 viewportSize;
    float maxDistance; // world units
    float thickness;   // world units
    uint32_t frameIndex;
    // The exposure the post process applies (scene_exposure): a hit's radiance is held to SSGIMaxMiddleGreys x middle grey at it.
    uint32_t autoExposureTextureIndex; // last frame's adapted luminance (0xFFFFFFFF: none)
    float exposureCompensation;
    float autoExposureMinEV;
    float autoExposureMaxEV;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};
// SSGI's firefly limit: a hit brighter than this many times middle grey on screen counts as this bright (a lamp's bulb a ray of two
// happens to hit would otherwise light the wall in specks).
STATIC_CONST float SSGIMaxMiddleGreys = 32.0;

// SSGI's denoiser (SSGI.slang denoiseMain): a 5x5 bilateral average of the raw rays, then last frame's result where this surface was
// (the previous camera; kept only where last frame's view depth there matches), accumulated over up to maxHistory frames.
struct PushConstantSSGIDenoise
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t rawTextureIndex;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t historyTextureIndex;      // last frame's denoised result (0xFFFFFFFF: none, start over)
    uint32_t historyDepthTextureIndex; // last frame's (view depth, frames accumulated)
    float maxHistory;                  // frames at most (the blend is 1 / frames)
    vec2 viewportSize;
    // The exposure (as PushConstantSSGI's): the neighbours are averaged by their brightness on screen (Karis), outliers count less.
    uint32_t autoExposureTextureIndex;
    float exposureCompensation;
    float autoExposureMinEV;
    float autoExposureMaxEV;
    uint32_t pad0;
    uint32_t pad1;
};

// Volumetric fog (RS6; VolumetricFog.slang): lightMain lights each froxel (the height fog's density, the sun and local lights with the
// frame's shadow maps or a sun ray, the sky around) and blends it with last frame's; integrateMain accumulates along each column.
// Local Volumetric Fog Volume injected into VolumetricFog.slang froxels (docs/VFX_And_Volumes_Plan_2026-11.md step N3).
struct GpuLocalFogVolume
{
    mat4 invModel;             // render space -> unit local space [-1, 1]^3
    vec4 albedoAndDensity;     // rgb = scatter albedo, w = extinction per world unit
    vec4 emissiveAndFalloff;   // rgb = emissive radiance per world unit, w = edgeFalloff (0..1)
    vec4 noiseParams;          // x = noiseStrength, y = noiseScale (cycles/wu), z = noiseSpeed, w = shape (0 Box, 1 Sphere, 2 Cyl)
};

struct PushConstantVolumetricFog
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint32_t lightingStorageIndex;  // lightMain: this frame's froxel lighting (RWTexture2DArray); integrateMain: read
    uint32_t historyTextureIndex;   // lightMain: last frame's (sampled), 0xFFFFFFFF: none
    uint32_t integratedStorageIndex; // integrateMain: the integrated volume
    uint32_t gridWidth;
    uint32_t gridHeight;
    uint32_t slices;
    float maxDistance;     // world units
    float anisotropy;      // Henyey-Greenstein g
    float extinctionScale; // times the height fog's density
    float historyWeight;
    vec3 previousCameraPos; // render space
    float jitter;          // [0, 1): this frame's depth offset in each froxel
    vec3 albedo;           // scattering / extinction
    uint32_t localLights;  // 1: point / spot lights scatter too
    uint64_t localFogVolumesReference; // GpuLocalFogVolume[] (step N3), 0 if none
    uint32_t localFogVolumeCount;
    uint32_t padFog0;
};

struct PushConstantSSAOBlur
{
    uint64_t matrixReference;
    uint32_t ssaoTextureIndex; // the raw (unblurred) AO this pass reads
    uint32_t depthTextureIndex;
    vec2 viewportSize;
};

// Ray-traced ambient occlusion: same hemisphere kernel and per-pixel rotation as PushConstantSSAO (SSAOKernel is
// shared), but each sample is a real traced ray instead of a screen-space reprojection -- no radius/bias tradeoff
// against depth-buffer precision, no missed occluders outside the screen or behind other geometry, at the cost of
// a TLAS trace per sample instead of a texture read. Writes the same resources.SSAORaw slot SSAO does (see the
// comment on PushConstantSSAO): consumers never need to know which of the two actually ran.
struct PushConstantRTAO
{
    mat4 invViewProj;
    uint64_t matrixReference;
    uint64_t kernelReference;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t kernelSize;
    float radius;
    vec2 viewportSize;
    float power;
    float shortRangePixels; // > 0: the radius at most this many pixels' width at the surface (ambient_occlusion_radius)
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
    // RT Pipeline output storage slots
    uint32_t outputAccumIndex;
    uint32_t outputDiffuseIndex;
    uint32_t outputSpecularIndex;
    uint32_t environmentImportanceIndex;
    uint32_t maxDiffuseBounces;
    float fireflyFilterThreshold;      // RTXPT adaptive cap; 0 disables it
    // DLSS Ray Reconstruction guides written from the actual primary path-traced surface.
    // All are 0xFFFFFFFF when RR is inactive.
    uint32_t rrDiffuseAlbedoStorageIndex;
    uint32_t rrSpecularAlbedoStorageIndex;
    uint32_t rrNormalRoughnessStorageIndex;
    uint32_t rrDepthStorageIndex;
    uint32_t rrMotionStorageIndex;
    uint32_t rrSpecularMotionStorageIndex;
    uint32_t rrSpecularHitDistanceStorageIndex;
    float cameraRayJitterScale; // RTXPT: 0.1 for RR micro-jitter, 0 for DLSS-SR, 1 for native accumulation
    uint32_t neeCandidateSamples; // RTXPT realtime NEE-AT default: 5
    uint32_t neeFullSamples;      // RTXPT realtime NEE-AT default: 1
    uint32_t environmentDiffuseMip; // RTXPT default: 2
    // The exposure the image is shown with (scene_exposure): RTXPT's firefly filter and RR brightness clamp scale with the
    // luminance it maps to middle grey. 0xFFFFFFFF: no auto exposure (last frame's adapted average otherwise).
    uint32_t autoExposureTextureIndex;
    float exposureCompensation;
    float autoExposureMinEV;
    float autoExposureMaxEV;
};

struct PushConstantPTEnvironment
{
    uint32_t sourceIndex;
    uint32_t outputIndex;
    uint32_t sourceMip;
    uint32_t dimension;
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
    uint32_t autoExposureTextureIndex;
    uint32_t debugMode;
    uint32_t tonemapMode; // 0 = None, 1 = ACES Narkowicz, 2 = ACES Hill, 3 = ACES Hill Exp, 4 = Khronos PBR Neutral, 5 = Reinhard, 6 = Reinhard Mod, 7 = Heji Hable ALU, 8 = Hable UC2 (RTXPT), 9 = ACES (RTXPT)
    uint32_t autoExposureEnabled;
    float exposureCompensation;
    float autoExposureMinEV;
    float autoExposureMaxEV;
    // Post effects (PP1; Renderer::addPostEffectPasses, PostProcess.slang): 0 / 0xFFFFFFFF where off.
    uint32_t bloomTextureIndex;   // the bloom chain's top (PostEffects.slang), its mips for the lens flares
    float bloomIntensity;         // how much of the bloom the image becomes (lerp)
    float lensFlareIntensity;
    float vignetteIntensity;
    float filmGrainIntensity;
    float chromaticAberration;    // radial colour fringes towards the edges, in % of the screen
    float sharpen;                // CAS sharpness 0..1 (0: off)
    float saturation;             // colour grading before the tonemapper (1: unchanged)
    float contrast;               // around mid grey, in log space (1: unchanged)
    uint32_t lutTextureIndex;     // a colour grading LUT strip (size^2 x size), applied after the tonemapper; 0xFFFFFFFF: none
    float lutStrength;
    uint32_t frameIndex;
};

// The post effects' compute passes (PP1; PostEffects.slang): bloom's chain, depth of field, motion blur. Sizes are the written image's.
struct PushConstantPostEffect
{
    uint64_t matrixReference;
    uint32_t sourceIndex;  // sampled (heap offset included)
    uint32_t outputIndex;  // storage slot
    uint32_t auxIndex;     // sampled: the bloom chain's next mip (upsample), the depth (DOF), the motion vectors (motion blur)
    uint32_t sourceMip;
    uint32_t width;
    uint32_t height;
    vec4 params;           // per pass (PostEffects.slang)
    vec4 params2;
};

// SceneOutline.slang (stylized rendering step 2): the G-buffer's depth, normals and material flags (sampled, heap offset included); the
// lit scene is the colour target it multiplies.
struct PushConstantSceneOutline
{
    uint64_t matrixReference;   // the UniformBufferObject (invViewProj, the camera)
    uint32_t depthIndex;
    uint32_t normalIndex;
    uint32_t materialIndex;
    uint32_t toonOnly;          // 1: only toon pixels (gbuffer_toon) get lines
    uint32_t padding0;          // (push constants lay vec4s out on 16 bytes, as every PushConstant* here: color at 32)
    uint32_t padding1;
    vec4 color;                 // rgb: what an edge multiplies the scene by; w: unused
    vec4 params;                // x thickness (render pixels), y depth threshold (relative), z normal threshold (1 - cos), w fade distance (0: none)
    vec4 params2;               // xy: the render size, zw: unused
};
#ifndef __SLANG__
static_assert(offsetof(PushConstantSceneOutline, color) % 16 == 0 && offsetof(PushConstantSceneOutline, params) % 16 == 0 &&
              offsetof(PushConstantSceneOutline, params2) % 16 == 0, "push constant vec4s must start on 16 bytes (the shader's layout)");
#endif

struct PushConstantAutoExposure
{
    uint32_t sourceIndex;
    uint32_t outputIndex;
    uint32_t sourceMip;
    uint32_t dimension;
    float adaptation;      // adaptMain: how far the adapted average moves towards this frame's (0..1, from the frame time)
    uint32_t historyValid; // adaptMain: 0: start at this frame's average
};

// ColorPyramid.slang: the refraction source of transparent surfaces (copy, then 2x2 downsamples).
struct PushConstantColorPyramid
{
    uint32_t sourceIndex;  // sampled slot (image heap offset included)
    uint32_t outputIndex;  // storage slot of the mip being written
    uint32_t sourceMip;
    uint32_t width;        // of the mip being written
    uint32_t height;
    uint32_t padding;
};

// TAA.slang: the engine's own temporal anti-aliasing (when DLSS does not run). Sampled slots with the image heap's offset added.
struct PushConstantTAA
{
    uint32_t colorIndex;    // this frame's lit scene (jittered)
    uint32_t depthIndex;    // reverse Z
    uint32_t velocityIndex; // GBufferVelocity: uv offset to where the pixel was last frame
    uint32_t historyIndex;  // last frame's result
    uint32_t outputIndex;   // storage slot of this frame's result
    uint32_t width;
    uint32_t height;
    uint32_t reset;         // 1: no usable history (first frame, resize, cut)
    float blend;            // the weight of this frame where the history is valid (0.1: about ten frames)
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

// Particles.slang (Hybrid_Renderer_Plan_2026-10.md §6 V1 / V2): one camera-facing quad per particle, written each frame by the CPU
// (Renderer::SubmitParticles) into a host-visible buffer read through its address.
struct GpuParticle
{
    vec3 position;     // world units
    float size;        // edge of the quad, world units
    vec4 color;        // linear rgb (intensity applied), a = opacity
    float rotation;    // radians around the view direction
    uint32_t frame;    // flipbook frame
    float padding0;
    float padding1;
    vec3 previousPosition; // where it was last frame (its motion vectors, Particles.slang fragMainMotion)
    float padding2;
};

STATIC_CONST uint32_t PARTICLE_LIT = 1;      // shaded by the scene's lights and environment (per particle, in the mesh stage)
STATIC_CONST uint32_t PARTICLE_SOFT = 2;     // faded near the geometry behind (the scene depth)
STATIC_CONST uint32_t PARTICLE_ADDITIVE = 4; // light added: alpha only scales it (else premultiplied alpha blending)
STATIC_CONST uint32_t PARTICLE_SHADOW = 8;   // lit particles trace a shadow ray to the first directional light

struct PushConstantParticles
{
    uint64_t matrixReference;
    uint64_t particlesReference;  // GpuParticle[]
    uint64_t countersReference;   // 0 for CPU batches; non-zero: GpuVfxCounters[0].aliveInCount (VFX step V1)
    uint32_t firstParticle;
    uint32_t particleCount;
    uint32_t textureIndex;        // flipbook sheet slot (image heap offset added in the shader), 0xFFFFFFFF: a soft round dot
    uint32_t flipbookColumns;
    uint32_t flipbookRows;
    uint32_t flags;               // PARTICLE_*
    uint32_t depthTextureIndex;   // the scene depth (render resolution), image heap offset added in the shader
    float softDistance;           // world units
};

// GPU Compute VFX core (docs/VFX_And_Volumes_Plan_2026-11.md step V1): per-emitter GPU particle pool, dead/alive ping-pong lists,
// atomic counters and indirect mesh-task dispatch arguments.
struct GpuVfxParticle
{
    vec3 position;         // render-space world units (or emitter-local when localSpace != 0)
    float age;             // seconds
    vec3 velocity;         // world units / second
    float lifetime;        // seconds
    vec3 previousPosition; // render-space world units (for DLSS / TAA motion vectors)
    float sizeRandom;      // 0..1 per-particle size multiplier
    float rotation;        // radians
    float spin;            // radians / second
    uint32_t seed;         // per-particle RNG seed
    uint32_t flags;
};

struct GpuVfxCounters
{
    uint32_t aliveInCount;  // alive particles entering updateMain (and rendered this frame)
    uint32_t aliveOutCount; // surviving + newly spawned count written by updateMain
    uint32_t deadCount;     // available indices in DeadList
    uint32_t spawnActual;   // clamped spawn count for this frame
};

// Raw 3-uint dispatch arguments matching VkDrawMeshTasksIndirectCommandEXT / D3D12_DISPATCH_MESH_ARGUMENTS at offset 0
// (unlike MeshTasksIndirectCommand, which has a leading drawID at offset 0 for drawMeshTasksIndirectCountWithDrawId).
struct GpuVfxIndirectCommand
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
    uint32_t pad0;
};

// GPU VFX Module Flags (docs/VFX_And_Volumes_Plan_2026-11.md step V2)
STATIC_CONST uint32_t VFX_MOD_CURL_NOISE      = 1u << 0;
STATIC_CONST uint32_t VFX_MOD_VORTEX          = 1u << 1;
STATIC_CONST uint32_t VFX_MOD_ATTRACTOR       = 1u << 2;
STATIC_CONST uint32_t VFX_MOD_DEPTH_COLLISION = 1u << 3;
STATIC_CONST uint32_t VFX_MOD_CURVES          = 1u << 4;
STATIC_CONST uint32_t VFX_MOD_MESH_SURFACE    = 1u << 5;
STATIC_CONST uint32_t VFX_MOD_KILL_ON_COLLIDE = 1u << 6;

struct GpuVfxEmitterParams
{
    vec3 origin;           // emitter world position relative to renderOrigin (world units)
    float dt;              // frame delta time (seconds)
    vec3 basisX;           // emitter right axis
    float time;            // seconds
    vec3 basisY;           // emitter up axis
    uint32_t spawnCount;   // requested particles to spawn this frame
    vec3 basisZ;           // emitter forward axis
    uint32_t maxParticles; // pool capacity

    uint32_t shape;        // 0 Point, 1 Sphere, 2 Box, 3 Cone, 4 MeshSurface
    float coneAngleRad;    // radians
    uint32_t frameSeed;    // per-frame RNG salt
    uint32_t localSpace;   // 1: simulate in emitter space, transform on render write

    vec3 shapeSize;        // world units
    float gravity;         // world units / s^2 (already multiplied by 9.81 * unitsPerMeter)
    vec3 velocity;         // world units / s
    float drag;            // 1/s
    vec3 velocityRandom;   // +/- world units / s per axis
    float lifetimeMin;     // seconds

    float lifetimeMax;     // seconds
    float spinMinRad;      // radians / s
    float spinMaxRad;      // radians / s
    float sizeStart;       // world units

    float sizeEnd;         // world units
    float sizeRandom;      // 0..1 fraction
    float intensity;       // HDR brightness multiplier
    uint32_t flipbookCols;

    uint32_t flipbookRows;
    float flipbookFPS;
    uint32_t pad0;
    uint32_t pad1;

    vec4 colorStart;       // linear rgba
    vec4 colorEnd;         // linear rgba

    // Step V2 Niagara-style GPU modules (all 16-byte aligned from offset 160 onward).
    uint64_t matrixReference;          // UniformBufferObject (viewProj, invViewProj, cameraWorldPos)
    uint64_t meshVertexReference;      // SkinnedVertex[] (meshIsSkinned=1) or Vertex[] (meshIsSkinned=0)
    uint32_t moduleFlags;              // VFX_MOD_*
    uint32_t depthTextureIndex;        // scene depth (image heap offset added in shader), 0xFFFFFFFF: none
    uint32_t normalTextureIndex;       // G-buffer world normal (image heap offset added in shader), 0xFFFFFFFF: none
    uint32_t meshVertexOffset;         // first vertex in meshVertexReference
    uint32_t meshVertexCount;          // vertex count for mesh surface sampling
    uint32_t meshIsSkinned;            // 1: SkinnedVertex, 0: static Vertex
    uint32_t meshRenderChunkCount;     // 0: Sprite mode ((alive+31)/32, 1, 1); >0: Mesh Particle mode (chunks, min(alive,65535), 1)
    uint32_t padVfx1;

    vec4 curlParams;                   // x strength (wu/s^2), y frequency (1/wu), z scrollSpeed, w unused
    vec4 vortexAxisAndStrength;        // xyz normalized world axis, w tangential strength (wu/s^2)
    vec4 vortexParams;                 // x radialPull (wu/s^2), y radius (wu), z/w unused
    vec4 attractorTargetAndStrength;   // xyz target position (render-space wu), w strength (wu/s^2)
    vec4 attractorParams;              // x radius (wu), y killRadius (wu, 0=off), z/w unused
    vec4 collisionParams;              // x restitution, y friction, z thickness (wu), w viewportWidth
    vec4 collisionParams2;             // x viewportHeight, y meshSurfaceNormalSpeed (wu/s), z/w unused
    mat4 meshModelMatrix;              // render-space model matrix for mesh surface sampling
    vec4 colorCurveLut[16];            // 16-sample RGBA gradient over life [0..1]
    vec4 sizeCurveLut[4];              // 16-sample size (world units) over life [0..1], packed 4 per vec4
};

struct PushConstantVfxSimulate
{
    uint64_t paramsReference;          // GpuVfxEmitterParams
    uint64_t particlesReference;       // GpuVfxParticle[maxParticles]
    uint64_t renderParticlesReference; // GpuParticle[maxParticles]
    uint64_t deadListReference;        // uint32_t[maxParticles]
    uint64_t aliveInReference;         // uint32_t[maxParticles]
    uint64_t aliveOutReference;        // uint32_t[maxParticles]
    uint64_t countersReference;        // GpuVfxCounters[1]
    uint64_t indirectArgsReference;    // GpuVfxIndirectCommand[1]
    uint32_t depthTextureIndex;        // RenderGraph scene depth slot (0xFFFFFFFF if unused)
    uint32_t normalTextureIndex;       // RenderGraph G-buffer normal slot (0xFFFFFFFF if unused)
};

// VFX Ribbons, Trails & Beams (docs/VFX_And_Volumes_Plan_2026-11.md step V3a)
struct GpuVfxRibbonNode
{
    vec3 position; // render-space world units
    float width;   // world units
    vec4 color;    // linear HDR rgba
    float u;       // normalized ribbon U in [0..1] (0 head -> 1 tail)
    float pad0;
    float pad1;
    float pad2;
};

STATIC_CONST uint32_t VFX_RIBBON_SOFT     = 1u << 0;
STATIC_CONST uint32_t VFX_RIBBON_ADDITIVE = 1u << 1;
STATIC_CONST uint32_t VFX_RIBBON_BEAM     = 1u << 2;

struct PushConstantVfxRibbon
{
    uint64_t matrixReference;
    uint64_t nodesReference;     // GpuVfxRibbonNode[]
    uint32_t firstNode;
    uint32_t nodeCount;
    uint32_t textureIndex;       // 0xFFFFFFFF: procedural glowing ribbon/beam
    uint32_t depthTextureIndex;  // scene depth slot
    uint32_t flags;              // VFX_RIBBON_*
    float softDistance;          // world units
    float uvTileU;
    float uvScrollU;
};

// VFX Projected Decals (docs/VFX_And_Volumes_Plan_2026-11.md step V3a)
struct GpuVfxDecal
{
    mat4 model;                  // unit cube [-0.5, 0.5]^3 -> render space
    mat4 invModel;               // render space -> unit cube [-0.5, 0.5]^3
    vec4 color;                  // linear RGBA
    uint32_t textureIndex;       // 0xFFFFFFFF: procedural arcane rune ring
    float emissiveIntensity;     // HDR multiplier
    float normalFade;            // min dot(surfaceNormal, decalUp)
    float edgeFade;              // vertical fade near box faces
    uint32_t flags;              // bit 0: additive
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct PushConstantVfxDecal
{
    uint64_t matrixReference;
    uint64_t decalsReference;    // GpuVfxDecal[]
    uint32_t decalIndex;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t pad0;
    vec2 viewportSize;
};

// GPU Mesh Particles (docs/VFX_And_Volumes_Plan_2026-11.md step V3b)
STATIC_CONST uint32_t VFX_MESH_ALIGN_VELOCITY = 1u << 4;

struct PushConstantVfxMeshParticle
{
    uint64_t matrixReference;
    uint64_t particlesReference;       // GpuVfxParticle[]
    uint64_t renderParticlesReference; // GpuParticle[]
    uint64_t aliveListReference;       // uint32_t[] (indices into GpuVfxParticle[])
    uint64_t countersReference;        // GpuVfxCounters[1]
    uint64_t verticesReference;        // PackedVertex[] (0: built-in procedural crystal shard)
    uint64_t indicesReference;         // uint32_t[]
    uint32_t verticesOffset;
    uint32_t indicesOffset;
    uint32_t triangleCount;
    uint32_t textureIndex;             // 0xFFFFFFFF: untextured
    uint32_t depthTextureIndex;        // scene depth (for soft/depth test)
    uint32_t flags;                    // PARTICLE_LIT | PARTICLE_ADDITIVE | VFX_MESH_ALIGN_VELOCITY
    float roughness;
    float metallic;
};

// Screen-Space Distortion (docs/VFX_And_Volumes_Plan_2026-11.md step V3b): shockwave rings, heat haze, portal warping.
struct GpuVfxDistortion
{
    vec3 position;         // render-space world units
    float radius;          // world units
    vec3 right;            // entity basis X (when cameraFacing == 0)
    float strength;        // screen UV warp scale
    vec3 up;               // entity basis Z (when cameraFacing == 0)
    float ringWidth;       // normalized ring thickness
    vec4 rimTint;          // optional energy crest tint (rgb * a)
    uint32_t mode;         // 0 ShockwaveRing, 1 HeatHaze, 2 PortalSwirl
    float speed;
    float chromaticAberration;
    uint32_t cameraFacing; // 1: billboard, 0: oriented quad
};

struct PushConstantVfxDistortion
{
    uint64_t matrixReference;
    uint64_t distortionsReference; // GpuVfxDistortion[]
    uint32_t distortionIndex;
    uint32_t sceneColorIndex;      // copied HDRScene
    uint32_t depthTextureIndex;    // scene depth (Reverse-Z occlusion & foreground anti-bleed)
    uint32_t pad0;
};

// Heterogeneous Volumes (docs/VFX_And_Volumes_Plan_2026-11.md step N2):
// Ray-marched FP16 .nvdb volumes (or procedural fire/smoke fallback) with Beer-Lambert transmittance,
// physical blackbody emission, Henyey-Greenstein scattering, self-shadowing, and Reverse-Z depth occlusion.
STATIC_CONST uint32_t VOLUME_FLAG_HAS_DENSITY     = 1u << 0;
STATIC_CONST uint32_t VOLUME_FLAG_HAS_TEMPERATURE = 1u << 1;
STATIC_CONST uint32_t VOLUME_FLAG_HAS_FLAME       = 1u << 2;
STATIC_CONST uint32_t VOLUME_FLAG_PROCEDURAL      = 1u << 3;

struct GpuHeterogeneousVolume
{
    mat4 model;                    // unit box [0, 1]^3 -> render space
    mat4 invModel;                 // render space -> unit box [0, 1]^3
    uint64_t densityBufferRef;     // packed FP16 uint32[] (2 halfs per uint), 0 if unused
    uint64_t temperatureBufferRef; // packed FP16 uint32[], 0 if unused
    uint64_t flameBufferRef;       // packed FP16 uint32[], 0 if unused
    uint32_t dimX;
    uint32_t dimY;
    uint32_t dimZ;
    uint32_t flags;                // VOLUME_FLAG_*
    uint32_t padVol0;
    uint32_t padVol1;
    vec4 albedoAndDensity;         // rgb = smoke scatter albedo, w = densityScale
    vec4 emissionParams;           // x = emissionScale, y = blackbodyKelvin, z = anisotropy g, w = ambientStrength
    vec4 marchParams;              // x = stepCount, y = shadowSteps, z = invMaxDensity, w = invMaxTemperature
};

struct PushConstantHeterogeneousVolume
{
    uint64_t matrixReference;
    uint64_t volumesReference;     // GpuHeterogeneousVolume[]
    uint32_t volumeIndex;
    uint32_t depthTextureIndex;
    uint32_t frameIndex;
    uint32_t pad0;
    vec2 viewportSize;
};

struct PushConstantDDGIRadiance
{
    uint64_t matrixReference;
    uint32_t raysPerProbe;
    uint32_t probeCountTotal;
    uint32_t frameIndex;
    uint32_t probeOffsetsTextureIndex; // where the probes trace from: last frame's relocation (0xFFFFFFFF: the grid points)
    uint32_t prevIrradianceTextureIndex; // multi-bounce: last frame's atlases (0xFFFFFFFF on their first frame: not written yet)
    uint32_t prevDistanceTextureIndex;
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
    uint32_t outputStorageIndex; // the blends: the atlas they write (storage image)
};

// Ray traced results at a lower ray tracing resolution back to the render resolution (RTUpsample.slang).
struct PushConstantRTUpsample
{
    uint64_t matrixReference;
    uint32_t sourceTextureIndex; // the reduced result
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;
    uint32_t mode;               // 0: color blended, alpha from the best match; 1: everything from the best match (the shadow mask)
    vec2 viewportSize;           // render resolution
    vec2 sourceSize;
};

// Screen-probe GI (ScreenProbeCommon.slang): one layout for its five passes (Place, Trace, Irradiance, Filter, Integrate).
struct PushConstantScreenProbe
{
    mat4 invViewProj;     // this frame's: the G-buffer's pixels -> world
    mat4 prevInvViewProj; // last frame's (Integrate: its depth snapshot -> world)
    uint64_t matrixReference;
    uint32_t depthTextureIndex;
    uint32_t normalTextureIndex;        // the G-buffer's
    uint32_t probePositionTextureIndex; // ScreenProbePlace's outputs
    uint32_t probeNormalTextureIndex;
    uint32_t inputTextureIndex;         // what the pass reads: the rays, the irradiance atlas, the filtered atlas
    uint32_t historyTextureIndex;       // Integrate: last frame's result
    uint32_t prevDepthTextureIndex;
    uint32_t prevNormalTextureIndex;
    uint32_t velocityTextureIndex;      // 0xFFFFFFFF: reprojected with the camera's matrices
    uint32_t frameIndex;
    vec2 viewportSize;
    uint32_t probeCountX;
    uint32_t probeCountY;
    uint32_t tileSize;   // pixels per probe each way
    uint32_t historyValid;
    uint32_t maxHistory; // frames accumulated at most
    uint32_t emissiveTextureIndex; // Irradiance: the trace's emissive light samples (0xFFFFFFFF: none)
    // Integrate: how far the camera jitter moved the image this frame and last frame (uv, y down). The motion vectors are
    // jitter-free, the pixels (and the history's) are not.
    vec2 currJitterUv;
    vec2 prevJitterUv;
    uint32_t materialTextureIndex; // Integrate: the G-buffer's material (double-sided: foliage), 0xFFFFFFFF: none
    // The pass's targets (storage slots, RGPassContext::StorageSlot): the compute passes write them as storage images.
    uint32_t outputStorageIndex0;
    uint32_t outputStorageIndex1;
    uint32_t screenProbePadding;
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
    uint32_t pad0;
    uint32_t pad1;
    // Lighting of the path vertices (RTXDI/RTXDILightSampling.slang); 16-aligned (offset 128) for both Vulkan and D3D12 CBV rules.
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
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2; // the struct below starts 16-aligned (offset 144) for both Vulkan and D3D12 CBV rules
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
    // SelectionMask.slang's display-resolution, non-jittered mask of the selection (bindless slot); 0xFFFFFFFF: none this
    // frame, the outline falls back to the (jittered) entity IDs.
    uint32_t selectionMaskTextureIndex;
    // The entity ID texture to outline from (bindless slot): an editor extra view's own; 0xFFFFFFFF: the camera's (the uniforms').
    uint32_t entityTextureIndex;
    uint32_t pad0;
};

struct PushConstantQuad
{
    uint64_t matrixReference;
    uint64_t quadDataReference;
    uint numOfElements;
    uint firstElement;
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
    uint firstElement;
};
struct TextData
{
    mat4 transform;
    vec2 quadMin;
    vec2 quadMax;
    vec2 texMin;
    vec2 texMax;
    vec4 color;
    vec4 outlineColor;
    uint32_t materialIndex;
    float outlineThickness; // 0..1 of the atlas' distance range, 0 = no outline

    // Editor-only
    int entityID;
    int pad0;
};

struct PushConstantLine
{
    uint64_t matrixReference;
    uint64_t lineDataReference;
    uint numOfElements;
    uint firstElement;
};
struct PushConstantGrid
{
    uint64_t matrixReference;
    float centerA;       // where the lines are centered on the grid plane (multiples of the large step around the camera), along axis a
    float centerB;       // and along axis b (the plane's normal is axis c: a = (c + 1) % 3, b = (c + 2) % 3)
    float smallStep;     // distance between two lines
    float gridFadeSize;  // distance at which the grid has faded out
    float decimals;      // fractional part of the division level: blends primary and secondary lines
    int steps;           // every steps-th line is a primary line
    int gridSize;        // lines each way from the center
    int planeAxis;       // c: 0 = the plane x = 0, 1 = y = 0 (the ground), 2 = z = 0
    // The world's origin in the GPU's world space (large world coordinates: everything is relative to the render origin): where the
    // plane and the axis lines are. centerA / centerB are in that space too.
    float originA;
    float originB;
    float originC;
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
#include "ShaderHelpers.slang"
#endif

// ============================================================================
// Nox::UI GPU Primitives (shared C++20 & Slang)
// ============================================================================
enum UIPrimitiveType : uint32_t
{
    UI_PRIM_ROUNDED_RECT  = 0u,
    UI_PRIM_TEXTURED_QUAD = 1u,
    UI_PRIM_MSDF_GLYPH    = 2u,
    UI_PRIM_CUBIC_BEZIER  = 3u,
    UI_PRIM_GLYPH_BITMAP  = 4u  // a glyph from the UI glyph cache: 8-bit coverage, drawn texel for pixel (UIGlyphCache)
};

struct UIClipRect
{
    vec4 rect;        // (minX, minY, maxX, maxY) in pixels
    vec4 cornerRadii; // (topLeft, topRight, bottomRight, bottomLeft)
};

struct UIPrimitive
{
    vec4 bounds;          // (minX, minY, maxX, maxY) in pixels
    vec4 color0;          // primary fill / tint
    vec4 color1;          // secondary gradient color / MSDF outline color
    vec4 borderColor;     // border RGBA
    vec4 shadowColor;     // drop shadow RGBA
    vec4 cornerRadii;     // (topLeft, topRight, bottomRight, bottomLeft)
    vec4 uvOrP0P1;        // Textured/MSDF: (uMin, vMin, uMax, vMax); Bezier: (p0.xy, p1.xy)
    vec4 extraOrP2P3;     // Bezier: (p2.xy, p3.xy); RoundedRect: (gradientAngle, strokeWidth, 0, 0)
    vec2 shadowOffset;    // drop shadow offset in pixels
    float shadowBlur;     // drop shadow blur radius in pixels (0 = disabled)
    float borderThickness;
    uint32_t type;        // UIPrimitiveType
    uint32_t clipIndex;   // index into UIClipRect buffer
    uint32_t textureIndex;// bindless slot relative to imageHeapIndexOffset
    uint32_t flags;       // bit 0: gradient enabled, bit 1: MSDF outline enabled
};

struct UIPushConstants
{
    uint64_t primitiveReference;
    uint64_t clipRectReference;
    vec2 screenSize;
    uint32_t imageHeapIndexOffset;
    uint32_t primitiveCount;
};

#endif  // HOST_DEVICE_H
