#include "shaderIO.h"

// The editor's selection mask (Renderer::addSelectionMaskPass): only the selected instances, drawn again at display
// resolution with the NON-jittered projection, into a single-channel target the outline reads. The outline used to find
// its edges in the entity IDs, which come from the jittered render-resolution visibility pass (nearest-blitted to display
// resolution), so with DLSS on its edges moved by the camera jitter every frame. The task stage is Material_PBR_MeshTask's
// (LOD cut and meshlet frustum culling); every pixel any selected geometry covers is set, hidden parts included, so the
// outline follows the selection's whole silhouette.

    struct VSOutput
    {
        float4 pos : SV_Position;
        float2 uv0;
        float2 uv1;
        nointerpolation uint drawID;
    };

    [shader("mesh")]
    [outputtopology("triangle")]
    [numthreads(MESH_SHADER_DISPATCH_X, 1, 1)]
    void meshMain
    (
        in payload MeshletPayload sharedPayload,
        out indices uint3 triangles[MAX_PRIMITIVES],
        out vertices VSOutput vertices[MAX_VERTICES],
        uint3 groupId : SV_GroupID,
        uint3 gtid : SV_GroupThreadID,
        uniform PushConstantMeshlets pushConstant
    )
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        SceneInstances instances = SceneInstances(ubo);

        InstanceData instance = instances[sharedPayload.drawID];
        float4x4 modelMatrix = instance.modelMatrix;

        MeshletDraw* meshletDrawStream = (MeshletDraw*)ubo->geometryMeshletDrawsReference;
        Vertex* vertexStream = (Vertex*)ubo->geometryVerticesReference;
        uint32_t* meshletVertexStream = (uint32_t*)ubo->geometryMeshletVerticesReference;
        uint8_t* meshletTriangleStream = (uint8_t*)ubo->geometryMeshletTrianglesReference;

        uint8_t localMeshletOffset = sharedPayload.meshletIndices[groupId.x];
        uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;

        MeshletDraw meshlet = meshletDrawStream[globalMeshletIdx];
        // Not ubo.proj: that one carries the sub-pixel jitter DLSS needs, which is what made the outline crawl.
        float4x4 viewProj = mul(ubo.nonJitteredProj, ubo.view);

        SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

        for (uint i = gtid.x; i < meshlet.vertexCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint localVertexIndex = meshletVertexStream[meshlet.vertexOffset + i];
            Vertex vert = vertexStream[meshlet.globalVertexOffset + localVertexIndex];

            float4 localPos = float4(vert.pos, 1.0);

            // Skinned like the visibility pass (VisibilityBuffer.slang): the compute-skinned cache, else direct skinning.
            if (instance.skinnedVertexOffset != NoSkinnedVertices && pushConstant.skinnedVertexReference != 0)
            {
                SkinnedVertex* skinnedVertices = (SkinnedVertex*)pushConstant.skinnedVertexReference;
                localPos = skinnedVertices[instance.skinnedVertexOffset + localVertexIndex].position;
            }
            else if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
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

            vertices[i].pos = mul(viewProj, mul(modelMatrix, localPos));
            vertices[i].uv0 = vert.uv0;
            vertices[i].uv1 = vert.uv1;
            vertices[i].drawID = sharedPayload.drawID;
        }

        GroupMemoryBarrierWithGroupSync();

        for (uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint triBase = meshlet.triangleOffset + (i * 3);
            triangles[i] = uint3(meshletTriangleStream[triBase], meshletTriangleStream[triBase + 1], meshletTriangleStream[triBase + 2]);
        }
    }

    struct MaskOutput
    {
        float mask : SV_Target0;
    };

    [shader("fragment")]
    MaskOutput fragMain(VSOutput inVert, uniform PushConstantMeshlets pushConstant)
    {
        // Alpha-masked surfaces (foliage cards) cut out like in the visibility pass, so the mask follows the leaves.
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        SceneInstances instances = SceneInstances(ubo);
        InstanceData instance = instances[inVert.drawID];

        if (instance.alphaMode == 1 && instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            float2 uv = (instance.baseColorTextureSet == 0) ? inVert.uv0 : inVert.uv1;
            if ((baseColorTex.Sample(samp, uv).a * instance.baseColorFactor.a) < instance.alphaMaskCutoff)
                discard;
        }

        MaskOutput output;
        output.mask = 1.0;
        return output;
    }
