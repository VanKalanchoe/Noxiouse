#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/NodeGraph/NodeGraph.h"

// The material graph (docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 4): a NodeGraph of the "MaterialGraph" domain
// (.nmatgraph, Unreal's Material Editor graph) whose nodes compute a surface -- base colour, metallic, roughness, normal, emissive,
// ambient occlusion, opacity -- and that CompileMaterialGraph turns into Slang source. The renderer puts every graph of the project in a
// namespace of its own inside the generated project shader file (CustomLitShaders.h) and calls the one a material names by slot, in the
// G-buffer resolve (opaque surfaces) and the forward shader (translucent surfaces, previews): one shader for all graphs, as for the
// Material Shaders.
//
// Every pin carries a float, float2, float3 or float4 (Unreal's rules: a float goes anywhere, a vector is cut to fewer components or
// padded with 0 to more; two different vectors do not combine). Values carry their screen derivatives along (Unreal's analytic
// derivatives): a Texture Sample takes its mip from its UV's derivatives, so the full-screen G-buffer resolve samples as a raster pass
// would. Material Output pins left unconnected keep the material's own value (its .nmat fields and textures).
namespace Nox
{
    inline constexpr const char* kMaterialGraphDomain = "MaterialGraph";
    inline constexpr uint32_t MaterialGraphMaxParameters = 16; // float4 slots (a scalar parameter takes one, in x)
    inline constexpr uint32_t MaterialGraphMaxTextures = 8;

    // Which Material Output pins the graph connects (the others keep the material's values).
    enum MaterialGraphOutput : uint32_t
    {
        MaterialGraphOutputBaseColor = 1u << 0,
        MaterialGraphOutputMetallic = 1u << 1,
        MaterialGraphOutputRoughness = 1u << 2,
        MaterialGraphOutputNormal = 1u << 3,
        MaterialGraphOutputEmissive = 1u << 4,
        MaterialGraphOutputOcclusion = 1u << 5,
        MaterialGraphOutputOpacity = 1u << 6,
    };

    // A value a material (.nmat) may override: a Scalar / Vector Parameter node's, or a Texture Sample's with a parameter name. Texture
    // Samples without a name get a slot too (Name "#<node id>", not overridable).
    struct MaterialGraphParameter
    {
        enum class Kind : uint8_t { Scalar, Vector, Texture };
        std::string Name;
        Kind Type = Kind::Scalar;
        glm::vec4 Default = glm::vec4(0.0f); // a scalar's in x
        uint64_t DefaultTexture = 0;         // a texture's asset (0: none -- white, or flat for a normal map)
        uint32_t Slot = 0;                   // into MaterialGraphInputs::parameters / textures
    };

    struct MaterialGraphProgram
    {
        bool Valid = false;
        std::string Error;  // why not (which node and what), for the editor and the log
        std::string Code;   // `static const uint Outputs = ...;` and `void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)`
        std::vector<MaterialGraphParameter> Parameters;
        uint32_t ParameterSlots = 0;
        uint32_t TextureSlots = 0;
        uint32_t Outputs = 0; // MaterialGraphOutput bits
    };

    // Registers the domain's node types (Application start, with the animation and audio graphs').
    void RegisterMaterialGraphNodeTypes();
    // A new graph: a Material Output node alone.
    NodeGraph CreateEmptyMaterialGraph();
    // The Slang source and parameter layout of a graph. Deterministic: the editor, a development run and a packaged game get the same
    // layout from the same graph (the packed material's parameter slots must match the shipped shader's).
    MaterialGraphProgram CompileMaterialGraph(const NodeGraph& graph);
}
