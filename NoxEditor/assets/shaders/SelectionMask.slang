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
        float2 uv0 : TEXCOORD0;
        float2 uv1 : TEXCOORD1;
        float vertexAlpha : TEXCOORD2; // vertex color alpha: part of the mask's alpha (VisibilityBuffer.slang)
#ifdef NOX_MATERIAL_GRAPH
        float3 worldPos : TEXCOORD3;    // (a material graph's Opacity Mask: MaterialGraph.slang's material_graph_mask_cuts)
        float3 normal : TEXCOORD4;
        float4 vertexColor : TEXCOORD5;
#endif
        nointerpolation uint drawID : TEXCOORD6;
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
        UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
        SceneInstances instances = SceneInstances(ubo);

        InstanceData instance = instances[sharedPayload.drawID];
        float4x4 modelMatrix = instance.modelMatrix;

        BufferRef<MeshletDraw> meshletDrawStream = BufferRef<MeshletDraw>(ubo.geometryMeshletDrawsReference);
        BufferRef<uint32_t> meshletVertexStream = BufferRef<uint32_t>(ubo.geometryMeshletVerticesReference);
        BufferRef<uint> meshletTriangleStream = BufferRef<uint>(ubo.geometryMeshletTrianglesReference);

        uint localMeshletOffset = payload_meshlet_index(sharedPayload, groupId.x);
        uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;

        MeshletDraw meshlet = meshletDrawStream[globalMeshletIdx];
        // Not ubo.proj: that one carries the sub-pixel jitter DLSS needs, which is what made the outline crawl.
        float4x4 viewProj = mul(ubo.nonJitteredProj, ubo.view);

        SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

        for (uint i = gtid.x; i < meshlet.vertexCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint localVertexIndex = meshletVertexStream[meshlet.vertexOffset + i];
            Vertex vert = loadMeshVertex(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.boneMatrixOffset != 0xFFFFFFFF);

            float4 localPos = float4(vert.pos, 1.0);

            // Skinned like the visibility pass (VisibilityBuffer.slang): the compute-skinned cache, else direct skinning.
            if (instance.skinnedVertexOffset != NoSkinnedVertices && pushConstant.skinnedVertexReference != 0)
            {
                BufferRef<SkinnedVertex> skinnedVertices = BufferRef<SkinnedVertex>(pushConstant.skinnedVertexReference);
                localPos = skinnedVertices[instance.skinnedVertexOffset + localVertexIndex].position;
            }
            else if (instance.boneMatrixOffset != 0xFFFFFFFF && pushConstant.boneMatrixReference != 0)
            {
                BufferRef<float4x4> boneMatrices = BufferRef<float4x4>(pushConstant.boneMatrixReference);
                uint baseOffset = instance.boneMatrixOffset;
                float4x4 skinTransform =
                    boneMatrices[baseOffset + vert.boneIDs.x] * vert.boneWeights.x +
                    boneMatrices[baseOffset + vert.boneIDs.y] * vert.boneWeights.y +
                    boneMatrices[baseOffset + vert.boneIDs.z] * vert.boneWeights.z +
                    boneMatrices[baseOffset + vert.boneIDs.w] * vert.boneWeights.w;
                localPos = mul(skinTransform, localPos);
            }

            float4 worldPos = mul(modelMatrix, localPos);
#ifdef NOX_MATERIAL_GRAPH
            // A graph's World Position Offset (as the visibility pass moves the vertex: the outline follows the moving surface).
            worldPos.xyz += material_graph_vertex_offset(instances.material(sharedPayload.drawID), instance, vert, localVertexIndex,
                                                         meshlet.globalVertexOffset + localVertexIndex, worldPos.xyz, ubo, pushConstant);
#endif
            vertices[i].pos = mul(viewProj, worldPos);
            vertices[i].uv0 = vert.uv0;
            vertices[i].uv1 = vert.uv1;
            vertices[i].vertexAlpha = instance.alphaMode == 1 ? loadMeshVertexColor(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.vertexColors).a : 1.0;
#ifdef NOX_MATERIAL_GRAPH
            vertices[i].worldPos = worldPos.xyz;
            vertices[i].normal = material_graph_vertex_normal(instance, vert, localVertexIndex, pushConstant);
            vertices[i].vertexColor = loadMeshVertexColor(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.vertexColors);
#endif
            vertices[i].drawID = sharedPayload.drawID;
        }

        GroupMemoryBarrierWithGroupSync();

        for (uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint triBase = meshlet.triangleOffset + (i * 3);
            triangles[i] = uint3(LoadByte(meshletTriangleStream, triBase), LoadByte(meshletTriangleStream, triBase + 1), LoadByte(meshletTriangleStream, triBase + 2));
        }
    }

    struct MaskOutput
    {
        float mask : SV_Target0;
    };

    [shader("fragment")]
    MaskOutput fragMain(VSOutput inVert, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
    {
        // Alpha-masked surfaces (foliage cards) cut out like in the visibility pass, so the mask follows the leaves.
        UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
        SceneInstances instances = SceneInstances(ubo);
        InstanceData instance = instances[inVert.drawID];
#ifdef NOX_MATERIAL_GRAPH
        bool graphMask;
        if (material_graph_mask_cuts(inVert.uv0, inVert.uv1, inVert.worldPos, inVert.normal, inVert.vertexColor, isFrontFace, mul(ubo.nonJitteredProj, ubo.view),
                                     ubo, instances.material(inVert.drawID), instance, graphMask))
            discard;
        if (!graphMask)
#endif

        if (instance.alphaMode == 1 && (instance.baseColorTextureIndex != 0xFFFFFFFF || instance.vertexColors != 0))
        {
            float alpha = instance.baseColorFactor.a * inVert.vertexAlpha;
            if (instance.baseColorTextureIndex != 0xFFFFFFFF)
            {
                SamplerState samp = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
                float2 uv = instance_texture_uv(instance, instance.baseColorTextureSet, inVert.uv0, inVert.uv1);
                alpha *= SampleMaterialTexture(ubo, instance.baseColorTextureIndex, samp, uv).a;
            }
            if (alpha < instance.alphaMaskCutoff)
                discard;
        }

        MaskOutput output;
        output.mask = 1.0;
        return output;
    }
