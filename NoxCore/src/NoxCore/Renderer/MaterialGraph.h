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
        // Opacity Mask (Unreal's: a Masked material's pixels below its Opacity Mask Clip Value are not drawn): its own function,
        // opacity_mask(), evaluated by the passes that test the mask (the visibility buffer, the shadow maps, the forward shader).
        MaterialGraphOutputOpacityMask = 1u << 7,
        // World Position Offset (Unreal's): a vector in world units added to each vertex's world position, its own function,
        // world_position_offset(), evaluated per vertex by every pass that draws the geometry (the visibility buffer, the shadow maps,
        // the G-buffer resolve -- this frame and the last, for motion vectors --, the forward shader, the editor's overlays).
        MaterialGraphOutputWorldPositionOffset = 1u << 8,
        // Custom Light / Custom Ambient (material graph step 4c part 5): the graph's own lighting for a Custom Lit material -- each light's
        // share and the light from all around, as a Material Shader's light() / ambient() (CustomLit.slang). Read by the Custom Lit pass
        // and the forward shader through the Material Shaders' dispatcher (the graph takes a slot there: MaterialGraphProgram::LightCode).
        MaterialGraphOutputCustomLight = 1u << 9,
        MaterialGraphOutputCustomAmbient = 1u << 10,
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
        uint32_t Slot = 0;                   // into GpuMaterial::graphParameters / graphTextureIndex
    };

    struct MaterialGraphProgram
    {
        bool Valid = false;
        std::string Error;  // why not (which node and what), for the editor and the log
        // `static const uint Outputs = ...;`, `void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)`,
        // `float opacity_mask(MaterialGraphInputs i)` (only the nodes feeding the Opacity Mask pin; 1 when it is not connected) and
        // `float3 world_position_offset(MaterialGraphInputs i)` (only the nodes feeding World Position Offset; 0 when not connected).
        std::string Code;
        // `float opacity_mask(MaterialGraphInputs i)` alone (the ray tracing passes' variants get the graphs' masks only: MaterialGraphMasks.slang,
        // which changes only when a mask does). Empty when the graph does not connect Opacity Mask.
        std::string MaskCode;
        // `void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)` alone: the ray tracing passes that shade their hits (reflections,
        // the path tracer, the GI passes) get the graphs' surfaces from MaterialGraphSurfaces.slang -- no World Position Offset, so an offset
        // edit leaves them alone.
        std::string SurfaceCode;
        // `float3 light(CustomLitSurface s, CustomLitLight l)` and `float3 ambient(CustomLitSurface s, CustomLitAmbient a)` from Custom Light
        // and Custom Ambient (an unconnected one lights as Default Lit's diffuse would); empty when the graph connects neither.
        std::string LightCode;
        std::vector<MaterialGraphParameter> Parameters;
        uint32_t ParameterSlots = 0;
        uint32_t TextureSlots = 0;
        uint32_t Outputs = 0; // MaterialGraphOutput bits
        // The Material Output's Max World Position Offset Displacement (Unreal's, world units): the offset is clamped to that length and
        // the culling bounds grow by it, so a moving surface is not culled where its mesh's bounds end. 0: neither (no clamp, the mesh's
        // own bounds -- a surface pushed past them may be culled while still on screen).
        float MaxDisplacement = 0.0f;
    };

    // A graph of the project, compiled: its path in the project's Assets (generic separators; what MaterialData::SurfaceGraph names).
    struct ProjectMaterialGraph
    {
        std::string Path;
        MaterialGraphProgram Program;
    };

    // The active project's material graphs read from its Assets folder and compiled, sorted by path (CustomLitProject.cpp). `complete`
    // says whether every one registered was read: false in a packaged game (its assets are in the pack: Renderer loads them as assets).
    std::vector<ProjectMaterialGraph> GatherProjectMaterialGraphs(bool& complete);

    // Registers the domain's node types (Application start, with the animation and audio graphs').
    void RegisterMaterialGraphNodeTypes();
    // A new graph: a Material Output node alone.
    NodeGraph CreateEmptyMaterialGraph();
    // The Slang source and parameter layout of a graph. Deterministic: the editor, a development run and a packaged game get the same
    // layout from the same graph (the packed material's parameter slots must match the shipped shader's).
    MaterialGraphProgram CompileMaterialGraph(const NodeGraph& graph);
}
