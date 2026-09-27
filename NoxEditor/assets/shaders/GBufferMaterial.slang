 #include "shaderIO.h"
#include "MipFeedback.slang"

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

    // Slang assigns the output locations in member order (the pipeline's color formats and the pass's targets follow it).
    struct GBufferOutput
    {
        float4 albedo   : SV_Target0; // RGB: BaseColor, A: Occlusion
        float4 normal   : SV_Target1; // RGB: World Normal, A: Roughness
        float4 material : SV_Target2; // R: Roughness, G: Metallic, B: Workflow, A: double sided
        float4 emission : SV_Target3; // RGB: Emissive, A: 1.0
        int    entityID : SV_Target4; // Editor Entity Picking ID
        float2 velocity : SV_Target5; // Screen-Space Motion Vectors (RG32F)
    };

    struct GBufferOutputNoVelocity
    {
        float4 albedo   : SV_Target0;
        float4 normal   : SV_Target1;
        float4 material : SV_Target2;
        float4 emission : SV_Target3;
        int    entityID : SV_Target4;
    };

    // Ray/triangle-plane intersection, for the derivative samples (below) only: they need a barycentric solve for a
    // NEARBY screen pixel, which the main hit position (found from ITS OWN depth) doesn't give directly.
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

    // Barycentric solve for a point ALREADY known to be on the triangle's plane --
    // no ray/plane intersection (no division by dot(triNorm, rayDir), which is the part that goes numerically unstable
    // for a triangle seen at a grazing angle to the view ray).
    float3 barycentricOfPoint(float3 point, float3 v0, float3 e1, float3 e2, float d00, float d01, float d11, float invDenom)
    {
        float3 dp = point - v0;
        float d20 = dot(dp, e1);
        float d21 = dot(dp, e2);
        float bw1 = (d11 * d20 - d01 * d21) * invDenom;
        float bw2 = (d00 * d21 - d01 * d20) * invDenom;
        return float3(1.0 - bw1 - bw2, bw1, bw2);
    }

    static const float c_MinRoughness = 0.04;
    static const float PBR_WORKFLOW_METALLIC_ROUGHNESS = 0.0;
    static const float PBR_WORKFLOW_SPECULAR_GLOSSINESS = 1.0;

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

    GBufferOutput evaluateGBuffer(
        MSOutput input,
        uniform PushConstantVisibilityDebug pushConstant,
        bool writeVelocity
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
            output.velocity = float2(0.0, 0.0);

            // Compute camera background motion vector (rotation-only, jitter-free like the geometry path below)
            if (writeVelocity)
            {
                float2 uv = (input.pos.xy / pushConstant.viewportSize) * 2.0 - 1.0;
                float4 currClipJittered = float4(uv, 0.001, 1.0); // far along the ray: a precise direction (see Skybox.slang)
                // Reconstruct the actual world-space point along this (jittered) pixel's view ray.
                // ubo->invViewProj is jittered on purpose here: it must match the matrix the pixel was rasterized with.
                float4 worldPos4 = mul(ubo->invViewProj, currClipJittered);
                float3 worldPos = worldPos4.xyz / worldPos4.w;

                // Re-project that same world point with the CLEAN (non-jittered) matrices for both frames,
                // exactly like the geometry path does, so background pixels use the same jitter-free convention.
                // The background is at infinity: only the ray DIRECTION (w = 0) moves it, not the eye. Reprojecting the
                // unprojected near-plane point instead (w = 1, as this did) treated every sky pixel as 0.01 units from
                // the eye, so any camera translation -- an orbiting editor camera moves its eye all the time -- gave
                // it large false motion and DLSS / temporal filters smeared and shimmered the image.
                float3 rayDirection = worldPos - ubo->cameraWorldPos.xyz;
                float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(rayDirection, 0.0)));
                float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(rayDirection, 0.0)));
                float2 currNDC = currClip.xy / currClip.w;
                float2 prevNDC = prevClip.xy / prevClip.w;

                output.velocity = (prevNDC - currNDC) * float2(0.5, -0.5);
            }
            return output;
        }

        // Decode Visibility Buffer IDs
        uint instanceSlot = visData.x - 1;
        uint meshletID = visData.y >> 8;
        uint primitiveID = visData.y & 0xFF;

        // Pull Instance
        SceneInstances instances = SceneInstances(ubo);
        InstanceData instance = instances[instanceSlot];

        // Pull the geometry streams
        MeshletDraw* meshletDrawStream = (MeshletDraw*)ubo->geometryMeshletDrawsReference;
        Vertex* vertexStream = (Vertex*)ubo->geometryVerticesReference;
        uint32_t* meshletVertexStream = (uint32_t*)ubo->geometryMeshletVerticesReference;
        uint8_t* meshletTriangleStream = (uint8_t*)ubo->geometryMeshletTrianglesReference;

        MeshletDraw meshlet = meshletDrawStream[meshletID];

        // Fetch Triangle Indices
        uint triBase = meshlet.triangleOffset + (primitiveID * 3);
        uint i0 = meshletTriangleStream[triBase];
        uint i1 = meshletTriangleStream[triBase + 1];
        uint i2 = meshletTriangleStream[triBase + 2];

        // Fetch Local Vertex Indices
        uint vIdx0 = meshletVertexStream[meshlet.vertexOffset + i0];
        uint vIdx1 = meshletVertexStream[meshlet.vertexOffset + i1];
        uint vIdx2 = meshletVertexStream[meshlet.vertexOffset + i2];

        // Fetch the 3 Vertices via BDA
        Vertex v0 = vertexStream[meshlet.globalVertexOffset + vIdx0];
        Vertex v1 = vertexStream[meshlet.globalVertexOffset + vIdx1];
        Vertex v2 = vertexStream[meshlet.globalVertexOffset + vIdx2];

        // Skinning
        float4 localPos0 = float4(v0.pos, 1.0);
        float4 localPos1 = float4(v1.pos, 1.0);
        float4 localPos2 = float4(v2.pos, 1.0);
        float3 localNorm0 = v0.normal;
        float3 localNorm1 = v1.normal;
        float3 localNorm2 = v2.normal;
        float4 localTangent0 = v0.tangent;
        float4 localTangent1 = v1.tangent;
        float4 localTangent2 = v2.tangent;
        float4 previousLocalPos0 = localPos0;
        float4 previousLocalPos1 = localPos1;
        float4 previousLocalPos2 = localPos2;

        if (instance.skinnedVertexOffset != NoSkinnedVertices && pushConstant.skinnedVertexReference != 0)
        {
            SkinnedVertex* skinnedVertices = (SkinnedVertex*)pushConstant.skinnedVertexReference;
            const uint skinnedBase = instance.skinnedVertexOffset;
            SkinnedVertex skinned0 = skinnedVertices[skinnedBase + vIdx0];
            SkinnedVertex skinned1 = skinnedVertices[skinnedBase + vIdx1];
            SkinnedVertex skinned2 = skinnedVertices[skinnedBase + vIdx2];
            localPos0 = skinned0.position;
            localPos1 = skinned1.position;
            localPos2 = skinned2.position;
            localNorm0 = skinned0.normal.xyz;
            localNorm1 = skinned1.normal.xyz;
            localNorm2 = skinned2.normal.xyz;
            localTangent0 = skinned0.tangent;
            localTangent1 = skinned1.tangent;
            localTangent2 = skinned2.tangent;

            previousLocalPos0 = localPos0;
            previousLocalPos1 = localPos1;
            previousLocalPos2 = localPos2;
            if (pushConstant.skinnedHistoryValid != 0 && pushConstant.previousSkinnedVertexReference != 0)
            {
                SkinnedVertex* previousSkinnedVertices = (SkinnedVertex*)pushConstant.previousSkinnedVertexReference;
                previousLocalPos0 = previousSkinnedVertices[skinnedBase + vIdx0].position;
                previousLocalPos1 = previousSkinnedVertices[skinnedBase + vIdx1].position;
                previousLocalPos2 = previousSkinnedVertices[skinnedBase + vIdx2].position;
            }
        }
        else if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
        {
            float4x4* boneMatrices = (float4x4*)pushConstant.boneMatrixReference;
            uint baseOffset = instance.boneMatrixOffset;

            float4x4 skin0 = boneMatrices[baseOffset + v0.boneIDs.x] * v0.boneWeights.x +
                             boneMatrices[baseOffset + v0.boneIDs.y] * v0.boneWeights.y +
                             boneMatrices[baseOffset + v0.boneIDs.z] * v0.boneWeights.z +
                             boneMatrices[baseOffset + v0.boneIDs.w] * v0.boneWeights.w;
            localPos0 = mul(skin0, localPos0);
            localNorm0 = mul((float3x3)skin0, localNorm0);
            localTangent0 = float4(mul((float3x3)skin0, localTangent0.xyz), localTangent0.w);

            float4x4 skin1 = boneMatrices[baseOffset + v1.boneIDs.x] * v1.boneWeights.x +
                             boneMatrices[baseOffset + v1.boneIDs.y] * v1.boneWeights.y +
                             boneMatrices[baseOffset + v1.boneIDs.z] * v1.boneWeights.z +
                             boneMatrices[baseOffset + v1.boneIDs.w] * v1.boneWeights.w;
            localPos1 = mul(skin1, localPos1);
            localNorm1 = mul((float3x3)skin1, localNorm1);
            localTangent1 = float4(mul((float3x3)skin1, localTangent1.xyz), localTangent1.w);

            float4x4 skin2 = boneMatrices[baseOffset + v2.boneIDs.x] * v2.boneWeights.x +
                             boneMatrices[baseOffset + v2.boneIDs.y] * v2.boneWeights.y +
                             boneMatrices[baseOffset + v2.boneIDs.z] * v2.boneWeights.z +
                             boneMatrices[baseOffset + v2.boneIDs.w] * v2.boneWeights.w;
            localPos2 = mul(skin2, localPos2);
            localNorm2 = mul((float3x3)skin2, localNorm2);
            localTangent2 = float4(mul((float3x3)skin2, localTangent2.xyz), localTangent2.w);

            // The direct fallback has no earlier pose cache; avoid inventing skeletal velocity for that one frame.
            previousLocalPos0 = localPos0;
            previousLocalPos1 = localPos1;
            previousLocalPos2 = localPos2;
        }

        // World & Screen Clip Space
        // World Space Vertices
            float4 worldPos0 = mul(instance.modelMatrix, localPos0);
            float4 worldPos1 = mul(instance.modelMatrix, localPos1);
            float4 worldPos2 = mul(instance.modelMatrix, localPos2);

            // 3D World-Space Barycentric Reconstruction
            float2 invViewport = 1.0 / pushConstant.viewportSize;
            float2 uvScreen = (float2(pixelCoord) + 0.5) * invViewport;
            float2 ndc = float2(uvScreen.x * 2.0 - 1.0, 1.0 - uvScreen.y * 2.0);

            float3 e1 = worldPos1.xyz - worldPos0.xyz;
            float3 e2 = worldPos2.xyz - worldPos0.xyz;
            float3 triNorm = cross(e1, e2);

            float d00 = dot(e1, e1);
            float d01 = dot(e1, e2);
            float d11 = dot(e2, e2);
            float denom = d00 * d11 - d01 * d01;
            float invDenom = (abs(denom) > 1e-12) ? (1.0 / denom) : 0.0;

            // The main barycentric weights: from THIS pixel's own depth, already resolved exactly by hardware rasterization
            // in the Visibility pass, unprojected the same reliable way DeferredLighting.slang gets a world position from
            // depth. Not from a world-space ray/triangle-plane intersection (what this used to do, and what the derivative
            // samples below still do): that intersection divides by dot(triNorm, rayDir), which loses precision fast wherever
            // the triangle is seen at a grazing angle to the view ray -- everywhere on a curved surface, nowhere on a flat
            // one, which is exactly the "fine on a cube, speckled on a sphere, in every view including the path tracer's
            // separate (unaffected) hardware ray intersection" pattern this turned out to cause.
            Texture2D<float> gbufferDepthTex = Texture2D<float>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.depthTextureIndex, 0));
            float pixelDepth = gbufferDepthTex.Load(int3(pixelCoord, 0)).r;
            if (pixelDepth <= 0.0)
            {
                output.albedo = float4(0.0, 0.0, 0.0, 0.0);
                output.normal = float4(0.0, 0.0, 0.0, 0.0);
                output.material = float4(0.0, 0.0, 0.0, 0.0);
                output.emission = float4(0.0, 0.0, 0.0, 0.0);
                output.entityID = -1;
                return output;
            }
            float4 hitPosH = mul(ubo->invViewProj, float4(ndc, pixelDepth, 1.0));
            float3 hitPos = hitPosH.xyz / hitPosH.w;
            float3 bary = barycentricOfPoint(hitPos, worldPos0.xyz, e1, e2, d00, d01, d11, invDenom);
            // Camera-to-surface direction (double-sided normal flip, below), from the same accurate hit position.
            float3 viewDir = normalize(hitPos - ubo->cameraWorldPos.xyz);

            // Derivatives (texture LOD only, not the normal/position above): an analytic ray/triangle-plane solve
            // against THIS pixel's own triangle, like the main bary above used to be -- not a hardware ddx/ddy of the
            // per-pixel-independently-computed bary. A visibility buffer's neighbouring screen pixel can belong to a
            // completely different triangle (a different mesh, even); a hardware quad derivative doesn't know that and
            // differences two unrelated barycentric spaces, which showed up as a visible seam anywhere a mip picked on
            // one side of a mesh/UV boundary didn't match the other (a balcony floor looking like two different
            // textures meeting in the middle, sharp and continuous in the path tracer's own, separate intersection).
            // The neighbour ray unprojects at THIS pixel's own depth, not the near plane (what the version before this
            // one did, and the one before ddx/ddy did too): unprojecting at the near plane forces the point to within
            // about 0.1 units of the camera regardless of what is actually being rendered, so subtracting the camera's
            // position from it recovers a tiny delta from two large, nearly-equal floats -- precision loss growing
            // directly with distance from the world origin, which was the original "blurry far from the origin" bug.
            // Unprojecting at this pixel's own depth instead recovers a delta on the same scale as the real distance to
            // the surface (the same reason hitPos above is precise), at any distance from the origin.
            float4 targetH_dx = mul(ubo->invViewProj, float4(ndc + float2(2.0 * invViewport.x, 0.0), pixelDepth, 1.0));
            float4 targetH_dy = mul(ubo->invViewProj, float4(ndc - float2(0.0, 2.0 * invViewport.y), pixelDepth, 1.0));
            float3 pAtDepth_dx = targetH_dx.xyz / targetH_dx.w;
            float3 pAtDepth_dy = targetH_dy.xyz / targetH_dy.w;
            float3 rayOrigin = ubo->cameraWorldPos.xyz;
            float3 rayDir_dx = normalize(pAtDepth_dx - rayOrigin);
            float3 rayDir_dy = normalize(pAtDepth_dy - rayOrigin);
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

            // Tangent, like position, transforms with the model matrix directly (not the inverse-transpose normal
            // matrix -- it is a direction ALONG the surface, not perpendicular to it, so an inverse-transpose would be
            // wrong under non-uniform scale). Handedness (w) is a fixed sign, the same for all three corners of a
            // triangle by construction (MeshImporter::GenerateMissingTangents keeps every triangle's corners on one
            // side of any duplicate split), so any one of the three is as good as any other -- not something to blend.
            float3 worldTangent0 = mul((float3x3)instance.modelMatrix, localTangent0.xyz);
            float3 worldTangent1 = mul((float3x3)instance.modelMatrix, localTangent1.xyz);
            float3 worldTangent2 = mul((float3x3)instance.modelMatrix, localTangent2.xyz);
            float3 rawGeomTangent = worldTangent0 * w0 + worldTangent1 * w1 + worldTangent2 * w2;
            float tangentHandedness = localTangent0.w;

        float3 worldPos = worldPos0.xyz * w0 + worldPos1.xyz * w1 + worldPos2.xyz * w2;
        // Where this surface point was last frame, including the previous skeletal pose when its cache is valid.
        float3 previousWorldPos = mul(instance.previousModelMatrix, previousLocalPos0).xyz * w0 +
                                  mul(instance.previousModelMatrix, previousLocalPos1).xyz * w1 +
                                  mul(instance.previousModelMatrix, previousLocalPos2).xyz * w2;
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

        // Texture streaming: the mips this surface's textures need, from the same derivatives the samples use.
        if (ubo->mipFeedbackReference != 0 && isMipFeedbackPixel(pixelCoord, ubo->mipFeedbackFrame))
        {
            reportMipFeedback(ubo, instance.baseColorTextureIndex, ddx_bc, ddy_bc);
            reportMipFeedback(ubo, instance.metallicRoughnessTextureIndex, ddx_mr, ddy_mr);
            reportMipFeedback(ubo, instance.normalTextureIndex, instance.normalTextureSet == 0 ? ddx_uv0 : ddx_uv1, instance.normalTextureSet == 0 ? ddy_uv0 : ddy_uv1);
            reportMipFeedback(ubo, instance.occlusionTextureIndex, instance.occlusionTextureSet == 0 ? ddx_uv0 : ddx_uv1, instance.occlusionTextureSet == 0 ? ddy_uv0 : ddy_uv1);
            reportMipFeedback(ubo, instance.emissiveTextureIndex, instance.emissiveTextureSet == 0 ? ddx_uv0 : ddx_uv1, instance.emissiveTextureSet == 0 ? ddy_uv0 : ddy_uv1);
            reportMipFeedback(ubo, instance.transmissionTextureIndex, instance.transmissionTextureSet == 0 ? ddx_uv0 : ddx_uv1, instance.transmissionTextureSet == 0 ? ddy_uv0 : ddy_uv1);
        }

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
                // The specular-glossiness texture is imported as SRGBA8 (EditorAssetManager::ImportMeshTextures), which
                // hardware-decodes the RGB (specular color, authored sRGB) through the sRGB curve on every sample while
                // always leaving alpha (glossiness, authored linear) untouched -- exactly this extension's mixed encoding.
                // Decoding RGB a second time here (this used to call srgbToLinear on it, on top of that) compounded the
                // gamma curve, darkening/skewing specular color -- and therefore the metallic/base-color reconstruction
                // below -- for every specular-glossiness material (Bistro; never DamagedHelmet's plain metallic-roughness).
                specular = specGlossSample.rgb * instance.specularFactor.rgb;
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

        // 3. NORMAL MAPPING (stored per-vertex tangent, not a per-pixel analytic reconstruction)
        float3 finalNormal = geomNormal;
        float3 rawNormalTex = geomNormal * 0.5 + 0.5; // fallback matching Sascha's normalize(inNormal)
        if (instance.normalTextureIndex != 0xFFFFFFFF)
        {
            float2 normalUV = (instance.normalTextureSet == 0) ? uv0 : uv1;
            float2 ddx_normal = (instance.normalTextureSet == 0) ? ddx_uv0 : ddx_uv1;
            float2 ddy_normal = (instance.normalTextureSet == 0) ? ddy_uv0 : ddy_uv1;
            Texture2D normalTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.normalTextureIndex, 0));
            rawNormalTex = normalTex.SampleGrad(samp, normalUV, ddx_normal, ddy_normal).xyz;
            float3 tangentNormal;
            tangentNormal.xy = rawNormalTex.xy * 2.0 - 1.0;
            tangentNormal.z = rawNormalTex.z <= 0.0
                ? sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)))
                : abs(rawNormalTex.z * 2.0 - 1.0);
            tangentNormal = dot(tangentNormal, tangentNormal) > 1e-12 ? normalize(tangentNormal) : float3(0.0, 0.0, 1.0);

            // Gram-Schmidt re-orthonormalize the stored tangent against the (interpolated, possibly non-uniformly
            // scaled) geometric normal -- it does not have to stay exactly perpendicular through that -- then derive
            // the bitangent as cross(N, T) * handedness, the standard glTF/MikkTSpace convention the stored w means.
            // This used to be reconstructed per pixel from the triangle's own world-space/UV derivatives instead (no
            // stored tangent existed): correct in principle, but numerically ambiguous on a UV-mirrored seam without
            // the handedness a stored tangent carries -- right on one side, flipped on the other, a visible seam in
            // specular response on an otherwise perfectly continuous surface (never in diffuse, which is why it only
            // showed up under lighting, never in the raw albedo or normal-map-texture debug views).
            float3 N = geomNormal;
            float lenGeomTangent = length(rawGeomTangent);
            if (lenGeomTangent > 1e-12)
            {
                float3 T = normalize(rawGeomTangent - N * dot(N, rawGeomTangent));
                float3 B = cross(N, T) * tangentHandedness;
                float3x3 TBN = float3x3(T, B, N);
                finalNormal = normalize(mul(tangentNormal, TBN));
            }
        }
        else
        {
            // Visual Diagnostic: Bright red if normal texture is not bound
            finalNormal = geomNormal;
        }

        // Only flip normal if the material is explicitly DOUBLE-SIDED (e.g. foliage/cloth)
        if (instance.doubleSided != 0 && dot(finalNormal, -viewDir) < 0.0)
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
        // Debug view 20: the base color texture's mip residency against what this pixel needs.
        if (pushConstant.debugMode == 20)
        {
            output.albedo.rgb = instance.baseColorTextureIndex != 0xFFFFFFFF
                ? mipStreamingDebugColor(relativeMipLod(ubo, instance.baseColorTextureIndex, ddx_bc, ddy_bc), baseColor.rgb)
                : float3(0.3, 0.3, 0.3);
        }
        // Debug view 21: the cluster's LOD level (original geometry green, then yellow, orange, red, magenta, blue).
        if (pushConstant.debugMode == 21)
        {
            MeshletBounds* bounds = &((MeshletBounds*)ubo->geometryMeshletBoundsReference)[instance.boundsOffset + (meshletID - instance.drawsOffset)];
            const float3 levelColors[6] = { float3(0.2, 0.85, 0.2), float3(0.95, 0.9, 0.2), float3(1.0, 0.55, 0.1),
                                            float3(0.95, 0.15, 0.1), float3(0.85, 0.2, 0.85), float3(0.25, 0.35, 1.0) };
            output.albedo.rgb = levelColors[min(bounds.lodLevel, 5u)] * (0.35 + 0.65 * dot(baseColor.rgb, float3(0.2126, 0.7152, 0.0722)));
        }
        output.normal   = (pushConstant.debugMode == 2) ? float4(rawNormalTex, roughness) : float4(finalNormal, roughness);

        // Encode unlit flag: 2.0 = UNLIT
        float workflow  = (instance.unlit != 0) ? 2.0 : instance.workflow;
        float isDoubleSided = (instance.doubleSided != 0) ? 1.0 : 0.0;
        output.material = float4(roughness, metallic, workflow, isDoubleSided);

        output.emission = float4(emission, 1.0);
        output.entityID = instance.entityID;

        output.velocity = float2(0.0, 0.0);
        if (writeVelocity)
        {
            // --- Screen-Space Motion Vector (Jitter-Free) ---
            float4 currClip = mul(ubo->nonJitteredProj, mul(ubo->view, float4(worldPos, 1.0)));
            float4 prevClip = mul(ubo->prevProj, mul(ubo->prevView, float4(previousWorldPos, 1.0)));

            float2 currNDC = currClip.xy / currClip.w;
            float2 prevNDC = prevClip.xy / prevClip.w;

            // NVIDIA Streamline / DLSS standard: delta in UV space [0, 1]
            output.velocity = (prevNDC - currNDC) * float2(0.5, -0.5);
        }

        return output;
    }

    [shader("fragment")]
    GBufferOutput fragMain(
        MSOutput input,
        uniform PushConstantVisibilityDebug pushConstant)
    {
        return evaluateGBuffer(input, pushConstant, true);
    }

    [shader("fragment")]
    GBufferOutputNoVelocity fragMainNoVelocity(
        MSOutput input,
        uniform PushConstantVisibilityDebug pushConstant)
    {
        GBufferOutput full = evaluateGBuffer(input, pushConstant, false);
        GBufferOutputNoVelocity output;
        output.albedo = full.albedo;
        output.normal = full.normal;
        output.material = full.material;
        output.emission = full.emission;
        output.entityID = full.entityID;
        return output;
    }
