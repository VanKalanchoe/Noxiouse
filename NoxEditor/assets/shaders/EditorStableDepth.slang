#include "shaderIO.h"

// The editor's display-resolution entity IDs and depth while the camera is jittered (DLSS / DLAA, Renderer::addStableEntityDepthPass).
// They used to be a nearest blit of the jittered render-resolution targets, so everything reading them moved with the jitter every
// frame: the 2D overlay's depth test (the grid, gizmo lines and collider wireframes crawled along geometry edges), picking and the
// measure tool's depth. The camera's visible instances are drawn again here with the NON-jittered projection, depth and entity IDs
// only. The task stage is Material_PBR_MeshTask's (LOD cut, meshlet culling).

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
    float3 localPos : TEXCOORD8;    // (Local Position: before skinning and morphs)
    float4 tangent : TEXCOORD9;     // (Vertex Tangent / Bitangent: world, w the handedness)
#endif
    nointerpolation uint drawID : TEXCOORD6;
    nointerpolation int entityID : TEXCOORD7;
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
    // Not ubo.proj: that one carries the sub-pixel jitter DLSS needs.
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
        // A graph's World Position Offset (as the visibility pass moves the vertex: picking and the overlays' depth follow it).
        worldPos.xyz += material_graph_vertex_offset(instances.material(sharedPayload.drawID), instance, vert, localVertexIndex,
                                                     meshlet.globalVertexOffset + localVertexIndex, worldPos.xyz, ubo, pushConstant);
#endif
        vertices[i].pos = depth_bias_clip(mul(viewProj, worldPos), worldPos.xyz, ubo.cameraWorldPos.xyz, instance.depthBias); // as the visibility pass
        vertices[i].uv0 = vert.uv0;
        vertices[i].uv1 = vert.uv1;
        vertices[i].vertexAlpha = instance.alphaMode == 1 ? loadMeshVertexColor(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.vertexColors).a : 1.0;
#ifdef NOX_MATERIAL_GRAPH
        vertices[i].worldPos = worldPos.xyz;
        vertices[i].normal = material_graph_vertex_normal(instance, vert, localVertexIndex, pushConstant);
        vertices[i].vertexColor = loadMeshVertexColor(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.vertexColors);
        vertices[i].localPos = vert.pos;
        vertices[i].tangent = material_graph_vertex_tangent(instance, vert, localVertexIndex, pushConstant);
#endif
        vertices[i].drawID = sharedPayload.drawID;
        vertices[i].entityID = instance.entityID;
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
    {
        uint triBase = meshlet.triangleOffset + (i * 3);
        triangles[i] = uint3(LoadByte(meshletTriangleStream, triBase), LoadByte(meshletTriangleStream, triBase + 1), LoadByte(meshletTriangleStream, triBase + 2));
    }
}

struct StableOutput
{
    int entityID : SV_Target0;
};

[shader("fragment")]
StableOutput fragMain(VSOutput inVert, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
{
    // Alpha-masked surfaces (foliage cards) cut out like in the visibility pass.
    UniformBufferObject ubo = loadUbo(pushConstant.matrixReference);
    SceneInstances instances = SceneInstances(ubo);
    InstanceData instance = instances[inVert.drawID];
#ifdef NOX_MATERIAL_GRAPH
    bool graphMask;
    if (material_graph_mask_cuts(inVert.uv0, inVert.uv1, inVert.worldPos, inVert.normal, inVert.vertexColor, isFrontFace, mul(ubo.nonJitteredProj, ubo.view),
                                 ubo, instances.material(inVert.drawID), instance, graphMask, inVert.localPos, inVert.tangent))
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

    StableOutput output;
    output.entityID = inVert.entityID;
    return output;
}
