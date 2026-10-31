#pragma once
#include <cmath>
#include <map>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <span>
#include "Renderer2D.h"
#include "NoxCore/Core/Window.h"
#include "Mesh.h"
#include <mutex>
#include <variant>

#include "GeometryArena.h"
#include "MemoryBudget.h"
#include "UploadManager.h"
#include "GeometryStreamer.h"
#include "VirtualTextureStreamer.h"
#include "NoxCore/Core/WorldUnits.h"
#include "GpuScene.h"
#include "NoxCore/RenderGraph/RenderGraph.h"
#include "NRI/ShaderBindingTable.h"

// Forward declaration only -- the RTXDI SDK must never be #included from this header (it's engine-
// public, pulled in by EditorLayer.cpp and others). Renderer.cpp is the only place that includes
// <Rtxdi/PT/ReSTIRPT.h> and touches this type; ~Renderer() is declared here but defined out-of-line in
// Renderer.cpp, which is what lets std::unique_ptr<rtxdi::ReSTIRPTContext> work with just this forward
// declaration (same reasoning NRI itself uses to keep Vulkan-specific types out of the engine-facing API).
namespace rtxdi { class ReSTIRPTContext; }

const std::string MODEL_PATH = "assets/models/viking_room.obj";
const std::string MODEL_PATH_GLTF = "assets/models/viking_room.glb";
const std::string MODEL_PATH_GLTF_STANDFORD = "assets/models/stanford_bunny/stanford_bunny.gltf";
const std::string MODEL_PATH_FOX_GLTF = "assets/models/Fox/Fox.gltf";
const std::string MODEL_PATH_GLTF_BIOHAZAR_Crate = "assets/models/Biohazard_Crate/Model/Untitled.gltf";
const std::string TEXTURE_PATH_FOX = "assets/models/Fox/Texture.png";
const std::string TEXTURE_PATH = "assets/textures/viking_room.ktx2";

// 2 quads
/*const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
    {{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
    {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},

    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
    {{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
    {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
    {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}
};

const std::vector<uint16_t> indices = {
    0, 1, 2, 2, 3, 0,
    4, 5, 6, 6, 7, 4
};*/

// This per-model data will be accessed via resource heaps
struct ModelData // not used was a example for descriptor heap buffer 
    //but im using bda need to remove this later with createDescriptorHeaps commented out thing
{
    glm::vec4 pos;
    glm::vec4 color;
};

struct PushConstantBlock
{
    uint64_t matrixReference;
    uint64_t vertexReference;
    uint64_t instanceReference;
};

inline int32_t selectedSampler{0};

inline std::vector<std::string> samplerNames{"Linear", "Nearest"};

namespace Nox
{
    struct ProjectRendererSettings;
    struct FrameGraphResources;
    struct ViewDrawResources;

    struct MeshBLAS
    {
        std::unique_ptr<NRI::Buffer> storageBuffer;
        std::unique_ptr<NRI::AccelerationStructure> as;
        uint64_t serial = 0; // which mesh's BLAS this is: ids are reused, a compaction must find the BLAS it measured
        // Destroyed backwards, so `as` goes before its buffer. Never overwrite a live one by assignment (that runs the members forwards
        // and frees the buffer under the structure): move it out first, or reset `as` then `storageBuffer`.
    };

    // A texture on its way to the GPU (§5.11.4): created with staging on the main thread, its texels written into the
    // staging from any thread, copied and published on the main thread. A streamed texture's image holds only its
    // resident mips (§5.12): image mip 0 is the texture's mip FirstMip.
    struct TextureUpload
    {
        Ref<Texture2D> Texture;
        StagingSpan Staging;            // the uploaded mips (image mips 0..MipOffsets.size()-1), empty when none
        std::vector<size_t> MipOffsets; // of each uploaded mip in Staging
        uint32_t FirstMip = 0;
    };

    // One submesh's geometry on its way to the GPU: ranges and staging on the main thread, the staging written from
    // any thread (Renderer::WriteMeshUpload), copies recorded and the mesh published on the main thread.
    // The texture asset a material's texture path names (MaterialData stores source paths): the cooked .ntex beside it, else the
    // source registered as itself. Main thread (cached).
    AssetHandle FindTextureAsset(const std::string& texturePath);

    struct MeshUpload
    {
        MeshHandle Handle;
        StagingSpan Staging;
        shaderio::GpuMesh GpuMesh{}; // counts and bounds, filled by WriteMeshUpload
        bool IsOpaque = true;
        // Geometry that streams (§5.7.2): what the geometry streamer keeps once the mesh is published; null otherwise.
        std::shared_ptr<StreamedMeshSource> Streaming;
    };

    // Everything whose release must wait for the frames that could still reference it (§5.8.4): a GPU buffer to
    // destroy, the last reference to an asset to drop, or a mesh's geometry ranges and BLAS id to return.
    struct DeferredRelease
    {
        uint32_t framesRemaining = MAX_FRAMES_IN_FLIGHT;
        std::variant<std::unique_ptr<NRI::Buffer>, Ref<Asset>, MeshHandle, MeshBLAS> payload;
    };

    struct PickRequest
    {
        int32_t x = -1;
        int32_t y = -1;
        uint32_t width = 1; // Default 1 for single click
        uint32_t height = 1; // Default 1 for single click
        bool active = false;
        uint64_t frameNumber = 0; // scene frame whose entity IDs were copied (set when the copy is recorded)
        // The block of depth values copied around the first pixel (the measure tool looks for the nearest drawn pixel in it).
        int32_t patchX = 0;
        int32_t patchY = 0;
        uint32_t patchWidth = 1;
        uint32_t patchHeight = 1;
    };

    // Entity IDs of the newest pick copy whose frame has finished on the GPU (§5.3 frame-ID readbacks).
    struct PickResult
    {
        PickRequest request;
        std::vector<int32_t> pixels;
        std::vector<float> depthPatch; // reverse-Z depths around the first pixel, row by row (0 = nothing drawn there)
        bool hasDepth = false;
    };

    // Procedural sky (SkyAtmosphere.slang, Hillaire 2020): Earth's atmosphere lit by the sun, used as the scene environment
    // instead of an HDR file. Scales multiply the Earth defaults.
    struct SkyAtmosphereSettings
    {
        glm::vec3 SunDirection{ 0.0f, 1.0f, 0.0f }; // world space, towards the sun
        glm::vec3 SunIlluminance{ 1.0f };           // the directional light's color * intensity
        glm::vec3 GroundAlbedo{ 0.3f };
        float RayleighScale = 1.0f;                 // air: the blue of the sky, the red of sunsets
        float MieScale = 1.0f;                      // haze / aerosols: the bright glow around the sun
        float OzoneScale = 1.0f;                    // the blue tint of twilight
        float MieAnisotropy = 0.8f;                 // how strongly haze scatters forward (0 = evenly)
        float AltitudeKm = 0.0f;                    // the viewer's height above the ground
        float SunAngularRadius = 0.004675f;         // radians: the disc the skybox draws (the light's angular diameter / 2)
        float AerialPerspectiveScale = 1.0f;        // multiplies view distances for the haze on the scene (UE's distance scale)
        // Volumetric clouds, raymarched into the environment with the sky.
        bool Clouds = true;
        float CloudCoverage = 0.35f;                // 0 = clear, 1 = overcast
        float CloudDensity = 1.0f;
        float CloudBottomKm = 1.5f;
        float CloudThicknessKm = 2.0f;
        float CloudScaleKm = 8.0f;                  // one tile of the shape noise: clouds of ~2 km
        glm::vec2 CloudOffsetKm{ 0.0f };            // moves them
        glm::vec2 CloudWindKmPerSecond{ 0.0f };     // and moves them on over time (x, z)
        // Night: the moon lights the air and the clouds like the sun (MoonIlluminance 0 by day: it does not count then); the
        // skybox draws its disc and the stars.
        glm::vec3 MoonDirection{ 0.0f, -1.0f, 0.0f };
        glm::vec3 MoonIlluminance{ 0.0f };          // above the atmosphere, its lit fraction included
        glm::vec3 MoonDiscIlluminance{ 0.0f };      // a full moon's light, for its disc (the skybox shades the phase); 0 = no moon
        float MoonAngularRadius = 0.0046f;          // radians
        float StarIntensity = 0.0f;                 // 0 = no stars (daylight)

        bool operator==(const SkyAtmosphereSettings&) const = default;
    };

    // Exponential height fog (HeightFog.slang): density(y) = DensityPerKm * exp(-FalloffPerKm * (y - BaseHeight)).
    struct HeightFogSettings
    {
        bool Enabled = false;
        float DensityPerKm = 0.5f;   // extinction per km at BaseHeight
        float FalloffPerKm = 5.0f;   // density halves every ln(2) / falloff km upwards
        float BaseHeight = 0.0f;     // world units
        float StartDistance = 0.0f;  // world units: no fog closer than this
        float MaxOpacity = 1.0f;
        glm::vec3 Color{ 1.0f };     // tint of the environment's light (FromEnvironment) or the fog's own radiance
        bool FromEnvironment = true;
        glm::vec3 SunDirection{ 0.0f, 1.0f, 0.0f }; // towards the sun
        glm::vec3 SunColor{ 0.0f };  // the sun's illuminance * the fog's directional scattering
        float SunExponent = 8.0f;
    };

    class Renderer
    {
    public:
        Renderer(std::shared_ptr<Nox::Window> window, bool isEditor);
        ~Renderer();

        // Call this from EditorLayer during mouse hover/click
        // Single click (1x1)
        void setPickRequest(int32_t x, int32_t y, bool active = true)
        {
            m_pickRequest = {x, y, 1, 1, active};
        }

        // Box drag (WxH)
        void setBoxPickRequest(int32_t x, int32_t y, uint32_t width, uint32_t height)
        {
            m_pickRequest = {x, y, width, height, true};
        }

        // Newest completed pick (1x1 hover or first pixel of a box); reads a CPU copy, never an in-flight GPU buffer.
        int32_t getPickedEntityID() const;

        // All unique entity IDs of the newest completed pick area
        std::vector<int32_t> getPickedEntityIDs() const;
        // Scene frame the newest completed pick was taken in.
        uint64_t getPickedFrameNumber() const { return m_pickResult.request.frameNumber; }
        // Depth near the first pixel of the newest completed pick: the pixel itself when something is drawn there, else the closest drawn
        // pixel within `radius` pixels (x, y = that pixel; depth 0 when nothing is drawn near). Reverse-Z infinite projection: depth = near /
        // view depth. False before the first pick completed.
        bool getPickedDepth(int32_t& x, int32_t& y, float& depth, int32_t radius = 0) const;

        void SetSelectedEntityID(const std::vector<int32_t>& entityIDs) { m_SelectedEntityIDs = entityIDs; }

        // Render graph tooling (editor panel): report, validation, synchronization strategy.
        RenderGraph& getRenderGraph() { return m_renderGraph; }
        const RenderGraph& getRenderGraph() const { return m_renderGraph; }

        // Texture inspection: while Name is set, that render graph texture is converted to a displayable image every frame
        // (RenderGraph::InspectTexture) and the raw value of the probe texel is read back.
        struct TextureInspection
        {
            std::string Name;           // empty: off
            uint32_t Occurrence = 0;    // n-th texture with that name (history slots share it)
            uint32_t Mip = 0;
            float Exposure = 1.0f;      // float formats
            uint32_t ChannelMask = 0xF; // bits R, G, B, A
            bool DepthCurve = false;    // reverse-Z curve, for depth kept in a color format (the Hi-Z pyramid)
            int32_t ProbeX = -1;        // texel in mip coordinates; -1: none
            int32_t ProbeY = -1;
        };
        void setTextureInspection(const TextureInspection& inspection) { m_textureInspection = inspection; }
        const TextureInspection& getTextureInspection() const { return m_textureInspection; }
        // Display image of the last inspected frame, nullptr when nothing is inspected. Stays valid for a few frames after
        // its size changes (render graph history).
        Texture2D* getInspectionImage() const { return m_inspectionImage; }
        // Physical description of the inspected texture (imports: size only).
        const RGTextureKey& getInspectedTextureKey() const { return m_inspectedTextureKey; }
        // Raw value of the probe texel from the newest finished frame that probed it.
        bool getInspectionProbe(glm::vec4& outValue) const
        {
            outValue = m_inspectionProbeValue;
            return m_inspectionProbeValid;
        }

        void drawFrame();
        void resizeWindow();
        void initImGui();
        void shutdownImGui();
        void beginImGui();
        void endImGui();

        void BeginScene(const Camera& camera, const glm::dmat4& cameraWorldMatrix);
        // Large world coordinates (Engine_Architecture_Plan_2026.md §5.14a): the GPU's world space is centred on this point, which
        // follows the camera in steps of RenderOriginStep (BeginScene moves it). Everything the GPU sees -- transforms, lights,
        // the camera, overlays -- is relative to it, in float, so it stays exact however far from the world's origin the camera is.
        const glm::dvec3& GetRenderOrigin() const { return m_renderOrigin; }
        glm::vec3 ToRenderSpace(const glm::dvec3& world) const { return glm::vec3(world - m_renderOrigin); }
        glm::mat4 ToRenderSpace(const glm::dmat4& world) const;
        glm::dvec3 FromRenderSpace(const glm::vec3& relative) const { return m_renderOrigin + glm::dvec3(relative); }
        glm::dmat4 FromRenderSpace(const glm::mat4& relative) const
        {
            glm::dmat4 world(relative);
            world[3] += glm::dvec4(m_renderOrigin, 0.0);
            return world;
        }
        // How far (meters, along any axis) the camera goes from the render origin before the origin moves to it.
        static constexpr float RenderOriginStepMeters = 1024.0f;
        void BeginScene(const EditorCamera& camera);
        void EndScene();

        // Rebuild the image-based lighting set for a scene environment. Equirectangular
        // HDR files are converted to a cubemap; DDS cubemaps are uploaded directly.
        // `rotation` turns the map around world +Y (radians), baked into the cubemap so the skybox, IBL, reflections, DDGI
        // and the path tracer all see it; a new angle on the same map is applied once it has stopped changing for a moment.
        void SetEnvironmentMap(const std::filesystem::path& path, float rotation = 0.0f);
        // Use the procedural sky as the scene environment (skybox with the sun's disc, IBL, reflections, DDGI, path tracer
        // misses). Called every frame while the scene wants it; a frame in which the settings changed updates the sky and
        // its IBL inside the frame (a few compute passes, no wait). SetEnvironmentMap switches back to a texture or none.
        void SetProceduralSky(const SkyAtmosphereSettings& settings);
        // The air's transmittance from the viewer (sky.AltitudeKm, scales of sky) towards a light in `direction` (normalized),
        // as SkyAtmosphere.slang's atmosphere has it: what reaches the ground of the sun's or the moon's light. Fades to 0 as a
        // disc of angularRadius (radians) sinks below the horizon.
        static glm::vec3 ComputeSkyTransmittance(const SkyAtmosphereSettings& sky, const glm::vec3& direction, float angularRadius);
        // Per frame, main thread (Scene::SetRenderer): the fog of the scene's environment, or disabled.
        void SetHeightFog(const HeightFogSettings& settings) { m_heightFog = settings; }

