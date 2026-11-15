#include "shaderIO.h"

// The material graph variant (VisibilityBufferMaterialGraph.slang, generated: NOX_MATERIAL_GRAPH and the project's graphs): a Masked
// material whose graph connects Opacity Mask tests that instead of its base colour's alpha (material_graph_opacity_mask), so the
// visibility buffer and the shadow maps hold the graph's shape; a graph's World Position Offset moves the vertices (the renderer draws
// materials with one in the Masked buckets, which use this variant: Unreal's programmable raster bins). Its vertices carry what a graph's
// inputs read besides the UVs.

    struct VSOutput
    {
        float4 pos : SV_Position;
        float2 uv0;
        float2 uv1;
        float vertexAlpha; // vertex color alpha (glTF COLOR_0): part of the base color's alpha the mask tests
        nointerpolation uint drawID;
        nointerpolation uint meshletID;
#ifdef NOX_MATERIAL_GRAPH
        float3 worldPos;    // render space (camera-relative origin: UniformBufferObject::renderOrigin)
        float3 normal;      // world
        float4 vertexColor; // glTF COLOR_0 (white without)
        float3 localPos;    // before skinning and morphs, the mesh's space (a graph's Local Position)
        float4 tangent;     // world, skinned; w the bitangent's handedness (Vertex Tangent / Bitangent)
#endif
    };

    struct PrimOutput
    {
        uint primitiveID : SV_PrimitiveID;
        bool cull : SV_CullPrimitive;
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
        SceneInstances instances = SceneInstances(ubo);

        // The payload carries the instance slot.
        InstanceData instance = instances[sharedPayload.drawID];
        float4x4 modelMatrix = instance.modelMatrix;

        MeshletDraw* meshletDrawStream = (MeshletDraw*)ubo->geometryMeshletDrawsReference;
        uint32_t* meshletVertexStream = (uint32_t*)ubo->geometryMeshletVerticesReference;
        uint8_t* meshletTriangleStream = (uint8_t*)ubo->geometryMeshletTrianglesReference;

        uint8_t localMeshletOffset = sharedPayload.meshletIndices[groupId.x];
        uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;

        MeshletDraw meshlet = meshletDrawStream[globalMeshletIdx];
        float4x4 viewProj = mul(ubo.proj, ubo.view);

        SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

        // Process vertices (Position, UVs & IDs)
        for (uint i = gtid.x; i < meshlet.vertexCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint localVertexIndex = meshletVertexStream[meshlet.vertexOffset + i];
            Vertex vert = loadMeshVertex(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.boneMatrixOffset != 0xFFFFFFFF);

            float4 localPos = float4(vert.pos, 1.0);

            // Compute-skinned geometry is shared with the G-buffer resolve. Keep direct skinning as a safe fallback for
            // a frame where the cache is unavailable (for example immediately after shader hot reload).
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

            float4 worldPos = mul(modelMatrix, localPos);
#ifdef NOX_MATERIAL_GRAPH
            // A graph's World Position Offset (the same function the G-buffer resolve rebuilds the triangle with; the shadow maps draw
            // with this mesh stage too, so the shadow moves with the surface).
            worldPos.xyz += material_graph_vertex_offset(instances.material(sharedPayload.drawID), instance, vert, localVertexIndex,
                                                         meshlet.globalVertexOffset + localVertexIndex, worldPos.xyz, ubo, pushConstant);
            // A graph's World Position, Vertex Normal (skinned as the position is) and Vertex Color.
            vertices[i].worldPos = worldPos.xyz;
            vertices[i].normal = material_graph_vertex_normal(instance, vert, localVertexIndex, pushConstant);
            vertices[i].vertexColor = loadMeshVertexColor(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.vertexColors);
            vertices[i].localPos = vert.pos;
            vertices[i].tangent = material_graph_vertex_tangent(instance, vert, localVertexIndex, pushConstant);
#endif
            // The material's depth bias (depth_bias_clip): decals and overlays over the surface they lie on.
            float4 clipPos = depth_bias_clip(mul(viewProj, worldPos), worldPos.xyz, ubo->cameraWorldPos.xyz, instance.depthBias);
            vertices[i].pos = clipPos;
            vertices[i].uv0 = vert.uv0;
            vertices[i].uv1 = vert.uv1;
            vertices[i].vertexAlpha = instance.alphaMode == 1 ? loadMeshVertexColor(ubo, meshlet.globalVertexOffset + localVertexIndex, instance.vertexColors).a : 1.0;
            vertices[i].drawID = sharedPayload.drawID;
            vertices[i].meshletID = globalMeshletIdx;
        }

         GroupMemoryBarrierWithGroupSync();

        // Process triangle indices & per-primitive ID
        for (uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
        {
            uint triBase = meshlet.triangleOffset + (i * 3);
            uint idx0 = meshletTriangleStream[triBase];
            uint idx1 = meshletTriangleStream[triBase + 1];
            uint idx2 = meshletTriangleStream[triBase + 2];

            triangles[i] = uint3(idx0, idx1, idx2);
            primitives[i].primitiveID = i;
            // Let rasterization clip triangles against the near plane. Culling the
            // complete triangle here makes large surfaces disappear near the camera.
            primitives[i].cull = false;
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
        bool isFrontFace : SV_IsFrontFace,
        uniform PushConstantMeshlets pushConstant
    )
    {
        // Alpha mask discard: prevents transparent quad areas from writing depth & visibility
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        SceneInstances instances = SceneInstances(ubo);
        InstanceData instance = instances[inVert.drawID];

#ifdef NOX_MATERIAL_GRAPH
        bool graphMask;
        if (material_graph_mask_cuts(inVert.uv0, inVert.uv1, inVert.worldPos, inVert.normal, inVert.vertexColor, isFrontFace, mul(ubo->proj, ubo->view), ubo,
                                     instances.material(inVert.drawID), instance, graphMask, inVert.localPos, inVert.tangent))
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
            {
                discard;
            }
        }

        VisOutput output;
        // .x = instance slot (+ 1 so 0 is reserved strictly for background)
        // .y = (MeshletID << 8) | (PrimitiveID & 0xFF)
        output.visibility = uint2(inVert.drawID + 1, (inVert.meshletID << 8) | (primitiveID & 0xFF));
        return output;
    }

    // =========================================================================
    // SHADOW MAP DEPTH (Renderer::addShadowMapPasses): the same geometry from a cascade's view, depth only
    // =========================================================================
    // Opaque casters: nothing to compute (an empty fragment stage keeps early depth testing).
    [shader("fragment")]
    void depthFragMain(VSOutput inVert)
    {
    }

    // Masked casters: the visibility pass's alpha test, so leaves cast the shape of their texture as with the RT shadows.
    [shader("fragment")]
    void depthMaskFragMain(VSOutput inVert, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
    {
        UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
        SceneInstances instances = SceneInstances(ubo);
        InstanceData instance = instances[inVert.drawID];

#ifdef NOX_MATERIAL_GRAPH
        // (the graph's view inputs -- Camera Vector, Screen Position -- are the shadow view's here, as in Unreal's shadow depths)
        bool graphMask;
        if (material_graph_mask_cuts(inVert.uv0, inVert.uv1, inVert.worldPos, inVert.normal, inVert.vertexColor, isFrontFace, mul(ubo->proj, ubo->view), ubo,
                                     instances.material(inVert.drawID), instance, graphMask, inVert.localPos, inVert.tangent))
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
    }
