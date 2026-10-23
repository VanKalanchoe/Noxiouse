
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
    SAMPLER_NEAREST_CLAMP = 6,

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
    uint32_t skinnedVertexOffset; // 0xFFFFFFFF: no compute-skinned vertex cache
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
    // Volume absorption (KHR_materials_volume): what crossing attenuationDistance of the volume leaves of the light (0: none)
    vec3 attenuationColor;
    float attenuationDistance;

    uint32_t alphaMode;   // 0 = Opaque, 1 = Mask, 2 = Blend
    float alphaMaskCutoff;
    uint32_t doubleSided;
    uint32_t unlit;
    float diffuseTransmission; // through a thin surface to its back (leaves, cloth); 0: none
    uint32_t pathTracerFlags;  // PathTracerThin... (above)
    float depthBias; // MaterialData::DepthBias: drawn this much closer to the camera along the view ray, world units (depth_bias_clip)
};

#ifdef __SLANG__
// The G-buffer material target's alpha (RGBA8): 0 for a single-sided surface; a double-sided one 0.25 + 0.75 x its diffuse
// transmission (leaves, cloth: light from behind comes through).
float gbuffer_encode_back_light(bool doubleSided, float diffuseTransmission)
{
    return doubleSided ? 0.25 + 0.75 * saturate(diffuseTransmission) : 0.0;
}

bool gbuffer_double_sided(float encoded)
{
    return encoded > 0.125;
}

// How much of a light behind a double-sided surface comes through to the side seen: its diffuse transmission, or 0.6 when the
// material sets none (the look double-sided foliage always had here).
float gbuffer_back_light(float encoded)
{
    const float diffuseTransmission = saturate((encoded - 0.25) / 0.75);
    return diffuseTransmission > 0.01 ? diffuseTransmission : 0.6;
}

// Beer-Lambert absorption over `length` (world units) of a volume whose light keeps attenuationColor over attenuationDistance.
float3 volume_transmittance(float3 attenuationColor, float attenuationDistance, float length)
{
    if (attenuationDistance <= 0.0 || length <= 0.0)
        return float3(1.0);
    return pow(max(attenuationColor, float3(1e-6)), float3(length / attenuationDistance));
}

// Weighted blended order-independent transparency (McGuire and Bavoil 2013), what a transparent fragment adds: to the
// accumulation target (additive) its premultiplied color and alpha times a weight that favors the near ones, to the revealage
// target (multiplied in: dst * (1 - alpha)) its alpha. OITComposite.slang divides the sum out over the scene behind.
struct OITOutput
{
    float4 accum;
    float reveal;
};

OITOutput oit_output(float4 premultiplied, float viewDistanceMeters)
{
    const float alpha = saturate(premultiplied.a);
    const float z = max(viewDistanceMeters, 0.0);
    // Eq. 9 of the paper, z in meters; the cap is lower than its 3e3: HDR colors in a half-float target overflow sooner.
    float weight = alpha * clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 3e2);
    const float brightest = max(max(premultiplied.r, premultiplied.g), premultiplied.b);
    if (brightest * weight > 2048.0) // a few overlapping layers of the brightest still fit under the half float's 65504
        weight = 2048.0 / brightest;
    OITOutput output;
    output.accum = float4(premultiplied.rgb, alpha) * weight;
    output.reveal = alpha;
    return output;
}
#endif

// Ray tracing view of an instance (same slot as GpuInstance): everything the ray tracing shaders read at candidates and
// hits, in one record. Ray queries look instances up per candidate triangle; reading instance -> material -> mesh ->
// transform from separate tables there made ReSTIR GI ~30% slower than one record (Bistro, §5.5.4).
struct GpuRayTracingInstance
{
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
    uint64_t skinnedVertexBufferAddress; // 0 for rigid instances; local-space SkinnedVertex stream otherwise
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
    uint32_t pathTracerFlags;       // PathTracerThin... (the record stays a multiple of 8 bytes: its addresses)
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

    // Local bounds (GPU culling)
    vec4 boundsSphere; // xyz: center, w: radius
    vec3 boundsMin;
    float padding0;
    vec3 boundsMax;
    float padding1;
};

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
    uint32_t boneMatrixOffset;
};
STATIC_CONST uint32_t SKINNING_VERTICES_PER_WORK_ITEM = 256;