        // GPU scene (§5.5), main thread. Mesh entities live on the GPU until their owner changes them; the renderer
        // uploads only what changed.
        // The scene that renders registers its entities. Returns true when sceneID takes over: every instance of the
        // previous owner is gone and the scene registers all of its entities again.
        bool BindGpuScene(uint64_t sceneID);
        // One instance per drawn submesh, appended to outInstances. Returns false when an asset is not loaded yet (appended
        // to outMissingAssets, request it on the main thread): the entity is registered with what is loaded and has to be
        // registered again once it is.
        bool AddMeshInstances(const glm::dmat4& world, const MeshComponent& mesh, const MaterialComponent* material, int32_t entityID,
                              std::vector<uint32_t>& outInstances, std::vector<AssetHandle>& outMissingAssets);
        void RemoveMeshInstances(std::span<const uint32_t> instances);
        void SetMeshInstancesTransform(std::span<const uint32_t> instances, const glm::dmat4& world);
        // Skinned instances, every frame: this frame's joint matrices.
        void SetMeshInstancesBones(std::span<const uint32_t> instances, std::span<const glm::mat4> bones);
        // This frame's morph target weights of an entity's instances, by target name (a target not named: its default weight).
        void SetMeshInstancesMorphWeights(std::span<const uint32_t> instances, const std::map<std::string, float>& weights);
        // An entity with material parameters (MaterialComponent::Parameters, a sequencer's): its slots' records repacked with the
        // parameters as they are now (every frame: a timeline animates them).
        void RefreshEntityMaterials(const MeshComponent& component, const MaterialComponent& material, int32_t entityID);
        bool HasMorphTargets(std::span<const uint32_t> instances) const
        {
            for (uint32_t instance : instances)
                if (m_gpuScene.GetInstanceMorphs(instance))
                    return true;
            return false;
        }
        // Entities whose instances lost their mesh (it was unloaded) since the last call: register them again.
        void ConsumeInvalidatedMeshEntities(std::vector<int32_t>& outEntityIDs) { outEntityIDs.swap(m_invalidatedMeshEntities); m_invalidatedMeshEntities.clear(); }
        // A material asset's data was edited: instances using it shade with the new data from this frame.
        static void MarkMaterialChanged(AssetHandle material);
        // A mesh's own (embedded) material data changed in place (DynamicMeshes::SetColor): every GPU material is packed again.
        static void MarkMeshMaterialsChanged();

        // Per frame, from the scene's submit systems (one call at a time): an emitter's particles, drawn after the forward pass
        // (Particles.slang; docs/Hybrid_Renderer_Plan_2026-10.md §6). Alpha-blended emitters are sorted back to front here.
        struct ParticleBatchSettings
        {
            uint32_t TextureSlot = UINT32_MAX; // the flipbook sheet's bindless slot, UINT32_MAX: soft round dots
            uint32_t FlipbookColumns = 1;
            uint32_t FlipbookRows = 1;
            bool Additive = true;
            bool Lit = false;
            bool Soft = true;
            float SoftDistance = 0.0f;         // world units
        };
        void SubmitParticles(const ParticleBatchSettings& settings, std::span<const shaderio::GpuParticle> particles);
        void SubmitLight(const glm::dmat4& transform, const DirectionalLightComponent& light);
        void SubmitLight(const glm::dmat4& transform, const PointLightComponent& light);
        void SubmitLight(const glm::dmat4& transform, const SpotLightComponent& light);
        void SubmitLight(const glm::dmat4& transform, const RectLightComponent& light);
        void SubmitLight(const glm::dmat4& transform, const DiscLightComponent& light);

        Texture2D* GetSceneResource() const { return m_sceneResource.get(); }

        // Extra editor views (docs/Future.md item 5: the viewports besides the active one). Each draws another camera raster only --
        // forward-lit opaque, unlit, sky and transparent geometry, tonemapped, with the 2D overlay's world lines and its own grid -- into
        // a texture of its own. Submit it every frame it should draw, from OnUpdate (before ImGui shows the texture). GetExtraViewTexture is
        // the last image drawn completely (after a resize the old one, until the new size has been drawn once; null before the first).
        // grid: the view's world grid (Renderer2D::MakeGrid), or null.
        // Slots 0-3 are the editor's viewports; the asset previews use their own (preview: no shadow rays, no DDGI, fixed exposure).
        static constexpr uint32_t MaxExtraViews = 7;
        static constexpr uint32_t ThumbnailView = 4;
        static constexpr uint32_t LivePreviewView = 5;
        static constexpr uint32_t AnimationPreviewView = 6;
        // Show flags of an editor viewport drawn as an extra view (its View menu): base colors without lighting, the triangles' edges
        // over the image, no sky, no particles, the selection's outline.
        enum ExtraViewFlags : uint32_t
        {
            ExtraViewUnlit = 1u << 0,
            ExtraViewWireframe = 1u << 1,
            ExtraViewNoSky = 1u << 2,
            ExtraViewNoParticles = 1u << 3,
            ExtraViewOutline = 1u << 4,
        };
        void SubmitExtraView(uint32_t index, const EditorCamera& camera, NRI::Extent2D size, const shaderio::PushConstantGrid* grid, bool preview = false,
                             uint32_t flags = 0);
        // A ray against the scene's geometry (ray query on the TLAS: the real surface, not bounding boxes), answered a frame or two
        // later on the main thread: the distance to the first opaque surface, or a miss (also without ray tracing / a TLAS).
        // Up to shaderio::EDITOR_RAYCAST_MAX a frame; more wait for the next one.
        using RaycastCallback = std::function<void(bool hit, float distance)>;
        void RequestRaycast(const glm::dvec3& origin, const glm::vec3& direction, float maxDistance, RaycastCallback callback);
        Texture2D* GetExtraViewTexture(uint32_t index) const { return index < MaxExtraViews ? m_extraViews[index].Shown.get() : nullptr; }
        // Drops a view's texture (its viewport closed).
        void ReleaseExtraView(uint32_t index);
        // Hands the view's last drawn image over for good (an asset thumbnail): the view starts a new texture at its next submit.
        Ref<Texture2D> TakeExtraViewTexture(uint32_t index);
        // Copies a texture's first mip to the CPU (4 bytes a texel: RGBA8 / BGRA8, the surface format of the extra views' images):
        // copied in the next recorded frame, `done` runs on the main thread once that frame has finished on the GPU, with tightly
        // packed rows (empty when the copy could not be mapped). A few per frame; the rest wait. Main thread.
        using TextureReadbackCallback = std::function<void(std::vector<uint8_t> pixels, uint32_t width, uint32_t height)>;
        void ReadbackTexture(Ref<Texture2D> texture, TextureReadbackCallback done);
        // The camera jumped (the editor's active viewport changed): the temporal histories start over from the next frame (DLSS,
        // the denoisers, path tracer accumulation, the occlusion test's depth pyramid of the old view).
        void ResetCameraHistory()
        {
            m_renderGraph.ResetHistory(DLSSHistoryKey);
            m_renderGraph.ResetHistory(NRDHistoryKey);
            m_renderGraph.ResetHistory("Path Tracer Accumulation");
            m_renderGraph.ResetHistory("Camera Hi-Z");
        }

        void setVSync(bool enabled);
        void onViewportSizeChange(NRI::Extent2D size);
        bool getVSync() const { return m_vSync; }
        // Frame command buffers recorded on the JobSystem workers when there is enough work (off: one command buffer).
        void setParallelCommandRecording(bool enabled) { m_renderGraph.SetParallelRecording(enabled); }
        bool isParallelCommandRecording() const { return m_renderGraph.IsParallelRecording(); }
        NRI::Extent2D getViewPortSize() const { return m_viewportSize; }
        // Final output image size (editor viewport or swapchain), i.e. the 2D overlay pass render area.
        NRI::Extent2D getOutputSize() const { return m_isEditor ? m_viewportSize : m_swapChainExtent; }
        // Non-jittered view-projection submitted by this frame's BeginScene (editor or runtime camera).
        glm::mat4 getViewProjection() const { return m_currentNonJitteredProj * m_currentView; }
        NRI::Extent2D getRenderSize() const { return m_renderSize; }
        Renderer2D* getRenderer2D() const { return m_renderer2D.get(); }
        // Freezes the culling view (frustum and camera position of the moment it is frozen).
        void setFrozen(bool frozen)
        {
            m_frozen = frozen;
            m_frozenDone = false;
        }
        bool getFrozen() const { return m_frozen; }

        // GPU culling (§5.6). Meshlet culling: shaderio::MESHLET_CULL_* flags (0: lean task shader).
        void setInstanceCullingEnabled(bool enabled) { m_instanceCullingEnabled = enabled; }
        bool isInstanceCullingEnabled() const { return m_instanceCullingEnabled; }
        // Hi-Z occlusion culling (§5.6.4): instances hidden behind closer geometry, re-tested in phase 2 so nothing pops.
        void setOcclusionCullingEnabled(bool enabled) { m_occlusionCullingEnabled = enabled; }
        bool isOcclusionCullingEnabled() const { return m_occlusionCullingEnabled; }
        void setMeshletCulling(uint32_t flags) { m_meshletCulling = flags; }
        uint32_t getMeshletCulling() const { return m_meshletCulling; }
        // Cluster LOD (§5.7): the screen-space error in pixels a drawn cluster may have, or the original clusters only.
        void setLodErrorPixels(float pixels) { m_lodErrorPixels = pixels; }
        float getLodErrorPixels() const { return m_lodErrorPixels; }

        // SSAO / RTAO (mutually exclusive occlusion producers, one shared set of radius/power settings -- see the
        // comment on m_ssaoEnabled in the private section below)
        void setSSAOEnabled(bool enabled) { m_ssaoEnabled = enabled; if (enabled) m_rtaoEnabled = false; }
        bool getSSAOEnabled() const { return m_ssaoEnabled; }
        void setRTAOEnabled(bool enabled) { m_rtaoEnabled = enabled; if (enabled) m_ssaoEnabled = false; }
        bool getRTAOEnabled() const { return m_rtaoEnabled; }
        void setSSAORadius(float radius) { m_ssaoRadiusMeters = std::max(WorldUnits::ToMeters(radius), 0.0001f); }
        float getSSAORadius() const { return WorldUnits::FromMeters(m_ssaoRadiusMeters); }
        void setSSAOPower(float power) { m_ssaoPower = power; }
        float getSSAOPower() const { return m_ssaoPower; }
        void setLodFullDetail(bool fullDetail) { m_lodFullDetail = fullDetail; }
        bool getLodFullDetail() const { return m_lodFullDetail; }
        // Draw list entries, and of the newest finished frame: the instances drawn in phase 1 and the candidates phase 2
        // re-tested (some of those draw too, which only the GPU knows).
        uint32_t getDrawListSize() const { return static_cast<uint32_t>(m_drawList.size()); }
        uint32_t getVisibleInstanceCount() const { return m_visibleInstanceCount; }
        uint32_t getLateCandidateCount() const { return m_lateCandidateCount; }
        uint32_t getLateDrawnCount() const { return m_lateDrawnCount; }
        uint32_t getVisibleTriangleCount() const { return m_visibleTriangleCount; }
        // What the visibility passes of the newest finished frame drew after the LOD cut (opaque and masked geometry).
        uint32_t getDrawnClusterCount() const { return m_clusterStats.drawnClusters; }
        uint32_t getDrawnTriangleCount() const { return m_clusterStats.drawnTriangles; }
        // EditorLayer Settings
        void setDebugMode(uint32_t mode) { m_debugMode = mode; }
        uint32_t getDebugMode() const { return m_debugMode; }
        void setTonemapMode(uint32_t mode) { m_tonemapMode = mode; }
        uint32_t getTonemapMode() const { return m_tonemapMode; }
        // The editor viewport's exposure compensation (stops), on top of the scene camera's.
        float getViewportExposureCompensation() const { return m_viewportExposureCompensation; }
        void setViewportExposureCompensation(float stops) { m_viewportExposureCompensation = stops; }
        // The scene camera's exposure (Unreal's model): auto exposure meters the image (EV100, AutoExposureKey); without it the
        // manual EV100. The compensation (stops) applies to both.
        void setAutoExposure(bool enabled, float compensation, float minEV, float maxEV, float manualEV100)
        {
            m_autoExposure = enabled || m_viewportAutoExposure;
            m_exposureCompensation = compensation;
            m_autoExposureMinEV = minEV;
            m_autoExposureMaxEV = maxEV;
            m_manualEV100 = manualEV100;
        }
        float getManualEV100() const { return m_manualEV100; }
        // A camera's exposure at an EV100 (Unreal's saturation-based convention, ISO 100: 1 / (1.2 x 2^EV100)).
        static float ExposureFromEV100(float ev100) { return 1.0f / (1.2f * std::exp2(ev100)); }
        // The editor's Auto Exposure: on whatever the scene's camera says (a night scene is ~10 stops under daylight; without a primary
        // camera there is no camera setting at all). The camera's compensation and EV range still apply when it has them.
        void setViewportAutoExposure(bool enabled) { m_viewportAutoExposure = enabled; m_autoExposure = m_autoExposure || enabled; }
        bool getViewportAutoExposure() const { return m_viewportAutoExposure; }
        float getGamma() const { return m_gamma; }
        void setGamma(float gamma) { m_gamma = gamma; }
        float getScaleIBLAmbient() const { return m_scaleIBLAmbient; }
        void setScaleIBLAmbient(float scale) { m_scaleIBLAmbient = scale; }
        // RayTracing
        // This master toggle changes the EFFECTIVE state of shadows/reflections too (both are
        // m_rayTracingEnabled && their own flag), even though it doesn't touch m_rayTracingShadows/
        // m_rayTracingReflections themselves -- so it needs the same history reset those do.
        void setRayTracingEnabled(bool enabled)
        {
            if (m_rayTracingEnabled != enabled)
            {
                m_rayTracingEnabled = enabled;
                m_renderGraph.ResetHistory(NRDHistoryKey);
            }
        }
        bool getRayTracingEnabled() const { return m_rayTracingEnabled; }
        // Toggling this changes what the NRD shadow denoiser pass sees frame-to-frame (it's gated on
        // this flag -- see its call site in Renderer.cpp), so its temporal history needs a clean reset
        // on every transition, the same way every other denoiser-affecting setter in this file already
        // does (setNRDGIDenoiser/setNRDDIDenoiser/setNRDPTDenoiser/setDLSSRayReconstructionEnabled).
        // Without this, re-enabling shadows blended fresh 1-SPP shadow data into old, stale history,
        // producing visible leaking/ghosting for several frames until the denoiser caught up.
        void setRayTracingShadows(bool enabled)
        {
            if (m_rayTracingShadows != enabled)
            {
                m_rayTracingShadows = enabled;
                m_renderGraph.ResetHistory(NRDHistoryKey);
            }
        }
        bool getRayTracingShadows() const { return m_rayTracingShadows; }
        // Same reasoning as setRayTracingShadows above -- the NRD reflection denoiser pass is gated on
        // this flag, so toggling it needs a clean history reset too.
        void setRayTracingReflections(bool enabled)
        {
            if (m_rayTracingReflections != enabled)
            {
                m_rayTracingReflections = enabled;
                m_renderGraph.ResetHistory(NRDHistoryKey);
            }
        }
        bool getRayTracingReflections() const { return m_rayTracingReflections; }
        // Ray tracing culling (§5.14, Unreal's ray tracing culling radius / angle): instances farther than the radius whose bounds
        // look smaller than the angle leave the TLAS. On by default at 300 m and 1 degree, which leaves a scene like Bistro whole.
        void setRayTracingCulling(bool enabled) { m_rtCullingEnabled = enabled; }
        bool getRayTracingCulling() const { return m_rtCullingEnabled; }
        // Async compute (§5.4.4): the screen-probe GI runs on the device's compute-only queue beside the graphics work. On by default
        // where the device has such a queue (isAsyncComputeAvailable); off, or without one, it runs on the graphics queue as before.
        void setAsyncComputeEnabled(bool enabled) { m_asyncComputeEnabled = enabled; m_renderGraph.SetAsyncCompute(enabled); }
        bool getAsyncComputeEnabled() const { return m_asyncComputeEnabled; }
        bool isAsyncComputeAvailable() const { return m_renderGraph.IsAsyncComputeAvailable(); }
        uint32_t getAsyncComputePassCount() const { return m_renderGraph.GetAsyncComputePassCount(); }
        void setRayTracingCullRadius(float radius) { m_rtCullRadiusMeters = std::max(WorldUnits::ToMeters(radius), 0.0f); }
        float getRayTracingCullRadius() const { return WorldUnits::FromMeters(m_rtCullRadiusMeters); }
        void setRayTracingCullAngle(float degrees) { m_rtCullAngleDegrees = std::clamp(degrees, 0.0f, 90.0f); }
        float getRayTracingCullAngle() const { return m_rtCullAngleDegrees; }
        // The TLAS's instances and those culling leaves out; BLAS evicted under the ray tracing budget (rebuilt when needed).
        uint32_t getTlasInstanceCount() const { return m_gpuScene.GetTlasBuildCount(); }
        uint32_t getTlasCulledCount() const { return m_gpuScene.GetTlasCulledCount(); }
        uint32_t getEvictedBlasCount() const { return m_evictedBlasCount; }
        void setPathTracingEnabled(bool enabled) { m_pathTracingEnabled = enabled; }
        bool isPathTracingEnabled() const { return m_pathTracingEnabled; }
        void setPathTracingAccumulation(bool enabled) { m_pathTracingAccumulation = enabled; }
        bool isPathTracingAccumulation() const { return m_pathTracingAccumulation; }
        void setNRDShadowsEnabled(bool enabled)
        {
            if (m_nrdShadowsEnabled != enabled)
            {
                m_nrdShadowsEnabled = enabled;
                m_renderGraph.ResetHistory(NRDHistoryKey);
            }
        }
        bool getNRDShadowsEnabled() const { return m_nrdShadowsEnabled; }
        NRI::NRDReflectionDenoiser getNRDReflectionDenoiser() const { return m_nrdReflectionDenoiser; }
        void setNRDReflectionDenoiser(NRI::NRDReflectionDenoiser mode);

