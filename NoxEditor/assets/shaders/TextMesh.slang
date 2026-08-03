#include "shaderIO.h"

struct TextPayLoad
{
    uint textID;
};
groupshared TextPayLoad dummyPayLoad;

[shader("task")]
[numthreads(1, 1, 1)]
void taskMain(uniform PushConstantText pushConstant)
{
    DispatchMesh(pushConstant.numOfElements, 1, 1, dummyPayLoad);
}

struct VSOutput
{
    float4 pos : SV_Position;
    float2 texCooord;
    uint32_t materialIndex;
    int entityID;
    float4 color;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshMain
(
    out indices uint3 triangles[2], out vertices VSOutput vertices[4],
    uint3 DispatchThreadID : SV_DispatchThreadID,
    uniform PushConstantText pushConstant
)
{
    uint textID = DispatchThreadID.x;
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    TextData* textData = (TextData*)pushConstant.textDataReference;
    TextData text = textData[textID];

	SetMeshOutputCounts(4, 2); // 4 vertices, 2 triangles

	for (uint i = 0; i < 4; i++) 
    {
		// Determine which corner of the quad this vertex corresponds to
        uint quadVertex = i;
        float2 pos;
        float2 uv;

        // Correct winding order and flip X if needed
        if (quadVertex == 0) { pos = float2(text.quadMax.x, text.quadMin.y); uv = float2(text.texMax.x, text.texMin.y); } // bottom-right
        else if (quadVertex == 1) { pos = text.quadMax; uv = text.texMax; }                                                // top-right
        else if (quadVertex == 2) { pos = float2(text.quadMin.x, text.quadMax.y); uv = float2(text.texMin.x, text.texMax.y); } // top-left
        else { pos = text.quadMin; uv = text.texMin; }                                                                    // bottom-left

        float4 worldPos = mul(text.transform, float4(pos, 0.0, 1.0));
		vertices[i].pos = mul(ubo.proj, mul(ubo.view, worldPos));
        vertices[i].texCooord = uv;
        vertices[i].color = text.color;
        vertices[i].materialIndex =  text.materialIndex;
        vertices[i].entityID = text.entityID;
	}

	triangles[0] = uint3(0, 1, 2);
    triangles[1] = uint3(0, 2, 3);
}

struct FSOutput
{
    float4 color : SV_TARGET0; // Final color output (RGBA)
    int entityID : SV_TARGET1;
};

float screenPxRange(Texture2D atlas, float2 uv) 
{
    uint w, h;
    atlas.GetDimensions(w, h);
	const float pxRange = 2.0; // set to distance field's pixel range
    float2 unitRange = float2(pxRange)/float2(w, h);
    float2 screenTexSize = float2(1.0)/fwidth(uv);
    return max(0.5*dot(unitRange, screenTexSize), 1.0);
}

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, uniform PushConstantQuad pushConstant)
{
    FSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;

    Texture2D texHandle = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + vertIn.materialIndex, 0));
    SamplerState sampHandle = SamplerState.Handle(uint2(0, 0));
    float4 img = texHandle.Sample(sampHandle, vertIn.texCooord);
    
    float4 texColor = img * vertIn.color;

    float3 msd = img.rgb;
    float sd = median3(msd.r, msd.g, msd.b);

    float screenPxDistance = screenPxRange(texHandle, vertIn.texCooord) * (sd - 0.5);
    float opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
    if(opacity == 0.0)
        discard;

    float4 bgColor = float4(0.0);
    output.color = lerp(bgColor, vertIn.color, opacity);
    if(output.color.a == 0.0)
        discard;

    output.entityID = vertIn.entityID;

    return output;
}