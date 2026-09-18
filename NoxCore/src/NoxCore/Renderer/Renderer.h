#pragma once
#include <array>
#include <chrono>
#include "Renderer2D.h"
#include "NoxCore/Core/Window.h"
#include "Mesh.h"
#include <mutex>
#include <variant>

#include "GeometryArena.h"
#include "MemoryBudget.h"
#include "UploadManager.h"
#include "GpuScene.h"
#include "NoxCore/RenderGraph/RenderGraph.h"

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
    struct FrameGraphResources;
    struct ViewDrawResources;

    struct MeshBLAS
    {
        std::unique_ptr<NRI::Buffer> storageBuffer;
        std::unique_ptr<NRI::AccelerationStructure> as;
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
    struct MeshUpload
    {
        MeshHandle Handle;
        StagingSpan Staging;
        shaderio::GpuMesh GpuMesh{}; // counts and bounds, filled by WriteMeshUpload
        bool IsOpaque = true;
    };

    // Everything whose release must wait for the frames that could still reference it (§5.8.4): a GPU buffer to
    // destroy, the last reference to an asset to drop, or a mesh's geometry ranges and BLAS id to return.
    struct DeferredRelease
    {
        uint32_t framesRemaining = MAX_FRAMES_IN_FLIGHT;
        std::variant<std::unique_ptr<NRI::Buffer>, Ref<Asset>, MeshHandle> payload;
    };

    struct PickRequest
    {
        int32_t x = -1;
        int32_t y = -1;
        uint32_t width = 1; // Default 1 for single click
        uint32_t height = 1; // Default 1 for single click
        bool active = false;
        uint64_t frameNumber = 0; // scene frame whose entity IDs were copied (set when the copy is recorded)
    };

    // Entity IDs of the newest pick copy whose frame has finished on the GPU (§5.3 frame-ID readbacks).
    struct PickResult
    {
        PickRequest request;
        std::vector<int32_t> pixels;
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

        void BeginScene(const Camera& camera, const glm::mat4& cameraWorldMatrix);
        void BeginScene(const EditorCamera& camera);
        void EndScene();

        // GPU scene (§5.5), main thread. Mesh entities live on the GPU until their owner changes them; the renderer
        // uploads only what changed.
        // The scene that renders registers its entities. Returns true when sceneID takes over: every instance of the
        // previous owner is gone and the scene registers all of its entities again.
        bool BindGpuScene(uint64_t sceneID);
        // One instance per drawn submesh, appended to outInstances. Returns false when an asset is not loaded yet (appended
        // to outMissingAssets, request it on the main thread): the entity is registered with what is loaded and has to be
        // registered again once it is.
        bool AddMeshInstances(const glm::mat4& world, const MeshComponent& mesh, const MaterialComponent* material, int32_t entityID,
                              std::vector<uint32_t>& outInstances, std::vector<AssetHandle>& outMissingAssets);
        void RemoveMeshInstances(std::span<const uint32_t> instances);
        void SetMeshInstancesTransform(std::span<const uint32_t> instances, const glm::mat4& world);
        // Skinned instances, every frame: this frame's joint matrices.
        void SetMeshInstancesBones(std::span<const uint32_t> instances, std::span<const glm::mat4> bones);
        // Entities whose instances lost their mesh (it was unloaded) since the last call: register them again.
        void ConsumeInvalidatedMeshEntities(std::vector<int32_t>& outEntityIDs) { outEntityIDs.swap(m_invalidatedMeshEntities); m_invalidatedMeshEntities.clear(); }
        // A material asset's data was edited: instances using it shade with the new data from this frame.
        static void MarkMaterialChanged(AssetHandle material);

        void SubmitLight(const glm::mat4& transform, const DirectionalLightComponent& light);
        void SubmitLight(const glm::mat4& transform, const PointLightComponent& light);
        void SubmitLight(const glm::mat4& transform, const SpotLightComponent& light);

        Texture2D* GetSceneResource() const { return m_sceneResource.get(); }

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
        // Draw list entries, and of the newest finished frame: the instances drawn in phase 1 and the candidates phase 2
        // re-tested (some of those draw too, which only the GPU knows).
        uint32_t getDrawListSize() const { return static_cast<uint32_t>(m_drawList.size()); }
        uint32_t getVisibleInstanceCount() const { return m_visibleInstanceCount; }
        uint32_t getLateCandidateCount() const { return m_lateCandidateCount; }
        uint32_t getLateDrawnCount() const { return m_lateDrawnCount; }
        uint32_t getVisibleTriangleCount() const { return m_visibleTriangleCount; }
        // EditorLayer Settings
        void setDebugMode(uint32_t mode) { m_debugMode = mode; }
        uint32_t getDebugMode() const { return m_debugMode; }
        void setTonemapMode(uint32_t mode) { m_tonemapMode = mode; }
        uint32_t getTonemapMode() const { return m_tonemapMode; }
        float getExposure() const { return m_exposure; }
        void setExposure(float exposure) { m_exposure = exposure; }
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
        uint32_t getDDGIProbeCountTotal() const { return m_ddgiProbeCountX * m_ddgiProbeCountY * m_ddgiProbeCountZ; }
        uint32_t getDDGIRaysPerProbe() const { return m_ddgiRaysPerProbe; }
        void setDDGIRaysPerProbe(uint32_t count) { m_ddgiRaysPerProbe = count; }
        float& getDDGIHysteresis() { return m_ddgiHysteresis; }
        float& getDDGINormalBias() { return m_ddgiNormalBias; }
        float& getDDGIDebugSphereRadius() { return m_ddgiDebugSphereRadius; }
        bool& getDDGIDebugXRay() { return m_ddgiDebugXRay; }
        void resetDDGIHistory() { m_renderGraph.ResetHistory(DDGIHistoryKey); }
        void resetDDGIGridToDefaults();

        // ReSTIR GI (Screen-Space Diffuse Indirect Resampling via RTXDI)
        uint32_t getDiffuseGIMode() const { return m_diffuseGIMode; }
        void setDiffuseGIMode(uint32_t mode) { m_diffuseGIMode = mode; }
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

        void setCameraJitterEnabled(bool enabled) { m_cameraJitterEnabled = enabled; }
        bool getCameraJitterEnabled() const { return m_cameraJitterEnabled; }
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
        uint64_t EndMeshUpload(const MeshUpload& upload, bool nextFrameReads);
        // Draws from now on, ray traced once its BLAS is built in a frame.
        MeshHandle PublishMesh(const MeshUpload& upload);
        // Staging of a Begin* whose copies were never recorded.
        void AbandonUpload(const StagingSpan& staging) { m_uploads.ReleaseStaging(staging); }
        uint64_t GetCompletedUploadValue() const { return m_uploads.GetCompletedValue(); }
        // Meshes that draw but are not ray traced yet (their BLAS waits for a frame's build budget).
        size_t GetPendingBlasBuilds() const { return m_blasBuilds.size(); }
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
        void initPBR();

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
        bool addSubmeshInstances(const glm::mat4& world, const MeshAsset& mesh, const MeshComponent& component, const MaterialComponent* material,
                                 int32_t entityID, std::vector<uint32_t>& outInstances, std::vector<AssetHandle>& outMissingAssets);
        // Shared material record of a submesh: its material asset when loaded, else the mesh's embedded material.
        uint32_t acquireGpuMaterial(uint64_t meshAsset, uint32_t submesh, const MaterialData& meshMaterial, AssetHandle materialAsset,
                                    bool& outComplete, std::vector<AssetHandle>& outMissingAssets);
        // Re-packs materials whose texture slots may have changed (texture imported or unloaded).
        void refreshGpuMaterials();
        void readPickResult(uint32_t frameSlot);
        void readInspectionProbe(uint32_t frameSlot);
        void readCullStats(uint32_t frameSlot);
        void readMipFeedback(uint32_t frameSlot);

        void initRenderer();
        void cleanupSwapChain();
        void recreateSwapChain();
        void createSwapChain();
        void createCompiler();
        void watchShader(const std::filesystem::path& path, const std::string& pipelineKey, std::function<void()> reloadFn);

        void createUnlitPipeline(bool forceCompile);
        void createTransparentLitPipeline(bool forceCompile);
        void createPresentPipeline(bool forceCompile);
        void createComputePipeline();
        void createSkyboxPipeline(bool forceCompile);
        void createCommandPool();

        void createSceneResources();
        // Recomputes m_renderSize from the current output size + DLSS mode. Graph-owned render targets follow the new
        // size by themselves; this resets every history and the size-dependent denoiser/upscaler/RTXDI state. Called on
        // viewport resize, DLSS enable/disable, and DLSS mode change.
        void applyRenderResolution();
        void applyPendingRenderResolutionIfNeeded();
        
        // Visability
        void createVisibilityPipeline(bool forceCompile);
        // G-Buffer
        void createGBufferPipeline(bool forceCompile = false);
        // PBR
        void createDeferredLightingPipeline(bool forceCompile = false);
        // Post Process
        void createPostProcessPipeline(bool forceCompile = false);
        // Render graph texture inspection
        void createTextureInspectPipeline(bool forceCompile = false);
        // GPU instance culling (InstanceCulling.slang: cull, count, offset, write, late) and the depth pyramid build
        void createInstanceCullingPipelines(bool forceCompile = false);
        void createHiZBuildPipeline(bool forceCompile = false);
        
        // NRD
        void createShadowMaskPipeline(bool forceCompile = false);
        void createReflectionPipeline(bool forceCompile = false);
        
        // Path Tracer
        void createPathTracerPipeline(bool forceCompile = false);

        // DDGI (Dynamic Diffuse Global Illumination)
        void createDDGIPipelines(bool forceCompile = false);

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
        std::span<NRI::CommandBuffer* const> recordFrame(uint32_t imageIndex);
        // Start of every frame command buffer: descriptor heaps and the dynamic state every pass builds on.
        void applyCommandBufferBaseline(NRI::CommandBuffer& cmd) const;
        void prepareFrameGraph(uint32_t imageIndex);
        void addGpuSceneUpdatePass();
        // Culls the draw list for the camera view into its visible instances, indirect commands and draw counts.
        void addInstanceCullingPass();
        // Depth pyramid of this frame's visibility depth, then phase 2: the occluded candidates against it.
        void addHiZBuildPass();
        void addInstanceCullingLatePass();
        // Draws what phase 2 found visible into the visibility buffer and depth.
        void addVisibilityLatePass();
        void addBLASBuildPass();
        void addTLASBuildPass();
        void addVisibilityPass();
        void addGBufferPass();
        void addMipFeedbackReadbackPass();
        void addRTShadowPasses();
        void addRTReflectionPasses();
        // Decides whether DDGI runs and declares its atlases up front: RT reflections and forward shading, which come
        // earlier in the frame, sample them too.
        void prepareDDGIFrame(FrameGraphResources& resources);
        void addDDGIPasses();
        void addReSTIRGIPasses();
        void addPreviousFrameCopyPass();
        void addReSTIRDIPasses();
        void addReSTIRPTPasses();
        void addPathTracerPasses();
        void addDeferredLightingPass();
        void addForward3DPass();
        void addDLSSPass();
        void addEntityDepthBlitPass();
        void addPostProcessPass();
        void addOverlay2DPass();
        void addOutlinePass();
        void addPresentPass();
        void addPickReadbackPass();
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
        };
        MeshletDrawCursor beginMeshletDraws(const RGPassContext& context, const ViewDrawResources& draws, bool late = false) const;
        void drawMeshletBucket(NRI::CommandBuffer& cmd, MeshletDrawCursor& cursor, RenderBucket bucket, NRI::Pipeline& pipeline, NRI::CullMode cullMode, bool depthWrite, bool blendEnable) const;
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
        void updateDrawListBuffers(uint32_t currentImage);
        // Releases whose wait has passed; a mesh returns its ranges to the streams, the others die with their entry.
        void processDeferredReleases();
        std::vector<char> readFile(const std::string& filename);
        // Sub-allocates from a geometry stream, copying the stream into a larger buffer first when it no longer fits.
        GeometryRange allocateGeometry(GeometryArena& stream, uint32_t count);
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
        std::unique_ptr<NRI::Pipeline> m_presentPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_computePipeline = nullptr;

        // Visability
        std::unique_ptr<NRI::Pipeline> m_visibilityPipeline = nullptr;
        // G-Buffer
        std::unique_ptr<NRI::Pipeline> m_gbufferPipeline = nullptr;
        // PBR
        std::unique_ptr<NRI::Pipeline> m_deferredLightingPipeline = nullptr;
        // Post Process
        std::unique_ptr<NRI::Pipeline> m_postProcessPipeline = nullptr;
        // NRD
        std::unique_ptr<NRI::Pipeline> m_shadowMaskPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_reflectionPipeline = nullptr;
        // Path Tracer
        std::unique_ptr<NRI::Pipeline> m_pathTracerPipeline = nullptr;
        // DDGI
        std::unique_ptr<NRI::Pipeline> m_ddgiRadiancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiBlendIrradiancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiBlendDistancePipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_ddgiDebugSpheresPipeline = nullptr;

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
            bool restirGIAdded = false;
            bool nrdGIAdded = false;
            bool restirDIAdded = false;
            bool nrdDIAdded = false;
            bool ptNRDAdded = false;
            bool deferredLightingAdded = false;
            bool dlssAdded = false;
            bool inspectionProbe = false;
        };
        RenderGraph m_renderGraph;
        FrameGraphState m_frame;

        // History keys: histories that live outside the graph (NRD, DLSS/NGX) are reset through the same keys.
        static constexpr const char* NRDHistoryKey = "NRD";
        static constexpr const char* DLSSHistoryKey = "DLSS";
        static constexpr const char* DDGIHistoryKey = "DDGI";
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
        uint32_t m_tonemapMode = 4; // Default: KhronosPbrNeutral
        float m_exposure = 1.0f;
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
        uint32_t m_ddgiProbeCountX = 22;
        uint32_t m_ddgiProbeCountY = 10;
        uint32_t m_ddgiProbeCountZ = 14;
        uint32_t m_ddgiRaysPerProbe = 128;
        float m_ddgiHysteresis = 0.97f;
        float m_ddgiNormalBias = 0.2f;
        float m_ddgiDebugSphereRadius = 0.15f;
        bool m_ddgiDebugXRay = true;

        // ReSTIR GI (Screen-Space Diffuse Path Resampling via RTXDI) -- proper 3-pass pipeline
        // (Initial candidate -> Temporal -> Spatial), matching RTXPT's actual architecture instead
        // of the fused SpatioTemporal SDK function (see PushConstantReSTIRGITemporal comment).
        std::unique_ptr<NRI::Pipeline> m_restirGIInitialPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirGITemporalPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_restirGISpatialPipeline = nullptr;
        std::unique_ptr<NRI::Buffer> m_restirGINeighborOffsetsBuffer;

        uint32_t m_diffuseGIMode = 0; // 0 = Off (IBL), 1 = DDGI, 2 = ReSTIR GI
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
        uint32_t m_restirDINumInfiniteLights = 0;

        // Defaults match RTXPT's own RtxdiApplicationSettings.cpp (getReSTIRDIInitialSamplingParams/
        // getReSTIRDITemporalResamplingParams/getReSTIRDISpatialResamplingParams) exactly, except
        // where we deliberately deviate (noted at each dispatch site): BASIC bias correction instead
        // of Raytraced (avoids extra shadow rays per neighbor/per history sample), and no initial
        // visibility ray yet (RTXPT's enableInitialVisibility = true).
        uint32_t m_directLightingMode = 0; // 0 = brute-force analytic loop, 1 = ReSTIR DI
        uint32_t m_restirDINumLocalLightSamples = 8;
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

        // Entity ID readback (entity IDs themselves are render graph textures, see Passes/FrameGraphResources.h)
        std::vector<std::unique_ptr<NRI::Buffer>> m_pickerStagingBuffers;
        std::vector<PickRequest> m_pickerReadbackRequests;
        PickRequest m_pickRequest;
        PickResult m_pickResult;
        std::vector<int32_t> m_SelectedEntityIDs;

        // Render graph texture inspection (editor)
        std::unique_ptr<NRI::Pipeline> m_textureInspectPipeline = nullptr;
        std::array<std::unique_ptr<NRI::Pipeline>, 5> m_instanceCullingPipelines; // cull, count, offset, write, late
        std::unique_ptr<NRI::Pipeline> m_hiZBuildPipeline = nullptr;
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
        std::vector<std::unique_ptr<NRI::Buffer>> m_boneBuffers;
        std::vector<void*> m_boneBuffersMapped;
        uint64_t m_BoneBufferCapacity = 0;

        void updateBoneBuffer(uint32_t currentImage);
        void createBoneBuffer(uint64_t size);

        // Meshes
        // Buffers, assets and geometry ranges wait here until no frame in flight can reference them (§5.8.4).
        // Guarded: an asset destructor queues its geometry, and the last reference to an asset can be dropped on any
        // thread (loader tasks included), while the main thread drains at the frame sync point.
        std::vector<DeferredRelease> m_deferredReleases;
        std::mutex m_deferredReleasesMutex;

        // Unified geometry streams (§5.8.2): one buffer each, sub-allocated per mesh, addressed by offset.
        GeometryArena m_vertexStream;
        GeometryArena m_meshletDrawStream;
        GeometryArena m_meshletBoundsStream;
        GeometryArena m_meshletVertexStream;
        GeometryArena m_meshletTriangleStream;
        GeometryArena m_rtIndexStream;
        MemoryBudget m_memoryBudget;
        std::vector<MeshBLAS> m_meshBLASes;
        std::vector<uint32_t> m_freeBLASIds;

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
        };
        std::vector<BlasBuild> m_blasBuilds;
        std::unique_ptr<NRI::Buffer> m_blasScratch; // shared by the builds of one frame, recorded one after another
        // Bounds each frame's build work while a large load streams in (one submission building thousands of BLAS
        // could exceed the driver timeout). 2M made a 42 ms GPU frame on Bistro (RTX, Release); a quarter keeps a
        // loading frame near a normal one and Bistro still ray traces within ~15 frames.
        static constexpr uint64_t BlasBuildPrimitivesPerFrame = 500'000;
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
        std::unique_ptr<NRI::Buffer> m_mipFeedbackBuffer;
        std::vector<std::unique_ptr<NRI::Buffer>> m_mipFeedbackReadback;
        std::array<bool, MAX_FRAMES_IN_FLIGHT> m_mipFeedbackPending{};
        std::vector<uint32_t> m_mipFeedback;
        uint64_t m_mipFeedbackSerial = 0;
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

        void createOutlinePipeline(bool forceCompile = false);

        // PBR stuff
        Ref<Texture2D> m_environmentCubemap;
        std::unique_ptr<NRI::Pipeline> m_skyboxPipeline = nullptr;
        Ref<Texture2D> m_irradianceCubemap;
        Ref<Texture2D> m_prefilteredEnvMap;
        uint32_t prefilterCubeMipLevels = 0;
        Ref<Texture2D> m_brdfLUT;

        // Lighting
        std::vector<shaderio::LightData> m_lightBufferObjects;
        std::vector<std::unique_ptr<NRI::Buffer>> m_lightBuffers;
        std::vector<void*> m_lightBuffersMapped;
        uint64_t m_LightBufferCapacity = 0;

        // Temporal & Motion Vector State
        glm::mat4 m_prevView = glm::mat4(1.0f);
        glm::mat4 m_prevNonJitteredProj = glm::mat4(1.0f);
        glm::mat4 m_currentNonJitteredProj = glm::mat4(1.0f);
        glm::mat4 m_currentView = glm::mat4(1.0f);
        glm::vec3 m_prevPrevCameraWorldPos = glm::vec3(0.0f);
        glm::vec3 m_prevCameraWorldPos = glm::vec3(0.0f);
        glm::vec3 m_currentCameraWorldPos = glm::vec3(0.0f);
        uint64_t m_sceneFrameCounter = 0;
        uint64_t m_lastSceneFrameCounter = UINT64_MAX;
        bool m_isFirstFrame = true;

        bool m_cameraJitterEnabled = false;
        uint32_t m_jitterPhase = 0;
        glm::vec2 m_currentJitter = glm::vec2(0.0f);
        
        // DLSS Super Resolution
        bool m_dlssEnabled = false;
        NRI::UpscaleMode m_dlssMode = NRI::UpscaleMode::Off;
        bool m_pendingRenderResolutionUpdate = false;
        bool m_dlssRayReconstructionEnabled = false; // Enabled by default when DLSS is on

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
