#include "shaderIO.h"

struct VSOutput
{
    float4 pos : SV_Position;
    float3 worldPos;
    float3 normal;
    float2 uv0;
    float2 uv1;
    float4 color;
    nointerpolation uint drawID;

    nointerpolation int entityID;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MESH_SHADER_DISPATCH_X, 1, 1)]
void meshMain
(
    in payload MeshletPayload sharedPayload,
    out indices uint3 triangles[MAX_PRIMITIVES], out vertices VSOutput vertices[MAX_VERTICES],
    uint3 groupId : SV_GroupID, uint3 gtid : SV_GroupThreadID,
    uniform PushConstantMeshlets pushConstant
)
{
    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    InstanceData* instances = (InstanceData*)pushConstant.instanceReference;

    // Retrieve model matrix using drawID stored in payload
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
    
    // Fetch compacted meshlet index from task payload
    uint8_t localMeshletOffset = sharedPayload.meshletIndices[groupId.x];
    uint globalMeshletIdx = sharedPayload.groupMeshletOffset + localMeshletOffset;
    
    MeshletDraw meshlet = meshletDrawPage[globalMeshletIdx];
    float4x4 viewProj = mul(ubo.proj, ubo.view);

    uint hash = globalMeshletIdx * 747796405u + 2891336453u;
    float3 color = float3(
        (hash & 0xFF) / 255.0,
        ((hash >> 8) & 0xFF) / 255.0,
        ((hash >> 16) & 0xFF) / 255.0
    );

    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    // Process vertices
    for(uint i = gtid.x; i < meshlet.vertexCount; i += MESH_SHADER_DISPATCH_X)
    {
        uint localVertexIndex = meshletVerticesPage[meshlet.vertexOffset + i];
        Vertex v = vertexPage[meshlet.globalVertexOffset + localVertexIndex];

        float4 localPos = float4(v.pos, 1.0);
        float3 localNormal = v.normal;

        if(instance.boneMatrixOffset != 0xFFFFFFFF)
        {
            float4x4* boneMatrices = (float4x4*)pushConstant.boneMatrixReference;
            uint baseOffset = instance.boneMatrixOffset;

            // Blend bone matrices using vertex weights
            float4x4 skinTransform =
                boneMatrices[baseOffset + v.boneIDs.x] * v.boneWeights.x +
                boneMatrices[baseOffset + v.boneIDs.y] * v.boneWeights.y +
                boneMatrices[baseOffset + v.boneIDs.z] * v.boneWeights.z +
                boneMatrices[baseOffset + v.boneIDs.w] * v.boneWeights.w;

            localPos = mul(skinTransform, localPos);
            localNormal = normalize(mul((float3x3)skinTransform, localNormal));
        }

        float4 worldPos = mul(modelMatrix, localPos);
        float3 worldNormal = normalize(mul(float3x3(instance.normalMatrix), localNormal));
        float4 clipPos = mul(viewProj, worldPos);

        vertices[i].pos = clipPos;
        vertices[i].worldPos = worldPos.xyz;
        vertices[i].normal = worldNormal;
        vertices[i].uv0 = v.uv0;
        vertices[i].uv1 = v.uv1;
        vertices[i].color = float4(color, 1.0);
        vertices[i].entityID = instance.entityID;
        vertices[i].drawID = sharedPayload.drawID;
    }

    // Process triangles
    for(uint i = gtid.x; i < meshlet.triangleCount; i += MESH_SHADER_DISPATCH_X)
    {
        uint triOffset = meshlet.triangleOffset + i * 3;

        uint idx0 = meshletTrianglePage[triOffset + 0];
        uint idx1 = meshletTrianglePage[triOffset + 1];
        uint idx2 = meshletTrianglePage[triOffset + 2];

        triangles[i] = uint3(idx0, idx1, idx2);
    }
}

// Encapsulate the various inputs used by the various functions in the shading equation
// We store values in this struct to simplify the integration of alternative implementations
// of the shading terms, outlined in the Readme.MD Appendix.
struct PBRInfo
{
	float NdotL;                  // cos angle between normal and light direction
	float NdotV;                  // cos angle between normal and view direction
	float NdotH;                  // cos angle between normal and half vector
	float LdotH;                  // cos angle between light direction and half vector
	float VdotH;                  // cos angle between view direction and half vector
	float perceptualRoughness;    // roughness value, as authored by the model creator (input to shader)
	float metalness;              // metallic value at the surface
	float3 reflectance0;            // full reflectance color (normal incidence angle)
	float3 reflectance90;           // reflectance color at grazing angle
	float alphaRoughness;         // roughness mapped to a more linear change in the roughness (proposed by [2])
	float3 diffuseColor;            // color contribution from diffuse lighting
	float3 specularColor;           // color contribution from specular lighting
};