#ifdef __SLANG__
// Ray traversal can use compact deformed positions while hit shading retains the base vertex's UVs and material data.
Vertex LoadRayTracingVertex(GpuRayTracingInstance instance, uint vertexIndex)
{
    Vertex vertex = ((Vertex*)instance.vertexBufferAddress)[vertexIndex];
    if (instance.skinnedVertexBufferAddress != 0)
    {
        SkinnedVertex skinned = ((SkinnedVertex*)instance.skinnedVertexBufferAddress)[vertexIndex];
        vertex.pos = skinned.position.xyz;
        vertex.normal = skinned.normal.xyz;
    }
    return vertex;
}
#endif

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

#ifdef __SLANG__
// The pixel a ray traced pass's fragment traces (UniformBufferObject::rtSubsample): its own at full ray tracing resolution, one of
// its block's otherwise.
// Which pixel of its block a block traces this frame: each block its own place in the 4-frame cycle (a hash of the block). The
// whole screen moving its pattern together every frame was structured noise NRD and DLSS RR kept as 2x2 blotches.
int2 rt_block_offset(UniformBufferObject* ubo, int2 block)
{
    if (ubo->rtSubsample <= 1u)
        return int2(0, 0);
    static const int2 order[4] = { int2(0, 0), int2(1, 1), int2(1, 0), int2(0, 1) };
    uint h = (uint(block.x) * 0x8da6b343u) ^ (uint(block.y) * 0xd8163841u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return order[(ubo->frameIndex + h) & 3u] * int(ubo->rtSubsample) / 2;
}

int2 rt_trace_pixel(UniformBufferObject* ubo, float2 fragmentPosition, float2 viewportSize)
{
    const uint subsample = max(ubo->rtSubsample, 1u);
    const int2 block = int2(fragmentPosition);
    return min(block * int(subsample) + rt_block_offset(ubo, block), int2(viewportSize) - 1);
}

// NRD's distances are the scene's (world units). The view depth it reads for the sky lies beyond its denoising range (the renderer
// sets CommonSettings::denoisingRange to NRDDenoisingRangeMeters): the sky is neither denoised nor mixed into the geometry beside it.
float nrd_sky_view_z(UniformBufferObject* ubo)
{
    return 4.0 * NRDDenoisingRangeMeters * ubo->unitsPerMeter;
}

// The hit distance of a reflection ray that found nothing: far (10 km), but within half floats (the reflection targets are
// RGBA16F), so ~600 m in a centimeter project.
float reflection_miss_distance(UniformBufferObject* ubo)
{
    return min(10000.0 * ubo->unitsPerMeter, 60000.0);
}
#endif

#ifdef __SLANG__
// ---- Light evaluation every shader shares (docs/Hybrid_Renderer_Plan_2026-10.md §7) ----

// The directional lights' discs a ray sees (over a procedural sky, UniformBufferObject::skyDiscs): each its illuminance over its
// solid angle, capped as the skybox's sun is (the real ratio only saturates the tonemapper harder). For the rays nothing else
// sends the lights' light along: camera rays and mirror paths -- every other path samples them directly.
float3 directional_light_discs(UniformBufferObject* ubo, float3 direction)
{
    if (ubo->skyDiscs == 0)
        return float3(0.0);
    LightData* lights = (LightData*)ubo->lightDataReference;
    float3 radiance = float3(0.0);
    for (uint i = 0; i < ubo->lightCount; ++i)
    {
        const LightData light = lights[i];
        if ((uint)light.position.w != 0u)
            continue;
        const float cosRadius = cos(clamp(light.spotParams.z, 0.0005, 0.2));
        if (dot(direction, normalize(light.direction.xyz)) < cosRadius)
            continue;
        const float solidAngle = 2.0 * 3.14159265 * (1.0 - cosRadius);
        radiance += light.color.rgb * light.color.w * min(1.0 / solidAngle, 64.0);
    }
    return radiance;
}

// How much of a light the clouds let through above worldPos: the cloud shadow map's transmittance for the shadow mask's light (the
// sun, or the moon at night), 1 for any other light and without clouds. The map holds it from the ground up: a point above the
// ground looks it up where its ray towards the light starts at ground level. Fades out towards the map's edge.
float cloud_shadow(UniformBufferObject* ubo, float3 worldPos, uint lightIndex, float3 toLight)
{
    if (ubo->cloudShadowTextureIndex == 0xFFFFFFFF || lightIndex != ubo->shadowMaskLightIndex || toLight.y <= 0.0)
        return 1.0;
    const float kmPerUnit = 0.001 / max(ubo->unitsPerMeter, 1e-6);
    const float2 groundKm = worldPos.xz * kmPerUnit - toLight.xz * (worldPos.y * kmPerUnit / max(toLight.y, 0.02));
    const float2 uv = (groundKm - ubo->cloudShadowCenterKm) / (2.0 * ubo->cloudShadowExtentKm) + 0.5;
    const float2 edge = min(uv, 1.0 - uv);
    const float fade = saturate(min(edge.x, edge.y) * 10.0);
    if (fade <= 0.0)
        return 1.0;
    Texture2D<float> map = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + ubo->cloudShadowTextureIndex, 0));
    SamplerState linearClamp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_CLAMP, 0));
    return lerp(1.0, map.SampleLevel(linearClamp, uv, 0.0), fade);
}

