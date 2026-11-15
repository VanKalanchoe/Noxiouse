#include "NoxCore/Renderer/CustomLitShaders.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <SDL3/SDL.h>

#include "NoxCore/Core/Log.h"

namespace Nox::CustomLitShaders
{
    namespace
    {
        constexpr const char* kGraphManifestMarker = "#MaterialGraphs";

        constexpr std::string_view kTemplate = R"(#include "CustomLit.slang" // (for your code editor: the engine's inputs and helpers; the engine has them already)

// A Material Shader (Custom Lit): how a material whose Shading Model is "Custom Lit" is lit.
//   light()   runs once for every light that reaches the surface: that light's share of the pixel's light.
//   ambient() runs once: the light from all around (sky, GI, reflections).
// The pixel is the sum of light() over the lights + ambient() + the material's emissive (then the fog in front of it).
// Inputs: CustomLitSurface s (position, normal, view, baseColor, metallic, roughness, occlusion, uv, screenUV, time,
// parameters[0..3], textures), CustomLitLight l (direction, color, shadow, distance, type), CustomLitAmbient a (diffuse,
// reflection). Helpers: custom_lit_texture(s, 0 or 1, uv), custom_lit_luminance, custom_lit_fresnel, custom_lit_lambert.
// (NoxEditor/assets/shaders/CustomLit.slang describes every field.) No other #include: the engine puts this file in a namespace.
//
// This one: a two-tone toon. Parameter 1 (rgb) tints the shadow side, Parameter 2 x is the rim's strength.

float3 light(CustomLitSurface s, CustomLitLight l)
{
    const float facing = dot(s.normal, l.direction);
    const float lit = smoothstep(0.0, 0.04, facing) * step(0.5, l.shadow); // lit or not: no gradient
    const float3 shadowTint = s.parameters[0].rgb;
    const float3 tone = lerp(shadowTint * 0.25, float3(1.0), lit);
    const float rim = smoothstep(0.65, 0.7, 1.0 - saturate(dot(s.normal, s.view))) * lit * s.parameters[1].x;
    return (s.baseColor * tone + rim) / CustomLitPi * l.color;
}

float3 ambient(CustomLitSurface s, CustomLitAmbient a)
{
    return s.baseColor * a.diffuse * s.occlusion;
}
)";