static const float M_PI = 3.141592653589793;
static const float c_MinRoughness = 0.04;

static const float PBR_WORKFLOW_METALLIC_ROUGHNESS = 0.0;
static const float PBR_WORKFLOW_SPECULAR_GLOSSINESS = 1.0;

import Tonemapping;
import Math;

// Find the normal for this fragment, pulling either from a predefined normal map
// or from the interpolated mesh normal and tangent attributes.
float3 getNormal(UniformBufferObject* ubo, InstanceData instance, VSOutput vertIn, SamplerState samplerState)
{
	// Perturb normal, see http://www.thetenthplanet.de/archives/1180
    Texture2D normalTexture = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.normalTextureIndex, 0));
    float3 tangentNormal = normalTexture.Sample(samplerState, instance.normalTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).xyz * 2.0 - 1.0;
	
    float3 q1 = ddx(vertIn.worldPos);
	float3 q2 = ddy(vertIn.worldPos);
	float2 st1 = ddx(vertIn.uv0);
	float2 st2 = ddy(vertIn.uv0);

	float3 N = normalize(vertIn.normal);
	float3 T = normalize(q1 * st2.y - q2 * st1.y);
	float3 B = normalize(cross(N, T));
	float3x3 TBN = float3x3(T, B, N);

	return normalize(mul(tangentNormal, TBN));
}

// Calculation of the lighting contribution from an optional Image Based Light source.
// Precomputed Environment Maps are required uniform inputs and are computed as outlined in [1].
// See our README.md on Environment Maps [3] for additional discussion.
float3 getIBLContribution(UniformBufferObject* ubo, PBRInfo pbrInputs, vec3 n, vec3 reflection)
{
	float lod = (pbrInputs.perceptualRoughness * ubo->prefilteredCubeMipLevels);

    SamplerState samplerBrdf = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_BRDFLUT, 0));
    SamplerState samplerIrradiance = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_IRRADIANCE, 0));
    SamplerState samplerPrefiltered = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_PREFILTER, 0));

    TonemapParameters tonemapParams;
    tonemapParams.exposure = ubo->exposure;
    tonemapParams.gamma = ubo->gamma;

    // retrieve a scale and bias to F0. See [1], Figure 3
    // Sample BRDF LUT (2D Texture from heap)
    Texture2D brdfTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + ubo->brdfLutIndex, 0));
    float3 brdf = brdfTex.Sample(samplerBrdf, float2(pbrInputs.NdotV, 1.0 - pbrInputs.perceptualRoughness)).rgb;

    // Sample Irradiance Map (Cube Texture from heap)
    TextureCube irradianceTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->irradianceMapIndex, 0));
    float4 irradianceSample = irradianceTex.Sample(samplerIrradiance, n);
    float3 diffuseLight = SRGBtoLINEAR(tonemap(irradianceSample, tonemapParams)).rgb;

    // Sample Prefiltered Environment Map with Mip Level LOD (Cube Texture from heap)
    TextureCube prefilteredTex = TextureCube.Handle(uint2(ubo->imageHeapIndexOffset + ubo->prefilteredMapIndex, 0));
    float4 prefilteredSample = prefilteredTex.SampleLevel(samplerPrefiltered, reflection, lod);
    float3 specularLight = SRGBtoLINEAR(tonemap(prefilteredSample, tonemapParams)).rgb;

	float3 diffuse = diffuseLight * pbrInputs.diffuseColor;
	float3 specular = specularLight * (pbrInputs.specularColor * brdf.x + brdf.y);

	// For presentation, this allows us to disable IBL terms
	// For presentation, this allows us to disable IBL terms
	diffuse *= ubo->scaleIBLAmbient;
	specular *= ubo->scaleIBLAmbient;

	return diffuse + specular;
}

// Basic Lambertian diffuse
// Implementation from Lambert's Photometria https://archive.org/details/lambertsphotome00lambgoog
// See also [1], Equation 1
float3 diffuse(PBRInfo pbrInputs)
{
	return pbrInputs.diffuseColor / M_PI;
}