// A point / spot light's falloff: the inverse square law, held at the source's radius (exact for a sphere light, and no blow-up on
// a lamp's own shade right next to the bulb), smoothly windowed to 0 at Range (Karis 2013; 0 = unlimited).
float light_distance_attenuation(LightData light, float distance)
{
    const float radius = light.spotParams.z;
    float attenuation = 1.0 / max(distance * distance, max(radius * radius, 1e-8));
    const float range = light.direction.w;
    if (range > 0.0)
    {
        const float ratio = distance / range;
        const float window = saturate(1.0 - ratio * ratio * ratio * ratio);
        attenuation *= window * window;
    }
    return attenuation;
}

// A spot light's cone (KHR_lights_punctual: smooth from the inner to the outer angle); 1 for the other lights. toLight: unit, from
// the surface towards the light.
float light_spot_attenuation(LightData light, float3 toLight)
{
    if ((uint)light.position.w != 2u)
        return 1.0;
    const float cone = saturate(dot(normalize(light.direction.xyz), -toLight) * light.spotParams.x + light.spotParams.y);
    return cone * cone;
}

// Rect and disc lights are one kind (an area light: a lit face, its two half axes); these functions take either.
bool is_area_light(uint type) { return type == 3u || type == 4u; }

float3 rect_light_axis_x(LightData light) { return light.spotParams.xyz; }

float3 rect_light_axis_y(LightData light)
{
    const float3 x = light.spotParams.xyz;
    const float lengthX = length(x);
    return lengthX > 1e-12 ? normalize(cross(x / lengthX, normalize(light.direction.xyz))) * light.spotParams.w : float3(0.0);
}

float rect_light_area(LightData light)
{
    const float halfX = length(light.spotParams.xyz);
    return (uint)light.position.w == 4u ? 3.14159265 * halfX * light.spotParams.w : 4.0 * halfX * light.spotParams.w;
}

// How much a light lights a point, roughly (luminance; a ranking for UniformBufferObject::shadowRayLightLimit, not a shading term):
// a directional light its illuminance, a point / spot light its intensity over the distance squared with its range window and cone,
// a rect / disc light its intensity times its area over the distance squared.
float light_shadow_priority(LightData light, float3 position)
{
    const float strength = dot(light.color.rgb, float3(0.2126, 0.7152, 0.0722)) * light.color.w;
    const uint type = (uint)light.position.w;
    if (type == 0)
        return strength;
    const float3 toLight = light.position.xyz - position;
    const float distance = length(toLight);
    if (type == 3 || type == 4)
    {
        const float area = rect_light_area(light);
        return strength * area / max(distance * distance, area);
    }
    const float3 l = distance > 0.0001 ? toLight / distance : float3(0.0, 1.0, 0.0);
    return strength * light_distance_attenuation(light, distance) * light_spot_attenuation(light, l);
}

// Whether a point of the light's plane (offset from its centre) lies on it: inside the rectangle, or the disc.
bool area_light_contains(LightData light, float3 offset)
{
    const float3 axisX = rect_light_axis_x(light);
    const float3 axisY = rect_light_axis_y(light);
    const float x = dot(offset, axisX) / max(dot(axisX, axisX), 1e-12);
    const float y = dot(offset, axisY) / max(dot(axisY, axisY), 1e-12);
    return (uint)light.position.w == 4u ? x * x + y * y <= 1.0 : abs(x) <= 1.0 && abs(y) <= 1.0;
}

