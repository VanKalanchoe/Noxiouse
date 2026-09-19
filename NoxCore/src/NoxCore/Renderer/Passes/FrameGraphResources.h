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
        RGTexture GBufferAlbedo;
        RGTexture GBufferNormal;
        RGTexture GBufferMaterial;
        RGTexture GBufferEmission;
        RGTexture GBufferVelocity;
        RGTexture RRDiffuseAlbedo;  // DLSS Ray Reconstruction guides (RRGuides.slang)
        RGTexture RRSpecularAlbedo;
        RGTexture RRSpecularHitDistance;
        RGTexture PrevDepth;
        RGTexture PrevNormal;
        RGTexture PrevAlbedo;
        RGTexture PrevMaterial;
        RGTexture HDRScene;
        RGTexture Scene;
        RGTexture EnvironmentCubemap;

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
        RGBuffer TLAS;
        RGBuffer PickerStaging;
        RGBuffer MipFeedback; // texture streaming feedback (§5.12)
        RGBuffer ClusterStats; // what the visibility passes drew after the LOD cut (§5.7)

        // GPU scene tables
        RGBuffer SceneInstances;
        RGBuffer SceneTransforms;
        RGBuffer SceneMaterials;
        RGBuffer SceneMeshes;
        RGBuffer SceneRayTracingInstances;

        // Views
        ViewDrawResources CameraDraws;
        RGTexture CameraHiZ; // depth pyramid of the camera view (history: phase 1 tests against the previous frame)
    };

    // Declares a read only when the resource exists.
    inline void ReadIfValid(RGBuilder& builder, RGTexture texture)
    {
        if (texture.IsValid())
            builder.Read(texture);
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