        // Writes `text` to `path` unless it already holds exactly that. Returns whether it wrote.
        bool WriteIfChanged(const std::filesystem::path& path, const std::string& text, bool& failed)
        {
            {
                std::ifstream existing(path, std::ios::binary);
                if (existing)
                {
                    std::stringstream current;
                    current << existing.rdbuf();
                    if (current.str() == text)
                        return false;
                }
            }
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size())))
            {
                NOX_CORE_ERROR("Custom Lit: could not write {}", path.generic_string());
                failed = true;
                return false;
            }
            return true;
        }

        // A Material Shader's code as it goes into the generated file: its `#include "CustomLit.slang"` / `"shaderIO.h"` lines (there for a
        // code editor, which compiles the file on its own) blanked -- the generated file has them already, outside the namespace -- so
        // the line numbers stay the file's.
        std::string WithoutEngineIncludes(const std::string& code)
        {
            std::string result;
            result.reserve(code.size());
            size_t start = 0;
            while (start <= code.size())
            {
                size_t end = code.find('\n', start);
                const bool last = end == std::string::npos;
                if (last)
                    end = code.size();
                std::string_view line(code.data() + start, end - start);
                const size_t first = line.find_first_not_of(" \t");
                const bool engineInclude = first != std::string_view::npos && line.substr(first).starts_with("#include") &&
                                           (line.find("CustomLit.slang") != std::string_view::npos || line.find("shaderIO.h") != std::string_view::npos);
                if (!engineInclude)
                    result.append(line);
                if (last)
                    break;
                result.push_back('\n');
                start = end + 1;
            }
            return result;
        }

        // A Material Shader's path inside a #line directive (quotes and backslashes would end or escape the string).
        std::string LinePath(const std::string& path)
        {
            std::string safe = path;
            std::replace(safe.begin(), safe.end(), '"', '\'');
            std::replace(safe.begin(), safe.end(), '\\', '/');
            return safe;
        }
    }

    std::string_view TemplateSource()
    {
        return kTemplate;
    }

    std::filesystem::path GeneratedFolder()
    {
        const char* base = SDL_GetBasePath();
        return std::filesystem::path(base ? base : "") / "shaders" / "Generated";
    }

    std::filesystem::path DeferredSourcePath()
    {
        return GeneratedFolder() / "DeferredLightingCustomLit.slang";
    }

    std::filesystem::path ForwardSourcePath()
    {
        return GeneratedFolder() / "TransparentLitCustomLit.slang";
    }

    std::filesystem::path CheckSourcePath()
    {
        return GeneratedFolder() / "CustomLitCheck.slang";
    }

    std::filesystem::path GBufferSourcePath()
    {
        return GeneratedFolder() / "GBufferMaterialGraph.slang";
    }

    std::filesystem::path VisibilitySourcePath()
    {
        return GeneratedFolder() / "VisibilityBufferMaterialGraph.slang";
    }

    std::filesystem::path StableDepthSourcePath()
    {
        return GeneratedFolder() / "EditorStableDepthMaterialGraph.slang";
    }

    std::filesystem::path SelectionMaskSourcePath()
    {
        return GeneratedFolder() / "SelectionMaskMaterialGraph.slang";
    }

    std::filesystem::path RayTracingOffsetSourcePath()
    {
        return GeneratedFolder() / "RayTracingOffsetMaterialGraph.slang";
    }

    std::filesystem::path ForwardMeshSourcePath()
    {
        return GeneratedFolder() / "ForwardMeshMaterialGraph.slang";
    }

    std::filesystem::path GraphMasksSourcePath()
    {
        return GeneratedFolder() / "MaterialGraphMasks.slang";
    }

    std::filesystem::path GraphSurfacesSourcePath()
    {
        return GeneratedFolder() / "MaterialGraphSurfaces.slang";
    }

    const std::vector<RayTracingMaskVariant>& RayTracingMaskVariants()
    {
        // Every pass that alpha tests ray hits (RayHitAlpha / RayHitCoverage, the path tracer's Bridge::AlphaTest and occluder
        // transmittance): ray-traced shadows (sun, local lights), AO, reflections, screen-probe GI, DDGI, ReSTIR DI / GI / PT and the
        // path tracer's hit shaders. Those that shade their hits (the hit's colour, roughness, emission: reflections, screen-probe GI, DDGI,
        // ReSTIR GI / PT, the path tracer's closest hit) read the graphs' surfaces too. (The forward shader's own rays: its graph
        // variant, TransparentLitCustomLit.slang, which has the whole graphs.)
        static const std::vector<RayTracingMaskVariant> variants = []
        {
            const std::pair<const char*, bool> sources[] = {
                { "ShadowMask.slang", false }, { "DeferredLighting.slang", false }, { "RTAO.slang", false }, { "Reflection.slang", true },
                { "ScreenProbeTrace.slang", true }, { "DDGIRadiance.slang", true }, { "RTXDI/DI/ReSTIRDIFinalShading.slang", false },
                { "RTXDI/GI/ReSTIRGIInitial.slang", true }, { "RTXDI/PT/ReSTIRPTInitial.slang", true }, { "RTXDI/PT/ReSTIRPTTemporal.slang", true },
                { "PathTracer/PathTracerAnyHit.slang", false }, { "PathTracer/PathTracerHit.slang", true },
            };
            std::vector<RayTracingMaskVariant> list;
            for (const auto& [source, surface] : sources)
            {
                const std::filesystem::path relative(source);
                list.push_back({ std::string("assets/shaders/") + source,
                                 (GeneratedFolder() / (relative.stem().string() + "GraphMask.slang")).generic_string(), surface });
            }
            return list;
        }();
        return variants;
    }

    std::string GenerateDispatcher(const std::vector<std::string>& paths, const std::vector<CustomLitSource>& sources)
    {
        std::string text;
        text += "// Generated by the Nox renderer from the project's Material Shaders (Renderer/CustomLitShaders.cpp): written again whenever one\n"
                "// changes -- edit the Material Shaders, not this file. Each is in a namespace of its own; the functions at the end call the one\n"
                "// a pixel's material names by its slot (GpuMaterial::customLitSlot).\n"
                "#pragma once\n"
                "#include \"CustomLit.slang\"\n\n";
        // A material graph's lighting reads the graph's inputs, rebuilt from the Custom Lit surface (MaterialGraph.slang without its raster
        // helpers when nothing included it whole before: the deferred pass has the masks' copy, the forward shader the whole graphs).
        const bool graphLighting = std::any_of(sources.begin(), sources.end(), [&](const CustomLitSource& s)
        {
            return s.Graph && std::find(paths.begin(), paths.end(), s.Path) != paths.end();
        });
        if (graphLighting)
            text += "#ifndef NOX_MATERIAL_GRAPH_MASK_ONLY\n#define NOX_MATERIAL_GRAPH_MASK_ONLY 1\n#endif\n"
                    "#include \"MaterialGraph.slang\"\n\n"
                    "// A material graph's inputs where its Custom Light / Custom Ambient run: from the surface the lighting sees (no second UV set\n"
                    "// or vertex colour there; texture mips from the surface's UV footprint).\n"
                    "MaterialGraphInputs material_graph_lit_inputs(CustomLitSurface s)\n{\n"
                    "    MaterialGraphInputs i = (MaterialGraphInputs)0;\n"
                    "    i.uv0 = s.uv;\n    i.uv0Dx = float2(s.uvFootprint, 0.0);\n    i.uv0Dy = float2(0.0, s.uvFootprint);\n"
                    "    i.uv1 = s.uv;\n    i.uv1Dx = i.uv0Dx;\n    i.uv1Dy = i.uv0Dy;\n"
                    "    i.position = s.position + s.ubo->renderOrigin;\n"
                    "    i.normal = s.normal;\n    i.vertexColor = float4(1.0);\n    i.view = s.view;\n    i.screenUV = s.screenUV;\n"
                    "    i.time = s.time;\n    i.cameraPosition = s.ubo->cameraWorldPos.xyz + s.ubo->renderOrigin;\n"
                    "    i.material = s.material;\n    i.mipFeedback = false;\n    i.ubo = s.ubo;\n    return i;\n}\n\n";
        std::vector<bool> present(paths.size(), false);
        for (size_t slot = 0; slot < paths.size(); ++slot)
        {
            if (paths[slot].empty())
                continue;
            const auto source = std::find_if(sources.begin(), sources.end(), [&](const CustomLitSource& s) { return s.Path == paths[slot]; });
            if (source == sources.end())
                continue;
            present[slot] = true;
            text += "namespace CustomLit" + std::to_string(slot) + "\n{\n";
            text += "#line 1 \"" + LinePath(source->File.empty() ? source->Path : source->File) + "\"\n";
            const std::string code = WithoutEngineIncludes(source->Code);
            text += code;
            if (!code.empty() && code.back() != '\n')
                text += '\n';
            text += "}\n\n";
        }
        text += "#line 1 \"CustomLitShaders.slang (generated)\"\n";
        // A slot without a case (a pipeline built before that Material Shader existed, while the new build compiles) lights as
        // Default Lit's diffuse would.
        auto dispatcher = [&](const char* name, const char* function, const char* inputType, const char* input, const char* fallback)
        {
            text += std::string("float3 ") + name + "(uint slot, CustomLitSurface s, " + inputType + " " + input + ")\n{\n    switch (slot)\n    {\n";
            for (size_t slot = 0; slot < paths.size(); ++slot)
            {
                if (present[slot])
                    text += "    case " + std::to_string(slot) + "u: return CustomLit" + std::to_string(slot) + "::" + function + "(s, " + input + ");\n";
            }
            text += std::string("    default: return ") + fallback + ";\n    }\n}\n\n";
        };
        dispatcher("custom_lit_light", "light", "CustomLitLight", "l", "custom_lit_lambert(s, l)");
        dispatcher("custom_lit_ambient", "ambient", "CustomLitAmbient", "a", "s.baseColor * a.diffuse * s.occlusion");
        return text;
    }

    std::string GenerateGraphDispatcher(const std::vector<std::string>& graphPaths, const std::vector<MaterialGraphSource>& graphs)
    {
        // A file of its own (not the Material Shaders'): a graph's edit compiles again what uses graphs -- the G-buffer resolve's variant and
        // the forward variant -- and leaves the Custom Lit pass's binary as it is (the pipelines are cached by their sources' hash).
        std::string text;
        text += "// Generated by the Nox renderer from the project's material graphs (Renderer/CustomLitShaders.cpp, MaterialGraph.h): written again\n"
                "// whenever one is saved -- edit the graphs, not this file. Each is in a namespace of its own; material_graph_surface calls the\n"
                "// one a pixel's material names by its slot (GpuMaterial::graphSlot).\n"
                "#pragma once\n"
                "#include \"MaterialGraph.slang\"\n\n";
        std::vector<bool> graphPresent(graphPaths.size(), false);
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (graphPaths[slot].empty())
                continue;
            const auto graph = std::find_if(graphs.begin(), graphs.end(), [&](const MaterialGraphSource& g) { return g.Path == graphPaths[slot]; });
            if (graph == graphs.end() || graph->Code.empty())
                continue;
            graphPresent[slot] = true;
            text += "namespace MaterialGraph" + std::to_string(slot) + "\n{\n";
            text += "#line 1 \"" + LinePath(graph->Path) + " (generated)\"\n";
            text += graph->Code;
            if (graph->Code.back() != '\n')
                text += '\n';
            text += "}\n\n";
        }
        text += "#line 1 \"MaterialGraphs.slang (generated)\"\n";
        // A slot without a case (a pipeline built before that graph existed, or a graph that does not compile): the material's own values.
        text += "void material_graph_surface(uint slot, MaterialGraphInputs i, inout MaterialGraphOutputs o)\n{\n    switch (slot)\n    {\n";
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (graphPresent[slot])
                text += "    case " + std::to_string(slot) + "u: MaterialGraph" + std::to_string(slot) + "::surface(i, o); break;\n";
        }
        text += "    default: break;\n    }\n}\n\n";
        // Opacity Mask (a Masked material's alpha test: the visibility buffer, the shadow maps, the forward shader). An unknown slot: 1.
        text += "float material_graph_opacity_mask(uint slot, MaterialGraphInputs i)\n{\n    switch (slot)\n    {\n";
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (graphPresent[slot])
                text += "    case " + std::to_string(slot) + "u: return MaterialGraph" + std::to_string(slot) + "::opacity_mask(i);\n";
        }
        text += "    default: return 1.0;\n    }\n}\n\n";
        // World Position Offset (per vertex: every pass that draws the geometry). An unknown slot: no offset.
        text += "float3 material_graph_world_position_offset(uint slot, MaterialGraphInputs i)\n{\n    switch (slot)\n    {\n";
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (graphPresent[slot])
                text += "    case " + std::to_string(slot) + "u: return MaterialGraph" + std::to_string(slot) + "::world_position_offset(i);\n";
        }
        text += "    default: return float3(0.0);\n    }\n}\n";
        return text;
    }

    std::string GenerateGraphMaskDispatcher(const std::vector<std::string>& graphPaths, const std::vector<MaterialGraphSource>& graphs)
    {
        // Only the masks: a graph's surface or World Position Offset edit leaves this file -- and every ray tracing variant's binary -- as
        // it is. MaterialGraph.slang without its raster helpers (NOX_MATERIAL_GRAPH_MASK_ONLY: they call the full dispatchers).
        std::string text;
        text += "// Generated by the Nox renderer from the project's material graphs (Renderer/CustomLitShaders.cpp, MaterialGraph.h): their Opacity\n"
                "// Mask functions alone, for the ray tracing passes' alpha tests (material_graph_ray_mask). Edit the graphs, not this file.\n"
                "#pragma once\n"
                "#ifndef NOX_MATERIAL_GRAPH_MASK_ONLY\n#define NOX_MATERIAL_GRAPH_MASK_ONLY 1\n#endif\n"
                "#include \"MaterialGraph.slang\"\n\n";
        std::vector<bool> present(graphPaths.size(), false);
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (graphPaths[slot].empty())
                continue;
            const auto graph = std::find_if(graphs.begin(), graphs.end(), [&](const MaterialGraphSource& g) { return g.Path == graphPaths[slot]; });
            if (graph == graphs.end() || graph->MaskCode.empty())
                continue;
            present[slot] = true;
            text += "namespace MaterialGraph" + std::to_string(slot) + "\n{\n";
            text += "#line 1 \"" + LinePath(graph->Path) + " (generated)\"\n";
            text += graph->MaskCode;
            if (graph->MaskCode.back() != '\n')
                text += '\n';
            text += "}\n\n";
        }
        text += "#line 1 \"MaterialGraphMasks.slang (generated)\"\n";
        text += "float material_graph_opacity_mask(uint slot, MaterialGraphInputs i)\n{\n    switch (slot)\n    {\n";
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (present[slot])
                text += "    case " + std::to_string(slot) + "u: return MaterialGraph" + std::to_string(slot) + "::opacity_mask(i);\n";
        }
        text += "    default: return 1.0;\n    }\n}\n";
        return text;
    }

    std::string GenerateGraphSurfaceDispatcher(const std::vector<std::string>& graphPaths, const std::vector<MaterialGraphSource>& graphs)
    {
        // The surfaces alone (no World Position Offset, no mask -- MaterialGraphMasks.slang has those): an offset edit leaves the ray
        // tracing variants that include this as they are. Namespaces of their own (MaterialGraphSurface<slot>: the masks' file has
        // MaterialGraph<slot>).
        std::string text;
        text += "// Generated by the Nox renderer from the project's material graphs (Renderer/CustomLitShaders.cpp, MaterialGraph.h): their surface\n"
                "// functions alone, for the ray tracing passes that shade their hits (material_graph_ray_surface). Edit the graphs, not this file.\n"
                "#pragma once\n"
                "#ifndef NOX_MATERIAL_GRAPH_MASK_ONLY\n#define NOX_MATERIAL_GRAPH_MASK_ONLY 1\n#endif\n"
                "#include \"MaterialGraph.slang\"\n\n";
        std::vector<bool> present(graphPaths.size(), false);
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (graphPaths[slot].empty())
                continue;
            const auto graph = std::find_if(graphs.begin(), graphs.end(), [&](const MaterialGraphSource& g) { return g.Path == graphPaths[slot]; });
            if (graph == graphs.end() || graph->SurfaceCode.empty())
                continue;
            present[slot] = true;
            text += "namespace MaterialGraphSurface" + std::to_string(slot) + "\n{\n";
            text += "#line 1 \"" + LinePath(graph->Path) + " (generated)\"\n";
            text += graph->SurfaceCode;
            if (graph->SurfaceCode.back() != '\n')
                text += '\n';
            text += "}\n\n";
        }
        text += "#line 1 \"MaterialGraphSurfaces.slang (generated)\"\n";
        text += "void material_graph_surface(uint slot, MaterialGraphInputs i, inout MaterialGraphOutputs o)\n{\n    switch (slot)\n    {\n";
        for (size_t slot = 0; slot < graphPaths.size(); ++slot)
        {
            if (present[slot])
                text += "    case " + std::to_string(slot) + "u: MaterialGraphSurface" + std::to_string(slot) + "::surface(i, o); return;\n";
        }
        text += "    default: return;\n    }\n}\n";
        return text;
    }

    bool WriteGenerated(const std::vector<std::string>& paths, const std::vector<CustomLitSource>& sources,
                        const std::vector<std::string>& graphPaths, const std::vector<MaterialGraphSource>& graphs)
    {
        const std::filesystem::path folder = GeneratedFolder();
        bool failed = false;
        bool changed = WriteIfChanged(folder / "CustomLitShaders.slang", GenerateDispatcher(paths, sources), failed);
        changed |= WriteIfChanged(folder / "MaterialGraphs.slang", GenerateGraphDispatcher(graphPaths, graphs), failed);
        changed |= WriteIfChanged(GraphMasksSourcePath(), GenerateGraphMaskDispatcher(graphPaths, graphs), failed);
        changed |= WriteIfChanged(GraphSurfacesSourcePath(), GenerateGraphSurfaceDispatcher(graphPaths, graphs), failed);
        // The ray tracing passes with the graphs' masks (built while a graph connects Opacity Mask: Renderer::rayTracingPassDesc).
        for (const RayTracingMaskVariant& variant : RayTracingMaskVariants())
        {
            const std::string engineRelative = variant.Source.substr(std::string("assets/shaders/").size());
            const std::string text = variant.Surface
                ? "// Generated by the Nox renderer (Renderer/CustomLitShaders.cpp): " + engineRelative +
                      " with the material graphs' Opacity Mask in its ray alpha tests and their surfaces at its hits.\n"
                      "#define NOX_MATERIAL_GRAPH_MASK 1\n#define NOX_MATERIAL_GRAPH_SURFACE 1\n#include \"MaterialGraphMasks.slang\"\n"
                      "#include \"MaterialGraphSurfaces.slang\"\n#include \"" + engineRelative + "\"\n"
                : "// Generated by the Nox renderer (Renderer/CustomLitShaders.cpp): " + engineRelative +
                      " with the material graphs' Opacity Mask in its ray alpha tests.\n"
                      "#define NOX_MATERIAL_GRAPH_MASK 1\n#include \"MaterialGraphMasks.slang\"\n#include \"" + engineRelative + "\"\n";
            changed |= WriteIfChanged(variant.Variant, text, failed);
        }
        const char* header = "// Generated by the Nox renderer (Renderer/CustomLitShaders.cpp): the %s compiled with the project's Material Shaders and material graphs.\n";
        const char* defines = "";
        auto wrapper = [&](const char* what, const char* shader)
        {
            char line[256];
            std::snprintf(line, sizeof(line), header, what);
            return std::string(line) + defines + shader + "\n";
        };
        // (its ray-traced shadows read the graphs' masks too: MaterialGraphMasks.slang, which changes only when a mask does)
        defines = "#define NOX_CUSTOM_LIT 1\n#define NOX_MATERIAL_GRAPH_MASK 1\n#include \"MaterialGraphMasks.slang\"\n#include \"CustomLitShaders.slang\"\n";
        changed |= WriteIfChanged(DeferredSourcePath(), wrapper("deferred lighting (the Custom Lit pass)", "#include \"DeferredLighting.slang\""), failed);
        // (its rays' hits -- refraction, reflections -- read the graphs' surfaces too: NOX_MATERIAL_GRAPH_SURFACE, from the whole graphs)
        defines = "#define NOX_CUSTOM_LIT 1\n#define NOX_MATERIAL_GRAPH 1\n#define NOX_MATERIAL_GRAPH_MASK 1\n#define NOX_MATERIAL_GRAPH_SURFACE 1\n"
                  "#include \"MaterialGraphs.slang\"\n#include \"CustomLitShaders.slang\"\n";
        changed |= WriteIfChanged(ForwardSourcePath(), wrapper("forward shader (its Custom Lit and material graph variant)", "#include \"Material_TransparentLit_Mesh.slang\""), failed);
        // The forward shader's mesh stage with the graphs: a graph's World Position Offset for translucent surfaces.
        defines = "#define NOX_MATERIAL_GRAPH 1\n#include \"MaterialGraphs.slang\"\n";
        changed |= WriteIfChanged(ForwardMeshSourcePath(), wrapper("forward shader's mesh stage (its material graph variant: World Position Offset)",
                                                                   "#include \"Material_PBR_Mesh.slang\""), failed);
        defines = "#define NOX_MATERIAL_GRAPH 1\n#include \"MaterialGraphs.slang\"\n";
        changed |= WriteIfChanged(GBufferSourcePath(), wrapper("G-buffer resolve (its material graph variant)", "#include \"GBufferMaterial.slang\""), failed);
        changed |= WriteIfChanged(VisibilitySourcePath(), wrapper("visibility buffer and shadow map depth (their material graph variant: Opacity Mask)",
                                                                  "#include \"VisibilityBuffer.slang\""), failed);
        // The editor's display-resolution entity IDs / depth and its selection outline: they cut a graph's Opacity Mask too.
        changed |= WriteIfChanged(StableDepthSourcePath(), wrapper("editor's stable entity IDs and depth (their material graph variant: Opacity Mask)",
                                                                   "#include \"EditorStableDepth.slang\""), failed);
        changed |= WriteIfChanged(SelectionMaskSourcePath(), wrapper("editor's selection mask (its material graph variant: Opacity Mask)",
                                                                     "#include \"SelectionMask.slang\""), failed);
        // World Position Offset in the ray tracing scene: the moved vertices its BLAS is refit from (the defines are still the graphs').
        changed |= WriteIfChanged(RayTracingOffsetSourcePath(), wrapper("ray tracing scene's World Position Offset (its moved vertices)",
                                                                        "#include \"RayTracingOffset.slang\""), failed);
        changed |= WriteIfChanged(CheckSourcePath(),
                                  "// Generated by the Nox renderer (Renderer/CustomLitShaders.cpp): the Material Shaders and material graphs alone, compiled first\n"
                                  "// to report their errors.\n"
                                  "#define NOX_MATERIAL_GRAPH 1\n"
                                  "#include \"CustomLitShaders.slang\"\n"
                                  "#include \"MaterialGraphs.slang\"\n"
                                  "RWStructuredBuffer<float4> customLitCheckOutput;\n"
                                  "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
                                  "void customLitCheckMain(uint3 id : SV_DispatchThreadID)\n{\n"
                                  "    CustomLitSurface s = (CustomLitSurface)0;\n"
                                  "    const CustomLitLight l = custom_lit_make_light(float3(0.0, 1.0, 0.0), float3(1.0), 1.0, 1.0, 0u);\n"
                                  "    const CustomLitAmbient a = custom_lit_make_ambient(float3(1.0), float3(1.0));\n"
                                  "    MaterialGraphInputs gi = (MaterialGraphInputs)0; // (compiled only, never run)\n"
                                  "    MaterialGraphOutputs go = (MaterialGraphOutputs)0;\n"
                                  "    material_graph_surface(id.x, gi, go);\n"
                                  "    go.opacity *= material_graph_opacity_mask(id.x, gi);\n"
                                  "    go.emissive += material_graph_world_position_offset(id.x, gi);\n"
                                  "    customLitCheckOutput[id.x] = float4(custom_lit_light(id.x, s, l) + custom_lit_ambient(id.x, s, a) + go.baseColor + go.emissive, go.opacity);\n}\n",
                                  failed);
        std::string manifest;
        for (const std::string& path : paths)
            manifest += path + "\n";
        // The material graphs after a marker line (a manifest written before material graphs has none: no graphs).
        manifest += std::string(kGraphManifestMarker) + "\n";
        for (const std::string& path : graphPaths)
            manifest += path + "\n";
        changed |= WriteIfChanged(folder / "CustomLitShaders.txt", manifest, failed);
        return changed && !failed;
    }

    bool WriteCodeEditorConfig(const std::filesystem::path& projectDirectory, const std::filesystem::path& engineShaderDirectory)
    {
        // Read by the Slang language server (the VS Code / Visual Studio Slang extensions) from a file's folder or any parent: its
        // search paths find CustomLit.slang and shaderIO.h when the editor checks a Material Shader on its own.
        std::string folder = std::filesystem::absolute(engineShaderDirectory).lexically_normal().generic_string();
        std::string escaped;
        for (const char c : folder)
        {
            if (c == '"' || c == '\\')
                escaped.push_back('\\');
            escaped.push_back(c);
        }
        const std::string text = "{\n    \"slang.additionalSearchPaths\": [ \"" + escaped + "\" ]\n}\n";
        bool failed = false;
        WriteIfChanged(projectDirectory / "slangdconfig.json", text, failed);
        return !failed;
    }

    namespace
    {
        // The manifest's lines before the material graph marker (`graphs` false) or after it.
        std::vector<std::string> ReadManifestSection(bool graphs)
        {
            std::vector<std::string> paths;
            std::ifstream file(GeneratedFolder() / "CustomLitShaders.txt");
            std::string line;
            bool inGraphs = false;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line == kGraphManifestMarker)
                {
                    inGraphs = true;
                    continue;
                }
                if (inGraphs == graphs)
                    paths.push_back(line);
            }
            return paths;
        }
    }

    std::vector<std::string> ReadManifest()
    {
        return ReadManifestSection(false);
    }

    std::vector<std::string> ReadGraphManifest()
    {
        return ReadManifestSection(true);
    }

    std::vector<std::string> AssignSlots(const std::vector<std::string>& previous, const std::vector<CustomLitSource>& sources)
    {
        std::vector<std::string> present;
        present.reserve(sources.size());
        for (const CustomLitSource& source : sources)
            present.push_back(source.Path);
        return AssignPathSlots(previous, present);
    }

    std::vector<std::string> AssignPathSlots(const std::vector<std::string>& previous, const std::vector<std::string>& present)
    {
        std::vector<std::string> paths;
        paths.reserve(std::max(previous.size(), present.size()));
        auto isPresent = [&](const std::string& path) { return std::find(present.begin(), present.end(), path) != present.end(); };
        for (const std::string& path : previous)
            paths.push_back(!path.empty() && isPresent(path) ? path : std::string());
        for (const std::string& path : present)
        {
            if (std::find(paths.begin(), paths.end(), path) == paths.end())
                paths.push_back(path);
        }
        // Trailing empty slots are dropped (nothing names them any more).
        while (!paths.empty() && paths.back().empty())
            paths.pop_back();
        return paths;
    }
}
