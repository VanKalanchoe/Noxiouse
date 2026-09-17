#pragma once
#include "NoxCore/RenderGraph/RenderGraph.h"

// Internal to the renderer's pass files: handles of this frame's graph resources (blackboard entry added by
// Renderer::prepareFrameGraph; features that own history or size-specific resources fill theirs in their add* function).
// A handle is invalid when its feature does not run this frame.
namespace Nox
{
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
        RGTexture GBufferSpecular;
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
        RGTexture ReSTIRDIDirect;
        RGTexture ReSTIRDIDenoised;
        RGTexture LightPDF;
        RGBuffer ReSTIRGIReservoirs[2];
        RGBuffer NeighborOffsets;
        RGBuffer ReSTIRDIReservoirs[3];
        RGBuffer ReSTIRDIRIS;

        // Path tracing
        RGTexture PathTracerAccum[2];
        RGTexture PathTracerDenoised;
        RGTexture ReSTIRPTOutput;
        RGTexture ReSTIRPTPrimaryDirect;
        RGBuffer ReSTIRPTReservoirs[3];

        // Upscaling / output / editor
        RGTexture DLSSOutput;
        RGBuffer TLAS;
        RGBuffer PickerStaging;
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
