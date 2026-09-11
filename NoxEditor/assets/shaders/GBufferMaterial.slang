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
        float4 normal   : SV_Target2; // RGB: World Normal, A: 1.0
        float4 material : SV_Target3; // R: Roughness, G: Metallic, B: Workflow, A: 1.0
        float4 emission : SV_Target4; // RGB: Emissive, A: 1.0
        int    entityID : SV_Target5; // Editor Entity Picking ID
        float2 velocity : SV_Target6; // Screen-Space Motion Vectors (RG16F)
        float4 specular : SV_Target1; // RGB: Specular Albedo (F0), A: 1.0
    };

    float4 computeBarycentric3D(float3 rayOrigin, float3 rayDir, float3 v0, float3 e1, float3 e2, float3 triNorm, float d00, float d01, float d11, float invDenom)
    {
        float normDotRay = dot(triNorm, rayDir);
        float t = (abs(normDotRay) > 1e-12) ? (dot(v0 - rayOrigin, triNorm) / normDotRay) : 0.0;
        float3 p = rayOrigin + t * rayDir;
        float3 dp = p - v0;
        float d20 = dot(dp, e1);
        float d21 = dot(dp, e2);
        float bw1 = (d11 * d20 - d01 * d21) * invDenom;
        float bw2 = (d00 * d21 - d01 * d20) * invDenom;
        float bw0 = 1.0 - bw1 - bw2;
        return float4(bw0, bw1, bw2, t);
    }

    static const float c_MinRoughness = 0.04;
    static const float PBR_WORKFLOW_METALLIC_ROUGHNESS = 0.0;
    static const float PBR_WORKFLOW_SPECULAR_GLOSSINESS = 1.0;

    // Convert sRGB color to linear RGB (Khronos IEC 61966-2-1 transfer function)
    float3 srgbToLinear(float3 c)
    {
        float3 bLess = step(c, float3(0.04045));
        return lerp(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, bLess);
    }

    // Gets metallic factor from specular glossiness workflow inputs (Khronos / Sascha Willems PBR formula)
    float convertMetallic(float3 diffuse, float3 specular, float maxSpecular)
    {
        float perceivedDiffuse = sqrt(0.299 * diffuse.r * diffuse.r + 0.587 * diffuse.g * diffuse.g + 0.114 * diffuse.b * diffuse.b);
        float perceivedSpecular = sqrt(0.299 * specular.r * specular.r + 0.587 * specular.g * specular.g + 0.114 * specular.b * specular.b);
        if (perceivedSpecular < c_MinRoughness)
        {
            return 0.0;
        }
        float a = c_MinRoughness;
        float b = perceivedDiffuse * (1.0 - maxSpecular) / (1.0 - c_MinRoughness) + perceivedSpecular - 2.0 * c_MinRoughness;
        float c = c_MinRoughness - perceivedSpecular;
        float D = max(b * b - 4.0 * a * c, 0.0);
        return clamp((-b + sqrt(D)) / (2.0 * a), 0.0, 1.0);
    }

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
            output.specular = float4(0.0, 0.0, 0.0, 0.0);

            // Compute camera background motion vector (rotation-only, jitter-free like the geometry path below)
        float2 uv = (input.pos.xy / pushConstant.viewportSize) * 2.0 - 1.0;
        float4 currClipJittered = float4(uv, 1.0, 1.0);
        // Reconstruct the actual world-space point along this (jittered) pixel's view ray.
        // ubo->invViewProj is jittered on purpose here: it must match the matrix the pixel was rasterized with.
        float4 worldPos4 = mul(ubo->invViewProj, currClipJittered);
        float3 worldPos = worldPos4.xyz / worldPos4.w;

        // Re-project that same world point with the CLEAN (non-jittered) matrices for both frames,
        // exactly like the geometry path does, so background pixels use the same jitter-free convention.
        float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(worldPos, 1.0)));
        float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(worldPos, 1.0)));
        float2 currNDC = currClip.xy / currClip.w;
        float2 prevNDC = prevClip.xy / prevClip.w;

        output.velocity = (prevNDC - currNDC) * float2(0.5, -0.5);
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
        // World Space Vertices
            float4 worldPos0 = mul(instance.modelMatrix, localPos0);
            float4 worldPos1 = mul(instance.modelMatrix, localPos1);
            float4 worldPos2 = mul(instance.modelMatrix, localPos2);

            // 3D World-Space Ray-Triangle Intersection & Barycentric Reconstruction
            // Immune to near-plane clipping singularities (avoids 2D perspective divide by clip.w <= 0)
            float2 invViewport = 1.0 / pushConstant.viewportSize;
            float2 uvScreen = (float2(pixelCoord) + 0.5) * invViewport;
            float2 ndc = float2(uvScreen.x * 2.0 - 1.0, 1.0 - uvScreen.y * 2.0);

            float4 targetH    = mul(ubo->invViewProj, float4(ndc, 1.0, 1.0));
            float4 targetH_dx = mul(ubo->invViewProj, float4(ndc + float2(2.0 * invViewport.x, 0.0), 1.0, 1.0));
            float4 targetH_dy = mul(ubo->invViewProj, float4(ndc - float2(0.0, 2.0 * invViewport.y), 1.0, 1.0));

            float3 pNear    = targetH.xyz / targetH.w;
            float3 pNear_dx = targetH_dx.xyz / targetH_dx.w;
            float3 pNear_dy = targetH_dy.xyz / targetH_dy.w;

            float3 rayOrigin = ubo->cameraWorldPos.xyz;
            float3 rayDir    = normalize(pNear - rayOrigin);
            float3 rayDir_dx = normalize(pNear_dx - rayOrigin);
            float3 rayDir_dy = normalize(pNear_dy - rayOrigin);

            float3 e1 = worldPos1.xyz - worldPos0.xyz;
            float3 e2 = worldPos2.xyz - worldPos0.xyz;
            float3 triNorm = cross(e1, e2);

            float d00 = dot(e1, e1);
            float d01 = dot(e1, e2);
            float d11 = dot(e2, e2);
            float denom = d00 * d11 - d01 * d01;
            float invDenom = (abs(denom) > 1e-12) ? (1.0 / denom) : 0.0;

            float4 bary4 = computeBarycentric3D(rayOrigin, rayDir, worldPos0.xyz, e1, e2, triNorm, d00, d01, d11, invDenom);
                if (bary4.w <= 0.0)
                {
                    output.albedo = float4(0.0, 0.0, 0.0, 0.0);
                    output.normal = float4(0.0, 0.0, 0.0, 0.0);
                    output.material = float4(0.0, 0.0, 0.0, 0.0);
                    output.emission = float4(0.0, 0.0, 0.0, 0.0);
                    output.entityID = -1;
                    return output;
                }
                float3 bary    = bary4.xyz;
                float3 bary_dx = computeBarycentric3D(rayOrigin, rayDir_dx, worldPos0.xyz, e1, e2, triNorm, d00, d01, d11, invDenom).xyz;
                float3 bary_dy = computeBarycentric3D(rayOrigin, rayDir_dy, worldPos0.xyz, e1, e2, triNorm, d00, d01, d11, invDenom).xyz;

            float w0 = clamp(bary.x, 0.0, 1.0);
            float w1 = clamp(bary.y, 0.0, 1.0);
            float w2 = clamp(bary.z, 0.0, 1.0);
            float sumW = w0 + w1 + w2;
            if (sumW > 1e-7)
            {
                w0 /= sumW;
                w1 /= sumW;
                w2 /= sumW;
            }
            else
            {
                w0 = 1.0; w1 = 0.0; w2 = 0.0;
            }

            float3 dbary_dx = bary_dx - bary;
            float3 dbary_dy = bary_dy - bary;

            float w0_dx = w0 + dbary_dx.x;
            float w1_dx = w1 + dbary_dx.y;
            float w2_dx = w2 + dbary_dx.z;

            float w0_dy = w0 + dbary_dy.x;
            float w1_dy = w1 + dbary_dy.y;
            float w2_dy = w2 + dbary_dy.z;

            float2 uv0 = v0.uv0 * w0 + v1.uv0 * w1 + v2.uv0 * w2;
            float2 ddx_uv0 = v0.uv0 * dbary_dx.x + v1.uv0 * dbary_dx.y + v2.uv0 * dbary_dx.z;
            float2 ddy_uv0 = v0.uv0 * dbary_dy.x + v1.uv0 * dbary_dy.y + v2.uv0 * dbary_dy.z;

            float2 uv1 = v0.uv1 * w0 + v1.uv1 * w1 + v2.uv1 * w2;
            float2 ddx_uv1 = v0.uv1 * dbary_dx.x + v1.uv1 * dbary_dx.y + v2.uv1 * dbary_dx.z;
            float2 ddy_uv1 = v0.uv1 * dbary_dy.x + v1.uv1 * dbary_dy.y + v2.uv1 * dbary_dy.z;

            float3 worldNorm0 = mul((float3x3)instance.normalMatrix, localNorm0);
            float3 worldNorm1 = mul((float3x3)instance.normalMatrix, localNorm1);
            float3 worldNorm2 = mul((float3x3)instance.normalMatrix, localNorm2);
            float3 rawGeomNorm = worldNorm0 * w0 + worldNorm1 * w1 + worldNorm2 * w2;
            float lenNorm = length(rawGeomNorm);
            float3 geomNormal = (lenNorm > 1e-6) ? (rawGeomNorm / lenNorm) : normalize(triNorm);

        float3 worldPos = worldPos0.xyz * w0 + worldPos1.xyz * w1 + worldPos2.xyz * w2;
        float3 worldPos_dx = worldPos0.xyz * w0_dx + worldPos1.xyz * w1_dx + worldPos2.xyz * w2_dx;
        float3 worldPos_dy = worldPos0.xyz * w0_dy + worldPos1.xyz * w1_dy + worldPos2.xyz * w2_dy;
        float3 ddx_pos = worldPos_dx - worldPos;
        float3 ddy_pos = worldPos_dy - worldPos;

        SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

        // 1. TEXTURE COORDINATE SETS & DERIVATIVES
        float2 baseColorUV = (instance.baseColorTextureSet == 0) ? uv0 : uv1;
        float2 ddx_bc = (instance.baseColorTextureSet == 0) ? ddx_uv0 : ddx_uv1;
        float2 ddy_bc = (instance.baseColorTextureSet == 0) ? ddy_uv0 : ddy_uv1;

        float2 mrUV = (instance.physicalDescriptorTextureSet == 0) ? uv0 : uv1;
        float2 ddx_mr = (instance.physicalDescriptorTextureSet == 0) ? ddx_uv0 : ddx_uv1;
        float2 ddy_mr = (instance.physicalDescriptorTextureSet == 0) ? ddy_uv0 : ddy_uv1;

        // 2. PBR MATERIAL EVALUATION (Metallic-Roughness vs Specular-Glossiness)
        float4 baseColor;
        float roughness;
        float metallic;

        if (instance.workflow == PBR_WORKFLOW_SPECULAR_GLOSSINESS)
        {
            // Diffuse Color (baseColorTextureIndex holds diffuseTexture)
            float4 diffuse = instance.diffuseFactor;
            if (instance.baseColorTextureIndex != 0xFFFFFFFF)
            {
                Texture2D diffuseTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
                // BaseColor/Diffuse texture is loaded as SRGBA8, hardware sampler automatically decodes to linear RGB
                diffuse = diffuseTex.SampleGrad(samp, baseColorUV, ddx_bc, ddy_bc) * instance.diffuseFactor;
            }

            // Specular Color & Glossiness (metallicRoughnessTextureIndex holds specularGlossinessTexture)
            float3 specular = instance.specularFactor.rgb;
            float glossiness = instance.specularFactor.a;
            if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
            {
                Texture2D specGlossTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
                float4 specGlossSample = specGlossTex.SampleGrad(samp, mrUV, ddx_mr, ddy_mr);
                // Specular texture RGB is authored in sRGB (imported as UNORM), decode to linear RGB:
                float3 linearSpec = srgbToLinear(specGlossSample.rgb);
                specular = linearSpec * instance.specularFactor.rgb;
                glossiness = specGlossSample.a * instance.specularFactor.a;
            }

            // Perceptual Roughness is the inverse of glossiness
            roughness = clamp(1.0 - glossiness, 0.0, 1.0);

            // Convert Specular-Glossiness inputs to Metallic & Base Color (Khronos / Sascha Willems formula)
            float maxSpecular = max(max(specular.r, specular.g), specular.b);
            metallic = convertMetallic(diffuse.rgb, specular, maxSpecular);

            const float epsilon = 1e-6;
            float3 baseColorDiffusePart = diffuse.rgb * ((1.0 - maxSpecular) / (1.0 - c_MinRoughness) / max(1.0 - metallic, epsilon));
            float3 baseColorSpecularPart = specular - (float3(c_MinRoughness) * (1.0 - metallic) * (1.0 / max(metallic, epsilon)));
            baseColor = float4(clamp(lerp(baseColorDiffusePart, baseColorSpecularPart, metallic * metallic), float3(0.0), float3(1.0)), diffuse.a);
        }
        else
        {
            // Standard Metallic-Roughness Workflow
            baseColor = instance.baseColorFactor;
            if (instance.baseColorTextureIndex != 0xFFFFFFFF)
            {
                Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
                baseColor = baseColorTex.SampleGrad(samp, baseColorUV, ddx_bc, ddy_bc) * instance.baseColorFactor;
            }

            roughness = instance.roughnessFactor;
            metallic = instance.metallicFactor;
            if (instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
            {
                Texture2D mrTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
                float4 mrSample = mrTex.SampleGrad(samp, mrUV, ddx_mr, ddy_mr);
                roughness *= mrSample.g;
                metallic  *= mrSample.b;
            }
            roughness = clamp(roughness, 0.0, 1.0);
            metallic  = clamp(metallic, 0.0, 1.0);
        }

        // 3. NORMAL MAPPING (Analytical Tangent Frame from 3D Triangle Geometry)
        float3 finalNormal = geomNormal;
        float3 rawNormalTex = geomNormal * 0.5 + 0.5; // fallback matching Sascha's normalize(inNormal)
        if (instance.normalTextureIndex != 0xFFFFFFFF)
        {
            float2 normalUV = (instance.normalTextureSet == 0) ? uv0 : uv1;
            Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.normalTextureIndex, 0));
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

        // Only flip normal if the material is explicitly DOUBLE-SIDED (e.g. foliage/cloth)
        if (instance.doubleSided != 0 && dot(finalNormal, -rayDir) < 0.0)
        {
            finalNormal = -finalNormal;
        }

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
        output.normal   = (pushConstant.debugMode == 2) ? float4(rawNormalTex, roughness) : float4(finalNormal, roughness);

        // Encode unlit flag: 2.0 = UNLIT
        float workflow  = (instance.unlit != 0) ? 2.0 : instance.workflow;
        float isDoubleSided = (instance.doubleSided != 0) ? 1.0 : 0.0;
        output.material = float4(roughness, metallic, workflow, isDoubleSided);

        output.emission = float4(emission, 1.0);
        output.entityID = instance.entityID;

        // --- Screen-Space Motion Vector (Jitter-Free) ---
        float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(worldPos, 1.0)));
        float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(worldPos, 1.0)));

        float2 currNDC = currClip.xy / currClip.w;
        float2 prevNDC = prevClip.xy / prevClip.w;

        // NVIDIA Streamline / DLSS standard: delta in UV space [0, 1]
        float2 velocity = (prevNDC - currNDC) * float2(0.5, -0.5);
        output.velocity = velocity;

        // Specular Albedo (F0) for DLSS-RR
        output.specular = float4(lerp(float3(0.04), baseColor.rgb, metallic), 1.0);

        return output;
    }