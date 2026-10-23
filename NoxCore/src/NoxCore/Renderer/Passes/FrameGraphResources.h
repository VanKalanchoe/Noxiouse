#pragma once
#include "NoxCore/RenderGraph/RenderGraph.h"

// Internal to the renderer's pass files: handles of this frame's graph resources (blackboard entry added by
// Renderer::prepareFrameGraph; features that own history or size-specific resources fill theirs in their add* function).
// A handle is invalid when its feature does not run this frame.
namespace Nox
{
    // What one view draws (§5.6): its visible instance slots, one indirect command per visible instance and the draw
    // count per bucket, written by that view's instance culling.
    struct ViewDrawResources
    {
        RGBuffer VisibleInstances;
        RGBuffer Commands;
        RGBuffer Counts;
        // Phase 2 (§5.6.4): instances the occlusion test rejected, re-tested against this frame's depth pyramid.
        RGBuffer LateInstances;
        RGBuffer LateCommands;
    };

    struct FrameGraphResources
    {
        // Raster / G-buffer
        RGTexture Visibility;
        RGTexture Depth;
        RGTexture DepthHi;
        RGTexture Entity;
        RGTexture EntityHi;
        RGTexture SelectionMask; // editor: the selection at display resolution, not jittered (addSelectionMaskPass)
        RGTexture GBufferAlbedo;
        RGTexture GBufferNormal;
        RGTexture GBufferMaterial;
        RGTexture GBufferEmission;
        RGTexture GBufferVelocity;
        RGTexture RRDiffuseAlbedo;  // DLSS-RR guides: RRGuides.slang or the path tracer's primary hit
        RGTexture RRSpecularAlbedo;
        RGTexture RRNormalRoughness; // path-traced world normal.rgb + linear roughness.a
        RGTexture RRSpecularHitDistance;
        RGTexture RRPathDepth;
        RGTexture RRPathMotionVectors;
        RGTexture RRSpecularMotionVectors;
        RGTexture RRTransparencyLayer;   // the glass under DLSS-RR, premultiplied color + opacity (addRRTransparencyLayerPass)
        RGTexture RRTransparencyOpacity; // ...and its opacity per channel (tinted glass)
        RGTexture PrevDepth;
        RGTexture PrevNormal;
        RGTexture PrevAlbedo;
        RGTexture PrevMaterial;
        RGTexture HDRScene;
        RGTexture RefractionSource; // the lit scene behind transparent surfaces with blur mips (addForward3DPass, glass in view)
        RGTexture OITAccum;  // order-independent transparency: weighted premultiplied color and alpha (Renderer::setOITEnabled)
        RGTexture OITReveal; // ...and the product of (1 - alpha), how much of the scene behind still shows
        RGTexture AutoExposure;
        RGTexture Scene;
        RGTexture EnvironmentCubemap;
        RGTexture SSAORaw;  // SSAO.slang's hash-dithered output, before SSAOBlur.slang denoises it
        RGTexture SSAO;

        // Hybrid RT + NRD
        RGTexture RawShadowMask;
        RGTexture DenoisedShadowMask;
        RGTexture ViewZ;
        RGTexture NRDNormalRoughness;
        RGTexture RawReflection;
        RGTexture DenoisedReflection;

        // DDGI
        RGTexture DDGIRayData;
        RGTexture DDGIIrradiance[2];
        RGTexture DDGIDistance[2];
        RGTexture DDGIProbeOffsets[2]; // probe relocation, ping-ponged with the atlases (invalid while relocation is off)

        // Screen-probe GI (ScreenProbe*.slang)
        RGTexture ScreenProbePosition;     // per probe: world position (w: 1, 0 for none)
        RGTexture ScreenProbeNormal;       // per probe: the surface's flat normal
        RGTexture ScreenProbeRays;         // 8 x 8 rays per probe
        RGTexture ScreenProbeIrradiance[2]; // octahedral irradiance per probe, ping-pong of the spatial filter
        RGTexture ScreenProbeGI[2];        // the per-pixel result, ping-pong history (FrameGraphState::screenProbeGIWriteIndex)
        RGTexture ScreenProbeGIDistance;   // per pixel: the distance from the camera of the surface the result is for (SceneGI.slang)
        RGTexture AerialPerspective;       // the camera volume of the air over a procedural sky (deferred and forward fog)

        // ReSTIR GI / DI
        RGTexture ReSTIRGIRaw;
        RGTexture ReSTIRGIDenoised;
        RGTexture ReSTIRDIDiffuse;  // de-modulated direct lighting (NRDFrontEnd.slang), raw and NRD-denoised
        RGTexture ReSTIRDISpecular;
        RGTexture ReSTIRDIDiffuseDenoised;
        RGTexture ReSTIRDISpecularDenoised;
        RGTexture LightPDF;
        RGBuffer ReSTIRGIReservoirs[2];
        RGBuffer NeighborOffsets;
        RGBuffer ReSTIRDIReservoirs[3];
        RGBuffer ReSTIRDIRIS;

