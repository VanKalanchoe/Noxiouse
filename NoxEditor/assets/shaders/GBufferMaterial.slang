 #include "shaderIO.h"

    struct GBufferPayload
    {
        uint dummy;
    };
    groupshared GBufferPayload gPayload;

    [shader("task")]
    [numthreads(1, 1, 1)]
    void taskMain()
    {
        DispatchMesh(1, 1, 1, gPayload);
    }

    struct MSOutput
    {
        float4 pos : SV_Position;
        float2 uv : TEXCOORD0;
    };

    static const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    static const float2 uvs[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };

    [shader("mesh")]
    [outputtopology("triangle")]
    [numthreads(3, 1, 1)]
    void meshMain(
        out indices uint3 triangles[1],
        out vertices MSOutput vertices[3],
        uint threadID : SV_GroupIndex,
        uniform PushConstantVisibilityDebug pushConstant
    )
    {
        SetMeshOutputCounts(3, 1);

        if (threadID < 3)
        {
            vertices[threadID].pos = float4(positions[threadID], 0.0, 1.0);
            vertices[threadID].uv = uvs[threadID];
        }

        if (threadID == 0)
        {
            triangles[0] = uint3(0, 1, 2);
        }
    }

    struct GBufferOutput
    {
        float4 albedo   : SV_Target0; // RGB: BaseColor, A: Occlusion
        float4 normal   : SV_Target1; // RGB: World Normal, A: 1.0
        float4 material : SV_Target2; // R: Roughness, G: Metallic, B: Workflow, A: 1.0
        float4 emission : SV_Target3; // RGB: Emissive, A: 1.0
        int    entityID : SV_Target4; // Editor Entity Picking ID
    };

    [shader("fragment")]
    GBufferOutput fragMain(
        MSOutput input,
        uniform PushConstantVisibilityDebug pushConstant
    )
    {
        GBufferOutput output;

        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));

        int2 pixelCoord = int2(input.pos.xy);
        uint4 rawData = visTex.Load(int3(pixelCoord, 0));
        uint2 visData = rawData.xy;

        // Unrendered background pixel
        if (visData.x == 0)
        {
            output.albedo = float4(0.0, 0.0, 0.0, 0.0);
            output.normal = float4(0.0, 0.0, 0.0, 0.0);
            output.material = float4(0.0, 0.0, 0.0, 0.0);
            output.emission = float4(0.0, 0.0, 0.0, 0.0);
            output.entityID = -1;
            return output;
        }

        // Decode Visibility Buffer IDs
        uint drawID = visData.x - 1;
        uint meshletID = visData.y >> 8;
        uint primitiveID = visData.y & 0xFF;

        // Pull Instance
        InstanceData* instances = (InstanceData*)pushConstant.instanceReference;
        InstanceData instance = instances[drawID];

        // Pull Page Tables
        uint64_t* drawsPageTable  = (uint64_t*)pushConstant.meshletDrawsPageTableReference;
        uint64_t* vertsPageTable  = (uint64_t*)pushConstant.vertexPageTableReference;
        uint64_t* mVertsPageTable = (uint64_t*)pushConstant.meshletVerticesPageTableReference;
        uint64_t* mTrisPageTable  = (uint64_t*)pushConstant.meshletTrianglesPageTableReference;

        MeshletDraw* meshletDrawPage = (MeshletDraw*)drawsPageTable[instance.drawsPageIndex];
        Vertex* vertexPage = (Vertex*)vertsPageTable[instance.verticesPageIndex];
        uint32_t* meshletVerticesPage = (uint32_t*)mVertsPageTable[instance.meshletVerticesPageIndex];
        uint8_t* meshletTrianglePage = (uint8_t*)mTrisPageTable[instance.meshletTrianglesPageIndex];

        MeshletDraw meshlet = meshletDrawPage[meshletID];

        // Fetch Triangle Indices
        uint triBase = meshlet.triangleOffset + (primitiveID * 3);
        uint i0 = meshletTrianglePage[triBase];
        uint i1 = meshletTrianglePage[triBase + 1];
        uint i2 = meshletTrianglePage[triBase + 2];

        // Fetch Local Vertex Indices
        uint vIdx0 = meshletVerticesPage[meshlet.vertexOffset + i0];
        uint vIdx1 = meshletVerticesPage[meshlet.vertexOffset + i1];
        uint vIdx2 = meshletVerticesPage[meshlet.vertexOffset + i2];

        // Fetch the 3 Vertices via BDA
        Vertex v0 = vertexPage[meshlet.globalVertexOffset + vIdx0];
        Vertex v1 = vertexPage[meshlet.globalVertexOffset + vIdx1];
        Vertex v2 = vertexPage[meshlet.globalVertexOffset + vIdx2];

        // Skinning
        float4 localPos0 = float4(v0.pos, 1.0);
        float4 localPos1 = float4(v1.pos, 1.0);
        float4 localPos2 = float4(v2.pos, 1.0);
        float3 localNorm0 = v0.normal;
        float3 localNorm1 = v1.normal;
        float3 localNorm2 = v2.normal;

        if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
        {
            float4x4* boneMatrices = (float4x4*)pushConstant.boneMatrixReference;
            uint baseOffset = instance.boneMatrixOffset;

            float4x4 skin0 = boneMatrices[baseOffset + v0.boneIDs.x] * v0.boneWeights.x +
                             boneMatrices[baseOffset + v0.boneIDs.y] * v0.boneWeights.y +
                             boneMatrices[baseOffset + v0.boneIDs.z] * v0.boneWeights.z +
                             boneMatrices[baseOffset + v0.boneIDs.w] * v0.boneWeights.w;
            localPos0 = mul(skin0, localPos0);
            localNorm0 = mul((float3x3)skin0, localNorm0);

            float4x4 skin1 = boneMatrices[baseOffset + v1.boneIDs.x] * v1.boneWeights.x +
                             boneMatrices[baseOffset + v1.boneIDs.y] * v1.boneWeights.y +
                             boneMatrices[baseOffset + v1.boneIDs.z] * v1.boneWeights.z +
                             boneMatrices[baseOffset + v1.boneIDs.w] * v1.boneWeights.w;
            localPos1 = mul(skin1, localPos1);
            localNorm1 = mul((float3x3)skin1, localNorm1);

            float4x4 skin2 = boneMatrices[baseOffset + v2.boneIDs.x] * v2.boneWeights.x +
                             boneMatrices[baseOffset + v2.boneIDs.y] * v2.boneWeights.y +
                             boneMatrices[baseOffset + v2.boneIDs.z] * v2.boneWeights.z +
                             boneMatrices[baseOffset + v2.boneIDs.w] * v2.boneWeights.w;
            localPos2 = mul(skin2, localPos2);
            localNorm2 = mul((float3x3)skin2, localNorm2);
        }

        // World & Screen Clip Space
        float4x4 viewProj = mul(ubo->proj, ubo->view);
        float4 worldPos0 = mul(instance.modelMatrix, localPos0);
        float4 worldPos1 = mul(instance.modelMatrix, localPos1);
        float4 worldPos2 = mul(instance.modelMatrix, localPos2);

        float4 clip0 = mul(viewProj, worldPos0);
        float4 clip1 = mul(viewProj, worldPos1);
        float4 clip2 = mul(viewProj, worldPos2);

        float2 s0 = float2((clip0.x / clip0.w + 1.0) * 0.5 * pushConstant.viewportSize.x,
                           (1.0 - clip0.y / clip0.w) * 0.5 * pushConstant.viewportSize.y);
        float2 s1 = float2((clip1.x / clip1.w + 1.0) * 0.5 * pushConstant.viewportSize.x,
                           (1.0 - clip1.y / clip1.w) * 0.5 * pushConstant.viewportSize.y);
        float2 s2 = float2((clip2.x / clip2.w + 1.0) * 0.5 * pushConstant.viewportSize.x,
                           (1.0 - clip2.y / clip2.w) * 0.5 * pushConstant.viewportSize.y);

        float2 P = input.pos.xy;

        float2 e0 = s1 - s0;
        float2 e1 = s2 - s0;
        float2 ep = P - s0;

        float det = e0.x * e1.y - e1.x * e0.y;
        float invDet = (abs(det) > 1e-7) ? (1.0 / det) : 0.0;

        float l1 = (ep.x * e1.y - e1.x * ep.y) * invDet;
        float l2 = (e0.x * ep.y - ep.x * e0.y) * invDet;
        float l0 = 1.0 - l1 - l2;

        float invW0 = 1.0 / clip0.w;
        float invW1 = 1.0 / clip1.w;
        float invW2 = 1.0 / clip2.w;

        float w0 = max(0.0, l0) * invW0;
        float w1 = max(0.0, l1) * invW1;
        float w2 = max(0.0, l2) * invW2;
        float sumW = w0 + w1 + w2;
        float invSumW = (sumW > 1e-7) ? (1.0 / sumW) : 0.0;
        w0 *= invSumW;
        w1 *= invSumW;
        w2 *= invSumW;

        // Intra-triangle Screen-Space Differentials (+1px in X, +1px in Y)
        // Avoids cross-triangle derivative jumps (eliminating scissor line artifacts)
        float l0_dx = l0 + (-e1.y + e0.y) * invDet;
        float l1_dx = l1 + e1.y * invDet;
        float l2_dx = l2 - e0.y * invDet;

        float w0_dx = l0_dx * invW0;
        float w1_dx = l1_dx * invW1;
        float w2_dx = l2_dx * invW2;
        float sumW_dx = w0_dx + w1_dx + w2_dx;
        float invSumW_dx = (abs(sumW_dx) > 1e-7) ? (1.0 / sumW_dx) : 0.0;
        w0_dx *= invSumW_dx;
        w1_dx *= invSumW_dx;
        w2_dx *= invSumW_dx;

        float l0_dy = l0 + (e1.x - e0.x) * invDet;
        float l1_dy = l1 - e1.x * invDet;
        float l2_dy = l2 + e0.x * invDet;

        float w0_dy = l0_dy * invW0;
        float w1_dy = l1_dy * invW1;
        float w2_dy = l2_dy * invW2;
        float sumW_dy = w0_dy + w1_dy + w2_dy;
        float invSumW_dy = (abs(sumW_dy) > 1e-7) ? (1.0 / sumW_dy) : 0.0;
        w0_dy *= invSumW_dy;
        w1_dy *= invSumW_dy;
        w2_dy *= invSumW_dy;

        // Reconstruct Attributes & Exact Differentials
        float2 uv0 = v0.uv0 * w0 + v1.uv0 * w1 + v2.uv0 * w2;
        float2 uv0_dx = v0.uv0 * w0_dx + v1.uv0 * w1_dx + v2.uv0 * w2_dx;
        float2 uv0_dy = v0.uv0 * w0_dy + v1.uv0 * w1_dy + v2.uv0 * w2_dy;
        float2 ddx_uv0 = uv0_dx - uv0;
        float2 ddy_uv0 = uv0_dy - uv0;

        float2 uv1 = v0.uv1 * w0 + v1.uv1 * w1 + v2.uv1 * w2;
        float2 uv1_dx = v0.uv1 * w0_dx + v1.uv1 * w1_dx + v2.uv1 * w2_dx;
        float2 uv1_dy = v0.uv1 * w0_dy + v1.uv1 * w1_dy + v2.uv1 * w2_dy;
        float2 ddx_uv1 = uv1_dx - uv1;
        float2 ddy_uv1 = uv1_dy - uv1;

        float3 worldNorm0 = mul((float3x3)instance.normalMatrix, localNorm0);
        float3 worldNorm1 = mul((float3x3)instance.normalMatrix, localNorm1);
        float3 worldNorm2 = mul((float3x3)instance.normalMatrix, localNorm2);
        float3 geomNormal = normalize(worldNorm0 * w0 + worldNorm1 * w1 + worldNorm2 * w2);

        float3 worldPos = worldPos0.xyz * w0 + worldPos1.xyz * w1 + worldPos2.xyz * w2;
        float3 worldPos_dx = worldPos0.xyz * w0_dx + worldPos1.xyz * w1_dx + worldPos2.xyz * w2_dx;
        float3 worldPos_dy = worldPos0.xyz * w0_dy + worldPos1.xyz * w1_dy + worldPos2.xyz * w2_dy;
        float3 ddx_pos = worldPos_dx - worldPos;
        float3 ddy_pos = worldPos_dy - worldPos;

        SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        // 1. BASE COLOR (Albedo)
        float2 baseColorUV = (instance.baseColorTextureSet == 0) ? uv0 : uv1;
        float2 ddx_bc = (instance.baseColorTextureSet == 0) ? ddx_uv0 : ddx_uv1;
        float2 ddy_bc = (instance.baseColorTextureSet == 0) ? ddy_uv0 : ddy_uv1;
        float4 baseColor = instance.baseColorFactor;
        if (instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = baseColorTex.SampleGrad(samp, baseColorUV, ddx_bc, ddy_bc) * instance.baseColorFactor;
        }

        // Alpha Mask discard
        if (instance.alphaMode == 1 && baseColor.a < instance.alphaMaskCutoff)
        {
            discard;
        }

        // 2. NORMAL MAPPING (Analytical Tangent Frame from 3D Triangle Geometry)
            float3 finalNormal = geomNormal;
            float3 rawNormalTex = geomNormal * 0.5 + 0.5; // fallback matching Sascha's normalize(inNormal)
            if (instance.normalTextureIndex != 0xFFFFFFFF)
            {
                float2 normalUV = (instance.normalTextureSet == 0) ? uv0 : uv1;
                Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.normalTextureIndex,
  0));
                rawNormalTex = normalTex.SampleLevel(samp, normalUV, 0.0).xyz;
                float3 tangentNormal = rawNormalTex * 2.0 - 1.0;

                // Compute triangle edge vectors in world space and UV space
                float3 dp1 = worldPos1.xyz - worldPos0.xyz;
                float3 dp2 = worldPos2.xyz - worldPos0.xyz;
                float2 duv1 = (instance.normalTextureSet == 0) ? (v1.uv0 - v0.uv0) : (v1.uv1 - v0.uv1);
                float2 duv2 = (instance.normalTextureSet == 0) ? (v2.uv0 - v0.uv0) : (v2.uv1 - v0.uv1);

                float detUV = duv1.x * duv2.y - duv2.x * duv1.y;
                float3 T = dp1 * duv2.y - dp2 * duv1.y;
                float3 B = dp2 * duv1.x - dp1 * duv2.x;

                float3 N = geomNormal;
                float lenT = length(T);
                if (lenT > 1e-12 && abs(detUV) > 1e-12)
                {
                    // Gram-Schmidt orthonormalization
                    T = normalize(T - N * dot(N, T));
                    float3 B_ortho = normalize(cross(N, T));
                    if (dot(B_ortho, B) < 0.0)
                    {
                        B_ortho = -B_ortho;
                    }
                    float3x3 TBN = float3x3(T, B_ortho, N);
                    finalNormal = normalize(mul(tangentNormal, TBN));
                }
            }
            else
            {
                // Visual Diagnostic: Bright red if normal texture is not bound
                finalNormal = geomNormal;
            }

        // 3. METALLIC & ROUGHNESS
        float roughness = instance.roughnessFactor;
        float metallic = instance.metallicFactor;
        if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            float2 mrUV = (instance.physicalDescriptorTextureSet == 0) ? uv0 : uv1;
            float2 ddx_mr = (instance.physicalDescriptorTextureSet == 0) ? ddx_uv0 : ddx_uv1;
            float2 ddy_mr = (instance.physicalDescriptorTextureSet == 0) ? ddy_uv0 : ddy_uv1;
            Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            float4 mrSample = mrTex.SampleGrad(samp, mrUV, ddx_mr, ddy_mr);
            roughness *= mrSample.g;
            metallic  *= mrSample.b;
        }
        roughness = clamp(roughness, 0.0, 1.0);
        metallic  = clamp(metallic, 0.0, 1.0);

        // 4. OCCLUSION
        float occlusion = 1.0;
        if (instance.occlusionTextureIndex != 0xFFFFFFFF)
        {
            float2 occUV = (instance.occlusionTextureSet == 0) ? uv0 : uv1;
            float2 ddx_occ = (instance.occlusionTextureSet == 0) ? ddx_uv0 : ddx_uv1;
            float2 ddy_occ = (instance.occlusionTextureSet == 0) ? ddy_uv0 : ddy_uv1;
            Texture2D occTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.occlusionTextureIndex, 0));
            occlusion = occTex.SampleGrad(samp, occUV, ddx_occ, ddy_occ).r;
        }

        // 5. EMISSION
        float3 emission = instance.emissiveFactor * instance.emissiveStrength;
        if (instance.emissiveTextureIndex != 0xFFFFFFFF)
        {
            float2 emUV = (instance.emissiveTextureSet == 0) ? uv0 : uv1;
            float2 ddx_em = (instance.emissiveTextureSet == 0) ? ddx_uv0 : ddx_uv1;
            float2 ddy_em = (instance.emissiveTextureSet == 0) ? ddy_uv0 : ddy_uv1;
            Texture2D emTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.emissiveTextureIndex, 0));
            emission *= emTex.SampleGrad(samp, emUV, ddx_em, ddy_em).rgb;
        }

        // Output to MRT
        output.albedo   = float4(baseColor.rgb, occlusion);
        output.normal   = (pushConstant.debugMode == 2) ? float4(rawNormalTex, 1.0) : float4(finalNormal, 1.0);
        output.material = float4(roughness, metallic, instance.workflow, 1.0);
        output.emission = float4(emission, 1.0);
        output.entityID = instance.entityID;

        return output;
    }