        // DDGI (Dynamic Diffuse Global Illumination)
        bool isDDGIEnabled() const { return m_ddgiEnabled; }
        void setDDGIEnabled(bool enabled)
        {
            if (m_ddgiEnabled != enabled)
            {
                m_ddgiEnabled = enabled;
                m_renderGraph.ResetHistory(DDGIHistoryKey);
            }
        }
        glm::vec3& getDDGIGridOrigin() { return m_ddgiGridOrigin; }
        glm::vec3& getDDGIGridSpacing() { return m_ddgiGridSpacing; }
        uint32_t getDDGIProbeCountX() const { return m_ddgiProbeCountX; }
        uint32_t getDDGIProbeCountY() const { return m_ddgiProbeCountY; }
        uint32_t getDDGIProbeCountZ() const { return m_ddgiProbeCountZ; }
        // Every cascade's probes (the atlases hold them all).
        uint32_t getDDGIProbeCountTotal() const { return m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ * getDDGICascadeCount(); }
        // Cascades in use: nested volumes around the camera, each DDGI Cascade Scale times wider than the one inside (1 for the fixed grid).
        uint32_t getDDGICascadeCount() const { return m_ddgiFollowCamera ? std::clamp(m_ddgiCascadeCount, 1u, 4u) : 1u; }
        uint32_t& getDDGICascadeSetting() { return m_ddgiCascadeCount; }
        float& getDDGICascadeScale() { return m_ddgiCascadeScale; }
        uint32_t getDDGIRaysPerProbe() const { return m_ddgiRaysPerProbe; }
        void setDDGIRaysPerProbe(uint32_t count) { m_ddgiRaysPerProbe = count; }
        float& getDDGIHysteresis() { return m_ddgiHysteresis; }
        // Probe relocation (DDGIRelocation.slang): probes inside walls or against surfaces move up to 45 % of a cell to free space.
        bool& getDDGIRelocation() { return m_ddgiRelocation; }
        float& getDDGINormalBias() { return m_ddgiNormalBias; }
        float& getDDGIDebugSphereRadius() { return m_ddgiDebugSphereRadius; }
        bool& getDDGIDebugXRay() { return m_ddgiDebugXRay; }
        void resetDDGIHistory() { m_renderGraph.ResetHistory(DDGIHistoryKey); }
        void resetDDGIGridToDefaults();
        // The probe volume follows the camera (on by default): centered on it, Spacing in meters (scaled by the project's world
        // unit), scrolled in whole cells with each probe's history kept. Off: the grid stays where Grid Origin / Grid Spacing say
        // (world units). Either way the volume hands over to the environment's irradiance at its faces.
        bool& getDDGIFollowCamera() { return m_ddgiFollowCamera; }
        glm::vec3& getDDGISpacingMeters() { return m_ddgiSpacingMeters; }
        // 1: every probe traces every frame; 2 / 4: half / a quarter of them in turn (cheaper, the light settles more slowly).
        uint32_t& getDDGIUpdateStride() { return m_ddgiUpdateStride; }

        // Diffuse GI: 0 the environment's irradiance (IBL), 1 DDGI, 2 ReSTIR GI, 3 screen probes (with DDGI underneath as the
        // world radiance cache: turn DDGI on with it).
        uint32_t getDiffuseGIMode() const { return m_diffuseGIMode; }
        void setDiffuseGIMode(uint32_t mode) { m_diffuseGIMode = mode; }
        // Ray tracing resolution, independent of DLSS (which scales everything, textures included): 1 traces every pixel, 2 one of each
        // 2 x 2 block (a quarter of the rays), a different one every frame, for RT reflections, RT shadows and RTAO; RTUpsample.slang
        // fills in the rest before their denoisers. The GI has its own (Screen Probe Spacing).
        uint32_t getRayTracingResolutionDivisor() const { return m_rtSubsample; }
        void setRayTracingResolutionDivisor(uint32_t divisor)
        {
            divisor = std::clamp(divisor, 1u, 2u);
            if (m_rtSubsample != divisor)
            {
                m_rtSubsample = divisor;
                m_renderGraph.ResetHistory(NRDHistoryKey);
            }
        }

        // The deferred lighting's shadow-ray budget: per pixel only the N lights that light it the most trace shadow rays (1..8),
        // the weaker ones light it unshadowed; 0 every light in range traces. The sun's shadow mask is outside the budget.
        uint32_t getShadowRayLightLimit() const { return m_shadowRayLightLimit; }
        void setShadowRayLightLimit(uint32_t limit) { m_shadowRayLightLimit = std::min(limit, 8u); }

        // ReSTIR GI runs for its mode, and for the "Indirect Diffuse GI Only" debug view (16) when no GI technique is chosen.
        bool isReSTIRGIRequested() const { return m_diffuseGIMode == 2 || (m_debugMode == 16 && m_diffuseGIMode == 0); }
        // Screen-probe GI (ScreenProbeCommon.slang): pixels per probe each way (8..32), spatial filter passes (0..3), frames
        // accumulated at most.
        uint32_t& getScreenProbeTileSize() { return m_screenProbeTileSize; }
        uint32_t& getScreenProbeFilterPasses() { return m_screenProbeFilterPasses; }
        uint32_t& getScreenProbeMaxHistory() { return m_screenProbeMaxHistory; }
        void resetScreenProbeHistory() { m_renderGraph.ResetHistory(ScreenProbeHistoryKey); }
        // Probes over the render resolution (the tile size's grid).
        NRI::Extent2D getScreenProbeCounts() const;

        // ReSTIR GI (Screen-Space Diffuse Indirect Resampling via RTXDI)
        float& getReSTIRGISpatialRadius() { return m_restirGISpatialRadius; }
        uint32_t& getReSTIRGINumSpatialSamples() { return m_restirGINumSpatialSamples; }
        uint32_t& getReSTIRGIMaxHistoryLength() { return m_restirGIMaxHistoryLength; }
        float& getReSTIRGINormalThreshold() { return m_restirGINormalThreshold; }
        float& getReSTIRGIDepthThreshold() { return m_restirGIDepthThreshold; }
        bool& getReSTIRGIEnableBoilingFilter() { return m_restirGIEnableBoilingFilter; }
        float& getReSTIRGIBoilingFilterStrength() { return m_restirGIBoilingFilterStrength; }
        NRI::NRDDiffuseDenoiser getNRDGIDenoiser() const { return m_nrdGIDenoiser; }
        void setNRDGIDenoiser(NRI::NRDDiffuseDenoiser mode);

        // ReSTIR DI (Screen-Space Resampled Direct Lighting via RTXDI)
        uint32_t getDirectLightingMode() const { return m_directLightingMode; }
        void setDirectLightingMode(uint32_t mode) { m_directLightingMode = mode; }
        uint32_t& getReSTIRDINumLocalLightSamples() { return m_restirDINumLocalLightSamples; }
        uint32_t& getReSTIRDINumInfiniteLightSamples() { return m_restirDINumInfiniteLightSamples; }
        uint32_t& getReSTIRDIMaxHistoryLength() { return m_restirDIMaxHistoryLength; }
        float& getReSTIRDINormalThreshold() { return m_restirDINormalThreshold; }
        float& getReSTIRDIDepthThreshold() { return m_restirDIDepthThreshold; }
        uint32_t& getReSTIRDINumSpatialSamples() { return m_restirDINumSpatialSamples; }
        float& getReSTIRDISpatialRadius() { return m_restirDISpatialRadius; }
        bool& getReGIREnabled() { return m_regirEnabled; }
        float& getReGIRCellSize() { return m_regirCellSize; }
        glm::vec3& getReGIRGridCenter() { return m_regirGridCenter; }
        float& getReGIRSamplingJitter() { return m_regirSamplingJitter; }
        NRI::NRDDiffuseDenoiser getNRDDIDenoiser() const { return m_nrdDIDenoiser; }
        void setNRDDIDenoiser(NRI::NRDDiffuseDenoiser mode);
        NRI::NRDDiffuseDenoiser getNRDPTDenoiser() const { return m_nrdPTDenoiser; }
        void setNRDPTDenoiser(NRI::NRDDiffuseDenoiser mode);
        bool getPathTracerUsesRTXDI() const { return m_pathTracerUsesRTXDI; }
        void setPathTracerUsesRTXDI(bool enabled) { m_pathTracerUsesRTXDI = enabled; }

        // ReSTIR PT (Screen-Space Path Resampling via RTXDI) -- NOT to be confused with the NRD-PT
        // denoiser above (getNRDPTDenoiser/setNRDPTDenoiser), which denoises the PLAIN path tracer's
        // raw output. This is a separate, third path-tracing mode: RTXDI's own path-space resampling
        // (reuse whole light-carrying paths across pixels/frames via reconnection shift mapping), only
        // meaningful when Path Tracing is enabled.
        bool getReSTIRPTEnabled() const { return m_restirPTEnabled; }
        void setReSTIRPTEnabled(bool enabled) { m_restirPTEnabled = enabled; }
        // Temporal resampling (hybrid-shift reconnection against last frame's reservoir) currently
        // produces a visible lighting-rotation artifact under investigation -- defaults OFF so ReSTIR
        // PT stays in its known-good None-resampling state; flip this on only to test/debug Temporal.
        // Defined in Renderer.cpp (not inline here) -- it touches rtxdi::ReSTIRPTContext, which this
        // header only forward-declares (see the RTXDI-stays-behind-NRI rule at the top of this file).
        bool getReSTIRPTTemporalEnabled() const { return m_restirPTTemporalEnabled; }
        void setReSTIRPTTemporalEnabled(bool enabled);
        uint32_t& getReSTIRPTNumInitialSamples() { return m_restirPTNumInitialSamples; }
        uint32_t& getReSTIRPTMaxBounceDepth() { return m_restirPTMaxBounceDepth; }
        uint32_t& getReSTIRPTMaxRcVertexLength() { return m_restirPTMaxRcVertexLength; }
        uint32_t& getReSTIRPTNumNeeSamples() { return m_restirPTNumNeeSamples; }
        float& getReSTIRPTRoughnessThreshold() { return m_restirPTRoughnessThreshold; }
        float& getReSTIRPTDistanceThreshold() { return m_restirPTDistanceThreshold; }
        uint32_t& getReSTIRPTMaxHistoryLength() { return m_restirPTMaxHistoryLength; }
        uint32_t& getReSTIRPTMaxReservoirAge() { return m_restirPTMaxReservoirAge; }
        float& getReSTIRPTNormalThreshold() { return m_restirPTNormalThreshold; }
        float& getReSTIRPTDepthThreshold() { return m_restirPTDepthThreshold; }
        bool& getReSTIRPTEnablePermutationSampling() { return m_restirPTEnablePermutationSampling; }

        // The engine's own temporal anti-aliasing (TAA.slang) while DLSS does not run (not supported, or off): the camera jitters and
        // each frame blends into the reprojected history. The path tracer accumulates by itself and skips it.
        void setTAAEnabled(bool enabled) { m_taaEnabled = enabled; }
        bool getTAAEnabled() const { return m_taaEnabled; }
        // Whether TAA resolves this frame: on, DLSS not evaluating, no path tracer.
        bool isTAAActive() const
        {
            const bool dlss = m_dlssEnabled && m_dlssMode != NRI::UpscaleMode::Off && m_device && m_device->isDLSSSupported();
            return m_taaEnabled && m_taaPipeline && !dlss && !m_pathTracingEnabled;
        }
        void setCameraJitterEnabled(bool enabled) { m_cameraJitterEnabled = enabled; }
        bool getCameraJitterEnabled() const { return m_cameraJitterEnabled; }
        // Editor, while the camera is jittered (DLSS / DLAA): the display-resolution entity IDs and depth are drawn again without the
        // jitter instead of blitted, so the grid, gizmo lines, collider wireframes and picking hold still. Costs a depth-only pass.
        void setStableEditorDepth(bool enabled) { m_stableEditorDepth = enabled; }
        bool getStableEditorDepth() const { return m_stableEditorDepth; }
        // Quality presets (docs/Hybrid_Renderer_Plan_2026-10.md §2): the hybrid renderer's toggles as one set, every rung ray traced (no
        // shadow maps). Low is the fallback for smaller RT GPUs (RT shadows, the 4 strongest lights' a pixel, at half ray tracing
        // resolution; screen probes, SSAO, DLSS Performance), High the closest
        // to the path tracer (RT shadows of every light through ReSTIR DI, RT reflections, RTAO, ReSTIR GI, DLSS Quality). Custom: the
        // settings were changed one by one.
        enum class RenderQuality : uint8_t { Low, Medium, High, Custom };
        void ApplyQualityPreset(RenderQuality quality);
        RenderQuality GetCurrentQuality() const; // the preset the settings equal now, or Custom
        RenderQuality GetRecommendedQuality() const; // from this GPU's memory budget, its output resolution and DLSS
        static const char* GetQualityName(RenderQuality quality);
        static RenderQuality ParseQualityName(const std::string& name, RenderQuality fallback);
        // A project's setting (ProjectConfig::RenderQuality): "Auto" applies the recommended preset, a preset's name that preset,
        // anything else (Custom) leaves the settings as they are.
        void ApplyQualitySetting(const std::string& setting);
        // The project's settings on top of the preset (ProjectConfig::Renderer): each one that is set (>= 0) replaces the preset's.
        void ApplyProjectRendererSettings(const ProjectRendererSettings& settings);

        // Glass (transmissive materials) refracts the lit scene behind it, bent by IOR and thickness and blurred by roughness (a copy of
        // the scene with blur mips; transparent surfaces then draw in a pass of their own). Off: transmission only lowers the alpha.
        void setRefractionEnabled(bool enabled) { m_refractionEnabled = enabled; }
        bool getRefractionEnabled() const { return m_refractionEnabled; }
        // Smooth glass traces its refraction (Hybrid_Renderer_Plan_2026-10.md §4 H3) while ray tracing is on; rough glass and
        // this off use the screen-space refraction.
        void setRTRefractionEnabled(bool enabled) { m_rtRefractionEnabled = enabled; }
        bool getRTRefractionEnabled() const { return m_rtRefractionEnabled; }
        // Order-independent transparency (Hybrid_Renderer_Plan_2026-10.md §4 H4, weighted blended): transparent surfaces and
        // alpha-blended particles add into two targets in any order, resolved over the scene once (OITComposite.slang). Many
        // overlapping layers (foliage cards, smoke) stop popping when their order changes; layers of very different colors blend
        // toward their average instead of the nearest one winning. Off: sorted back to front, by object.
        // Editor: every mesh's triangle edges over the image (Show > Wireframe).
        void setWireframeOverlay(bool enabled) { m_wireframeOverlay = enabled; }
        bool getWireframeOverlay() const { return m_wireframeOverlay; }
        void setOITEnabled(bool enabled) { m_oitEnabled = enabled; }
        bool getOITEnabled() const { return m_oitEnabled; }
        // OIT is for the raster pipeline alone. The hybrid one sorts its glass back to front and traces through it (ray traced
        // glass, shadows, GI): blended OIT layers there averaged glass in front of glass into see-through.
        bool isOITActive() const { return m_oitEnabled && !m_rayTracingEnabled; }
        glm::vec2 getCurrentJitter() const { return m_currentJitter; }