// The point of the light nearest a point of its plane (offset from its centre): clamped into the rectangle, or the disc.
float3 area_light_clamp(LightData light, float3 offset)
{
    const float3 axisX = rect_light_axis_x(light);
    const float3 axisY = rect_light_axis_y(light);
    const float halfX = length(axisX);
    const float halfY = length(axisY);
    const float3 unitX = axisX / max(halfX, 1e-12);
    const float3 unitY = axisY / max(halfY, 1e-12);
    float2 local = float2(dot(offset, unitX), dot(offset, unitY));
    if ((uint)light.position.w == 4u)
    {
        const float radius = length(local);
        if (radius > halfX)
            local *= halfX / radius;
    }
    else
    {
        local = clamp(local, float2(-halfX, -halfY), float2(halfX, halfY));
    }
    return light.position.xyz + unitX * local.x + unitY * local.y;
}

// The emitting face of a rect light a ray meets first, closer than maxT: its radiance, and maxT made its distance (left as it is,
// and 0 returned, when there is none). A rect light shines along its front: a ray sees it travelling against that.
float3 ray_rect_lights(UniformBufferObject* ubo, float3 origin, float3 direction, inout float maxT)
{
    LightData* lights = (LightData*)ubo->lightDataReference;
    float3 radiance = float3(0.0);
    for (uint i = 0; i < ubo->lightCount; ++i)
    {
        const LightData light = lights[i];
        if (!is_area_light((uint)light.position.w))
            continue;
        const float3 front = normalize(light.direction.xyz);
        const float facing = dot(direction, front);
        if (facing >= -1e-6)
            continue;
        const float t = dot(light.position.xyz - origin, front) / facing;
        if (t <= 0.0 || t >= maxT)
            continue;
        if (!area_light_contains(light, origin + direction * t - light.position.xyz))
            continue;
        maxT = t;
        radiance = light.color.rgb * light.color.w;
    }
    return radiance;
}

// The range window of a rect light at `distance` from its centre (1 without a Range).
float rect_light_window(LightData light, float distance)
{
    const float range = light.direction.w;
    if (range <= 0.0)
        return 1.0;
    const float ratio = distance / range;
    const float window = saturate(1.0 - ratio * ratio * ratio * ratio);
    return window * window;
}

// The cosine-weighted solid angle (steradians, at most pi) the light fills above a surface's horizon: its irradiance there is the
// light's radiance times this (Lambert's polygon formula, the polygon clipped to the surface's hemisphere first). 0 behind it.
// A disc is the octagon of the same area: within a percent or two of the circle's.
float rect_light_form_factor(LightData light, float3 position, float3 normal)
{
    const float3 front = normalize(light.direction.xyz);
    if (dot(position - light.position.xyz, front) <= 0.0)
        return 0.0;
    const float3 x = rect_light_axis_x(light);
    const float3 y = rect_light_axis_y(light);
    const float3 c = light.position.xyz - position;
    float3 corners[8];
    uint cornerCount = 4;
    if ((uint)light.position.w == 4u)
    {
        cornerCount = 8;
        const float areaScale = 1.05390; // sqrt(pi / (2 sqrt 2)): an octagon through these points has the circle's area
        for (uint k = 0; k < 8; ++k)
        {
            const float angle = 0.78539816 * float(k);
            corners[k] = c + (x * cos(angle) + y * sin(angle)) * areaScale;
        }
    }
    else
    {
        corners[0] = c - x - y;
        corners[1] = c + x - y;
        corners[2] = c + x + y;
        corners[3] = c - x + y;
    }

    // Clip to the half-space in front of the surface (Sutherland-Hodgman: one more vertex at most).
    float3 clipped[9];
    uint count = 0;
    for (uint i = 0; i < cornerCount; ++i)
    {
        const float3 a = corners[i];
        const float3 b = corners[(i + 1) % cornerCount];
        const float da = dot(a, normal);
        const float db = dot(b, normal);
        if (da >= 0.0)
            clipped[count++] = a;
        if ((da >= 0.0) != (db >= 0.0))
            clipped[count++] = a + (b - a) * (da / (da - db));
    }
    if (count < 3)
        return 0.0;

    float sum = 0.0;
    for (uint i = 0; i < count; ++i)
    {
        const float3 a = normalize(clipped[i]);
        const float3 b = normalize(clipped[(i + 1) % count]);
        const float3 edge = cross(a, b);
        const float edgeLength = length(edge);
        if (edgeLength > 1e-9)
            sum += acos(clamp(dot(a, b), -1.0, 1.0)) * dot(edge / edgeLength, normal);
    }
    return min(0.5 * abs(sum), 3.14159265);
}

