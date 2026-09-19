// RTXDI passes: ReSTIR GI (+ NRD), previous-frame G-buffer snapshot, ReSTIR DI (+ light PDF, presampling, NRD),
// ReSTIR PT (+ NRD, YCoCg decode).
#include "NoxCore/Renderer/Renderer.h"

#include <Rtxdi/GI/ReSTIRGI.h>
#include <Rtxdi/PT/ReSTIRPT.h>
#include <Rtxdi/RtxdiUtils.h>

#include "FrameGraphResources.h"

namespace Nox
{
    void Renderer::addReSTIRGIPasses()
    {
        // ReSTIR GI ray-traces its initial candidate against the scene TLAS, so like DDGI it needs ray tracing access.
        const bool runReSTIRGI = (!m_frame.runPathTracer || m_frame.pathTracerUsesRTXDI) &&
                                 (m_rayTracingEnabled || m_frame.pathTracerUsesRTXDI) &&
                                 (m_diffuseGIMode == 2 || (m_debugMode == 16 && m_diffuseGIMode != 1)) &&
                                 m_hasTLASBuild && m_sceneTLAS && (uniformData.tlasDeviceAddress != 0) &&
                                 (uniformData.sceneInstancesReference != 0) &&
                                 m_restirGIInitialPipeline && m_restirGITemporalPipeline && m_restirGISpatialPipeline &&
                                 m_restirGINeighborOffsetsBuffer;
        // When it does not run, resolveFrameUniforms falls back (IBL ambient for GI mode 2).
        if (!runReSTIRGI)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.restirGIAdded = true;

        const RTXDI_ReservoirBufferParameters resParams = rtxdi::CalculateReservoirBufferParameters(
            m_renderSize.width, m_renderSize.height, rtxdi::CheckerboardMode::Off);
        const uint32_t blockRowPitch = resParams.reservoirBlockRowPitch;
        const uint32_t arrayPitch = resParams.reservoirArrayPitch;

        // Fixed buffer roles, matching RTXPT's ReSTIRGIContext::UpdateBufferIndices for TemporalAndSpatial mode (NOT
        // ping-ponged by frame parity): buffer 0 is this-frame scratch (Initial writes it, Temporal reads its own candidate
        // and overwrites it in place - safe, single-pixel read+write). Buffer 1 is the persistent cross-frame result:
        // Temporal reads it as history, Spatial reads every neighbor from buffer 0 and writes its result into buffer 1,
        // so it never races a neighbor's in-flight read of buffer 0.
        constexpr uint32_t kScratchBuffer = 0;
        constexpr uint32_t kPersistentBuffer = 1;
        // Block-linear 16x16 tiles per the RTXDI SDK. Resize recreates both (zeroed).
        const RGBufferDesc reservoirDesc{ static_cast<uint64_t>(arrayPitch) * sizeof(RTXDI_PackedGIReservoir) };
        resources.ReSTIRGIReservoirs[kScratchBuffer] = m_renderGraph.CreateBuffer("ReSTIR GI Reservoirs (Scratch)", reservoirDesc);
        resources.ReSTIRGIReservoirs[kPersistentBuffer] = m_renderGraph.GetHistoryBuffer("ReSTIR GI Reservoirs", reservoirDesc, 1).Buffers[0];

        m_renderGraph.PushGroup("ReSTIR GI");

        // 1. Initial candidate generation.
        m_renderGraph.AddPass("ReSTIR GI Initial", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferAlbedo);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.Visibility);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                // The path vertices' direct light samples this frame's presampled lights.
                if (m_frame.lightPresamplingAdded)
                    builder.Read(resources.ReSTIRDIRIS);
                builder.Write(resources.ReSTIRGIReservoirs[kScratchBuffer]);
                builder.ColorTarget(resources.ReSTIRGIRaw, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, blockRowPitch, arrayPitch](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirGIInitialPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRGIInitial initPush{};
                initPush.invViewProj = uniformData.invViewProj;
                initPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                initPush.reservoirBufferReference = context.Buffer(res->ReSTIRGIReservoirs[kScratchBuffer]).getDeviceAddress();
                initPush.depthTextureIndex = context.Slot(res->Depth);
                initPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                initPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                initPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                initPush.visibilityTextureIndex = context.Slot(res->Visibility);
                initPush.viewportSize = glm::vec2(rw, rh);
                initPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                initPush.reservoirBlockRowPitch = blockRowPitch;
                initPush.reservoirArrayPitch = arrayPitch;
                // Infinite lights do not need a RIS buffer. Populate the directional-light region
                // even when local-light presampling was skipped (sun-only scenes such as Bistro).
                initPush.lightSampling.lightDataReference = uniformData.lightDataReference;
                initPush.lightSampling.firstInfiniteLightIndex = m_restirDIFirstInfiniteLight;
                initPush.lightSampling.numInfiniteLights = m_restirDINumInfiniteLights;
                initPush.lightSampling.numInfiniteLightSamples = 1;
                if (m_frame.lightPresamplingAdded)
                {
                    initPush.lightSampling = lightSamplingParams(context, res->ReSTIRDIRIS);
                    initPush.lightSampling.numLocalLightSamples = 1;
                    initPush.lightSampling.numInfiniteLightSamples = 1;
                }
                cmd.pushData(&initPush, sizeof(shaderio::PushConstantReSTIRGIInitial));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 2. Temporal resampling: reads the candidate + history, overwrites the candidate in place.
        m_renderGraph.AddPass("ReSTIR GI Temporal", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.PrevDepth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.PrevNormal);
                builder.Read(resources.GBufferVelocity);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.ReSTIRGIReservoirs[kPersistentBuffer]);
                builder.Read(resources.ReSTIRGIReservoirs[kScratchBuffer]);
                builder.Write(resources.ReSTIRGIReservoirs[kScratchBuffer]);
                builder.ColorTarget(resources.ReSTIRGIRaw, NRI::LoadOP::dontCare, NRI::StoreOP::dontCare);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, blockRowPitch, arrayPitch](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirGITemporalPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRGITemporal tPush{};
                tPush.invViewProj = uniformData.invViewProj;
                // Last frame's depth/normal snapshot was rendered with last frame's camera: unproject it with the same
                // matrix, never this frame's.
                tPush.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
                tPush.cameraWorldPos = uniformData.cameraWorldPos;
                tPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                tPush.currentReservoirReference = context.Buffer(res->ReSTIRGIReservoirs[kScratchBuffer]).getDeviceAddress();
                tPush.previousReservoirReference = context.Buffer(res->ReSTIRGIReservoirs[kPersistentBuffer]).getDeviceAddress();
                tPush.depthTextureIndex = context.Slot(res->Depth);
                tPush.prevDepthTextureIndex = context.Slot(res->PrevDepth);
                tPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                tPush.prevNormalTextureIndex = context.Slot(res->PrevNormal);
                tPush.gbufferVelocityIndex = context.Slot(res->GBufferVelocity);
                tPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                tPush.viewportSize = glm::vec2(rw, rh);
                tPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                tPush.reservoirBlockRowPitch = blockRowPitch;
                tPush.reservoirArrayPitch = arrayPitch;
                tPush.maxHistoryLength = m_restirGIMaxHistoryLength;
                tPush.normalThreshold = m_restirGINormalThreshold;
                tPush.depthThreshold = m_restirGIDepthThreshold;
                tPush.enablePermutationSampling = 1; // matches RTXPT's default (true)
                tPush.maxReservoirAge = 50; // matches RTXPT's GI-specific default
                cmd.pushData(&tPush, sizeof(shaderio::PushConstantReSTIRGITemporal));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 3. Spatial resampling: reads this frame's scratch buffer (own pixel and neighbors), writes the persistent one.
        const bool denoiseGI = m_nrdGIDenoiser != NRI::NRDDiffuseDenoiser::Off && m_device->isNRDInitialized();
        m_renderGraph.AddPass("ReSTIR GI Spatial", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.ReSTIRGIReservoirs[kScratchBuffer]);
                builder.Read(resources.NeighborOffsets);
                builder.Write(resources.ReSTIRGIReservoirs[kPersistentBuffer]);
                builder.ColorTarget(resources.ReSTIRGIRaw, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, blockRowPitch, arrayPitch, denoiseGI](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirGISpatialPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRGISpatial sPush{};
                sPush.invViewProj = uniformData.invViewProj;
                sPush.cameraWorldPos = uniformData.cameraWorldPos;
                sPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                sPush.inputReservoirReference = context.Buffer(res->ReSTIRGIReservoirs[kScratchBuffer]).getDeviceAddress();
                sPush.outputReservoirReference = context.Buffer(res->ReSTIRGIReservoirs[kPersistentBuffer]).getDeviceAddress();
                sPush.neighborOffsetsReference = context.Buffer(res->NeighborOffsets).getDeviceAddress();
                sPush.depthTextureIndex = context.Slot(res->Depth);
                sPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                sPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                sPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                sPush.viewportSize = glm::vec2(rw, rh);
                sPush.reservoirBlockRowPitch = blockRowPitch;
                sPush.reservoirArrayPitch = arrayPitch;
                sPush.samplingRadius = m_restirGISpatialRadius;
                sPush.numSamples = m_restirGINumSpatialSamples;
                sPush.normalThreshold = m_restirGINormalThreshold;
                sPush.depthThreshold = m_restirGIDepthThreshold;
                sPush.neighborOffsetMask = 127;
                sPush.enableBoilingFilter = m_restirGIEnableBoilingFilter ? 1 : 0;
                sPush.boilingFilterStrength = m_restirGIBoilingFilterStrength;
                // Packed for the NRD pass that runs (the lighting unpacks the same way), plain without one.
                sPush.denoiserMode = denoiseGI ? static_cast<uint32_t>(m_nrdGIDenoiser) : 0u;
                cmd.pushData(&sPush, sizeof(shaderio::PushConstantReSTIRGISpatial));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 4. NRD diffuse (REBLUR / RELAX) of the 1-SPP GI.
        if (denoiseGI)
        {
            m_frame.nrdGIAdded = true;
            m_renderGraph.AddPass("NRD GI", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.ReSTIRGIRaw);
                    builder.Read(resources.GBufferVelocity);
                    builder.Read(resources.NRDNormalRoughness);
                    builder.Read(resources.ViewZ);
                    builder.Write(resources.ReSTIRGIDenoised);
                    builder.RecordExclusive("NRD");
                },
                [this, res = &resources](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();

                    NRI::NRDDiffuseParams giDenoiseParams{};
                    giDenoiseParams.inDiffuseRadianceHitDist = &context.Texture(res->ReSTIRGIRaw);
                    giDenoiseParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                    giDenoiseParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                    giDenoiseParams.inViewZ = &context.Texture(res->ViewZ);
                    giDenoiseParams.outDenoisedDiffuse = &context.Texture(res->ReSTIRGIDenoised);
                    giDenoiseParams.commandBuffer = &cmd;

                    giDenoiseParams.view = uniformData.view;
                    giDenoiseParams.proj = uniformData.nonJitteredProj;
                    giDenoiseParams.prevView = uniformData.prevView;
                    giDenoiseParams.prevProj = uniformData.prevProj;

                    giDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                    giDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    giDenoiseParams.resetHistory = m_isFirstFrame || m_frame.resetNRD;

                    // Lighting reads the denoised result (resolveFrameUniforms): evaluation only fails without NRD.
                    m_device->evaluateNRDDiffuse(giDenoiseParams, m_nrdGIDenoiser);
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                });
        }

        m_renderGraph.PopGroup();
    }

    void Renderer::addPreviousFrameCopyPass()
    {
        // Snapshot this frame's depth/normal (and albedo/material for ReSTIR PT) as next frame's "previous" G-buffer
        // for temporal reprojection validity checks. This pass is added after every ReSTIR consumer has read the old
        // snapshot, and is omitted when no ReSTIR lighting path needs that history.
        if (!m_frame.restirGIAdded && !m_frame.restirDIAdded && !m_frame.restirPTActive)
            return;

        const FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        m_renderGraph.AddPass("Previous Frame Copy", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth, RGTextureAccess::CopySource);
                builder.Read(resources.GBufferNormal, RGTextureAccess::CopySource);
                builder.Write(resources.PrevDepth, RGTextureAccess::CopyDestination);
                builder.Write(resources.PrevNormal, RGTextureAccess::CopyDestination);
                builder.Read(resources.GBufferAlbedo, RGTextureAccess::CopySource);
                builder.Read(resources.GBufferMaterial, RGTextureAccess::CopySource);
                builder.Write(resources.PrevAlbedo, RGTextureAccess::CopyDestination);
                builder.Write(resources.PrevMaterial, RGTextureAccess::CopyDestination);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const uint32_t width = m_frame.renderExtent.width;
                const uint32_t height = m_frame.renderExtent.height;
                cmd.copyTexture(context.Texture(res->Depth), context.Texture(res->PrevDepth), width, height);
                cmd.copyTexture(context.Texture(res->GBufferNormal), context.Texture(res->PrevNormal), width, height);
                cmd.copyTexture(context.Texture(res->GBufferAlbedo), context.Texture(res->PrevAlbedo), width, height);
                cmd.copyTexture(context.Texture(res->GBufferMaterial), context.Texture(res->PrevMaterial), width, height);
            });
    }

    void Renderer::addLightPresamplingPasses()
    {
        // RTXDI's light presampling (light PDF mip chain, RIS tiles, ReGIR cells), once per frame for everything that samples
        // lights through it: ReSTIR DI's primary surfaces and ReSTIR GI's path vertices (RTXDILightSampling.slang).
        const bool rtxdiLighting = m_directLightingMode == 1 || m_diffuseGIMode == 2 || (m_debugMode == 16 && m_diffuseGIMode != 1);
        const bool runPresampling = rtxdiLighting && m_restirDINumLocalLights > 0 &&
                                    (!m_frame.runPathTracer || m_frame.pathTracerUsesRTXDI) &&
                                    (m_rayTracingEnabled || m_frame.pathTracerUsesRTXDI) &&
                                    m_restirDIPresamplePipeline && m_restirDIPresampleReGIRPipeline &&
                                    m_restirDIWriteLightPDFPipeline && m_restirDIReduceLightPDFMipPipeline &&
                                    (uniformData.lightDataReference != 0);
        if (!runPresampling)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();

        const uint32_t regirCellCount = m_regirCellsX * m_regirCellsY * m_regirCellsZ;
        const uint32_t risBufferOffset = m_restirDIRISTileSize * m_restirDIRISTileCount; // where ReGIR's segment starts
        // ReGIR only contains local lights. Building all grid cells when the scene has only a sun
        // performs hundreds of thousands of useless RIS iterations and can dominate the frame.
        const bool regirActive = m_regirEnabled && regirCellCount > 0 && m_restirDINumLocalLights > 0;

        // uint2 per element: [0, risBufferOffset) = RIS tiles, [risBufferOffset, end) = ReGIR cells. Rebuilt every frame.
        const uint64_t risElements = static_cast<uint64_t>(risBufferOffset) + static_cast<uint64_t>(regirCellCount) * m_regirLightsPerCell;
        resources.ReSTIRDIRIS = m_renderGraph.CreateBuffer("ReSTIR DI RIS", RGBufferDesc{ risElements * sizeof(glm::uvec2) });

        m_renderGraph.PushGroup("Light Presampling");

        // -1. Light PDF mip chain rebuild (feeds RTXDI_PresampleLocalLights), every frame: lights can change every frame.
        m_renderGraph.AddPass("Light PDF Write", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Write(resources.LightPDF);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIWriteLightPDFPipeline);

                shaderio::PushConstantReSTIRDIWriteLightPDF wPush{};
                wPush.lightDataReference = uniformData.lightDataReference;
                wPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                wPush.numLocalLights = m_restirDINumLocalLights;
                wPush.pdfTextureStorageIndex = context.StorageSlot(res->LightPDF, 0);
                wPush.pdfTextureSize = m_lightPDFTextureSize;
                cmd.pushData(&wPush, sizeof(shaderio::PushConstantReSTIRDIWriteLightPDF));

                uint32_t pdfGroups = (m_lightPDFTextureSize + 7) / 8;
                cmd.dispatch(pdfGroups, pdfGroups, 1);
            });

        // One pass per mip: each reads the mip the previous pass wrote.
        for (uint32_t mip = 1; mip < m_lightPDFMipLevels; mip++)
        {
            m_renderGraph.AddPass("Light PDF Reduce", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.LightPDF);
                    builder.Write(resources.LightPDF);
                },
                [this, res = &resources, mip](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIReduceLightPDFMipPipeline);

                    shaderio::PushConstantReSTIRDIReduceLightPDFMip rPush{};
                    rPush.srcTextureIndex = context.Slot(res->LightPDF);
                    rPush.dstStorageIndex = context.StorageSlot(res->LightPDF, mip);
                    rPush.srcMip = mip - 1;
                    rPush.dstSize = m_lightPDFTextureSize >> mip;
                    cmd.pushData(&rPush, sizeof(shaderio::PushConstantReSTIRDIReduceLightPDFMip));

                    uint32_t mipGroups = ((rPush.dstSize > 0 ? rPush.dstSize : 1) + 7) / 8;
                    cmd.dispatch(mipGroups, mipGroups, 1);
                });
        }

        // 0a. RIS presample (1D compute; the SDK's RTXDI_PresampleLocalLights against the light PDF mip chain above;
        // also ReGIR's fallback for out-of-grid pixels).
        m_renderGraph.AddPass("RIS Presample", RGPassFlags::None,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.LightPDF);
                builder.Write(resources.ReSTIRDIRIS);
            },
            [this, res = &resources](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIPresamplePipeline);

                shaderio::PushConstantReSTIRDIPresample risPush{};
                risPush.lightDataReference = uniformData.lightDataReference;
                risPush.risBufferReference = context.Buffer(res->ReSTIRDIRIS).getDeviceAddress();
                risPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                risPush.numLocalLights = m_restirDINumLocalLights;
                risPush.risTileSize = m_restirDIRISTileSize;
                risPush.risTileCount = m_restirDIRISTileCount;
                risPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                risPush.pdfTextureIndex = context.Slot(res->LightPDF);
                risPush.pdfTextureSize = m_lightPDFTextureSize;
                cmd.pushData(&risPush, sizeof(shaderio::PushConstantReSTIRDIPresample));

                uint32_t risThreads = m_restirDIRISTileSize * m_restirDIRISTileCount;
                cmd.dispatch((risThreads + 63) / 64, 1, 1);
            });

        // 0b. ReGIR presample (1D compute, one thread per (cell, slot-within-cell)).
        if (regirActive)
        {
            m_renderGraph.AddPass("ReGIR Presample", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.ReSTIRDIRIS);
                    builder.Write(resources.ReSTIRDIRIS);
                },
                [this, res = &resources, regirCellCount, risBufferOffset](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_restirDIPresampleReGIRPipeline);

                    shaderio::PushConstantReSTIRDIPresampleReGIR regirPush{};
                    regirPush.lightDataReference = uniformData.lightDataReference;
                    regirPush.risBufferReference = context.Buffer(res->ReSTIRDIRIS).getDeviceAddress();
                    regirPush.gridCenterAndCellSize = glm::vec4(m_regirGridCenter, m_regirCellSize);
                    regirPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                    regirPush.numLocalLights = m_restirDINumLocalLights;
                    regirPush.risBufferOffset = risBufferOffset;
                    regirPush.lightsPerCell = m_regirLightsPerCell;
                    regirPush.cellsX = m_regirCellsX;
                    regirPush.cellsY = m_regirCellsY;
                    regirPush.cellsZ = m_regirCellsZ;
                    regirPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    regirPush.regirSamplingJitter = m_regirSamplingJitter;
                    regirPush.risTileSize = m_restirDIRISTileSize;
                    regirPush.risTileCount = m_restirDIRISTileCount;
                    regirPush.numRegirBuildSamples = m_regirNumBuildSamples;
                    cmd.pushData(&regirPush, sizeof(shaderio::PushConstantReSTIRDIPresampleReGIR));

                    uint32_t regirThreads = regirCellCount * m_regirLightsPerCell;
                    cmd.dispatch((regirThreads + 63) / 64, 1, 1);
                });
        }

        m_frame.lightPresamplingAdded = true;
        m_frame.risBufferOffset = risBufferOffset;
        m_frame.regirActive = regirActive;
        m_renderGraph.PopGroup();
    }

    shaderio::RTXDILightSamplingParams Renderer::lightSamplingParams(const RGPassContext& context, RGBuffer risBuffer) const
    {
        shaderio::RTXDILightSamplingParams params{};
        params.lightDataReference = uniformData.lightDataReference;
        params.risBufferReference = context.Buffer(risBuffer).getDeviceAddress();
        params.gridCenterAndCellSize = glm::vec4(m_regirGridCenter, m_regirCellSize);
        params.firstLocalLightIndex = m_restirDIFirstLocalLight;
        params.numLocalLights = m_restirDINumLocalLights;
        params.firstInfiniteLightIndex = m_restirDIFirstInfiniteLight;
        params.numInfiniteLights = m_restirDINumInfiniteLights;
        params.numLocalLightSamples = m_restirDINumLocalLightSamples;
        params.numInfiniteLightSamples = m_restirDINumInfiniteLightSamples;
        params.risBufferOffset = m_frame.risBufferOffset;
        params.risTileSize = m_restirDIRISTileSize;
        params.risTileCount = m_restirDIRISTileCount;
        params.regirEnabled = m_frame.regirActive ? 1u : 0u;
        params.cellsX = m_regirCellsX;
        params.cellsY = m_regirCellsY;
        params.cellsZ = m_regirCellsZ;
        params.lightsPerCell = m_regirLightsPerCell;
        params.regirSamplingJitter = m_regirSamplingJitter;
        return params;
    }

    void Renderer::addReSTIRDIPasses()
    {
        // ReSTIR DI ray-traces its final shadow against the scene TLAS, so it needs ray tracing access.
        const bool runReSTIRDI = (!m_frame.runPathTracer || m_frame.pathTracerUsesRTXDI) &&
                                 (m_rayTracingEnabled || m_frame.pathTracerUsesRTXDI) &&
                                 m_directLightingMode == 1 &&
                                 m_hasTLASBuild && m_sceneTLAS && (uniformData.tlasDeviceAddress != 0) &&
                                 m_restirDIInitialPipeline && m_restirDITemporalPipeline &&
                                 m_restirDISpatialPipeline && m_restirDIFinalShadingPipeline &&
                                 m_restirGINeighborOffsetsBuffer && (uniformData.lightDataReference != 0);
        // When it does not run, resolveFrameUniforms falls back to the brute-force loop (e.g. TLAS build pending).
        if (!runReSTIRDI)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        m_frame.restirDIAdded = true;

        const RTXDI_ReservoirBufferParameters diResParams = rtxdi::CalculateReservoirBufferParameters(
            m_renderSize.width, m_renderSize.height, rtxdi::CheckerboardMode::Off);
        const uint32_t blockRowPitch = diResParams.reservoirBlockRowPitch;
        const uint32_t arrayPitch = diResParams.reservoirArrayPitch;

        const RGBufferHistory reservoirs = m_renderGraph.GetHistoryBuffer("ReSTIR DI Reservoirs",
            RGBufferDesc{ static_cast<uint64_t>(arrayPitch) * sizeof(RTXDI_PackedDIReservoir) }, 3);
        for (uint32_t i = 0; i < 3; ++i)
            resources.ReSTIRDIReservoirs[i] = reservoirs.Buffers[i];
        if (reservoirs.WasReset)
            m_restirDILastFrameOutputReservoir = 0;

        // 3-buffer rotation (NOT GI's fixed 2-role scheme) - see the rotation math documented on
        // PushConstantReSTIRDIInitial in shaderIO.h. Only 2 buffers would reintroduce the read/write race fixed for GI.
        const uint32_t bufferA = (m_restirDILastFrameOutputReservoir + 1) % 3; // Initial writes, Temporal overwrites in place
        const uint32_t bufferC = m_restirDILastFrameOutputReservoir;          // Temporal's history (read only)
        const uint32_t bufferB = (bufferA + 1) % 3;                            // Spatial's output, Final Shading's input
        m_restirDILastFrameOutputReservoir = bufferB;
        const bool hasLocalLights = m_restirDINumLocalLights > 0;

        m_renderGraph.PushGroup("ReSTIR DI");

        if (hasLocalLights)
        {
        // 1. Initial sampling.
        m_renderGraph.AddPass("ReSTIR DI Initial", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferAlbedo);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.ReSTIRDIRIS);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                builder.Write(resources.ReSTIRDIReservoirs[bufferA]);
                builder.ColorTarget(resources.ReSTIRDIDiffuse, NRI::LoadOP::dontCare, NRI::StoreOP::dontCare);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, bufferA, blockRowPitch, arrayPitch](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                // The presample compute passes above bound compute state.
                cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDIInitialPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDIInitial diInitPush{};
                diInitPush.invViewProj = uniformData.invViewProj;
                diInitPush.cameraWorldPos = uniformData.cameraWorldPos;
                diInitPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diInitPush.reservoirBufferReference = context.Buffer(res->ReSTIRDIReservoirs[bufferA]).getDeviceAddress();
                diInitPush.depthTextureIndex = context.Slot(res->Depth);
                diInitPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                diInitPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                diInitPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                diInitPush.viewportSize = glm::vec2(rw, rh);
                diInitPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diInitPush.reservoirBlockRowPitch = blockRowPitch;
                diInitPush.reservoirArrayPitch = arrayPitch;
                diInitPush.lightSampling = lightSamplingParams(context, res->ReSTIRDIRIS);
                // Directional lights (especially the sun) are cheap and must never disappear due
                // to stochastic reservoir selection. Final shading evaluates them directly with
                // one shadow ray each; the ReSTIR reservoir is reserved for local lights.
                diInitPush.lightSampling.numInfiniteLightSamples = 0;
                cmd.pushData(&diInitPush, sizeof(shaderio::PushConstantReSTIRDIInitial));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 2. Temporal resampling.
        m_renderGraph.AddPass("ReSTIR DI Temporal", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.PrevDepth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.PrevNormal);
                builder.Read(resources.GBufferVelocity);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.ReSTIRDIReservoirs[bufferC]);
                builder.Read(resources.ReSTIRDIReservoirs[bufferA]);
                builder.Write(resources.ReSTIRDIReservoirs[bufferA]);
                builder.ColorTarget(resources.ReSTIRDIDiffuse, NRI::LoadOP::dontCare, NRI::StoreOP::dontCare);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, bufferA, bufferC, blockRowPitch, arrayPitch](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDITemporalPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDITemporal diTPush{};
                diTPush.invViewProj = uniformData.invViewProj;
                diTPush.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
                diTPush.cameraWorldPos = uniformData.cameraWorldPos;
                diTPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diTPush.lightDataReference = uniformData.lightDataReference;
                diTPush.currentReservoirReference = context.Buffer(res->ReSTIRDIReservoirs[bufferA]).getDeviceAddress();
                diTPush.previousReservoirReference = context.Buffer(res->ReSTIRDIReservoirs[bufferC]).getDeviceAddress();
                diTPush.depthTextureIndex = context.Slot(res->Depth);
                diTPush.prevDepthTextureIndex = context.Slot(res->PrevDepth);
                diTPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                diTPush.prevNormalTextureIndex = context.Slot(res->PrevNormal);
                diTPush.gbufferVelocityIndex = context.Slot(res->GBufferVelocity);
                diTPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                diTPush.viewportSize = glm::vec2(rw, rh);
                diTPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diTPush.reservoirBlockRowPitch = blockRowPitch;
                diTPush.reservoirArrayPitch = arrayPitch;
                diTPush.maxHistoryLength = m_restirDIMaxHistoryLength;
                diTPush.normalThreshold = m_restirDINormalThreshold;
                diTPush.depthThreshold = m_restirDIDepthThreshold;
                diTPush.enablePermutationSampling = 1;
                cmd.pushData(&diTPush, sizeof(shaderio::PushConstantReSTIRDITemporal));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 3. Spatial resampling.
        m_renderGraph.AddPass("ReSTIR DI Spatial", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.ReSTIRDIReservoirs[bufferA]);
                builder.Read(resources.NeighborOffsets);
                builder.Write(resources.ReSTIRDIReservoirs[bufferB]);
                builder.ColorTarget(resources.ReSTIRDIDiffuse, NRI::LoadOP::dontCare, NRI::StoreOP::dontCare);
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, bufferA, bufferB, blockRowPitch, arrayPitch](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDISpatialPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRDISpatial diSPush{};
                diSPush.invViewProj = uniformData.invViewProj;
                diSPush.cameraWorldPos = uniformData.cameraWorldPos;
                diSPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diSPush.lightDataReference = uniformData.lightDataReference;
                diSPush.inputReservoirReference = context.Buffer(res->ReSTIRDIReservoirs[bufferA]).getDeviceAddress();
                diSPush.outputReservoirReference = context.Buffer(res->ReSTIRDIReservoirs[bufferB]).getDeviceAddress();
                diSPush.neighborOffsetsReference = context.Buffer(res->NeighborOffsets).getDeviceAddress();
                diSPush.depthTextureIndex = context.Slot(res->Depth);
                diSPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                diSPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                diSPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diSPush.viewportSize = glm::vec2(rw, rh);
                diSPush.reservoirBlockRowPitch = blockRowPitch;
                diSPush.reservoirArrayPitch = arrayPitch;
                diSPush.samplingRadius = m_restirDISpatialRadius;
                diSPush.numSamples = m_restirDINumSpatialSamples;
                diSPush.normalThreshold = m_restirDINormalThreshold;
                diSPush.depthThreshold = m_restirDIDepthThreshold;
                diSPush.neighborOffsetMask = 127;
                cmd.pushData(&diSPush, sizeof(shaderio::PushConstantReSTIRDISpatial));

                cmd.drawMeshTasks(1, 1, 1);
            });

        }

        // Whether NRD denoises the result this frame: final shading packs for it, the lighting unpacks the same way.
        const bool denoiseDI = m_nrdDIDenoiser != NRI::NRDDiffuseDenoiser::Off && m_device->isNRDInitialized();

        // 4. Final shading: one shadow ray per pixel toward whichever light survived resampling (the brute-force loop in
        // DeferredLighting.slang only shadows light 0).
        m_renderGraph.AddPass("ReSTIR DI Final Shading", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferAlbedo);
                builder.Read(resources.GBufferMaterial);
                builder.Read(resources.ReSTIRDIReservoirs[bufferB]);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                builder.ColorTarget(resources.ReSTIRDIDiffuse, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.ColorTarget(resources.ReSTIRDISpecular, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 0.0f });
                builder.SetRenderArea(m_frame.renderExtent);
            },
            [this, res = &resources, bufferB, blockRowPitch, arrayPitch, denoiseDI](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirDIFinalShadingPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                // Both targets: diffuse and specular.
                for (uint32_t attachment = 0; attachment < 2; ++attachment)
                {
                    cmd.setColorBlendEnable(attachment, false);
                    cmd.setColorWriteMask(attachment, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                                      NRI::ColorComponent::B | NRI::ColorComponent::A);
                }

                shaderio::PushConstantReSTIRDIFinalShading diFPush{};
                diFPush.invViewProj = uniformData.invViewProj;
                diFPush.cameraWorldPos = uniformData.cameraWorldPos;
                diFPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                diFPush.lightDataReference = uniformData.lightDataReference;
                diFPush.reservoirReference = context.Buffer(res->ReSTIRDIReservoirs[bufferB]).getDeviceAddress();
                diFPush.depthTextureIndex = context.Slot(res->Depth);
                diFPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                diFPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                diFPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                diFPush.viewportSize = glm::vec2(rw, rh);
                diFPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                diFPush.reservoirBlockRowPitch = blockRowPitch;
                diFPush.reservoirArrayPitch = arrayPitch;
                // Packed for the denoiser that runs (the lighting unpacks with the same mode), plain without one.
                diFPush.denoiserMode = denoiseDI ? static_cast<uint32_t>(m_nrdDIDenoiser) : 0u;
                diFPush.firstInfiniteLightIndex = m_restirDIFirstInfiniteLight;
                diFPush.numInfiniteLights = m_restirDINumInfiniteLights;
                cmd.pushData(&diFPush, sizeof(shaderio::PushConstantReSTIRDIFinalShading));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 5. NRD diffuse + specular (REBLUR / RELAX) of the 1-SPP direct lighting.
        if (denoiseDI)
        {
            m_frame.nrdDIAdded = true;
            m_renderGraph.AddPass("NRD DI", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.ReSTIRDIDiffuse);
                    builder.Read(resources.ReSTIRDISpecular);
                    builder.Read(resources.GBufferVelocity);
                    builder.Read(resources.NRDNormalRoughness);
                    builder.Read(resources.ViewZ);
                    builder.Write(resources.ReSTIRDIDiffuseDenoised);
                    builder.Write(resources.ReSTIRDISpecularDenoised);
                    builder.RecordExclusive("NRD");
                },
                [this, res = &resources](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();

                    NRI::NRDDiffuseParams diDenoiseParams{};
                    diDenoiseParams.inDiffuseRadianceHitDist = &context.Texture(res->ReSTIRDIDiffuse);
                    diDenoiseParams.inSpecularRadianceHitDist = &context.Texture(res->ReSTIRDISpecular);
                    diDenoiseParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                    diDenoiseParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                    diDenoiseParams.inViewZ = &context.Texture(res->ViewZ);
                    diDenoiseParams.outDenoisedDiffuse = &context.Texture(res->ReSTIRDIDiffuseDenoised);
                    diDenoiseParams.outDenoisedSpecular = &context.Texture(res->ReSTIRDISpecularDenoised);
                    diDenoiseParams.commandBuffer = &cmd;

                    diDenoiseParams.view = uniformData.view;
                    diDenoiseParams.proj = uniformData.nonJitteredProj;
                    diDenoiseParams.prevView = uniformData.prevView;
                    diDenoiseParams.prevProj = uniformData.prevProj;

                    diDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                    diDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    diDenoiseParams.resetHistory = m_isFirstFrame || m_frame.resetNRD;

                    // Lighting reads the denoised result (resolveFrameUniforms): evaluation only fails without NRD.
                    m_device->evaluateNRDDirectLighting(diDenoiseParams, m_nrdDIDenoiser);
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                });
        }

        m_renderGraph.PopGroup();
    }

    void Renderer::addReSTIRPTPasses()
    {
        const bool runReSTIRPT = m_frame.runPathTracer && m_restirPTEnabled && m_restirPTInitialPipeline && m_restirPTFinalShadingPipeline &&
                                 m_restirPTContext && (uniformData.tlasDeviceAddress != 0) && (uniformData.lightDataReference != 0);
        if (!runReSTIRPT)
            return;

        FrameGraphResources& resources = m_renderGraph.GetBlackboard().Get<FrameGraphResources>();
        FrameGraphState& frame = m_frame;
        frame.restirPTActive = true;

        m_restirPTContext->SetFrameIndex(static_cast<uint32_t>(m_sceneFrameCounter));
        const RTXDI_PTBufferIndices ptBufferIndices = m_restirPTContext->GetBufferIndices();
        const RTXDI_ReservoirBufferParameters ptResParams = m_restirPTContext->GetReservoirBufferParameters();
        const uint32_t blockRowPitch = ptResParams.reservoirBlockRowPitch;
        const uint32_t arrayPitch = ptResParams.reservoirArrayPitch;
        // RTXDI_PackedPTReservoir per element; the context rotates which of the 3 plays which role.
        const RGBufferHistory reservoirs = m_renderGraph.GetHistoryBuffer("ReSTIR PT Reservoirs",
            RGBufferDesc{ static_cast<uint64_t>(arrayPitch) * sizeof(RTXDI_PackedPTReservoir) }, 3);
        for (uint32_t i = 0; i < 3; ++i)
            resources.ReSTIRPTReservoirs[i] = reservoirs.Buffers[i];
        frame.ptInitialOutputBuffer = ptBufferIndices.initialPathTracerOutputBufferIndex;
        frame.ptInitialPreservedBuffer = ptBufferIndices.initialPathTracerPreservedBufferIndex;
        frame.ptTemporalInputBuffer = ptBufferIndices.temporalResamplingInputBufferIndex;
        frame.ptFinalShadingInputBuffer = ptBufferIndices.finalShadingInputBufferIndex;
        // Temporal resampling only runs when the resampling mode needs it, so the buffer indices genuinely differ (None
        // mode leaves both equal to 0, matching Initial's own gate).
        frame.restirPTTemporalActive = m_restirPTTemporalPipeline && frame.ptTemporalInputBuffer != frame.ptInitialOutputBuffer;
        // NRD PT flushes its history when the camera moved (shared with the plain path tracer's view tracking).
        frame.restirPTCameraMoved = (uniformData.view != m_pathTracerPrevView);
        m_pathTracerPrevView = uniformData.view;

        m_renderGraph.PushGroup("ReSTIR PT");

        // 1. Initial sampling.
        m_renderGraph.AddPass("ReSTIR PT Initial", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.GBufferAlbedo);
                builder.Read(resources.GBufferMaterial);
                ReadIfValid(builder, resources.TLAS);
                ReadGpuScene(builder, resources);
                builder.Write(resources.ReSTIRPTReservoirs[frame.ptInitialOutputBuffer]);
                builder.Write(resources.ReSTIRPTReservoirs[frame.ptInitialPreservedBuffer]);
                // Debug-only output (the real result lives in the reservoir buffer), bounce-1 direct lighting.
                builder.ColorTarget(resources.ReSTIRPTOutput, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.ColorTarget(resources.ReSTIRPTPrimaryDirect, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.SetRenderArea(frame.renderExtent);
            },
            [this, res = &resources, blockRowPitch, arrayPitch](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                constexpr uint32_t attachmentCount = 2;
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);
                glm::mat4 ptViewProj = uniformData.proj * uniformData.view;

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirPTInitialPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                for (uint32_t a = 0; a < attachmentCount; ++a)
                {
                    cmd.setColorBlendEnable(a, false);
                    cmd.setColorWriteMask(a, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                             NRI::ColorComponent::B | NRI::ColorComponent::A);
                }

                shaderio::PushConstantReSTIRPTInitial ptiPush{};
                ptiPush.invViewProj = glm::inverse(ptViewProj);
                ptiPush.cameraWorldPos = uniformData.cameraWorldPos;
                ptiPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                ptiPush.lightDataReference = uniformData.lightDataReference;
                ptiPush.reservoirBufferReference = context.Buffer(res->ReSTIRPTReservoirs[m_frame.ptInitialOutputBuffer]).getDeviceAddress();
                ptiPush.preservedReservoirReference = context.Buffer(res->ReSTIRPTReservoirs[m_frame.ptInitialPreservedBuffer]).getDeviceAddress();
                ptiPush.depthTextureIndex = context.Slot(res->Depth);
                ptiPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                ptiPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                ptiPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                ptiPush.viewportSize = glm::vec2(rw, rh);
                ptiPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptiPush.reservoirBlockRowPitch = blockRowPitch;
                ptiPush.reservoirArrayPitch = arrayPitch;
                ptiPush.firstLocalLightIndex = m_restirDIFirstLocalLight;
                ptiPush.numLocalLights = m_restirDINumLocalLights;
                ptiPush.firstInfiniteLightIndex = m_restirDIFirstInfiniteLight;
                ptiPush.numInfiniteLights = m_restirDINumInfiniteLights;
                ptiPush.numInitialSamples = m_restirPTNumInitialSamples;
                ptiPush.maxBounceDepth = m_restirPTMaxBounceDepth;
                ptiPush.maxRcVertexLength = m_restirPTMaxRcVertexLength;
                ptiPush.numNeeSamples = m_restirPTNumNeeSamples;
                ptiPush.roughnessThreshold = m_restirPTRoughnessThreshold;
                ptiPush.distanceThreshold = m_restirPTDistanceThreshold;
                ptiPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                cmd.pushData(&ptiPush, sizeof(shaderio::PushConstantReSTIRPTInitial));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // 2. Temporal resampling (RandomReplay/hybrid-shift reconnection against last frame's finalized reservoir).
        if (frame.restirPTTemporalActive)
        {
            m_renderGraph.AddPass("ReSTIR PT Temporal", RGPassFlags::Raster,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.Depth);
                    builder.Read(resources.PrevDepth);
                    builder.Read(resources.GBufferNormal);
                    builder.Read(resources.PrevNormal);
                    builder.Read(resources.GBufferAlbedo);
                    builder.Read(resources.PrevAlbedo);
                    builder.Read(resources.GBufferMaterial);
                    builder.Read(resources.PrevMaterial);
                    builder.Read(resources.GBufferVelocity);
                    builder.Read(resources.ReSTIRPTReservoirs[frame.ptTemporalInputBuffer]);
                    builder.Read(resources.ReSTIRPTReservoirs[frame.ptInitialOutputBuffer]);
                    builder.Write(resources.ReSTIRPTReservoirs[frame.ptInitialOutputBuffer]);
                    builder.ColorTarget(resources.ReSTIRPTOutput, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                    builder.SetRenderArea(frame.renderExtent);
                },
                [this, res = &resources, blockRowPitch, arrayPitch](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();
                    const float rw = static_cast<float>(m_frame.renderExtent.width);
                    const float rh = static_cast<float>(m_frame.renderExtent.height);

                    cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirPTTemporalPipeline);
                    cmd.setCullMode(NRI::CullMode::None);
                    cmd.setDepthTestEnable(false);
                    cmd.setDepthWriteEnable(false);
                    cmd.setColorBlendEnable(0, false);
                    cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                             NRI::ColorComponent::B | NRI::ColorComponent::A);

                    shaderio::PushConstantReSTIRPTTemporal pttPush{};
                    // invViewProj deliberately omitted - the shader reads it from g_UBO->invViewProj (same value) to fit
                    // the struct under the device's 256-byte push-constant limit.
                    pttPush.prevInvViewProj = glm::inverse(uniformData.prevProj * uniformData.prevView);
                    pttPush.cameraWorldPos = uniformData.cameraWorldPos;
                    pttPush.prevCameraWorldPos = glm::vec4(m_prevCameraWorldPos, 0.0f);
                    pttPush.prevPrevCameraWorldPos = glm::vec4(m_prevPrevCameraWorldPos, 0.0f);
                    pttPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                    pttPush.lightDataReference = uniformData.lightDataReference;
                    pttPush.currentReservoirReference = context.Buffer(res->ReSTIRPTReservoirs[m_frame.ptInitialOutputBuffer]).getDeviceAddress();
                    pttPush.historyReservoirReference = context.Buffer(res->ReSTIRPTReservoirs[m_frame.ptTemporalInputBuffer]).getDeviceAddress();
                    pttPush.viewportSize = glm::vec2(rw, rh);
                    pttPush.depthTextureIndex = context.Slot(res->Depth);
                    pttPush.prevDepthTextureIndex = context.Slot(res->PrevDepth);
                    pttPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                    pttPush.prevNormalTextureIndex = context.Slot(res->PrevNormal);
                    pttPush.gbufferAlbedoIndex = context.Slot(res->GBufferAlbedo);
                    pttPush.prevAlbedoTextureIndex = context.Slot(res->PrevAlbedo);
                    pttPush.gbufferMaterialIndex = context.Slot(res->GBufferMaterial);
                    pttPush.prevMaterialTextureIndex = context.Slot(res->PrevMaterial);
                    pttPush.gbufferVelocityIndex = context.Slot(res->GBufferVelocity);
                    pttPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    pttPush.reservoirBlockRowPitch = blockRowPitch;
                    pttPush.reservoirArrayPitch = arrayPitch;
                    pttPush.maxBounceDepth = m_restirPTMaxBounceDepth;
                    pttPush.maxRcVertexLength = m_restirPTMaxRcVertexLength;
                    pttPush.roughnessThreshold = m_restirPTRoughnessThreshold;
                    pttPush.distanceThreshold = m_restirPTDistanceThreshold;
                    pttPush.depthThreshold = m_restirPTDepthThreshold;
                    pttPush.normalThreshold = m_restirPTNormalThreshold;
                    pttPush.maxHistoryLength = m_restirPTMaxHistoryLength;
                    pttPush.maxReservoirAge = m_restirPTMaxReservoirAge;
                    pttPush.enablePermutationSampling = m_restirPTEnablePermutationSampling ? 1u : 0u;
                    pttPush.skyboxTextureIndex = m_environmentCubemap ? m_environmentCubemap->GetDescriptorIndexSlot() : 0xFFFFFFFF;
                    cmd.pushData(&pttPush, sizeof(shaderio::PushConstantReSTIRPTTemporal));

                    cmd.drawMeshTasks(1, 1, 1);
                });
        }

        // 3. Final shading.
        const bool denoiseReSTIRPT = m_nrdPTDenoiser != NRI::NRDDiffuseDenoiser::Off && m_device->isNRDInitialized();
        m_renderGraph.AddPass("ReSTIR PT Final Shading", RGPassFlags::Raster,
            [&](RGBuilder& builder)
            {
                builder.Read(resources.Depth);
                builder.Read(resources.GBufferNormal);
                builder.Read(resources.ReSTIRPTPrimaryDirect);
                builder.Read(resources.ReSTIRPTReservoirs[frame.ptFinalShadingInputBuffer]);
                builder.Read(resources.ReSTIRPTReservoirs[frame.ptInitialPreservedBuffer]);
                builder.ColorTarget(resources.ReSTIRPTOutput, NRI::LoadOP::clear, NRI::StoreOP::store, { 0.0f, 0.0f, 0.0f, 1.0f });
                builder.SetRenderArea(frame.renderExtent);
            },
            [this, res = &resources, blockRowPitch, arrayPitch, denoiseReSTIRPT](RGPassContext& context)
            {
                NRI::CommandBuffer& cmd = context.Cmd();
                const float rw = static_cast<float>(m_frame.renderExtent.width);
                const float rh = static_cast<float>(m_frame.renderExtent.height);
                glm::mat4 ptViewProj = uniformData.proj * uniformData.view;

                cmd.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_restirPTFinalShadingPipeline);
                cmd.setCullMode(NRI::CullMode::None);
                cmd.setDepthTestEnable(false);
                cmd.setDepthWriteEnable(false);
                cmd.setColorBlendEnable(0, false);
                cmd.setColorWriteMask(0, NRI::ColorComponent::R | NRI::ColorComponent::G |
                                         NRI::ColorComponent::B | NRI::ColorComponent::A);

                shaderio::PushConstantReSTIRPTFinalShading ptfPush{};
                ptfPush.invViewProj = glm::inverse(ptViewProj);
                ptfPush.cameraWorldPos = uniformData.cameraWorldPos;
                ptfPush.matrixReference = m_uniformBuffers[frameIndex]->getDeviceAddress();
                ptfPush.lightDataReference = uniformData.lightDataReference;
                ptfPush.reservoirReference = context.Buffer(res->ReSTIRPTReservoirs[m_frame.ptFinalShadingInputBuffer]).getDeviceAddress();
                ptfPush.preservedReservoirReference = context.Buffer(res->ReSTIRPTReservoirs[m_frame.ptInitialPreservedBuffer]).getDeviceAddress();
                ptfPush.depthTextureIndex = context.Slot(res->Depth);
                ptfPush.gbufferNormalIndex = context.Slot(res->GBufferNormal);
                ptfPush.primaryDirectTextureIndex = context.Slot(res->ReSTIRPTPrimaryDirect);
                ptfPush.viewportSize = glm::vec2(rw, rh);
                ptfPush.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                ptfPush.reservoirBlockRowPitch = blockRowPitch;
                ptfPush.reservoirArrayPitch = arrayPitch;
                // Shares the "PT Denoiser" setting with the plain path tracer: REBLUR needs the shader's own YCoCg
                // pre-encode (LinearToYCoCg in ReSTIRPTFinalShading.slang), RELAX reads plain linear color.
                ptfPush.denoiserMode = denoiseReSTIRPT ? static_cast<uint32_t>(m_nrdPTDenoiser) : 0u;
                // RTXDI PT's final-shading decorrelation: randomly use the preserved, unresampled initial reservoir to break
                // temporal over-correlation. Stagnancy mode needs the SDK duplication-map pass, so Uniform mode here.
                ptfPush.decorrelationFactor = m_frame.restirPTTemporalActive ? 0.4f : 0.0f;
                ptfPush.decorrelationMode = m_frame.restirPTTemporalActive ? 1u : 0u; // RTXDI_PT_DECORRELATION_MODE_UNIFORM/NONE
                cmd.pushData(&ptfPush, sizeof(shaderio::PushConstantReSTIRPTFinalShading));

                cmd.drawMeshTasks(1, 1, 1);
            });

        // Shares m_nrdPTDenoiser and the denoised path tracer texture with the plain path tracer: only one of the two runs
        // per frame. DLSS-RR needs nothing extra: it takes the ReSTIR PT output as its input color.

        if (denoiseReSTIRPT)
        {
            frame.ptNRDAdded = true;
            m_renderGraph.AddPass("NRD PT", RGPassFlags::None,
                [&](RGBuilder& builder)
                {
                    builder.Read(resources.ReSTIRPTOutput);
                    builder.Read(resources.GBufferVelocity);
                    builder.Read(resources.NRDNormalRoughness);
                    builder.Read(resources.ViewZ);
                    builder.Write(resources.PathTracerDenoised);
                    builder.RecordExclusive("NRD");
                },
                [this, res = &resources](RGPassContext& context)
                {
                    NRI::CommandBuffer& cmd = context.Cmd();

                    NRI::NRDDiffuseParams ptDenoiseParams{};
                    ptDenoiseParams.inDiffuseRadianceHitDist = &context.Texture(res->ReSTIRPTOutput);
                    ptDenoiseParams.inMotionVectors = &context.Texture(res->GBufferVelocity);
                    ptDenoiseParams.inNormalRoughness = &context.Texture(res->NRDNormalRoughness);
                    ptDenoiseParams.inViewZ = &context.Texture(res->ViewZ);
                    ptDenoiseParams.outDenoisedDiffuse = &context.Texture(res->PathTracerDenoised);
                    ptDenoiseParams.commandBuffer = &cmd;

                    ptDenoiseParams.view = uniformData.view;
                    ptDenoiseParams.proj = uniformData.nonJitteredProj;
                    ptDenoiseParams.prevView = uniformData.prevView;
                    ptDenoiseParams.prevProj = uniformData.prevProj;

                    ptDenoiseParams.motionVectorScale = glm::vec2(1.0f, 1.0f);
                    ptDenoiseParams.frameIndex = static_cast<uint32_t>(m_sceneFrameCounter);
                    ptDenoiseParams.resetHistory = m_isFirstFrame || m_frame.resetNRD || m_frame.restirPTCameraMoved;

                    m_device->evaluateNRDDiffusePT(ptDenoiseParams, m_nrdPTDenoiser);
                    cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
                });

            if (m_nrdPTDenoiser == NRI::NRDDiffuseDenoiser::REBLUR && m_ycocgDecodePipeline)
            {
                // REBLUR's output is still YCoCg-encoded: decode in place before DLSS/tonemapping read it.
                m_renderGraph.AddPass("PT YCoCg Decode", RGPassFlags::None,
                    [&](RGBuilder& builder)
                    {
                        builder.Read(resources.PathTracerDenoised);
                        builder.Write(resources.PathTracerDenoised);
                    },
                    [this, res = &resources](RGPassContext& context)
                    {
                        NRI::CommandBuffer& cmd = context.Cmd();
                        cmd.bindPipeline(NRI::PipelineBindPoint::Compute, *m_ycocgDecodePipeline);
                        shaderio::PushConstantYCoCgDecode decodePush{};
                        decodePush.readTextureIndex = context.Slot(res->PathTracerDenoised);
                        decodePush.writeTextureIndex = context.StorageSlot(res->PathTracerDenoised);
                        decodePush.width = m_renderSize.width;
                        decodePush.height = m_renderSize.height;
                        cmd.pushData(&decodePush, sizeof(shaderio::PushConstantYCoCgDecode));
                        cmd.dispatch((m_renderSize.width + 7) / 8, (m_renderSize.height + 7) / 8, 1);
                    });
            }
        }

        m_renderGraph.PopGroup();
    }
}