        Ref<Texture2D> UploadTexture(const TextureData& cpuData);

        // Streamed uploads (§5.11.4), main thread unless noted. Begin* creates the resource with staging for it (empty
        // when wait is false and staging has no room this frame); once the staging is written, End* records the copies
        // and returns the value they complete at; Publish* hands the resource to frames once that value has passed
        // (GetCompletedUploadValue). With nextFrameReads the next frame waits for the copies instead, so the resource
        // can be published right away (synchronous loads).
        // The image for the texture's mips [firstMip, last] with staging for the first uploadMipCount of them, laid out
        // as in the cooked file (dataSize: the whole texture's texel bytes).
        std::optional<TextureUpload> BeginTextureUpload(const TextureData& texture, uint32_t firstMip, uint32_t uploadMipCount, uint64_t dataSize, bool wait);
        // The uploaded mips, then keptMipCount mips of keptFrom (from its mip keptFromMip) into the image mips after them.
        uint64_t EndTextureUpload(const TextureUpload& upload, bool nextFrameReads, NRI::Texture2D* keptFrom = nullptr, uint32_t keptFromMip = 0,
                                  uint32_t keptMipCount = 0);
        void PublishTexture(Texture2D& texture);
        std::optional<MeshUpload> BeginMeshUpload(const MeshData& data, bool isOpaque, bool wait);
        // Any thread: the submesh's streams into its staging, with the offsets of its ranges, and its counts and bounds.
        static void WriteMeshUpload(const MeshData& data, MeshUpload& upload);
        // Whether the cluster is in the submesh's first BLAS: the original clusters, or for streamed geometry the finest of its root pages.
        static bool rtCluster(const MeshData& data, size_t cluster);
        uint64_t EndMeshUpload(const MeshUpload& upload, bool nextFrameReads);
        // Draws from now on, ray traced once its BLAS is built in a frame.
        MeshHandle PublishMesh(const MeshUpload& upload);
        // Staging of a Begin* whose copies were never recorded.
        void AbandonUpload(const StagingSpan& staging) { m_uploads.ReleaseStaging(staging); }
        uint64_t GetCompletedUploadValue() const { return m_uploads.GetCompletedValue(); }
        // Meshes that draw but are not ray traced yet (their BLAS waits for a frame's build budget).
        size_t GetPendingBlasBuilds() const { return m_blasBuilds.size(); }
        // Geometry streaming (§5.7.2): once per frame from the asset manager, within the upload budget it shares with the loads.
        void UpdateGeometryStreaming(uint64_t& uploadBudget) { m_geometryStreamer.Update(uploadBudget); }
        GeometryStreamer& GetGeometryStreamer() { return m_geometryStreamer; }
        // Whether loads read only the root pages of geometry that streams (the rest follows the feedback); off: every page.
        bool IsGeometryStreamingEnabled() const { return m_geometryStreamer.IsEnabled(); }
        static bool StreamsGeometryPages();
        // Streaming virtual textures (§5.12.2): a loaded virtual texture's tail image and cooked header (its material texture index
        // carries shaderio::MaterialTextureVirtual from now on); false when it cannot stream (it draws with its tail).
        bool RegisterVirtualTexture(Texture2D& tail, const CookedTextureHeader& header);
        // Its tail image is being unloaded.
        void UnregisterVirtualTexture(uint32_t slot);
        // Once per frame from the asset manager, within the upload budget it shares with the loads.
        void UpdateVirtualTextures(uint64_t& uploadBudget) { m_virtualTextures.Update(uploadBudget); }
        VirtualTextureStreamer& GetVirtualTextures() { return m_virtualTextures; }
        // Whether textures cooked as virtual load as virtual textures (else mip streamed).
        bool IsVirtualTexturingEnabled() const { return m_virtualTextures.IsEnabled(); }
        // What a memory category holds and may hold (§5.8.3); the budget is 0 until the first sample.
        const MemoryCategoryStats& GetMemoryCategory(MemoryCategory category) const
        {
            return m_memoryBudget.GetCategories()[static_cast<size_t>(category)];
        }
        // The newest texture streaming feedback (one entry per image slot, MipFeedback.slang) and a number that changes
        // with every readback.
        const std::vector<uint32_t>& GetMipFeedback(uint64_t& outSerial) const
        {
            outSerial = m_mipFeedbackSerial;
            return m_mipFeedback;
        }
        // Textures finished loading: materials resolve their texture paths again (they drew without them so far).
        static void MarkTexturesLoaded();
        Ref<Texture2D> createSolidColorTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
        void InitializeEnvironmentLighting(const std::filesystem::path& environmentPath, float rotation);
        void releaseEnvironmentTextures(bool keepBrdfLut);
        Ref<Texture2D> createEnvironmentCube(uint32_t size, NRI::ImageFormat format);
        void createIBLPipelines();
        void generateEnvironmentLighting();
        void createBRDFLut();
        void createProceduralSkyPipelines();
        void createProceduralSkyResources();
        void addProceduralSkyPass();
        shaderio::PushConstantSkyAtmosphere makeSkyAtmospherePush(const SkyAtmosphereSettings& settings, const glm::vec2& cloudOffsetKm,
                                                                  float viewerAltitudeKm) const;
        // The height the sky is seen from (km): the settings' altitude of the world origin plus the camera's.
        float skyViewerAltitudeKm() const;
        RGTexture addAerialPerspectivePass();
        // The clouds' shadow map for this frame (SkyAtmosphere.slang cloudShadowMain) and its uniforms; none without clouds.
        void addCloudShadowPass();

        template <class T>
        void UploadBufferSlice(NRI::Buffer& dstBuffer, const T* data, uint32_t elementOffset, uint32_t elementCount);

        MeshHandle UploadMeshGeometry(const MeshData& data, bool isOpaque = true);

        static MeshHandle UploadMesh(const MeshData& data, bool isOpaque = true)
        {
            NOX_CORE_ASSERT(s_Instance, "Renderer instance does not exist!");

            return s_Instance->UploadMeshGeometry(data, isOpaque);
        }

        void UnloadMeshGeometry(const MeshHandle& handle);
        void deferAssetRelease(Ref<Asset> asset);

        // Keeps an unloaded asset alive until in-flight frames can no longer reference its GPU
        // resources; the asset's destructor (texture slot release, mesh free) runs afterwards.
        static void DeferAssetRelease(Ref<Asset> asset)
        {
            // Through a function, never into the members: inline access here bakes the renderer's layout into every
            // translation unit that includes this header.
            if (s_Instance && asset)
                s_Instance->deferAssetRelease(std::move(asset));
        }

        // Device-local (VRAM) bytes, as last sampled (a few times a second): what the engine's resources hold (allocated, not the
        // allocator's blocks, which stay when what is in them is freed), and what the process uses in all (VK_EXT_memory_budget).
        static uint64_t GetDeviceLocalAllocatedBytes();
        static uint64_t GetDeviceLocalMemoryUsage();

        // Drops cached path -> bindless-slot entries for the given (now freed) texture slots.
        static void InvalidateTextureDescriptorSlots(const std::vector<uint32_t>& slots);

        static void UnloadMesh(const MeshHandle& handle)
        {
            NOX_CORE_ASSERT(s_Instance, "Renderer instance does not exist!");
            s_Instance->UnloadMeshGeometry(handle);
        }
        
        void setDLSSEnabled(bool enabled);
        bool isDLSSEnabled() const { return m_dlssEnabled; }
        void setUpscaleMode(NRI::UpscaleMode mode);
        NRI::UpscaleMode getUpscaleMode() const { return m_dlssMode; }
        bool isDLSSSupported() const { return m_device && m_device->isDLSSSupported(); }
        void setDLSSRayReconstructionEnabled(bool enabled);
        bool isDLSSRayReconstructionEnabled() const { return m_dlssRayReconstructionEnabled; }
        bool isDLSSRayReconstructionSupported() const { return m_device && m_device->isDLSSRayReconstructionSupported(); }

    private:
        template <typename MeshAsset>
        bool addSubmeshInstances(const glm::dmat4& world, const MeshAsset& mesh, const MeshComponent& component, const MaterialComponent* material,
                                 int32_t entityID, std::vector<uint32_t>& outInstances, std::vector<AssetHandle>& outMissingAssets);
        // Shared material record of a submesh: its material asset when loaded, else the mesh's embedded material.
        MaterialData entityMaterialData(const MaterialData& meshMaterial, AssetHandle materialAsset, const MaterialComponent& component,
                                        uint32_t submesh, bool& outComplete, std::vector<AssetHandle>& outMissingAssets);
        uint32_t acquireEntityMaterial(int32_t entityID, uint32_t submesh, const MaterialData& data);
        template <typename MeshAsset>
        void refreshEntityMaterials(const MeshAsset& mesh, const MeshComponent& component, const MaterialComponent& material, int32_t entityID);
        uint32_t acquireGpuMaterial(uint64_t meshAsset, uint32_t submesh, const MaterialData& meshMaterial, AssetHandle materialAsset,
                                    bool& outComplete, std::vector<AssetHandle>& outMissingAssets);
        // Re-packs materials whose texture slots may have changed (texture imported or unloaded).
        void refreshGpuMaterials();
        void readPickResult(uint32_t frameSlot);
        void readRaycastResults(uint32_t frameSlot);
        void addEditorRaycastPass();
        void createEditorRaycastPipeline(bool forceCompile = false);
        void readTextureReadbacks(uint32_t frameSlot);
        void readInspectionProbe(uint32_t frameSlot);
        void readCullStats(uint32_t frameSlot);
        void readBlasCompactedSizes(uint32_t frameSlot);
        // BLAS residency under the ray tracing budget (§5.9 / §5.14): evicted BLAS a traced instance needs again are queued to
        // build; over the budget, the BLAS of meshes no traced instance needed for BlasEvictionFrames go, longest unneeded first.
        void manageBlasResidency();
        void readMipFeedback(uint32_t frameSlot);
        void readClusterStats(uint32_t frameSlot);
        void readGeometryFeedback(uint32_t frameSlot);

        void initRenderer();
        void cleanupSwapChain();
        void recreateSwapChain();
        void createSwapChain();
        void createCompiler();
        void watchShader(const std::filesystem::path& path, const std::string& pipelineKey, std::function<void()> reloadFn);

        void createUnlitPipeline(bool forceCompile);
        void createTransparentLitPipeline(bool forceCompile);
        void createWireframePipeline(bool forceCompile = false);
        void createPresentPipeline(bool forceCompile);
        void createComputePipeline();
        void createSkyboxPipeline(bool forceCompile);
        void createLightShapesPipeline(bool forceCompile);
        void createCommandPool();

        void createSceneResources();
        // Recomputes m_renderSize from the current output size + DLSS mode. Graph-owned render targets follow the new
        // size by themselves; this resets every history and the size-dependent denoiser/upscaler/RTXDI state. Called on
        // viewport resize, DLSS enable/disable, and DLSS mode change.
        void applyRenderResolution();
        void applyPendingRenderResolutionIfNeeded();
        
        // Visability
        void createVisibilityPipeline(bool forceCompile);
        void createComputeSkinningPipeline(bool forceCompile = false);
        // G-Buffer
        void createGBufferPipeline(bool forceCompile = false);
        // PBR
        void createDeferredLightingPipeline(bool forceCompile = false);
        // SSAO / RTAO
        void createSSAOPipeline(bool forceCompile = false);
        void createSSAOBlurPipeline(bool forceCompile = false);
        void createRTAOPipeline(bool forceCompile = false);
        void createSSAOKernel();
        void addSSAOPass();
        float ambientOcclusionShortRangePixels() const;
        void addRTAOPass();
        void addOcclusionBlurPass();
        // Post Process
        void createPostProcessPipeline(bool forceCompile = false);
        void createAutoExposurePipelines(bool forceCompile = false);
        void createColorPyramidPipelines(bool forceCompile = false);
        void createParticlePipeline(bool forceCompile = false);
        void createOITCompositePipeline(bool forceCompile = false);
        // Render graph texture inspection
        void createTextureInspectPipeline(bool forceCompile = false);
        // GPU instance culling (InstanceCulling.slang: cull, count, offset, write, late) and the depth pyramid build
        void createInstanceCullingPipelines(bool forceCompile = false);
        void createHiZBuildPipeline(bool forceCompile = false);
        void createRRGuidesPipeline(bool forceCompile = false);
        void createTAAPipeline(bool forceCompile = false);
        
        // NRD
        void createShadowMaskPipeline(bool forceCompile = false);
        void createNRDGuidesPipeline(bool forceCompile = false);
        void createReflectionPipeline(bool forceCompile = false);
        
        // Path Tracer
        void createPathTracerPipeline(bool forceCompile = false);

        // DDGI (Dynamic Diffuse Global Illumination)
        void createDDGIPipelines(bool forceCompile = false);
        void createScreenProbePipelines(bool forceCompile = false);
        void createRTUpsamplePipelines(bool forceCompile = false);

        // ReSTIR GI (Screen-Space Diffuse Path Resampling via RTXDI)
        void createRTXDINeighborOffsets();
        void createReSTIRGIPipelines(bool forceCompile = false);

        // ReSTIR DI (Screen-Space Resampled Direct Lighting via RTXDI)
        void createReSTIRDIPipelines(bool forceCompile = false);

        // ReSTIR PT (Screen-Space Path Resampling via RTXDI)
        // RTXDI's own buffer-index rotation and defaults; recreated with the render size.
        void createReSTIRPTContext();
        void createReSTIRPTPipelines(bool forceCompile = false);