// One sample of a light seen from `position` (u: two uniform random numbers): a point on it -- in the sun's cone, on a sphere
// light's disc, on a rect or disc light's area -- and the light arriving from there at a surface facing it, everything folded in
// (falloff, cone, for an area light its area / distance^2 and its own cosine). Light reflected = brdf * radiance * cos(surface),
// averaged over u.
struct LightPointSample
{
    float3 position;  // the sampled point (directional: far along the direction)
    float3 direction; // unit, from `position` towards the sample
    float distance;   // to the sample: the shadow ray's length
    float3 radiance;
};

void light_basis(float3 n, out float3 tangent, out float3 bitangent)
{
    const float sign = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sign + n.z);
    const float b = n.x * n.y * a;
    tangent = float3(1.0 + sign * n.x * n.x * a, sign * b, -sign * n.x);
    bitangent = float3(b, sign + n.y * n.y * a, -n.y);
}

LightPointSample sample_light_point(LightData light, float3 position, float2 u)
{
    LightPointSample s;
    const uint type = (uint)light.position.w;
    const float radiusSqrt = sqrt(saturate(u.x));
    const float phi = 6.28318530718 * frac(u.y);
    const float2 disc = radiusSqrt * float2(cos(phi), sin(phi));

    if (type == 0u)
    {
        float3 direction = normalize(light.direction.xyz);
        if (light.spotParams.z > 1e-5)
        {
            float3 tangent, bitangent;
            light_basis(direction, tangent, bitangent);
            direction = normalize(direction + (disc.x * tangent + disc.y * bitangent) * tan(light.spotParams.z));
        }
        s.direction = direction;
        s.distance = 1e7;
        s.position = position + direction * s.distance;
        s.radiance = light.color.rgb * light.color.w;
        return s;
    }

    if (is_area_light(type))
    {
        // Uniform over the area: the rectangle, or the disc (the same square-root mapping as above).
        const float2 onLight = type == 4u ? disc : float2(2.0 * u.x - 1.0, 2.0 * u.y - 1.0);
        s.position = light.position.xyz + rect_light_axis_x(light) * onLight.x + rect_light_axis_y(light) * onLight.y;
        const float3 toSample = s.position - position;
        s.distance = length(toSample);
        s.direction = s.distance > 1e-6 ? toSample / s.distance : normalize(light.direction.xyz);
        const float cosLight = dot(normalize(light.direction.xyz), -s.direction);
        // Area sampling: radiance * area * cos / d^2, which cannot exceed the hemisphere's 2 pi.
        const float solidAngle = cosLight > 0.0 ? min(rect_light_area(light) * cosLight / max(s.distance * s.distance, 1e-12), 6.28318530718) : 0.0;
        s.radiance = light.color.rgb * light.color.w * solidAngle * rect_light_window(light, length(light.position.xyz - position));
        return s;
    }

    float3 samplePosition = light.position.xyz;
    const float3 toCentre = samplePosition - position;
    const float centreDistance = length(toCentre);
    if (light.spotParams.z > 1e-5 && centreDistance > 1e-6)
    {
        float3 tangent, bitangent;
        light_basis(toCentre / centreDistance, tangent, bitangent);
        samplePosition += (disc.x * tangent + disc.y * bitangent) * light.spotParams.z;
    }
    const float3 toSample = samplePosition - position;
    s.position = samplePosition;
    s.distance = length(toSample);
    s.direction = s.distance > 1e-6 ? toSample / s.distance : float3(0.0, 1.0, 0.0);
    // The falloff by the centre's distance (the disc only spreads the shadow ray).
    s.radiance = light.color.rgb * light.color.w * light_distance_attenuation(light, centreDistance) * light_spot_attenuation(light, s.direction);
    return s;
}

