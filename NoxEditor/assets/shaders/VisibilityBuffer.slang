#include "shaderIO.h"

    struct VSOutput
    {
        float4 pos : SV_Position;
        float2 uv0;
        float2 uv1;
        nointerpolation uint drawID;
        nointerpolation uint meshletID;
    };

    struct PrimOutput
    {
        uint primitiveID : SV_PrimitiveID;
        bool cull : SV_CullPrimitive;
    };

    groupshared float s_vertexW[MAX_VERTICES];

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

        // Process vertices (Position, UVs & IDs)
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
            float4 clipPos = mul(viewProj, worldPos);
            vertices[i].pos = clipPos;
            s_vertexW[i] = clipPos.w;
            // Prevent projective divide-by-zero/negative-w singularities
            vertices[i].pos = float4(clipPos.xyz, max(clipPos.w, 0.001));
            vertices[i].uv0 = vert.uv0;
            vertices[i].uv1 = vert.uv1;
            vertices[i].drawID = sharedPayload.drawID;
            vertices[i].meshletID = globalMeshletIdx;
        }

         GroupMemoryBarrierWithGroupSync();

        // Process triangle indices & per-primitive ID
        for (uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint triBase = meshlet.triangleOffset + (i * 3);
            uint idx0 = meshletTrianglePage[triBase];
            uint idx1 = meshletTrianglePage[triBase + 1];
            uint idx2 = meshletTrianglePage[triBase + 2];

            triangles[i] = uint3(idx0, idx1, idx2);
            primitives[i].primitiveID = i;

            // Hardware primitive culling: completely drops the primitive before rasterization
            primitives[i].cull = (s_vertexW[idx0] <= 0.001 || s_vertexW[idx1] <= 0.001 || s_vertexW[idx2] <= 0.001);
        }
    }

    // =========================================================================
    // FRAGMENT SHADER (writes InstanceID + PrimitiveID, with alpha test for Mask)
    // =========================================================================
    struct VisOutput
    {
        uint2 visibility : SV_Target0;
    };

    [shader("fragment")]
    VisOutput fragMain(
        VSOutput inVert,
        uint primitiveID : SV_PrimitiveID,
        uniform PushConstantMeshlets pushConstant
    )
    {
        // Alpha mask discard: prevents transparent quad areas from writing depth & visibility
        InstanceData* instances = (InstanceData*)pushConstant.instanceReference;
        InstanceData instance = instances[inVert.drawID];

        if (instance.alphaMode == 1 && instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
            SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            float2 uv = (instance.baseColorTextureSet == 0) ? inVert.uv0 : inVert.uv1;
            float4 baseColor = baseColorTex.Sample(samp, uv);
            if ((baseColor.a * instance.baseColorFactor.a) < instance.alphaMaskCutoff)
            {
                discard;
            }
        }

        VisOutput output;
        // .x = Instance/Draw ID (+ 1 so 0 is reserved strictly for background)
        // .y = (MeshletID << 8) | (PrimitiveID & 0xFF)
        output.visibility = uint2(inVert.drawID + 1, (inVert.meshletID << 8) | (primitiveID & 0xFF));
        return output;
    }