        void createTextureImage();
        void initGeometryBuffers();
        void createUniformBuffers();
        void createDrawListBuffers(uint64_t bufferSize);
        void createSelectedEntityIDBuffers();
        void createDescriptorHeaps();
        std::unique_ptr<NRI::CommandBuffer> beginSingleTimeCommands();
        void endSingleTimeCommands(std::unique_ptr<NRI::CommandBuffer>&& commandBuffer);
        // Frame render graph (§5.4): recordFrame builds, compiles and executes it and returns the frame's command buffers
        // in submission order. prepareFrameGraph imports the renderer's resources and resolves this frame's CPU-side
        // state; the add* functions (Renderer/Passes/*.cpp) contribute each feature's passes in frame order and make every
        // CPU-side decision there: execute callbacks only record, possibly on worker threads.
        std::span<const RGSubmission> recordFrame(uint32_t imageIndex);
        std::vector<NRI::TimelinePoint> m_submitWaitScratch;
        // Start of every frame command buffer: descriptor heaps and the dynamic state every pass builds on.
        void applyCommandBufferBaseline(NRI::CommandBuffer& cmd) const;
        void prepareFrameGraph(uint32_t imageIndex);
        void addGpuSceneUpdatePass();
        void addComputeSkinningPass();
        void addAnimatedBLASPass();
        // Culls the draw list for the camera view into its visible instances, indirect commands and draw counts.
        void addInstanceCullingPass();
        // The culling dispatches of one view (the camera, or an extra view): its uniforms and CullView, the Hi-Z it tests against
        // (none: no occlusion test).
        void addCullingDispatchPass(const char* name, const ViewDrawResources& draws, RGBuffer flags, RGBuffer blocks, uint32_t entryCount,
                                    RGTexture hiZ, NRI::Buffer* uniformBuffer, NRI::Buffer* cullViewBuffer);
        // The editor's extra views (SubmitExtraView): culling, one forward raster pass, tonemapping and the world overlay each.
        void addExtraViewPasses();
        // After resolveFrameUniforms: each extra view's copy of the frame's uniforms with its own camera.
        void updateExtraViewUniforms();
        // Depth pyramid of this frame's visibility depth, then phase 2: the occluded candidates against it.
        void addHiZBuildPass();
        void addInstanceCullingLatePass();
        // Draws what phase 2 found visible into the visibility buffer and depth.
        void addVisibilityLatePass();
        void addBLASBuildPass();
        void addTLASBuildPass();
        void addVisibilityPass();
        void addGBufferPass();
        void addNRDGuidesPass();
        void addMipFeedbackReadbackPass();
        void addClusterStatsReadbackPass();
        // Geometry streaming (§5.7.2): the frame's page activations and evictions (cluster records, resident flags), right after the
        // GPU scene update and before anything reads them; the feedback (which pages the LOD wants) and its readback.
        void addGeometryPagesUpdatePass();
        void addGeometryFeedbackPass();
        void createGeometryStreamingPipelines();
        void addRTShadowPasses();
        void addRTReflectionPasses();
        // Decides whether DDGI runs and declares its atlases up front: RT reflections and forward shading, which come
        // earlier in the frame, sample them too.
        void prepareDDGIFrame(FrameGraphResources& resources);
        // Once per frame: where the probe volume is (camera-following or fixed) and how far it scrolled, into the uniforms.
        void updateDDGIVolume();
        void addDDGIPasses();
        // Screen-probe GI (diffuse GI mode 3): placement, trace, convolution, spatial filter, per-pixel integration.
        void addScreenProbeGIPasses();
        // A lower ray tracing resolution (m_rtSubsample): the size the reduced RT passes render at, a target of that size, and the
        // pass bringing such a result back to the render resolution (RTUpsample.slang; mode 1: best match only).
        NRI::Extent2D reducedRTExtent() const;
        RGTexture createReducedRTTarget(const char* name, NRI::ImageFormat format);
        void addRTUpsamplePass(const char* name, RGTexture source, RGTexture target, NRI::Pipeline* pipeline, uint32_t mode);
        void addLightPresamplingPasses();
        // RTXDI light sampling over this frame's presampled lights (RTXDILightSampling.slang), for ReSTIR DI and GI.
        shaderio::RTXDILightSamplingParams lightSamplingParams(const RGPassContext& context, RGBuffer risBuffer) const;
        void addReSTIRGIPasses();
        void addPreviousFrameCopyPass();
        void addReSTIRDIPasses();
        void addReSTIRPTPasses();
        void addPathTracerPasses();
        void addEmissiveLightGridPass();
        void addDeferredLightingPass();
        void addForward3DPass();
        void addParticlePass();
        // The order-independent transparency targets of this frame (created on first use; the first writer clears them), and
        // their resolve over the HDR scene once everything transparent is in them.
        void ensureOITTargets();
        // The lit scene so far with blur mips (resources.RefractionSource), for glass to refract: the HDR scene at the render
        // resolution, or (`source`, `extent`) the upscaled image the transparent surfaces are drawn over after DLSS-RR.
        void addRefractionSourcePasses();
        void addRefractionSourcePasses(RGTexture source, NRI::Extent2D extent);
        // Under DLSS Ray Reconstruction: the transparent surfaces over its output at display resolution, without the jitter. Drawn into
        // its input they reached RR as part of the surface behind them -- RR moved a window's reflection with the room behind it
        // (it trailed after the camera) and took the glass's own shading for noise to filter.
        // What a pass drawing forward surfaces over the scene reads and targets (after its own color target): entity IDs, depth,
        // fog, and for lit surfaces the GI, AO and TLAS they sample.
        void declareForwardInputs(RGBuilder& builder, bool lit);
        // How far this frame's jitter moves a point on screen, in uv (y down): from the jittered and the plain projection.
        glm::vec2 jitterScreenOffsetUv() const;
        // Screen-probe GI's last frame (its history reprojection): the jittered projection and view it was drawn with, its jitter.
        glm::mat4 m_screenProbePrevProj{ 1.0f };
        glm::mat4 m_screenProbePrevView{ 1.0f };
        glm::vec2 m_screenProbePrevJitterUv{ 0.0f };
        bool m_screenProbePrevMatricesValid = false;
        // Whether the transparent buckets have draws this frame.
        bool hasTransparentDraws() const;
        void addOITCompositePass();
        // Lit transparent draws whose material transmits (glass) in this frame's draw list.
        bool hasTransmissiveDraws() const;
        // The skybox push constants of the sun's disc (procedural sky), without the uniforms and cubemap.
        shaderio::PushConstantSkybox makeSkyboxSunPush() const;
        void addDLSSPass();
        void addTAAPass();
        void addEntityDepthBlitPass();
        // The blit's replacement in the editor while the camera is jittered (setStableEditorDepth).
        void addStableEntityDepthPass();
        void addPostProcessPass();
        void addOverlay2DPass();
        // Editor: the selected instances drawn again without camera jitter into a display-resolution mask the outline reads.
        void addSelectionMaskPass();
        void addOutlinePass();
        void addPresentPass();
        void addPickReadbackPass();
        void addTextureReadbackPasses();
        void addTextureInspection();
        // After Compile: writes the bindless slots of graph-owned textures into the uniforms uploaded for this frame.
        void resolveFrameUniforms();
        // Indirect meshlet draws of a view's visible instances, per bucket with the GPU-written draw count. Each pass keeps
        // its own cursor: passes may record in parallel.
        struct MeshletDrawCursor
        {
            shaderio::PushConstantMeshlets references{};
            NRI::Buffer* commands = nullptr;
            NRI::Buffer* counts = nullptr;
            NRI::Pipeline* boundPipeline = nullptr;
            bool late = false; // phase 2 draws its candidates with their own commands (empty while still occluded)
            uint32_t taskFlags = 0; // shaderio::MESHLET_* for PushConstantMeshlets::meshletCulling
        };
        MeshletDrawCursor beginMeshletDraws(const RGPassContext& context, const ViewDrawResources& draws, bool late = false) const;
        void drawMeshletBucket(NRI::CommandBuffer& cmd, MeshletDrawCursor& cursor, RenderBucket bucket, NRI::Pipeline& pipeline, NRI::CullMode cullMode, bool depthWrite, bool blendEnable) const;
        // The transparent buckets (both culling phases), blended over the HDR scene; the cursor carries the SSAO and refraction slots.
        // With `oit` into the order-independent transparency targets instead (accumulation, revealage, entity IDs).
        // How drawForwardTransparent blends: over what is drawn (back to front, premultiplied); the glass's two passes over the
        // scene, multiplied (what it lets through) and added (its own light), lit surfaces only.
        enum class TransparentBlend { Over, GlassMultiply, GlassAdd };
        void drawForwardTransparent(NRI::CommandBuffer& cmd, const RGPassContext& context, const ViewDrawResources& draws, MeshletDrawCursor& cursor,
                                    bool oit = false, TransparentBlend blend = TransparentBlend::Over) const;
        // Draw list entries of a bucket (its visible instances are at most that many).
        uint32_t getBucketEntryCount(RenderBucket bucket) const
        {
            const size_t index = static_cast<size_t>(bucket);
            return m_drawBucketStarts[index + 1] - m_drawBucketStarts[index];
        }
        void updateEntityIDBuffer(uint32_t currentImage);
        void updateUniformBuffer(uint32_t currentImage);
        // This frame's draw list, culling view and GPU scene uploads (staged for the GPU Scene Update pass).
        void updateGpuScene(uint32_t currentImage);
        void updateSkinningLayout(uint32_t currentImage);
        void updateAnimatedBLAS(uint32_t currentImage);
        void updateDrawListBuffers(uint32_t currentImage);
        // Releases whose wait has passed; a mesh returns its ranges to the streams, the others die with their entry.
        void processDeferredReleases();
        std::vector<char> readFile(const std::string& filename);
        // Sub-allocates from a geometry stream, copying the stream into a larger buffer first when it no longer fits.
        GeometryRange allocateGeometry(GeometryArena& stream, uint32_t count);
        // A stream that grew (only without sparse buffers): its content copied into the larger buffer, the old one released later.
        void handleStreamGrowth(GeometryArena& stream, std::unique_ptr<NRI::Buffer> grownFrom);
        void createLightBuffer(uint64_t bufferSize);
        void updateLightBuffer(uint32_t currentImage);
        void sampleMemoryStats();
        // What each category holds of the device budget, from the systems that own the memory (§5.8.3).
        void updateMemoryBudget();

    private:
        inline static Renderer* s_Instance = nullptr;
        std::unique_ptr<Renderer2D> m_renderer2D;
        std::shared_ptr<Nox::Window> m_window;
        std::unique_ptr<NRI::Device> m_device = nullptr;
        // Profiling (NOX_PROFILING_ENABLED): timestamp queries for the frame command buffer.
        std::unique_ptr<NRI::GpuProfiler> m_gpuProfiler = nullptr;
        // Sampled a few times a second in every build: the memory budget (texture streaming pool) and the stats.
        std::vector<NRI::MemoryHeapStats> m_memoryHeapStats;
        std::chrono::steady_clock::time_point m_lastMemoryStatsSample{};
        std::unique_ptr<NRI::Swapchain> m_swapChain = nullptr;
        NRI::Extent2D m_swapChainExtent{640, 480};
        NRI::Extent2D m_viewportSize{640, 480};
        // Resolution the pre-DLSS 3D pipeline (visibility, G-buffer, lighting) actually renders at.
        // Equal to the output size unless a DLSS mode other than Off/DLAA is active, in which case
        // it's whatever slDLSSGetOptimalSettings recommends for that mode - see applyRenderResolution().
        NRI::Extent2D m_renderSize{640, 480};
        bool m_vSync = false;
        bool m_isEditor = false;

        Utils::NOXWatcher m_fileWatcher;
        std::mutex m_reloadMutex;
        std::unordered_map<std::string, std::function<void()>> m_pendingReloads;
        std::unique_ptr<NRI::ShaderCompiler> m_shaderCompiler = nullptr;
        std::unique_ptr<NRI::Pipeline> m_unlitPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_transparentLitPipeline = nullptr;
        // The same fragments into the order-independent transparency targets (accumulation, revealage, entity IDs).
        std::unique_ptr<NRI::Pipeline> m_unlitOITPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_transparentLitOITPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_oitCompositePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_presentPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_computePipeline = nullptr;

        // Visability
        std::unique_ptr<NRI::Pipeline> m_visibilityPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_computeSkinningPipeline = nullptr;
        // G-Buffer
        std::unique_ptr<NRI::Pipeline> m_gbufferPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_gbufferPipelineNoVelocity = nullptr;
        // PBR
        std::unique_ptr<NRI::Pipeline> m_deferredLightingPipeline = nullptr;
        // SSAO: mutually exclusive with a future RTAO (both would only ever feed the same occlusion slot -- see
        // PushConstantSSAO's comment in shaderIO.h), so this is a plain toggle, not an enum with only one real member.
        std::unique_ptr<NRI::Pipeline> m_ssaoPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ssaoBlurPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_rtaoPipeline = nullptr;
        std::unique_ptr<NRI::Buffer> m_ssaoKernelBuffer; // shared: both SSAO and RTAO sample the same hemisphere kernel
        bool m_ssaoEnabled = true;
        bool m_rtaoEnabled = false;
        int m_ssaoKernelSize = 16;
        static constexpr int RTAORaysPerPixel = 4;
        // Renderer is constructed before Project::Load sets WorldUnits. Keep
        // physical lengths in meters and convert when filling each frame's data.
        float m_ssaoRadiusMeters = 0.5f;
        float m_ssaoBiasMeters = 0.025f;
        float m_ssaoPower = 1.5f;
        // Post Process
        std::unique_ptr<NRI::Pipeline> m_postProcessPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_autoExposureBuildPipeline;
        std::unique_ptr<NRI::Pipeline> m_autoExposureReducePipeline;
        std::unique_ptr<NRI::Pipeline> m_autoExposureAdaptPipeline;
        std::chrono::steady_clock::time_point m_autoExposureLastTime{};
        uint64_t m_autoExposureLastFrame = 0;
        // ColorPyramid.slang: the refraction source of glass (copy, downsample).
        std::unique_ptr<NRI::Pipeline> m_colorPyramidCopyPipeline;
        // Particles (SubmitParticles): this frame's, uploaded into a host-visible buffer per frame in flight.
        struct ParticleBatch
        {
            ParticleBatchSettings Settings;
            uint32_t First = 0;
            uint32_t Count = 0;
            float Distance = 0.0f; // of its center from the camera: emitters draw far to near
        };
        std::vector<shaderio::GpuParticle> m_particles;
        std::vector<ParticleBatch> m_particleBatches;
        std::vector<std::unique_ptr<NRI::Buffer>> m_particleBuffers;
        std::vector<void*> m_particleBuffersMapped;
        uint64_t m_particleBufferCapacity = 0;
        std::unique_ptr<NRI::Pipeline> m_particlePipeline;
        std::unique_ptr<NRI::Pipeline> m_particleOITPipeline; // alpha-blended particles into the OIT targets
        std::unique_ptr<NRI::Pipeline> m_particleMotionPipeline; // their motion into the velocity target (DLSS / TAA)
        std::unique_ptr<NRI::Pipeline> m_colorPyramidDownsamplePipeline;
        bool m_refractionEnabled = true;
        bool m_oitEnabled = false;
        bool m_wireframeOverlay = false;
        std::unique_ptr<NRI::Pipeline> m_wireframePipeline;
        bool m_rtRefractionEnabled = true;
        // NRD
        std::unique_ptr<NRI::Pipeline> m_shadowMaskPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_nrdGuidesPipeline = nullptr; // NRD view Z + normal/roughness (NRDGuides.slang)
        std::unique_ptr<NRI::Pipeline> m_reflectionPipeline = nullptr;
        // Path Tracer
        std::unique_ptr<NRI::Pipeline> m_pathTracerPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ptEnvironmentBuildPipeline;
        std::unique_ptr<NRI::Pipeline> m_ptEnvironmentReducePipeline;
        std::unique_ptr<NRI::Buffer> m_pathTracerSBTBuffer = nullptr;
        NRI::ShaderTableState m_pathTracerSBTState{};
        // DDGI
        std::unique_ptr<NRI::Pipeline> m_ddgiRadiancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiBlendIrradiancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiBlendDistancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiRelocationPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiDebugSpheresPipeline = nullptr;
        // RT results at a lower ray tracing resolution back to the render resolution: RGBA16F (reflections), RG16F (shadow mask),
        // R16F (RTAO).
        std::array<std::unique_ptr<NRI::Pipeline>, 3> m_rtUpsamplePipelines{};
        uint32_t m_rtSubsample = 1;
        // At a lower ray tracing resolution, reflections on surfaces smoother than this are traced again at full resolution.
        static constexpr float RTFullResolutionSmoothRoughness = 0.35f;
        uint32_t m_shadowRayLightLimit = 0;
        // Screen-probe GI
        std::unique_ptr<NRI::Pipeline> m_screenProbePlacePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_screenProbeTracePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_screenProbeIrradiancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_screenProbeFilterPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_screenProbeIntegratePipeline = nullptr;

        std::unique_ptr<NRI::CommandAllocator> m_commandAllocator = nullptr; // single-time commands