// The following equation models the Fresnel reflectance term of the spec equation (aka F())
// Implementation of fresnel from [4], Equation 15
float3 specularReflection(PBRInfo pbrInputs)
{
	return pbrInputs.reflectance0 + (pbrInputs.reflectance90 - pbrInputs.reflectance0) * pow(clamp(1.0 - pbrInputs.VdotH, 0.0, 1.0), 5.0);
}

// This calculates the specular geometric attenuation (aka G()),
// where rougher material will reflect less light back to the viewer.
// This implementation is based on [1] Equation 4, and we adopt their modifications to
// alphaRoughness as input as originally proposed in [2].
float geometricOcclusion(PBRInfo pbrInputs)
{
	float NdotL = pbrInputs.NdotL;
	float NdotV = pbrInputs.NdotV;
	float r = pbrInputs.alphaRoughness;

	float attenuationL = 2.0 * NdotL / (NdotL + sqrt(r * r + (1.0 - r * r) * (NdotL * NdotL)));
	float attenuationV = 2.0 * NdotV / (NdotV + sqrt(r * r + (1.0 - r * r) * (NdotV * NdotV)));
	return attenuationL * attenuationV;
}

// The following equation(s) model the distribution of microfacet normals across the area being drawn (aka D())
// Implementation from "Average Irregularity Representation of a Roughened Surface for Ray Reflection" by T. S. Trowbridge, and K. P. Reitz
// Follows the distribution function recommended in the SIGGRAPH 2013 course notes from EPIC Games [1], Equation 3.
float microfacetDistribution(PBRInfo pbrInputs)
{
	float roughnessSq = pbrInputs.alphaRoughness * pbrInputs.alphaRoughness;
	float f = (pbrInputs.NdotH * roughnessSq - pbrInputs.NdotH) * pbrInputs.NdotH + 1.0;
	return roughnessSq / (M_PI * f * f);
}

// Gets metallic factor from specular glossiness workflow inputs 
float convertMetallic(vec3 diffuse, vec3 specular, float maxSpecular) {
	float perceivedDiffuse = sqrt(0.299 * diffuse.r * diffuse.r + 0.587 * diffuse.g * diffuse.g + 0.114 * diffuse.b * diffuse.b);
	float perceivedSpecular = sqrt(0.299 * specular.r * specular.r + 0.587 * specular.g * specular.g + 0.114 * specular.b * specular.b);
	if (perceivedSpecular < c_MinRoughness) {
		return 0.0;
	}
	float a = c_MinRoughness;
	float b = perceivedDiffuse * (1.0 - maxSpecular) / (1.0 - c_MinRoughness) + perceivedSpecular - 2.0 * c_MinRoughness;
	float c = c_MinRoughness - perceivedSpecular;
	float D = max(b * b - 4.0 * a * c, 0.0);
	return clamp((-b + sqrt(D)) / (2.0 * a), 0.0, 1.0);
}

struct FSOutput
{
    float4 color : SV_TARGET0; // Final color output (RGBA)
    int entityID : SV_TARGET1;
};