// The irradiance a light gives a surface (normal `normal`, unshadowed: times the visibility of the returned sample), with the
// shadow ray to test: towards the sample. Exact for rect lights (the form factor; the sample only places the shadow ray).
float3 light_surface_irradiance(LightData light, float3 position, float3 normal, float2 u, out LightPointSample sample)
{
    sample = sample_light_point(light, position, u);
    if (is_area_light((uint)light.position.w))
        return light.color.rgb * light.color.w * rect_light_form_factor(light, position, normal) *
               rect_light_window(light, length(light.position.xyz - position));
    return sample.radiance * max(dot(normal, sample.direction), 0.0);
}
#endif

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

// Path tracer material flags (GpuMaterial / GpuRayTracingInstance::pathTracerFlags), from RTXPT's material files when present.
STATIC_CONST uint32_t PathTracerThinSet = 1u;         // the material says whether it is thin (else: thin when it has no thickness)
STATIC_CONST uint32_t PathTracerThin = 2u;            // thin-walled: no refraction, 1 / IoR on both faces (RTXPT ThinSurface)
STATIC_CONST uint32_t PathTracerExcludeFromNEE = 4u;  // shadow rays pass it untouched (RTXPT ExcludeFromNEE: window glass)
STATIC_CONST uint32_t PathTracerRtxptMaterial = 8u;   // from an RTXPT material file: shadow rays follow RTXPT's rule (block
                                                      // unless ExcludeFromNEE or cut out), not the tinted pass-through
// RTXPT's PSDDominantDeltaLobe (which of a smooth pane's lobes DLSS-RR's guides follow): neither flag the transmission (0, the
// default), PathTracerPSDDominantReflection the reflection (1), PathTracerPSDDominantSurface the pane itself (-1).
STATIC_CONST uint32_t PathTracerPSDDominantReflection = 16u;
STATIC_CONST uint32_t PathTracerPSDDominantSurface = 32u;

#ifdef __SLANG__
// Whether rays of the hybrid passes go through a surface (and the raster path draws it as glass): see-through transmission and
// not alpha-masked (masked leaves with transmission are cut-out solid surfaces). The same test as GpuScene's render bucket.
bool ray_see_through(float transmission, uint alphaMode)
{
    return transmission >= SeeThroughTransmission && alphaMode != 1u;
}

// A tangent-space normal map sample (0..1) as a unit tangent-space normal, as the G-buffer (GBufferMaterial.slang) and the path
// tracer (PathTracerBridge.slang) decode it: X and Y from the texture, Z rebuilt when the texture stores none (two-channel BC5 maps
// such as the Bistro's _ddna read 0 in blue -- taken as -1 it turned the normal into the surface).
float3 tangent_normal_from_map(float3 sampled)
{
    float3 tangentNormal;
    tangentNormal.xy = sampled.xy * 2.0 - 1.0;
    tangentNormal.z = sampled.z <= 0.0 ? sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy))) : abs(sampled.z * 2.0 - 1.0);
    return dot(tangentNormal, tangentNormal) > 1e-12 ? normalize(tangentNormal) : float3(0.0, 0.0, 1.0);
}

// A normal map sample to a world-space normal in the mesh's stored tangent frame (xyz world tangent, w bitangent handedness),
// Gram-Schmidt'ed against the interpolated normal, as the G-buffer does it.
float3 normal_from_map(float3 sampled, float3 normal, float4 tangent)
{
    const float3 tangentNormal = tangent_normal_from_map(sampled);

    const float3 N = normalize(normal);
    const float3 t = tangent.xyz - N * dot(N, tangent.xyz);
    if (dot(t, t) <= 1e-12)
        return N;
    const float3 T = normalize(t);
    const float3 B = cross(N, T) * (tangent.w < 0.0 ? -1.0 : 1.0);
    return normalize(mul(tangentNormal, float3x3(T, B, N)));
}