        // Render graph + the frame state its passes share. Resolved in prepareFrameGraph (and by passes that run
        // earlier in the same frame, e.g. denoiser results consumed by lighting).
        struct FrameGraphState
        {
            uint32_t imageIndex = 0;
            NRI::Extent2D renderExtent{};  // what DLSS upscales from
            NRI::Extent2D outputExtent{};  // editor viewport / swapchain
            shaderio::PushConstantMeshlets meshletReferences{};
            bool hiZReset = false; // the pyramid has no usable previous frame: no occlusion test
            bool resetNRD = false;
            bool resetDLSS = false;
            bool runPathTracer = false;
            bool pathTracerUsesRTXDI = false;
            uint32_t totalDDGIProbes = 0;
            uint32_t ddgiReadIndex = 0;
            uint32_t ddgiWriteIndex = 0;
            bool ddgiFirstFrame = false;
            uint32_t restirDIBufferA = 0;
            uint32_t restirDIBufferB = 0;
            uint32_t restirDIBufferC = 0;
            uint32_t ptInitialOutputBuffer = 0;
            uint32_t ptInitialPreservedBuffer = 0;
            uint32_t ptTemporalInputBuffer = 0;
            uint32_t ptFinalShadingInputBuffer = 0;
            bool restirPTActive = false;      // ReSTIR PT lighting path taken this frame
            bool pathTracerActive = false;    // plain path tracer lighting path taken this frame
            bool restirPTTemporalActive = false;
            bool restirPTCameraMoved = false;
            bool pathTracerCameraMoved = false;
            bool pathTracerAccumulate = false;
            uint32_t pathTracerReadIndex = 0;
            uint32_t pathTracerWriteIndex = 0;
            // Which passes were added this frame: consumers declare (and bind) only outputs that are produced.
            bool rtShadowsAdded = false;
            bool nrdShadowsAdded = false;
            bool rtReflectionsAdded = false;
            bool nrdReflectionsAdded = false;
            bool ddgiAdded = false;
            bool screenProbeGIAdded = false;
            uint32_t screenProbeGIWriteIndex = 0; // the result this frame writes (resources.ScreenProbeGI)
            bool restirGIAdded = false;
            bool nrdGIAdded = false;
            bool restirDIAdded = false;
            // RTXDI light presampling of this frame (addLightPresamplingPasses): ReSTIR DI and ReSTIR GI sample from it.
            bool lightPresamplingAdded = false;
            uint32_t risBufferOffset = 0; // where the ReGIR cells start in the RIS buffer
            bool regirActive = false;
            bool nrdDIAdded = false;
            bool ptNRDAdded = false;
            bool deferredLightingAdded = false;
            bool oitTargetsCreated = false; // resources.OITAccum / OITReveal exist this frame
            bool oitWritten = false;        // something was drawn into them (the next writer loads, the composite runs)
            bool occlusionRawAdded = false; // SSAO or RTAO wrote resources.SSAORaw this frame (never both)
            bool ssaoAdded = false;         // resources.SSAO (the blurred result) is valid this frame
            bool dlssAdded = false;
            // DLSS Ray Reconstruction is given the lit scene before any transparent surface (resources.RRColorBeforeTransparency).
            bool rrColorBeforeTransparency = false;
            bool taaAdded = false;      // resources.TAAHistory[taaWriteIndex] holds this frame's anti-aliased scene
            uint32_t taaWriteIndex = 0;
            bool gbufferVelocityWritten = false;
            bool computeSkinningAdded = false;
            bool skinnedHistoryValid = false;
            uint32_t skinningWorkCount = 0;
            bool animatedBLASAdded = false;
            bool inspectionProbe = false;
        };
        RenderGraph m_renderGraph;
        FrameGraphState m_frame;

        // History keys: histories that live outside the graph (NRD, DLSS/NGX) are reset through the same keys.
        static constexpr const char* NRDHistoryKey = "NRD";
        static constexpr const char* DLSSHistoryKey = "DLSS";
        static constexpr const char* DDGIHistoryKey = "DDGI";
        static constexpr const char* ScreenProbeHistoryKey = "Screen Probe GI"; // also the history texture's key
        // DDGI atlas layout: probes per atlas row (atlas sizes, uniforms and the blend shaders agree on it).
        static constexpr uint32_t DDGIProbesPerRow = 64;

        //decsriptor
        std::unique_ptr<NRI::DescriptorHeap> m_samplerHeap = nullptr;
        std::unique_ptr<NRI::DescriptorHeap> m_resourceHeap = nullptr;

        // Scene Data + Frustum Freeze
        std::vector<std::unique_ptr<NRI::Buffer>> m_uniformBuffers;
        std::vector<void*> m_uniformBuffersMapped;
        shaderio::UniformBufferObject uniformData = {};
        shaderio::UniformBufferObject frozenUniformData = {};
        bool m_frozen = false;
        bool m_frozenDone = false;

        // EditorLayer Settings
        uint32_t m_debugMode = 0;
        uint32_t m_tonemapMode = 8; // RTXPT SampleUI explicitly defaults to Hable UC2
        float m_viewportExposureCompensation = 0.0f;
        float m_manualEV100 = 2.0f; // DefaultManualEV100 (Components.h)
        bool m_autoExposure = false;
        bool m_viewportAutoExposure = false;
        float m_exposureCompensation = 0.0f;
        float m_autoExposureMinEV = -16.0f;
        float m_autoExposureMaxEV = 16.0f;
        float m_gamma = 2.2f;
        float m_scaleIBLAmbient = 1.0f;

        // Visability

        // G-Buffer Render Targets (Decoupled Material Pass)

        // NRD
        bool m_nrdShadowsEnabled = true;
        NRI::NRDReflectionDenoiser m_nrdReflectionDenoiser = NRI::NRDReflectionDenoiser::Off;

        // DDGI (Dynamic Diffuse Global Illumination)
        uint32_t m_ddgiHistoryIndex = 0;

        bool m_ddgiEnabled = false;
        glm::vec3 m_ddgiGridOrigin = glm::vec3(-20.0f, -0.5f, -12.0f);
        glm::vec3 m_ddgiGridSpacing = glm::vec3(1.8f, 1.4f, 1.7f);
        uint32_t m_ddgiProbeCountX = 20;
        uint32_t m_ddgiProbeCountY = 12;
        uint32_t m_ddgiProbeCountZ = 20;
        uint32_t m_ddgiRaysPerProbe = 128;
        float m_ddgiHysteresis = 0.97f;
        bool m_ddgiRelocation = true;
        float m_ddgiNormalBias = 0.2f;
        float m_ddgiDebugSphereRadius = 0.15f; // meters
        bool m_ddgiDebugXRay = true;
        bool m_ddgiFollowCamera = true;
        glm::vec3 m_ddgiSpacingMeters = glm::vec3(2.0f, 2.0f, 2.0f); // the first cascade's
        uint32_t m_ddgiUpdateStride = 2;
        uint32_t m_ddgiCascadeCount = 3;
        float m_ddgiCascadeScale = 3.0f;
        // Per cascade (camera-following): grid coordinate of its first probe, the spacing that is in, and whether it is placed yet.
        std::array<glm::ivec3, 4> m_ddgiCascadeGridMin{};
        std::array<glm::vec3, 4> m_ddgiCascadeSpacing{};
        uint32_t m_ddgiPlacedCascades = 0;

        // ReSTIR GI (Screen-Space Diffuse Path Resampling via RTXDI) -- proper 3-pass pipeline
        // (Initial candidate -> Temporal -> Spatial), matching RTXPT's actual architecture instead
        // of the fused SpatioTemporal SDK function (see PushConstantReSTIRGITemporal comment).
        std::unique_ptr<NRI::Pipeline> m_restirGIInitialPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirGITemporalPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirGISpatialPipeline = nullptr;
        std::unique_ptr<NRI::Buffer> m_restirGINeighborOffsetsBuffer;

        uint32_t m_diffuseGIMode = 0; // 0 = Off (IBL), 1 = DDGI, 2 = ReSTIR GI, 3 = screen probes
        // Screen-probe GI
        uint32_t m_screenProbeTileSize = 16;
        uint32_t m_screenProbeFilterPasses = 2;
        uint32_t m_screenProbeMaxHistory = 24;
        uint32_t m_screenProbeHistoryIndex = 0; // which of the two results is last frame's
        uint64_t m_screenProbeLastFrame = ~0ull; // scene frame it last ran (its history is only good for the next one)
        float m_restirGISpatialRadius = 32.0f;
        uint32_t m_restirGINumSpatialSamples = 2;
        uint32_t m_restirGIMaxHistoryLength = 10;
        float m_restirGINormalThreshold = 0.6f;
        float m_restirGIDepthThreshold = 0.1f;
        bool m_restirGIEnableBoilingFilter = true;
        float m_restirGIBoilingFilterStrength = 0.35f;
        NRI::NRDDiffuseDenoiser m_nrdGIDenoiser = NRI::NRDDiffuseDenoiser::Off;

        // ReSTIR DI (Screen-Space Resampled Direct Lighting via RTXDI) -- Initial -> Temporal ->
        // Spatial -> FinalShading, 4 passes matching RTXPT's own DI architecture. Reservoir buffer
        // rotation is NOT GI's fixed scratch/persistent trick -- RTXDI's own ReSTIRDIContext rotates
        // through 3 physical buffers every frame (see PushConstantReSTIRDIInitial comment in
        // shaderIO.h for the exact rotation math); using only 2 here would reintroduce the read/write
        // race already found and fixed once for GI's spatial pass.
        std::unique_ptr<NRI::Pipeline> m_restirDIInitialPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirDITemporalPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirDISpatialPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirDIFinalShadingPipeline = nullptr;
        NRI::NRDDiffuseDenoiser m_nrdDIDenoiser = NRI::NRDDiffuseDenoiser::Off;
        uint32_t m_restirDILastFrameOutputReservoir = 0;

        // Local (point/spot) lights and infinite (directional) lights must occupy separate
        // contiguous regions of the uploaded light buffer for RTXDI_LightBufferRegion -- computed
        // each frame by partitioning m_lightBufferObjects before upload (see updateUniformBuffer).
        uint32_t m_restirDIFirstLocalLight = 0;
        uint32_t m_restirDINumLocalLights = 0;
        uint32_t m_restirDIFirstInfiniteLight = 0;
        // The first directional light: the one the RT shadow pass (NRD SIGMA) is for, ~0u for none (updateLightBuffer).
        uint32_t m_shadowMaskLightIndex = ~0u;
        glm::vec3 m_shadowMaskLightDirection{ 0.0f, 1.0f, 0.0f };
        uint32_t m_restirDINumInfiniteLights = 0;

        // Defaults match RTXPT's own RtxdiApplicationSettings.cpp (getReSTIRDIInitialSamplingParams/
        // getReSTIRDITemporalResamplingParams/getReSTIRDISpatialResamplingParams) exactly, except
        // where we deliberately deviate (noted at each dispatch site): BASIC bias correction instead
        // of Raytraced (avoids extra shadow rays per neighbor/per history sample), and no initial
        // visibility ray yet (RTXPT's enableInitialVisibility = true).
        uint32_t m_directLightingMode = 0; // 0 = brute-force analytic loop, 1 = ReSTIR DI
        uint32_t m_restirDINumLocalLightSamples = 2;
        uint32_t m_restirDINumInfiniteLightSamples = 1;
        uint32_t m_restirDIMaxHistoryLength = 20;
        float m_restirDINormalThreshold = 0.5f;
        float m_restirDIDepthThreshold = 0.1f;
        uint32_t m_restirDINumSpatialSamples = 1;
        float m_restirDISpatialRadius = 32.0f;

        // RIS + ReGIR (phase 2 of ReSTIR DI): power-weighted local-light candidates instead of pure
        // uniform selection. Both presample passes are plain 1D compute dispatches (no G-buffer
        // involvement, unlike everything else in this engine's mesh-shader-fullscreen-triangle
        // pattern) -- see createReSTIRDIPipelines. RIS tiles are always built (cheap regardless of
        // light count, and serve as ReGIR's fallback for pixels outside the grid); ReGIR itself is
        // independently toggleable since it's a pure quality/perf layer on top.
        std::unique_ptr<NRI::Pipeline> m_restirDIPresamplePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirDIPresampleReGIRPipeline = nullptr;
        uint32_t m_restirDIRISTileSize = 256;
        uint32_t m_restirDIRISTileCount = 8;

        // Local-light PDF mip chain (Rtxdi/LightSampling/PresamplingFunctions.hlsli's real
        // RTXDI_PresampleLocalLights, not a hand-rolled O(numLights) draw) -- 128x128 supports up to
        // 16384 lights via Z-curve indexing, comfortably covering "thousands of lights" scenes while
        // staying cheap (a handful of small mips). Rebuilt every frame alongside the RIS/ReGIR
        // presample passes, same cadence as the rest of ReSTIR DI's per-frame light data.
        std::unique_ptr<NRI::Pipeline> m_restirDIWriteLightPDFPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirDIReduceLightPDFMipPipeline = nullptr;
        uint32_t m_lightPDFTextureSize = 128;
        uint32_t m_lightPDFMipLevels = 0; // == log2(m_lightPDFTextureSize); RTXDI_SamplePdfMipmap only
                                            // ever descends to a 2x2 mip, not all the way to 1x1

        bool m_regirEnabled = true;
        // Grid bounds informed by this scene's existing DDGI probe grid (m_ddgiGridOrigin/Spacing/
        // ProbeCount*), just generously padded -- ReGIR only needs to roughly cover where local lights
        // and camera-visible surfaces actually are, not be pixel-perfect.
        // NOTE: these defaults must roughly match whatever scene is loaded -- ReGIR's whole benefit is
        // localizing the light candidate list to a small region of world space, so the cell size needs
        // to be small relative to the scene's actual extent (a handful of meters for a small interior
        // like ShadowTest.nox, not Sponza's tens-of-meters scale). Too coarse and every surface in the
        // scene collapses into the same 1-2 cells, which shows up as flicker (cells are rebuilt with
        // fresh RNG every frame, so a near-tie between lights changes winner frame to frame) and no
        // noise reduction (no real spatial localization is happening). Retune via the editor's ReGIR
        // "Cell Size" / "Grid Center" controls to match whatever scene is actually loaded.
        glm::vec3 m_regirGridCenter = glm::vec3(0.0f, 0.5f, -0.5f);
        float m_regirCellSize = 0.5f;
        uint32_t m_regirCellsX = 16;
        uint32_t m_regirCellsY = 16;
        uint32_t m_regirCellsZ = 16;
        uint32_t m_regirLightsPerCell = 8;
        // 0 = fully static cell assignment (no per-frame randomness -- stable, but visible hard edges
        // right at grid cell boundaries), 1 = RTXPT's default full +/-0.5 cell jitter (diffuses that
        // discretization error across frames/pixels, but needs either heavy temporal accumulation or a
        // LOT of lights before the extra per-frame randomness pays for itself instead of just reading as
        // instability -- default lower than RTXPT's for small scenes with only a handful of lights).
        float m_regirSamplingJitter = 0.25f;
        // RTXPT's default: each cell slot combines this many weighted draws via streaming RIS
        // (RTXDI_PresampleLocalLightsForReGIR), rather than a single full-light-list pass.
        uint32_t m_regirNumBuildSamples = 8;

        // Path Tracer Accumulation Ping-Pong
        uint32_t m_pathTracerSampleCount = 0;
        glm::mat4 m_pathTracerPrevView = glm::mat4(1.0f);
        bool m_pathTracerUsesRTXDI = false; // RTXPT-style mode: run ReSTIR DI/GI alongside the plain path tracer

        // Path Tracer NRD Denoising (fallback for hardware/preference without DLSS Ray Reconstruction --
        // mutually exclusive with it, same as GI/DI/reflections; see setNRDPTDenoiser)
        NRI::NRDDiffuseDenoiser m_nrdPTDenoiser = NRI::NRDDiffuseDenoiser::Off;
        std::unique_ptr<NRI::Pipeline> m_ycocgDecodePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ptCompositePipeline = nullptr; // path tracer NRD composite (PTComposite.slang)