[shader("fragment")]
FSOutput fragMain(VSOutput vertIn, bool isFrontFace : SV_IsFrontFace, uniform PushConstantMeshlets pushConstant)
{
    FSOutput output;

    UniformBufferObject* ubo = (UniformBufferObject*)pushConstant.matrixReference;
    InstanceData* instances = (InstanceData*)pushConstant.instanceReference;
    InstanceData instance = instances[vertIn.drawID];

    SamplerState sampLinearHandle = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));

    float perceptualRoughness;
	float metallic;
	float3 diffuseColor;
	float4 baseColor;

    float3 f0 = float3(0.04);

    if(instance.alphaMode == 1) // 1 = Mask
    {
        if(instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = baseColorTex.Sample(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1) * instance.baseColorFactor;
        }
        else
        {
            baseColor = instance.baseColorFactor;
        }
        if(baseColor.a < instance.alphaMaskCutoff)
        {
            discard;
        }
    }

    if(instance.workflow == PBR_WORKFLOW_METALLIC_ROUGHNESS)
    {
        // Metallic and Roughness material properties are packed together
		// In glTF, these factors can be specified by fixed scalar values
		// or from a metallic-roughness map
        perceptualRoughness = instance.roughnessFactor;
        metallic = instance.metallicFactor;
        if(instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            // Roughness is stored in the 'g' channel, metallic is stored in the 'b' channel.
			// This layout intentionally reserves the 'r' channel for (optional) occlusion map data
            Texture2D metallicRoughnessTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            float4 mrSample = metallicRoughnessTex.Sample(sampLinearHandle, instance.physicalDescriptorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1);
            perceptualRoughness = mrSample.g * perceptualRoughness;
			metallic = mrSample.b * metallic;
        }
        else
        {
            perceptualRoughness = clamp(perceptualRoughness, c_MinRoughness, 1.0);
			metallic = clamp(metallic, 0.0, 1.0);
        }
        // Roughness is authored as perceptual roughness; as is convention,
		// convert to material roughness by squaring the perceptual roughness [2].

		// The albedo may be defined from a base texture or a flat color
        if(instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D baseColorTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            baseColor = baseColorTex.Sample(sampLinearHandle, instance.baseColorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1) * instance.baseColorFactor;
        }
        else
        {
            baseColor = instance.baseColorFactor;
        }
    }

    if(instance.workflow == PBR_WORKFLOW_SPECULAR_GLOSSINESS)
    {
        // Values from specular glossiness workflow are converted to metallic roughness
        if(instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D metallicRoughnessTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            perceptualRoughness = 1.0 - metallicRoughnessTex.Sample(sampLinearHandle, instance.physicalDescriptorTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).a;
        }
        else
        {
            perceptualRoughness = 0.0;
        }

        const float epsilon = 1e-6;

        float4 diffuse = float4(1.0);
        if(instance.baseColorTextureIndex != 0xFFFFFFFF)
        {
            Texture2D colorMap = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.baseColorTextureIndex, 0));
            diffuse = SRGBtoLINEAR(colorMap.Sample(sampLinearHandle, vertIn.uv0)); 
        }

        float3 specular = float3(0.0);
        if(instance.metallicRoughnessTextureIndex != 0xFFFFFFFF)
        {
            Texture2D specGlossTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.metallicRoughnessTextureIndex, 0));
            specular = SRGBtoLINEAR(specGlossTex.Sample(sampLinearHandle, vertIn.uv0)).rgb;
        }

        float maxSpecular = max(max(specular.r, specular.g), specular.b);

        // Convert metallic value from specular glossiness inputs
		metallic = convertMetallic(diffuse.rgb, specular, maxSpecular);
        
        float3 baseColorDiffusePart = diffuse.rgb * ((1.0 - maxSpecular) / (1 - c_MinRoughness) / max(1 - metallic, epsilon)) * instance.diffuseFactor.rgb;
		float3 baseColorSpecularPart = specular - (float3(c_MinRoughness) * (1 - metallic) * (1 / max(metallic, epsilon))) * instance.specularFactor.rgb;
		baseColor = float4(lerp(baseColorDiffusePart, baseColorSpecularPart, metallic * metallic), diffuse.a);
    }

    //baseColor *= vertIn.color; // im confused do i need this ? i think my colors are not in vertex but in instance basecolorfactor no ?

    diffuseColor = baseColor.rgb * (float3(1.0) - f0);
	diffuseColor *= 1.0 - metallic;

    float alphaRoughness = perceptualRoughness * perceptualRoughness;

	float3 specularColor = lerp(f0, baseColor.rgb, metallic);

    // Compute reflectance.
	float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);

    // For typical incident reflectance range (between 4% to 100%) set the grazing reflectance to 100% for typical fresnel effect.
	// For very low reflectance range on highly diffuse objects (below 4%), incrementally reduce grazing reflecance to 0%.
	float reflectance90 = clamp(reflectance * 25.0, 0.0, 1.0);
	float3 specularEnvironmentR0 = specularColor.rgb;
	float3 specularEnvironmentR90 = float3(1.0, 1.0, 1.0) * reflectance90;

    float3 n = (instance.normalTextureIndex != 0xFFFFFFFF) ? getNormal(ubo, instance, vertIn, sampLinearHandle) : vertIn.normal;
    // glTF specification: back-face MUST have its normals reversed before lighting is evaluated
    if (instance.doubleSided != 0 && !isFrontFace)
    {
        n = -n;
    }
    //n.y *= -1.0f;
    float3 v = normalize(ubo->cameraWorldPos.xyz - vertIn.worldPos);    // Vector from surface point to camera
    float3 reflection = normalize(reflect(-v, n));
    float NdotV = clamp(abs(dot(n, v)), 0.001, 1.0);

    float3 directLighting = float3(0.0);

    // Cas Buffer device adress to dynamic LightData array
    LightData* lights = (LightData*)ubo->lightDataReference;

    for(uint i = 0; i < ubo->lightCount; i++)
    {
        LightData light = lights[i];
        uint lightType = (uint)light.position.w;

        float3 l;
        float attenuation = 1.0;

        if(lightType == 0) // Directional Light
        {
            l = normalize(light.direction.xyz); // Vector from surface point to light
        }
        else // Point Light (1) or Spot Light (2)
        {
            float3 lightVec = light.position.xyz - vertIn.worldPos;
            float distance = length(lightVec);
            l = distance > 0.0001 ? lightVec / distance : float3(0.0, 1.0, 0.0);

            float range = light.direction.w;
            if (range > 0.0)
            {
                float dOverR = distance / range;

                float window = max(1.0 -  pow(dOverR, 4.0), 0.0);

                attenuation = window / (distance * distance);
            }
            else
            {
                attenuation = 1.0 / (distance * distance);
            }

            if(lightType == 2) // Spot Light Cone Attenuatuion
            {
                float cd = dot(normalize(light.direction.xyz), -l);
                float lightAngleScale = light.spotParams.x;
                float lightAngleOffset = light.spotParams.y;

                float angularAttenuation = saturate(cd * lightAngleScale + lightAngleOffset);
                angularAttenuation *= angularAttenuation;

                attenuation *= angularAttenuation;
            }
        }

        if(attenuation <= 0.0)
        {
            continue;
        }

        float3 h = normalize(l+v);                        // Half vector between both l and v

        float NdotL = clamp(dot(n, l), 0.001, 1.0);
        float NdotH = clamp(dot(n, h), 0.0, 1.0);
	    float LdotH = clamp(dot(l, h), 0.0, 1.0);
	    float VdotH = clamp(dot(v, h), 0.0, 1.0);

        PBRInfo pbrInputs = PBRInfo(
            NdotL,
            NdotV,
		    NdotH,
		    LdotH,
		    VdotH,
            perceptualRoughness,
		    metallic,
		    specularEnvironmentR0,
		    specularEnvironmentR90,
		    alphaRoughness,
		    diffuseColor,
		    specularColor
        );

        // Calculate the shading terms for the microfacet specular shading model
        float3 F = specularReflection(pbrInputs);
	    float G = geometricOcclusion(pbrInputs);
	    float D = microfacetDistribution(pbrInputs);

        // Calculation of analytical lighting contribution
	    float3 diffuseContrib = (1.0 - F) * diffuse(pbrInputs);
	    float3 specContrib = F * G * D / (4.0 * NdotL * NdotV);

        // Obtain final intensity as reflectance (BRDF) scaled by the energy of the light (cosine law)
        float3 lightRadiance = light.color.rgb * light.color.w * attenuation;
	    directLighting += NdotL * lightRadiance * (diffuseContrib + specContrib);
    }

    PBRInfo iblInputs = PBRInfo(
            1.0,
            NdotV,
            1.0,
            1.0,
            1.0,
            perceptualRoughness,
            metallic,
            specularEnvironmentR0,
            specularEnvironmentR90,
            alphaRoughness,
            diffuseColor,
            specularColor
        );

    // Calculate lighting contribution from image based lighting source (IBL)
	float3 color = directLighting + getIBLContribution(ubo, iblInputs, n, reflection);

    const float u_OcclusionStrength = 1.0f;
	// Apply optional PBR terms for additional (optional) shading
    if(instance.occlusionTextureIndex != 0xFFFFFFFF)
    {
        Texture2D occlusionTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.occlusionTextureIndex, 0));
        float oa = occlusionTex.Sample(sampLinearHandle, instance.occlusionTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).r;
        color = lerp(color, color * oa, u_OcclusionStrength);
    }

    float3 emissive = instance.emissiveFactor.rgb * instance.emissiveStrength;
    if(instance.emissiveTextureIndex != 0xFFFFFFFF)
    {
        Texture2D emissiveTex = Texture2D.Handle(uint2(ubo->imageHeapIndexOffset + instance.emissiveTextureIndex, 0));
        emissive *= emissiveTex.Sample(sampLinearHandle, instance.emissiveTextureSet == 0 ? vertIn.uv0 : vertIn.uv1).rgb;
    }
    color += emissive;

    output.color = float4(color, baseColor.a);

    output.entityID = vertIn.entityID;

    return output;
}