        // Path tracing
        RGTexture PathTracerAccum[2];
        RGTexture PathTracerDenoised;
        RGTexture PathTracerDiffuse;  // NRD signals of the path tracer (PathTracerSignals.slang), raw and denoised
        RGTexture PathTracerSpecular;
        RGTexture PathTracerDiffuseDenoised;
        RGTexture PathTracerSpecularDenoised;
        RGTexture ReSTIRPTOutput;
        RGTexture ReSTIRPTPrimaryDirect;
        RGBuffer ReSTIRPTReservoirs[3];

        // Upscaling / output / editor
        RGTexture DLSSOutput;
        RGTexture TAAHistory[2]; // the engine's TAA: last frame's result and this frame's (Renderer::addTAAPass)
        RGBuffer TLAS;
        RGBuffer PickerStaging;
        RGBuffer PickerDepthStaging; // the depth of the picked pixel (the editor's measure tool)
        RGBuffer MipFeedback; // texture streaming feedback (§5.12)
        RGBuffer ClusterStats; // what the visibility passes drew after the LOD cut (§5.7)

        // GPU scene tables
        RGBuffer SceneInstances;
        RGBuffer SceneTransforms;
        RGBuffer SceneMaterials;
        RGBuffer SceneMeshes;
        RGBuffer SceneRayTracingInstances;
        RGBuffer EmissiveLightGrid; // the emissive lights' local grid (EmissiveLightGrid.slang), when the lights are sampled
        RGBuffer EmissiveTriangles; // the path tracer's emissive triangle lights and the running sum of their power
        RGBuffer EmissiveTriangleCdf;

        // Compute-skinned local-space positions/normals. The previous ring slot supplies true skeletal motion vectors.
        RGBuffer SkinningSourceVertices;
        RGBuffer SkinningBoneMatrices;
        RGBuffer SkinningWorkItems;
        RGBuffer SkinnedVertices;
        RGBuffer PreviousSkinnedVertices;

        // SSAO
        RGBuffer SSAOKernel; // static, generated once at startup (Renderer::createSSAOKernel)

        // Views
        ViewDrawResources CameraDraws;
        RGTexture AutoExposureAdapted; // the adapted log2 average luminance (1 texel, kept across frames), with auto exposure
        bool AutoExposureAdaptedReset = false;
        RGTexture CameraHiZ; // depth pyramid of the camera view (history: phase 1 tests against the previous frame)
    };

    // Declares a read only when the resource exists.
    inline void ReadIfValid(RGBuilder& builder, RGTexture texture)
    {
        if (texture.IsValid())
            builder.Read(texture);
    }

    // A pass that samples the DDGI probes (through the uniforms): the atlases of `index` and the probes' relocation with them.
    inline void ReadDDGI(RGBuilder& builder, const FrameGraphResources& resources, uint32_t index)
    {
        builder.Read(resources.DDGIIrradiance[index]);
        builder.Read(resources.DDGIDistance[index]);
        if (resources.DDGIProbeOffsets[index].IsValid())
            builder.Read(resources.DDGIProbeOffsets[index]);
    }

    inline void ReadIfValid(RGBuilder& builder, RGBuffer buffer)
    {
        if (buffer.IsValid())
            builder.Read(buffer);
    }

    inline void ReadViewDraws(RGBuilder& builder, const ViewDrawResources& draws, bool late = false)
    {
        ReadIfValid(builder, late ? draws.LateInstances : draws.VisibleInstances);
        // The indirect stage fetches the commands and the draw counts.
        const RGBuffer commands = late ? draws.LateCommands : draws.Commands;
        if (commands.IsValid())
            builder.Read(commands, RGBufferAccess::IndirectRead);
        if (draws.Counts.IsValid())
            builder.Read(draws.Counts, RGBufferAccess::IndirectRead);
    }

    // Passes that draw or trace the scene read the GPU scene tables (through the uniforms).
    inline void ReadGpuScene(RGBuilder& builder, const FrameGraphResources& resources)
    {
        ReadIfValid(builder, resources.SceneInstances);
        ReadIfValid(builder, resources.SceneTransforms);
        ReadIfValid(builder, resources.SceneMaterials);
        ReadIfValid(builder, resources.SceneMeshes);
        ReadIfValid(builder, resources.SceneRayTracingInstances);
        ReadIfValid(builder, resources.EmissiveLightGrid);
        ReadIfValid(builder, resources.EmissiveTriangles);
        ReadIfValid(builder, resources.EmissiveTriangleCdf);
    }

    // The HDR image this frame's lighting produced (DLSS or post-process input): ReSTIR PT, else the plain path tracer,
    // else deferred lighting + forward. A path tracer result is read denoised when its NRD pass runs.
    inline void ReadLitScene(RGBuilder& builder, const FrameGraphResources& resources, bool restirPTActive,
                             bool pathTracerActive, bool ptNRDAdded, uint32_t pathTracerWriteIndex)
    {
        if ((restirPTActive || pathTracerActive) && ptNRDAdded)
            builder.Read(resources.PathTracerDenoised);
        else if (restirPTActive)
            builder.Read(resources.ReSTIRPTOutput);
        else if (pathTracerActive)
            builder.Read(resources.PathTracerAccum[pathTracerWriteIndex]);
        else
            builder.Read(resources.HDRScene);
    }
}