        // ReSTIR PT (Screen-Space Path Resampling via RTXDI) -- reuses the REAL rtxdi::ReSTIRPTContext
        // C++ class (Rtxdi/PT/ReSTIRPT.h) directly for buffer-index rotation and default parameters,
        // rather than hand-rolling an equivalent copy the way DI/GI's state is tracked: Source/ReSTIRPT.cpp
        // is already compiled into the build (CMakeLists.txt globs every vendors/RTXDI/Source/*.cpp), so
        // there's no reason to re-derive its switch-case buffer-index logic by hand and risk drifting
        // from the real thing.
        bool m_restirPTEnabled = false; // only meaningful while Path Tracing is active
        bool m_restirPTTemporalEnabled = false; // see getReSTIRPTTemporalEnabled's comment
        std::unique_ptr<rtxdi::ReSTIRPTContext> m_restirPTContext; // created lazily once render size is known
        std::unique_ptr<NRI::Pipeline> m_restirPTInitialPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirPTTemporalPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirPTFinalShadingPipeline = nullptr;
        // Numeric defaults match RTXPT's own GetDefaultReSTIRPT*Params() (see ReSTIRPT.cpp)
        uint32_t m_restirPTNumInitialSamples = 1;
        uint32_t m_restirPTMaxBounceDepth = 3;
        uint32_t m_restirPTMaxRcVertexLength = 5;
        uint32_t m_restirPTNumNeeSamples = 1;
        float m_restirPTRoughnessThreshold = 0.1f;
        float m_restirPTDistanceThreshold = 0.0f;
        // Temporal resampling tunables -- same defaults already established for ReSTIR GI's own
        // temporal pass (m_restirGiMaxHistoryLength/NormalThreshold/DepthThreshold), reused here since
        // they represent the same kind of reprojection validity check.
        uint32_t m_restirPTMaxHistoryLength = 8;
        uint32_t m_restirPTMaxReservoirAge = 30; // RTXDI_PTRESERVOIR_AGE_MAX caps this at 31
        float m_restirPTNormalThreshold = 0.6f;
        float m_restirPTDepthThreshold = 0.1f;
        bool m_restirPTEnablePermutationSampling = false;

        // Post Process

        Ref<Texture2D> m_whiteTexture;
        Ref<Texture2D> m_sceneResource;

        struct ExtraView
        {
            bool Submitted = false; // this frame (SubmitExtraView); cleared once the frame is recorded
            bool Recorded = false;  // its passes are in this frame's graph
            glm::mat4 View{ 1.0f };
            glm::mat4 Projection{ 1.0f };
            glm::vec3 Position{ 0.0f };
            float NearClip = 0.01f;
            bool Orthographic = false;
            NRI::Extent2D Size{ 0, 0 };
            bool HasGrid = false;
            bool Preview = false; // an asset preview: no shadow rays (the scene is above its stage), no DDGI, the uniforms' exposure
            uint32_t Flags = 0;   // ExtraViewFlags
            shaderio::PushConstantGrid Grid{};
            Ref<Texture2D> Output; // LDR (Surface format) this frame draws into
            Ref<Texture2D> Shown;  // the last texture drawn completely: what the viewport window shows
            std::vector<std::unique_ptr<NRI::Buffer>> UniformBuffers; // per frame slot, persistently mapped
            std::vector<void*> UniformBuffersMapped;
            std::vector<std::unique_ptr<NRI::Buffer>> CullViewBuffers;
        };
        std::array<ExtraView, MaxExtraViews> m_extraViews;
        // Images taken this frame (TakeExtraViewTexture): the present pass reads them once more, so the graph orders their last write
        // before ImGui samples them from now on.
        std::vector<Ref<Texture2D>> m_takenExtraViewImages;

        // Entity ID readback (entity IDs themselves are render graph textures, see Passes/FrameGraphResources.h)
        bool m_orthographic = false; // the editor camera is in an ortho view (no jitter, no upscaling, full mesh detail)
        std::vector<std::unique_ptr<NRI::Buffer>> m_pickerStagingBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_pickerDepthStagingBuffers;
        std::vector<PickRequest> m_pickerReadbackRequests;
        // ReadbackTexture: waiting (FrameSlot UINT32_MAX) or copied in the frame of that slot.
        struct TextureReadback
        {
            Ref<Texture2D> Texture;
            std::unique_ptr<NRI::Buffer> Staging;
            TextureReadbackCallback Done;
            uint32_t Width = 0;
            uint32_t Height = 0;
            uint32_t FrameSlot = UINT32_MAX;
        };
        std::vector<TextureReadback> m_textureReadbacks;
        PickRequest m_pickRequest;
        PickResult m_pickResult;
        // RequestRaycast: waiting, and in flight per frame slot (its rays are in that slot's buffer).
        // The origin in the world (double precision): made relative to the render origin when it goes to the GPU (it may have moved).
        struct PendingRaycast { shaderio::EditorRay Ray; glm::dvec3 Origin{ 0.0 }; RaycastCallback Callback; };
        std::vector<PendingRaycast> m_raycastQueue;
        std::vector<std::vector<RaycastCallback>> m_raycastsInFlight;
        std::vector<std::unique_ptr<NRI::Buffer>> m_raycastBuffers;
        std::unique_ptr<NRI::Pipeline> m_editorRaycastPipeline;
        std::vector<int32_t> m_SelectedEntityIDs;

        // Render graph texture inspection (editor)
        std::unique_ptr<NRI::Pipeline> m_textureInspectPipeline = nullptr;
        std::array<std::unique_ptr<NRI::Pipeline>, 5> m_instanceCullingPipelines; // cull, count, offset, write, late
        std::unique_ptr<NRI::Pipeline> m_hiZBuildPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_rrGuidesPipeline = nullptr; // DLSS Ray Reconstruction albedo guides
        std::unique_ptr<NRI::Pipeline> m_taaPipeline = nullptr;      // TAA.slang
        bool m_taaEnabled = true;
        uint32_t m_taaHistoryIndex = 0; // the history the next TAA pass reads
        bool m_taaRanLastFrame = false; // its history is last frame's (else it starts over)
        TextureInspection m_textureInspection;
        Texture2D* m_inspectionImage = nullptr;
        RGTextureKey m_inspectedTextureKey;
        std::vector<std::unique_ptr<NRI::Buffer>> m_inspectionProbeBuffers; // per frame slot, 1x1 RGBA32F readback
        std::vector<uint8_t> m_inspectionProbePending;                     // per frame slot
        glm::vec4 m_inspectionProbeValue{ 0.0f };
        bool m_inspectionProbeValid = false;

        // Textures
        Ref<Texture2D> m_textureResource;
        Ref<Texture2D> m_textureResource2;
        Ref<Texture2D> m_textureResource3;
        uint32_t mipLevels;

        // Animations
        std::vector<glm::mat4> m_boneMatrices;
        // This frame's morph target weights (per deformed instance with morph targets: one per target), uploaded like the bones.
        std::vector<float> m_morphWeights;
        std::vector<std::unique_ptr<NRI::Buffer>> m_morphWeightBuffers;
        std::vector<void*> m_morphWeightBuffersMapped;
        uint64_t m_morphWeightBufferCapacity = 0;
        std::vector<std::unique_ptr<NRI::Buffer>> m_boneBuffers;
        std::vector<void*> m_boneBuffersMapped;
        uint64_t m_BoneBufferCapacity = 0;

        struct SkinningLayoutEntry
        {
            uint32_t instanceSlot = 0;
            uint32_t sourceVertexOffset = 0;
            uint32_t destinationVertexOffset = 0;
            uint32_t vertexCount = 0;
            bool operator==(const SkinningLayoutEntry&) const = default;
        };
        std::vector<shaderio::SkinningWorkItem> m_skinningWorkItems;
        std::vector<SkinningLayoutEntry> m_skinningLayout;
        std::vector<std::unique_ptr<NRI::Buffer>> m_skinnedVertexBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_skinningWorkItemBuffers;
        std::vector<void*> m_skinningWorkItemBuffersMapped;
        uint64_t m_skinnedVertexBufferCapacity = 0;
        uint64_t m_skinningWorkItemBufferCapacity = 0;
        bool m_hasSkinnedVertexHistory = false;
        bool m_skinnedVertexHistoryValid = false;

        void updateBoneBuffer(uint32_t currentImage);
        void updateParticleBuffer(uint32_t currentImage);
        void createBoneBuffer(uint64_t size);
        void createMorphWeightBuffers(uint64_t size);
        void updateMorphWeightBuffer(uint32_t currentImage);
        void createSkinningBuffers(uint64_t skinnedVertexSize, uint64_t workItemSize);

        // Meshes
        // Buffers, assets and geometry ranges wait here until no frame in flight can reference them (§5.8.4).
        // Guarded: an asset destructor queues its geometry, and the last reference to an asset can be dropped on any
        // thread (loader tasks included), while the main thread drains at the frame sync point.
        std::vector<DeferredRelease> m_deferredReleases;
        std::mutex m_deferredReleasesMutex;

        // Unified geometry streams (§5.8.2): one buffer each, sub-allocated per mesh, addressed by offset.
        GeometryArena m_vertexStream; // shaderio::PackedVertex
        // Bone influences (shaderio::SkinVertex) of skinned meshes, at their vertices' own element ranges (GeometryArena::Reserve).
        GeometryArena m_skinStream;
        GeometryArena m_colorStream; // packed vertex colors, mirroring the vertex stream (only meshes with colors reserve their range)
        GeometryArena m_morphRangeStream; // GpuMorphRange per vertex of a mesh with morph targets, mirroring the vertex stream
        GeometryArena m_morphDeltaStream; // GpuMorphDelta, a range per mesh with morph targets
        GeometryArena m_meshletDrawStream;
        GeometryArena m_meshletBoundsStream;
        GeometryArena m_meshletVertexStream;
        GeometryArena m_meshletTriangleStream;
        GeometryArena m_rtIndexStream;
        MemoryBudget m_memoryBudget;
        std::vector<MeshBLAS> m_meshBLASes;
        std::vector<uint32_t> m_freeBLASIds;
        // What builds each BLAS again after an eviction (by BLAS id; ranges, not addresses: streams can grow).
        struct BlasSource
        {
            uint32_t meshSlot = UINT32_MAX;
            GeometryRange vertices;
            GeometryRange indices;
            bool isOpaque = true;
            bool valid = false;   // the mesh is loaded
            bool evicted = false; // its BLAS is freed until a traced instance needs it
            uint32_t streamingId = UINT32_MAX; // streamed geometry (§5.7.2): built from its leaf cut instead
        };
        std::vector<BlasSource> m_blasSources;
        uint32_t m_evictedBlasCount = 0;
        uint64_t m_memorySampleCount = 0;   // updateMemoryBudget calls
        uint64_t m_blasEvictionSample = ~0ull; // the sample the last eviction pass judged (one pass per fresh measurement)
        static constexpr uint64_t BlasEvictionFrames = 300;
        bool m_rtCullingEnabled = true;
        bool m_asyncComputeEnabled = true;
        float m_rtCullRadiusMeters = 300.0f;
        float m_rtCullAngleDegrees = 1.0f;

        struct AnimatedBLAS
        {
            MeshBLAS Resource;
            uint32_t meshSlot = UINT32_MAX;
            uint32_t destinationVertexOffset = 0;
            uint32_t vertexCount = 0;
            uint32_t indexOffset = 0;
            uint32_t triangleCount = 0;
            uint64_t buildScratchSize = 0;
            uint64_t updateScratchSize = 0;
            bool isOpaque = true;
            bool built = false;
            bool active = false;
        };
        struct AnimatedBLASUpdate
        {
            uint32_t instanceSlot = UINT32_MAX;
            NRI::AccelerationStructureBuildDesc desc;
            bool rebuild = false;
        };
        std::vector<AnimatedBLAS> m_animatedBLASes; // indexed by GPU scene instance slot
        std::vector<AnimatedBLASUpdate> m_animatedBLASUpdates;
        std::unique_ptr<NRI::Buffer> m_animatedBLASScratch;
        uint64_t m_animatedBLASScratchCapacity = 0;

        // Uploads run on the transfer queue (§5.11.4); frames wait on the GPU for what they read.
        UploadManager m_uploads;

        // A mesh draws once its geometry is copied and is ray traced once its BLAS is built: builds wait here and run
        // in frames under a primitive budget, on the graphics queue (transfer queues cannot build acceleration
        // structures). Ranges, not addresses: a stream can grow before the build is recorded.
        struct BlasBuild
        {
            uint32_t blasId = UINT32_MAX;
            uint32_t meshSlot = UINT32_MAX;
            GeometryRange vertices;
            GeometryRange indices;
            bool isOpaque = true;
            // Streamed geometry (§5.7.2): the leaf cut is taken when the build is recorded (GeometryStreamer::PrepareBlasBuild), into a
            // new structure; its indices address the whole vertex stream.
            uint32_t streamingId = UINT32_MAX;
            bool absolute = false; // indices into the whole vertex stream (vertices unused)
            uint32_t maxVertex = 0; // absolute: the highest vertex the indices reach
        };
        std::vector<BlasBuild> m_blasBuilds;
        std::unique_ptr<NRI::Buffer> m_blasScratch; // shared by the builds of one frame, recorded one after another
        // Bounds each frame's build work while a large load streams in (one submission building thousands of BLAS
        // could exceed the driver timeout). 2M made a 42 ms GPU frame on Bistro (RTX, Release); a quarter keeps a
        // loading frame near a normal one and Bistro still ray traces within ~15 frames.
        static constexpr uint64_t BlasBuildPrimitivesPerFrame = 500'000;

        // BLAS compaction (§5.14, NVIDIA RT best practices): a frame's builds write their compacted sizes into its slot's
        // query pool; once the slot is done the compacted structures are created, and the next BLAS pass copies each BLAS
        // into its compacted one and hands the old one to the deferred release.
        struct BlasCompactionQuery
        {
            uint32_t blasId = UINT32_MAX;
            uint32_t meshSlot = UINT32_MAX;
            uint64_t serial = 0;
        };
        struct BlasCompaction
        {
            uint32_t blasId = UINT32_MAX;
            uint32_t meshSlot = UINT32_MAX;
            MeshBLAS compacted;
        };
        static constexpr uint32_t BlasCompactionQueriesPerFrame = 1024; // also caps the builds of one frame
        std::vector<std::unique_ptr<NRI::QueryPool>> m_blasCompactionQueryPools; // per frame slot
        std::array<std::vector<BlasCompactionQuery>, MAX_FRAMES_IN_FLIGHT> m_blasCompactionQueries;
        std::vector<BlasCompaction> m_blasCompactions;
        uint64_t m_blasSerial = 0;
        NRI::AccelerationStructureBuildDesc blasBuildDesc(const BlasBuild& build) const;
        // --- Hardware Ray Tracing: Scene TLAS ---
        void updateSceneAccelerationStructure(uint32_t currentFrameIndex);
        void BuildSceneAccelerationStructure(NRI::CommandBuffer& cmd);

        bool m_rayTracingEnabled = false;
        bool m_rayTracingShadows = false;
        bool m_rayTracingReflections = false;
        bool m_pathTracingEnabled = false;
        bool m_pathTracingAccumulation = false;

        std::unique_ptr<NRI::AccelerationStructure> m_sceneTLAS;
        std::unique_ptr<NRI::Buffer> m_tlasBuffer;
        std::unique_ptr<NRI::Buffer> m_tlasScratchBuffer;
        std::vector<std::unique_ptr<NRI::Buffer>> m_rtInstanceBuffers; // per frame slot, written when this frame builds
        uint32_t m_sceneTLASCapacity = 0;
        uint32_t m_tlasHeapIndex = 0;
        uint32_t m_tlasHeapSlot = ~0u;
        bool m_hasTLASBuild = false;
        bool m_tlasNeedBuild = true; // sticky until a build is recorded (also while ray tracing is off)
        NRI::AccelerationStructureBuildDesc m_tlasBuildDesc{};