// A material's depth bias (MaterialData::DepthBias; HDRP's Mesh Decal View Bias): the vertex drawn depthBias closer to the camera
// along its view ray -- under the reverse-Z projection its clip z scaled by distance / (distance - bias), the clip x, y and w, and so
// where it lands on screen, unchanged. Geometry modelled onto another surface (the Bistro's edge decals, trims and overlay layers,
// opaque faces 0 mm from the walls) then shows over it instead of z-fighting, as their engine's decal materials did.
float4 depth_bias_clip(float4 clipPosition, float3 worldPosition, float3 cameraPosition, float depthBias)
{
    if (depthBias <= 0.0)
        return clipPosition;
    const float distance = length(worldPosition - cameraPosition);
    clipPosition.z *= distance / max(distance - depthBias, 0.01 * distance);
    return clipPosition;
}

// Whether a surface lets light through to the hybrid passes' shadow rays: glass seen through, and what RTXPT's materials exclude
// from shadow rays (ExcludeFromNEE: the Bistro's windows). Windows that blocked the sun left the rooms behind them dark -- only
// their reflection showed.
bool ray_shadow_passes(GpuRayTracingInstance instance)
{
    return (instance.pathTracerFlags & PathTracerExcludeFromNEE) != 0 || ray_see_through(instance.transmissionFactor, instance.alphaMode);
}

// The exposure the post process applies (PostProcess.slang): the base exposure and compensation, and with auto exposure the stops
// from the adapted log2 average luminance to the mid-grey key (0.042, RTXPT's TONEMAPPING_EXPOSURE_KEY), brightening only partly.
float scene_exposure(UniformBufferObject* ubo, uint autoExposureTextureIndex, float compensation, float minEV, float maxEV)
{
    float exposure = ubo->exposure * exp2(compensation);
    if (autoExposureTextureIndex != 0xFFFFFFFF)
    {
        Texture2D<float> exposureTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + autoExposureTextureIndex, 0));
        uint width, height, levels;
        exposureTex.GetDimensions(0, width, height, levels);
        const float logAverage = exposureTex.Load(int3(0, 0, int(levels) - 1));
        if (isfinite(logAverage))
        {
            float stops = log2(0.042) - logAverage;
            if (stops > 0.0)
                stops *= AutoExposureDarkAdaptation;
            exposure *= exp2(clamp(stops, minEV, maxEV));
        }
    }
    return max(exposure, 1e-8);
}
#endif

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

struct PushConstantSkinning
{
    uint64_t sourceVerticesReference;
    uint64_t skinnedVerticesReference;
    uint64_t boneMatricesReference;
    uint64_t workItemsReference;
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
};

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
    float aerialPerspectivePadding1;
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
};

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
    uint32_t padding[3];
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
    uint32_t firstParticle;
    uint32_t particleCount;
    uint32_t textureIndex;        // flipbook sheet slot (image heap offset added in the shader), 0xFFFFFFFF: a soft round dot
    uint32_t flipbookColumns;
    uint32_t flipbookRows;
    uint32_t flags;               // PARTICLE_*
    uint32_t depthTextureIndex;   // the scene depth (render resolution), image heap offset added in the shader
    float softDistance;           // world units
};

struct PushConstantDDGIRadiance
{
    uint64_t matrixReference;
    uint32_t raysPerProbe;
    uint32_t probeCountTotal;
    uint32_t frameIndex;
    uint32_t probeOffsetsTextureIndex; // where the probes trace from: last frame's relocation (0xFFFFFFFF: the grid points)
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
    float3 attenuationColor;
    float attenuationDistance;
    uint32_t alphaMode;
    float alphaMaskCutoff;
    uint32_t doubleSided;
    uint32_t unlit;
    float diffuseTransmission;
    float depthBias; // GpuMaterial::depthBias

    uint32_t boneMatrixOffset;
    uint32_t skinnedVertexOffset;
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
            data.attenuationColor = material.attenuationColor;
            data.attenuationDistance = material.attenuationDistance;
            data.alphaMode = material.alphaMode;
            data.alphaMaskCutoff = material.alphaMaskCutoff;
            data.doubleSided = material.doubleSided;
            data.unlit = material.unlit;
            data.diffuseTransmission = material.diffuseTransmission;
            data.depthBias = material.depthBias;

            data.boneMatrixOffset = instance.boneMatrixOffset;
            data.skinnedVertexOffset = instance.skinnedVertexOffset;
            data.entityID = instance.entityID;
            return data;
        }
    }
};

#endif

#endif  // HOST_DEVICE_H
