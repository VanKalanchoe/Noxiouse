#include "shaderIO.h"

        struct VisDebugPayload
        {
            uint dummy;
        };
        groupshared VisDebugPayload gPayload;

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

        // Pseudorandom hash for debug mode 2
        float3 hashToColor(uint2 id)
        {
            uint h = (id.x + 1u) * 0x85ebca6bu ^ (id.y * 0xc2b2ae35u);
            h ^= h >> 16;
            h *= 0x85ebca6bu;
            h ^= h >> 13;

            return float3(
                float(h & 0xFF) / 255.0,
                float((h >> 8) & 0xFF) / 255.0,
                float((h >> 16) & 0xFF) / 255.0
            );
        }

        [shader("fragment")]
        float4 fragMain(
            MSOutput input,
            uniform PushConstantVisibilityDebug pushConstant
        ) : SV_Target0
        {
            UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

            Texture2D<uint4> visTex = Texture2D<uint4>.Handle(uint2(ubo->imageHeapIndexOffset + pushConstant.visibilityTextureIndex, 0));

            int2 pixelCoord = int2(input.pos.xy);
            uint4 rawData = visTex.Load(int3(pixelCoord, 0));
            uint2 visData = rawData.xy;

            // Unrendered background pixels
            if (visData.x == 0)
            {
                return float4(0.12, 0.12, 0.18, 1.0);
            }

 // Mode 3: Step 1 Colored Meshlets
                if (pushConstant.debugMode == 3)
                {
                    return float4(hashToColor(visData), 1.0);
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

                // Fetch Triangle Indices (3 indices)
                uint triBase = meshlet.triangleOffset + (primitiveID * 3);
                uint i0 = meshletTrianglePage[triBase];
                uint i1 = meshletTrianglePage[triBase + 1];
                uint i2 = meshletTrianglePage[triBase + 2];

                // Fetch Local Vertex Indices
                uint vIdx0 = meshletVerticesPage[meshlet.vertexOffset + i0];
                uint vIdx1 = meshletVerticesPage[meshlet.vertexOffset + i1];
                uint vIdx2 = meshletVerticesPage[meshlet.vertexOffset + i2];

                // Fetch the 3 Vertices directly via BDA
                Vertex v0 = vertexPage[meshlet.globalVertexOffset + vIdx0];
                Vertex v1 = vertexPage[meshlet.globalVertexOffset + vIdx1];
                Vertex v2 = vertexPage[meshlet.globalVertexOffset + vIdx2];

                // Skinning / Bone animation (if applicable)
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

                // Project vertices to clip space
                float4x4 viewProj = mul(ubo->proj, ubo->view);
                float4 clip0 = mul(viewProj, mul(instance.modelMatrix, localPos0));
                float4 clip1 = mul(viewProj, mul(instance.modelMatrix, localPos1));
                float4 clip2 = mul(viewProj, mul(instance.modelMatrix, localPos2));

                // Convert projected vertices to screen pixel coordinates
                // (Matching Vulkan viewport {0.0f, h, w, -h})
                float2 s0 = float2((clip0.x / clip0.w + 1.0) * 0.5 * pushConstant.viewportSize.x,
                                   (1.0 - clip0.y / clip0.w) * 0.5 * pushConstant.viewportSize.y);
                float2 s1 = float2((clip1.x / clip1.w + 1.0) * 0.5 * pushConstant.viewportSize.x,
                                   (1.0 - clip1.y / clip1.w) * 0.5 * pushConstant.viewportSize.y);
                float2 s2 = float2((clip2.x / clip2.w + 1.0) * 0.5 * pushConstant.viewportSize.x,
                                   (1.0 - clip2.y / clip2.w) * 0.5 * pushConstant.viewportSize.y);

                // Screen pixel coordinate of current fragment
                float2 P = input.pos.xy;

                // 2D barycentric weights in screen pixel space
                float2 e0 = s1 - s0;
                float2 e1 = s2 - s0;
                float2 ep = P - s0;

                float det = e0.x * e1.y - e1.x * e0.y;
                float invDet = (abs(det) > 1e-7) ? (1.0 / det) : 0.0;

                float l1 = (ep.x * e1.y - e1.x * ep.y) * invDet;
                float l2 = (e0.x * ep.y - ep.x * e0.y) * invDet;
                float l0 = 1.0 - l1 - l2;

                // Perspective correction (/ w)
                float w0 = max(0.0, l0) / clip0.w;
                float w1 = max(0.0, l1) / clip1.w;
                float w2 = max(0.0, l2) / clip2.w;
                float sumW = w0 + w1 + w2;
                if (sumW > 1e-7)
                {
                    w0 /= sumW;
                    w1 /= sumW;
                    w2 /= sumW;
                }

                // Interpolate vertex attributes
                float2 uv0 = v0.uv0 * w0 + v1.uv0 * w1 + v2.uv0 * w2;
                float2 uv1 = v0.uv1 * w0 + v1.uv1 * w1 + v2.uv1 * w2;
                float2 finalUV = (instance.baseColorTextureSet == 0) ? uv0 : uv1;

                float3 worldNorm0 = mul((float3x3)instance.normalMatrix, localNorm0);
                float3 worldNorm1 = mul((float3x3)instance.normalMatrix, localNorm1);
                float3 worldNorm2 = mul((float3x3)instance.normalMatrix, localNorm2);
                float3 normal = normalize(worldNorm0 * w0 + worldNorm1 * w1 + worldNorm2 * w2);

                // Fetch Base Color Texture using reconstructed UVs
                SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

                float4 baseColor = instance.baseColorFactor;
                if (instance.baseColorTextureIndex != 0xFFFFFFFF)
                {
                    Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
                    baseColor = baseColorTex.SampleLevel(samp, finalUV, 0.0) * instance.baseColorFactor;
                }

                // Mode 0: BaseColor / Albedo Texture
                if (pushConstant.debugMode == 0)
                {
                    return baseColor;
                }

                // Mode 1: Reconstructed World Normals
                if (pushConstant.debugMode == 1)
                {
                    return float4(normal * 0.5 + 0.5, 1.0);
                }

                // Mode 2: Reconstructed UV Coordinates
                if (pushConstant.debugMode == 2)
                {
                    return float4(frac(finalUV.x), frac(finalUV.y), 0.0, 1.0);
                }

                // Mode 3: Colored Meshlets
                return float4(hashToColor(visData), 1.0);
            }