        // GPU scene and the draw list every view culls (instance slots in bucket order, uploaded per frame slot when changed)
        GpuScene m_gpuScene;
        uint64_t m_gpuSceneOwner = 0;
        std::vector<int32_t> m_invalidatedMeshEntities;
        bool m_gpuMaterialsDirty = false;
        std::vector<uint32_t> m_drawList;
        std::array<uint32_t, RenderBucketCount + 1> m_drawBucketStarts{};
        uint64_t m_DrawListBufferCapacity = 0;
        std::vector<std::unique_ptr<NRI::Buffer>> m_drawListBuffers;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> m_drawListStale{};

        // GPU culling: settings, the camera view's cull parameters and its visible counts (read back a frame late), per
        // frame slot
        bool m_instanceCullingEnabled = true;
        bool m_occlusionCullingEnabled = true;
        uint32_t m_meshletCulling = 0;
        std::vector<std::unique_ptr<NRI::Buffer>> m_cullViewBuffers;
        std::vector<std::unique_ptr<NRI::Buffer>> m_cullStatsBuffers;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> m_cullStatsPending{};

        // Texture streaming feedback (§5.12): cleared each frame, written by the G-buffer and transparent passes, read back
        // per frame slot into m_mipFeedback.
        // The mip feedback (MipFeedbackSlots entries) and after it the virtual texture tile requests (VirtualTextureFeedback).
        static constexpr uint64_t MipFeedbackBytes = sizeof(uint32_t) * shaderio::MipFeedbackSlots + sizeof(shaderio::VirtualTextureFeedback);
        std::unique_ptr<NRI::Buffer> m_mipFeedbackBuffer;
        std::vector<std::unique_ptr<NRI::Buffer>> m_mipFeedbackReadback;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> m_mipFeedbackPending{};
        std::vector<uint32_t> m_mipFeedback;
        uint64_t m_mipFeedbackSerial = 0;

        bool m_dlssCameraWarned = false; // addDLSSPass: a degenerate camera was reported (until it is fine again)

        // Cluster LOD (§5.7), and what the visibility passes drew with it: cleared each frame, read back per frame slot.
        float m_lodErrorPixels = 1.0f;
        bool m_lodFullDetail = false;
        std::unique_ptr<NRI::Buffer> m_clusterStatsBuffer;
        std::vector<std::unique_ptr<NRI::Buffer>> m_clusterStatsReadback;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> m_clusterStatsPending{};
        shaderio::ClusterStats m_clusterStats{};

        // Geometry streaming (§5.7.2). The page table (GpuGeometryPage per page of every streamed mesh) is a geometry stream too.
        friend class GeometryStreamer;
        GeometryArena m_geometryPageStream;
        // Changes the streamer makes to the draw stream and the page table, recorded by the frame (addGeometryPagesUpdatePass):
        // gathered here, copied into the frame slot's staging buffer once the slot is free.
        // Virtual textures (§5.12.2) change their page tables and descriptors the same way.
        enum class GeometryPatchTarget : uint8_t { Draws, Pages, VirtualPages, VirtualTextures };
        struct GeometryPatch
        {
            GeometryPatchTarget Target = GeometryPatchTarget::Draws;
            uint64_t Destination = 0; // bytes into the target buffer
            uint64_t Source = 0;      // bytes into m_geometryPatchData
            uint64_t Size = 0;
        };
        void stageGeometryPatch(GeometryPatchTarget target, uint64_t destination, const void* data, uint64_t size);
        // Into the frame slot's staging buffer (after its previous submission finished).
        void prepareGeometryPatches(uint32_t frameSlot);
        std::vector<uint8_t> m_geometryPatchData;
        std::vector<GeometryPatch> m_geometryPatches;
        std::array<std::unique_ptr<NRI::Buffer>, MAX_FRAMES_IN_FLIGHT> m_geometryPatchStaging;
        std::array<std::vector<NRI::BufferCopyRegion>, MAX_FRAMES_IN_FLIGHT> m_geometryDrawPatchRegions;
        std::array<std::vector<NRI::BufferCopyRegion>, MAX_FRAMES_IN_FLIGHT> m_geometryPagePatchRegions;
        std::array<std::vector<NRI::BufferCopyRegion>, MAX_FRAMES_IN_FLIGHT> m_virtualPagePatchRegions;
        std::array<std::vector<NRI::BufferCopyRegion>, MAX_FRAMES_IN_FLIGHT> m_virtualTexturePatchRegions;
        std::unique_ptr<NRI::Buffer> m_geometryFeedbackBuffer;
        std::vector<std::unique_ptr<NRI::Buffer>> m_geometryFeedbackReadback;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> m_geometryFeedbackPending{};
        std::unique_ptr<NRI::Pipeline> m_geometryFeedbackPipeline;
        std::unique_ptr<NRI::Pipeline> m_clusterIndicesPipeline;
        // Per frame slot: the cluster index jobs of the streamed BLAS builds recorded in it.
        std::array<std::unique_ptr<NRI::Buffer>, MAX_FRAMES_IN_FLIGHT> m_clusterIndexJobBuffers;
        GeometryStreamer m_geometryStreamer{ *this };

        // Streaming virtual textures (§5.12.2): the page table (uint per entry of every virtual texture), the frame each entry was last
        // requested (written by the feedback), and a GpuVirtualTexture per material texture slot. Made with the first virtual texture.
        friend class VirtualTextureStreamer;
        std::unique_ptr<NRI::Buffer> m_virtualPageTable;
        std::unique_ptr<NRI::Buffer> m_virtualTileStamps;
        std::unique_ptr<NRI::Buffer> m_virtualTextureTable;
        VirtualTextureStreamer m_virtualTextures{ *this };
        uint32_t m_visibleInstanceCount = 0;
        uint32_t m_lateCandidateCount = 0;
        uint32_t m_lateDrawnCount = 0;
        uint32_t m_visibleTriangleCount = 0;

        uint32_t frameIndex = 0;

        bool framebufferResized = false;
        // Debounces OS window-resize-driven swapchain recreation the same way m_viewportResizePending
        // debounces the editor's viewport panel (see that member's comment) -- SDL fires a resize event
        // per pixel during a live window-border drag, and recreateSwapChain() rebuilds the whole
        // G-buffer/ShadowMask/NRD-reinit/DDGI/ReSTIR resource stack, so reacting to every event tanks
        // perf. present() returning ResizeRequired still forces an immediate recreate regardless of this
        // timer -- this only debounces the proactive path driven by resizeWindow()'s SDL event handler.
        std::chrono::steady_clock::time_point m_lastWindowResizeRequestTime{};

        // Outline
        std::vector<std::unique_ptr<NRI::Buffer>> m_selectedEntityIDBuffers;
        std::vector<void*> m_selectedEntityIDBuffersMapped;
        std::unique_ptr<NRI::Pipeline> m_outlinePipeline = nullptr;
        // Selection mask (SelectionMask.slang): the selected instances' GPU scene slots for this frame slot (the task stage
        // reads them as its draw list) and one direct draw per slot.
        std::unique_ptr<NRI::Pipeline> m_selectionMaskPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_stableEntityDepthPipeline = nullptr; // EditorStableDepth.slang
        std::vector<std::unique_ptr<NRI::Buffer>> m_selectionInstanceBuffers;
        std::vector<void*> m_selectionInstanceBuffersMapped;
        std::vector<uint32_t> m_selectionTaskGroups; // per selected instance: task workgroups to cover its meshlets

        void createOutlinePipeline(bool forceCompile = false);
        void createSelectionMaskPipeline(bool forceCompile = false);
        void createStableEntityDepthPipeline(bool forceCompile = false);

        // PBR stuff
        Ref<Texture2D> m_environmentCubemap;
        std::filesystem::path m_environmentPath;
        float m_environmentRotation = 0.0f;
        // A new angle for the same map waits until it holds still (dragging it would reload and refilter every frame).
        bool m_environmentRotationPending = false;
        float m_pendingEnvironmentRotation = 0.0f;
        std::chrono::steady_clock::time_point m_environmentRotationChanged{};
        std::unique_ptr<NRI::Pipeline> m_skyboxPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_lightShapesPipeline = nullptr; // LightShapes.slang: the rect lights' faces
        uint32_t m_rectLightCount = 0; // in this frame's light buffer
        Ref<Texture2D> m_irradianceCubemap;
        Ref<Texture2D> m_prefilteredEnvMap;
        uint32_t prefilterCubeMipLevels = 0;
        Ref<Texture2D> m_brdfLUT;

        // Procedural sky: the settings the environment shows, and the newest request waiting for its rebuild.
        // IBL filters (IrradianceConvolution / PrefilterEnv.slang), created once: for an HDR map and the procedural sky.
        std::unique_ptr<NRI::Pipeline> m_irradiancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_prefilterPipeline = nullptr;

        // Procedural sky (SkyAtmosphere.slang): while it is the environment its textures stay (the environment, irradiance and
        // pre-filtered cubemaps, smaller than an HDR map's: the sky is smooth) and a frame whose settings changed rewrites
        // them in addProceduralSkyPass. The two atmosphere LUTs only change with the atmosphere, not with the sun.
        std::unique_ptr<NRI::Pipeline> m_skyTransmittancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyMultiScatteringPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyCubemapPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyAerialPerspectivePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyDownsamplePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyCloudShadowPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyHorizonBandPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyCloudShapeNoisePipeline = nullptr;   // the cloud noise, written when the sky is made
        std::unique_ptr<NRI::Pipeline> m_skyCloudDetailNoisePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_skyCloudWeatherPipeline = nullptr;
        // The sky near the horizon at ~4x the cubemap's resolution, for the skybox (shaderIO.h SkyHorizonBand*), and its storage slot.
        Ref<Texture2D> m_skyHorizonBand;
        uint32_t m_skyHorizonBandStorageSlot = ~0u;
        // Cloud noise (CloudNoise.slang), made once when the sky is created: shape and detail 3D noise as 2D arrays, weather.
        Ref<Texture2D> m_skyCloudShape;
        Ref<Texture2D> m_skyCloudDetail;
        Ref<Texture2D> m_skyCloudWeather;
        Ref<Texture2D> m_skyAerialPerspective; // camera volume, rewritten every frame the scene is lit by deferred lighting
        // The clouds' shadow map, one per frame in flight (a frame writes its own while the one before may still read its), and
        // their storage slots.
        std::array<Ref<Texture2D>, MAX_FRAMES_IN_FLIGHT> m_skyCloudShadow;
        std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> m_skyCloudShadowStorageSlots{};
        Ref<Texture2D> m_skyTransmittanceLut;
        Ref<Texture2D> m_skyMultiScatteringLut;
        // Storage slots: [transmittance, multiple scattering, environment mips..., irradiance mips..., prefiltered mips...,
        // aerial perspective].
        std::vector<uint32_t> m_skyStorageSlots;
        HeightFogSettings m_heightFog;
        bool m_proceduralSkyActive = false;
        bool m_skyLutsDirty = false;
        bool m_skyDirty = false;
        SkyAtmosphereSettings m_proceduralSky;
        // Wind and viewer height re-render the sky in slices (addProceduralSkyPass): the cycle's stage (-1: none running), what
        // it renders, and the wind offset / height last rendered.
        int m_skySliceStage = -1;
        shaderio::PushConstantSkyAtmosphere m_skyCyclePush{};
        glm::vec2 m_skyWindOffsetKm{ 0.0f };
        glm::vec2 m_skyRenderedWindOffsetKm{ 0.0f };
        float m_skyRenderedAltitudeKm = 0.0f;
        std::chrono::steady_clock::time_point m_skyLastUpdateTime{};

        // Lighting
        std::vector<shaderio::LightData> m_lightBufferObjects;
        std::vector<std::unique_ptr<NRI::Buffer>> m_lightBuffers;
        std::vector<void*> m_lightBuffersMapped;
        uint64_t m_LightBufferCapacity = 0;
        // The path tracer's emissive lights (GpuScene::CollectEmissiveLights), one host-visible buffer per frame in flight.
        std::vector<shaderio::EmissiveLightInstance> m_emissiveLights;
        std::array<std::unique_ptr<NRI::Buffer>, MAX_FRAMES_IN_FLIGHT> m_emissiveLightBuffers;
        std::array<void*, MAX_FRAMES_IN_FLIGHT> m_emissiveLightBuffersMapped{};
        uint64_t m_emissiveLightBufferCapacity = 0;
        // The emissive lights' local grid (EmissiveLightGrid.slang), rebuilt every frame the emissive lights are sampled.
        std::unique_ptr<NRI::Buffer> m_emissiveGridBuffer;
        std::unique_ptr<NRI::Pipeline> m_emissiveGridPipeline;
        // The path tracer's emissive triangle lights (EmissiveLightGrid.slang): their records and the running sum of their power.
        std::unique_ptr<NRI::Buffer> m_emissiveTriangleBuffer;
        std::unique_ptr<NRI::Buffer> m_emissiveTriangleCdfBuffer;
        uint32_t m_emissiveTriangleCapacity = 0;
        std::unique_ptr<NRI::Pipeline> m_emissiveTrianglesPipeline;
        std::unique_ptr<NRI::Pipeline> m_emissiveScanPipeline;

        // Temporal & Motion Vector State
        glm::mat4 m_prevView = glm::mat4(1.0f);
        glm::mat4 m_prevNonJitteredProj = glm::mat4(1.0f);
        glm::mat4 m_currentNonJitteredProj = glm::mat4(1.0f);
        glm::mat4 m_currentView = glm::mat4(1.0f);
        // Large world coordinates: the GPU's world space is relative to this (GetRenderOrigin). Moved by updateRenderOrigin, which
        // carries every piece of temporal state that holds a position over to the new origin.
        glm::dvec3 m_renderOrigin{ 0.0 };
        void updateRenderOrigin(const glm::dvec3& cameraWorld);
        glm::vec3 m_prevPrevCameraWorldPos = glm::vec3(0.0f);
        glm::vec3 m_prevCameraWorldPos = glm::vec3(0.0f);
        glm::vec3 m_currentCameraWorldPos = glm::vec3(0.0f);
        uint64_t m_sceneFrameCounter = 0;
        uint64_t m_lastSceneFrameCounter = UINT64_MAX;
        bool m_isFirstFrame = true;

        bool m_cameraJitterEnabled = false;
        bool m_stableEditorDepth = true;
        uint32_t m_jitterPhase = 0;
        glm::vec2 m_currentJitter = glm::vec2(0.0f);
        glm::vec2 m_prevJitter = glm::vec2(0.0f);
        
        // DLSS Super Resolution
        bool m_dlssEnabled = false;
        // RTXPT realtime defaults to DLSS-RR at Balanced resolution. DLSS itself remains opt-in in Nox.
        NRI::UpscaleMode m_dlssMode = NRI::UpscaleMode::Balanced;
        bool m_pendingRenderResolutionUpdate = false;
        bool m_dlssRayReconstructionEnabled = true;

        // Live viewport-panel resize debounce: EditorLayer calls onViewportSizeChange() every frame the
        // ImGui panel's pixel size differs from ours, which during a drag is every single frame. Applying
        // applyRenderResolution() (full G-buffer/ShadowMask/NRD-reinit/DDGI/ReSTIR resource rebuild + a
        // GPU waitIdle) on every one of those tanks perf and spams NRD reinit logs. Instead we record the
        // latest requested size and only actually apply it once no new request has arrived for a short
        // settle window (see applyPendingRenderResolutionIfNeeded()).
        NRI::Extent2D m_pendingViewportSize{};
        bool m_viewportResizePending = false;
        std::chrono::steady_clock::time_point m_lastViewportResizeRequestTime{};

        // Camera Cache (Reverse-Z: no far clip)
        glm::vec3 m_cameraPosition{0.0f};
        glm::vec3 m_cameraUp{0.0f, 1.0f, 0.0f};
        glm::vec3 m_cameraRight{1.0f, 0.0f, 0.0f};
        glm::vec3 m_cameraForward{0.0f, 0.0f, -1.0f};
        float m_cameraNear = 0.1f;
        float m_cameraFOV = 30.0f;
    };
}
