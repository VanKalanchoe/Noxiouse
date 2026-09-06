#include "shaderIO.h"

        struct VSOutput
        {
            float4 pos : SV_Position;
            nointerpolation uint drawID;
            nointerpolation uint meshletID;
        };

        struct PrimOutput
        {
            uint primitiveID : SV_PrimitiveID;
        };

        // =========================================================================
        // MESH SHADER (Pure Geometry: Positions & IDs only, no textures or lighting)
        // =========================================================================
        [shader("mesh")]
        [outputtopology("triangle")]
        [numthreads(MESH_SHADER_DISPATCH_X, 1, 1)]
        void meshMain
        (
            in payload MeshletPayload sharedPayload,
            out indices uint3 triangles[MAX_PRIMITIVES],
            out vertices VSOutput vertices[MAX_VERTICES],
            out primitives PrimOutput primitives[MAX_PRIMITIVES],
            uint3 groupId : SV_GroupID,
            uint3 gtid : SV_GroupThreadID,
            uniform PushConstantMeshlets pushConstant
        )
        {
            UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
            InstanceData* instances = (InstanceData*)pushConstant.instanceReference;

            InstanceData instance = instances[sharedPayload.drawID];
            float4x4 modelMatrix = instance.modelMatrix;

            uint64_t* drawsPageTable  = (uint64_t*)pushConstant.meshletDrawsPageTableReference;
            uint64_t* vertsPageTable  = (uint64_t*)pushConstant.vertexPageTableReference;
            uint64_t* mVertsPageTable = (uint64_t*)pushConstant.meshletVerticesPageTableReference;
            uint64_t* mTrisPageTable  = (uint64_t*)pushConstant.meshletTrianglesPageTableReference;

            MeshletDraw* meshletDrawPage = (MeshletDraw*)drawsPageTable[instance.drawsPageIndex];
            Vertex* vertexPage = (Vertex*)vertsPageTable[instance.verticesPageIndex];
            uint32_t* meshletVerticesPage = (uint32_t*)mVertsPageTable[instance.meshletVerticesPageIndex];
            uint8_t* meshletTrianglePage = (uint8_t*)mTrisPageTable[instance.meshletTrianglesPageIndex];

            uint8_t localMeshletOffset = sharedPayload.meshletIndices[groupId.x];
            uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;

            MeshletDraw meshlet = meshletDrawPage[globalMeshletIdx];
            float4x4 viewProj = mul(ubo.proj, ubo.view);

            SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

            // Process vertices (Position & IDs only)
            for (uint i = gtid.x; i < meshlet.vertexCount; i += MESH_SHADER_DISPATCH_X)
            {
                uint localVertexIndex = meshletVerticesPage[meshlet.vertexOffset + i];
                Vertex vert = vertexPage[meshlet.globalVertexOffset + localVertexIndex];

                float4 localPos = float4(vert.pos, 1.0);

                // Bone animation (if applicable)
                if (instance.boneMatrixOffset != 0xFFFFFFFF)
                {
                    float4x4* boneMatrices = (float4x4*)pushConstant.boneMatrixReference;
                    uint baseOffset = instance.boneMatrixOffset;
                    float4x4 skinTransform =
                        boneMatrices[baseOffset + vert.boneIDs.x] * vert.boneWeights.x +
                        boneMatrices[baseOffset + vert.boneIDs.y] * vert.boneWeights.y +
                        boneMatrices[baseOffset + vert.boneIDs.z] * vert.boneWeights.z +
                        boneMatrices[baseOffset + vert.boneIDs.w] * vert.boneWeights.w;
                    localPos = mul(skinTransform, localPos);
                }

                float4 worldPos = mul(modelMatrix, localPos);
                vertices[i].pos = mul(viewProj, worldPos);
                vertices[i].drawID = sharedPayload.drawID;
                vertices[i].meshletID = globalMeshletIdx;
            }

            // Process triangle indices & per-primitive ID
            for (uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
            {
                uint triBase = meshlet.triangleOffset + (i * 3);
                triangles[i] = uint3(
                    meshletTrianglePage[triBase],
                    meshletTrianglePage[triBase + 1],
                    meshletTrianglePage[triBase + 2]
                );
                primitives[i].primitiveID = i;
            }
        }

        // =========================================================================
        // FRAGMENT SHADER (Ultra-thin: writes InstanceID + PrimitiveID)
        // =========================================================================
        struct VisOutput
        {
            uint2 visibility : SV_Target0;
        };

       [shader("fragment")]
        VisOutput fragMain(
            VSOutput inVert,
            uint primitiveID : SV_PrimitiveID
        )
        {
            VisOutput output;
            // .x = Instance/Draw ID (+ 1 so 0 is reserved strictly for background)
            // .y = (MeshletID << 8) | (PrimitiveID & 0xFF)
            output.visibility = uint2(inVert.drawID + 1, (inVert.meshletID << 8) | (primitiveID & 0xFF));
            return output;
        }