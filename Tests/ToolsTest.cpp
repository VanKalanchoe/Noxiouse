// NoxToolsTest: the engine console's registry (docs/MCP_Plan_2026-11.md §4), from MCP0 the MCP protocol layer, and the MagicaVoxel
// reader / mesher (VoxTests.inl, docs/Voxel_Plan_2026-11.md) -- without a window, GPU or editor. Exit code 0 when every check passed.
#include "NoxCore/Console/Console.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/CustomLitShaders.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/NodeGraph/NodeType.h"
#include <algorithm>
#include <fstream>

#include <cstdio>
#include <format>
#include <string>

namespace
{
    int s_Checks = 0, s_Failed = 0;

    template<typename... Args>
    void Check(bool ok, std::format_string<Args...> format, Args&&... args)
    {
        ++s_Checks;
        if (!ok)
            ++s_Failed;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", std::format(format, std::forward<Args>(args)...).c_str());
    }
}
#define CHECK(condition, ...) Check((condition), __VA_ARGS__)

#ifdef NOX_TOOLS_TEST_MCP
#include "McpTests.inl"
#endif
#include "VoxTests.inl"

using namespace Nox;

static void TestConsole()
{
    std::printf("\n== console\n");
    Console& console = Console::Get();

    // Words: spaces split, quotes keep, empty quotes are a word.
    const auto words = Console::Split(R"(say "hello world"  x "")");
    CHECK(words.size() == 4 && words[0] == "say" && words[1] == "hello world" && words[2] == "x" && words[3].empty(),
          "split: {} words ('{}', '{}', '{}', '{}')", words.size(), words.size() > 0 ? words[0] : "", words.size() > 1 ? words[1] : "",
          words.size() > 2 ? words[2] : "", words.size() > 3 ? words[3] : "");

    // Variables: typed, ranged, printed, refused when wrong.
    auto* quality = console.RegisterVariable<int32_t>("test.Quality", "Shadow quality for the test", 2, 0, 3);
    int changes = 0;
    quality->OnChanged([&](ConsoleVariable&) { ++changes; });
    ConsoleOutput out = console.Execute("test.quality 3");
    CHECK(quality->Get() == 3 && changes == 1 && !out.HasErrors(), "an int variable is set (case ignored), its callback runs once: {} ({})", quality->Get(),
          out.Text());
    out = console.Execute("test.Quality 7");
    CHECK(quality->Get() == 3 && out.HasErrors() && changes == 1, "out of range refused: '{}'", out.Text());
    out = console.Execute("test.Quality abc");
    CHECK(quality->Get() == 3 && out.HasErrors(), "not a number refused: '{}'", out.Text());
    out = console.Execute("test.Quality");
    CHECK(out.Text().find("= 3") != std::string::npos && out.Text().find("0..3") != std::string::npos &&
          out.Text().find("default 2") != std::string::npos, "typed alone, it prints value, range, default and help: '{}'", out.Text());

    auto* enabled = console.RegisterVariable<bool>("test.Enabled", "A switch", false);
    console.Execute("test.Enabled on");
    const bool on = enabled->Get();
    console.Execute("test.Enabled 0");
    CHECK(on && !enabled->Get(), "a bool takes on / 0");
    auto* scale = console.RegisterVariable<float>("test.Scale", "A float", 1.0f, 0.0f, 4.0f);
    console.Execute("test.Scale 0.25");
    out = console.Execute("test.Scale 1e9");
    CHECK(scale->Get() == 0.25f && out.HasErrors(), "a float is set and range-checked: {}", scale->Get());
    auto* text = console.RegisterVariable<std::string>("test.Name", "A string", std::string("a"));
    console.Execute("test.Name two words");
    CHECK(text->Get() == "two words", "a string takes the rest of the line: '{}'", text->Get());

    // Commands: arguments, several per line, access levels, unknown names.
    std::vector<std::string> received;
    {
        Console::Registration registration = console.RegisterCommand({ "test.Echo", "test.Echo <words>", "Repeats its arguments", ConsoleAccess::Read,
            [&](const std::vector<std::string>& arguments, ConsoleOutput& output)
            {
                received = arguments;
                std::string joined;
                for (const std::string& argument : arguments)
                    joined += (joined.empty() ? "" : "|") + argument;
                output.Info(joined);
            } });
        out = console.Execute(R"(test.echo a "b c"; test.Quality 1)");
        CHECK(received.size() == 2 && received[1] == "b c" && quality->Get() == 1 && out.Text().find("a|b c") != std::string::npos,
              "a command gets its arguments; ';' runs the next one: '{}'", out.Text());

        Console::Registration quit = console.RegisterCommand({ "test.Quit", "", "Has consequences", ConsoleAccess::Execute,
            [&](const std::vector<std::string>&, ConsoleOutput& output) { output.Info("quit ran"); } });
        out = console.Execute("test.Quit", ConsoleAccess::Write);
        CHECK(out.HasErrors() && out.Text().find("execute") != std::string::npos, "an execute command is refused with write access: '{}'", out.Text());
        out = console.Execute("test.Quality 2", ConsoleAccess::Read);
        CHECK(out.HasErrors() && quality->Get() == 1, "setting a variable is refused with read access: '{}'", out.Text());
        out = console.Execute("test.Quality", ConsoleAccess::Read);
        CHECK(!out.HasErrors(), "printing a variable is allowed with read access");

        const auto completed = console.Complete("TEST.e");
        CHECK(completed.size() == 2 && completed[0] == "test.Echo" && completed[1] == "test.Enabled", "completion of 'TEST.e': {} names", completed.size());
        const auto found = console.List("repeats");
        CHECK(found.size() == 1 && found[0].Name == "test.Echo", "find by help text: {} found", found.size());
        out = console.Execute("help test.Echo");
        CHECK(out.Text().find("Repeats its arguments") != std::string::npos, "help describes a command: '{}'", out.Text());
    }
    out = console.Execute("test.Echo x");
    CHECK(out.HasErrors() && console.FindCommand("test.Echo") == nullptr, "a command goes away with its registration: '{}'", out.Text());
    out = console.Execute("tset.Quality");
    CHECK(out.HasErrors() && out.Text().find("Unknown") != std::string::npos, "an unknown name says so: '{}'", out.Text());

    // A registration does not remove a newer command registered under the same name.
    Console::Registration older = console.RegisterCommand({ "test.Same", "", "old", ConsoleAccess::Read, nullptr });
    Console::Registration newer = console.RegisterCommand({ "test.Same", "", "new", ConsoleAccess::Read, nullptr });
    older.Reset();
    CHECK(console.FindCommand("test.Same") && console.FindCommand("test.Same")->Help == "new", "an old registration leaves the newer command");
    newer.Reset();
    CHECK(console.FindCommand("test.Same") == nullptr, "the newer one removes it");

    // A command that removes itself while it runs.
    Console::Registration self;
    self = console.RegisterCommand({ "test.Once", "", "removes itself", ConsoleAccess::Read,
        [&](const std::vector<std::string>&, ConsoleOutput& output) { self.Reset(); output.Info("ran once"); } });
    out = console.Execute("test.Once");
    CHECK(out.Text() == "ran once" && console.FindCommand("test.Once") == nullptr, "a command may remove itself while running");

    // A variable only the person at the machine may set: refused even with execute access (a tool's highest).
    auto* guarded = console.RegisterVariable<int32_t>("test.Guarded", "Only the user", 0, 0, 2);
    guarded->SetRequiredAccess(ConsoleAccess::User);
    out = console.Execute("test.Guarded 2", ConsoleAccess::Execute);
    const bool refusedForTool = out.HasErrors() && guarded->Get() == 0;
    out = console.Execute("test.Guarded 2");
    CHECK(refusedForTool && !out.HasErrors() && guarded->Get() == 2, "a user-only variable: refused with execute access, set by the user");
    console.Unregister("test.Guarded");

    console.Unregister("test.Quality");
    CHECK(console.FindVariable("test.Quality") == nullptr, "a variable is unregistered");
}

// Custom Lit (docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 3): the generated file the renderer compiles the project's
// Material Shaders into, and the slots it gives them (CustomLitShaders.h). (Whether it compiles is the Slang check in the docs' test guide.)
static void TestCustomLitShaders()
{
    using namespace Nox;
    std::printf("\n== Custom Lit: generated Material Shader file and slots\n");
    const std::vector<CustomLitSource> sources = { { "Shaders/A.slang", "float3 light(CustomLitSurface s, CustomLitLight l) { return 1; }\n"
                                                                         "float3 ambient(CustomLitSurface s, CustomLitAmbient a) { return 0; }" },
                                                   { "Shaders/B.slang", "// b\n" } };
    const std::vector<std::string> paths = { "Shaders/A.slang", "", "Shaders/B.slang" };
    const std::string text = CustomLitShaders::GenerateDispatcher(paths, sources);
    CHECK(text.find("namespace CustomLit0") != std::string::npos && text.find("namespace CustomLit2") != std::string::npos &&
          text.find("namespace CustomLit1") == std::string::npos, "each Material Shader in the namespace of its slot, an empty slot left out");
    CHECK(text.find("#line 1 \"Shaders/A.slang\"") != std::string::npos, "#line names the Material Shader (compile errors point at its file)");
    CHECK(text.find("case 0u: return CustomLit0::light(s, l);") != std::string::npos &&
          text.find("case 2u: return CustomLit2::ambient(s, a);") != std::string::npos && text.find("case 1u") == std::string::npos,
          "the dispatchers call light() / ambient() by slot");
    CHECK(text.find("// b\n}") != std::string::npos, "a source without a final newline still closes its namespace on a line of its own");
    {
        const std::string named = CustomLitShaders::GenerateDispatcher({ "Shaders/A.slang" }, { { "Shaders/A.slang", "// a\n", "E:/Game/Assets/Shaders/A.slang" } });
        CHECK(named.find("#line 1 \"E:/Game/Assets/Shaders/A.slang\"") != std::string::npos,
              "with its file on disk known, #line names that full path (the compiler quotes the real line, the path opens)");
    }
    CHECK(text.find("default: return custom_lit_lambert(s, l);") != std::string::npos,
          "an unknown slot (a pipeline older than its Material Shader) lights as Default Lit's diffuse, not black");
    {
        // A Material Shader's own #include of the engine's file (there for a code editor): blanked in the generated file, the lines kept.
        const std::string withInclude = CustomLitShaders::GenerateDispatcher(
            { "S.slang" }, { { "S.slang", "#include \"CustomLit.slang\" // for the editor\n  #include \"shaderIO.h\"\nfloat3 light(CustomLitSurface s, CustomLitLight l) { return 0; }\n" } });
        const size_t body = withInclude.find("#line 1 \"S.slang\"\n");
        CHECK(body != std::string::npos && withInclude.find("#include \"CustomLit.slang\" // for") == std::string::npos &&
              withInclude.find("#include \"shaderIO.h\"\nfloat3") == std::string::npos &&
              withInclude.compare(body + std::string("#line 1 \"S.slang\"\n").size(), 3, "\n\nf") == 0,
              "a Material Shader's #include of CustomLit.slang / shaderIO.h is blanked, its line numbers kept");
    }

    const std::vector<std::string> first = CustomLitShaders::AssignSlots({}, { { "b.slang", "" }, { "a.slang", "" } });
    CHECK(first.size() == 2 && first[0] == "b.slang" && first[1] == "a.slang", "first slots: in the order given (GatherProjectSources sorts)");
    const std::vector<std::string> later = CustomLitShaders::AssignSlots(first, { { "a.slang", "" }, { "c.slang", "" } });
    CHECK(later.size() == 3 && later[0].empty() && later[1] == "a.slang" && later[2] == "c.slang",
          "later: a kept shader keeps its slot, a removed one leaves it empty, a new one is appended");
    const std::vector<std::string> trimmed = CustomLitShaders::AssignSlots(later, { { "a.slang", "" } });
    CHECK(trimmed.size() == 2 && trimmed[1] == "a.slang", "empty slots at the end are dropped");

    // Material graphs (MaterialGraph.h) in a file of their own: each in the namespace of its slot, a graph without code (it does not
    // compile, or connects nothing) left without a case; their slots kept as the Material Shaders' are.
    const std::string withGraphs = CustomLitShaders::GenerateGraphDispatcher({ "G/A.nmatgraph", "G/Broken.nmatgraph", "G/C.nmatgraph" },
        { { "G/A.nmatgraph", "static const uint Outputs = 1u;\nvoid surface(MaterialGraphInputs i, inout MaterialGraphOutputs o) { o.baseColor = 1; }" },
          { "G/Broken.nmatgraph", "" }, { "G/C.nmatgraph", "void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o) {}\n" } });
    const size_t graphSection = withGraphs.find("#include \"MaterialGraph.slang\"");
    CHECK(graphSection != std::string::npos && withGraphs.find("namespace MaterialGraph0") > graphSection &&
          withGraphs.find("namespace MaterialGraph2") != std::string::npos && withGraphs.find("namespace MaterialGraph1") == std::string::npos &&
          withGraphs.find("namespace CustomLit") == std::string::npos && withGraphs.find("custom_lit_light") == std::string::npos,
          "material graphs: a file of their own (no Material Shader in it), each in its slot's namespace (one without code left out)");
    CHECK(withGraphs.find("case 0u: MaterialGraph0::surface(i, o); break;") != std::string::npos &&
          withGraphs.find("case 2u: MaterialGraph2::surface(i, o); break;") != std::string::npos && withGraphs.find("case 1u: MaterialGraph") == std::string::npos &&
          withGraphs.find("default: break;") != std::string::npos && withGraphs.find("case 2u: return MaterialGraph2::opacity_mask(i);") != std::string::npos &&
          withGraphs.find("default: return 1.0;") != std::string::npos,
          "material_graph_surface calls the graph by slot (an unknown one: the material's own values)");
    CHECK(withGraphs.find("o.baseColor = 1; }\n}") != std::string::npos && withGraphs.find("#line 1 \"G/A.nmatgraph (generated)\"") != std::string::npos,
          "a graph's code without a final newline still closes its namespace; #line names the graph");
    CHECK(text.find("material_graph") == std::string::npos &&
          CustomLitShaders::GenerateGraphDispatcher({}, {}).find("void material_graph_surface(uint slot") != std::string::npos,
          "the Material Shaders' file has no graph; without graphs the graph dispatcher is there, empty (the variants always call it)");
    const std::vector<std::string> graphSlots = CustomLitShaders::AssignPathSlots({ "G/A.nmatgraph", "G/B.nmatgraph" }, { "G/B.nmatgraph", "G/D.nmatgraph" });
    CHECK(graphSlots.size() == 3 && graphSlots[0].empty() && graphSlots[1] == "G/B.nmatgraph" && graphSlots[2] == "G/D.nmatgraph",
          "graph slots: a kept graph keeps its slot, a removed one leaves it empty, a new one is appended");
    CHECK(CustomLitShaders::TemplateSource().starts_with("#include \"CustomLit.slang\"") &&
          CustomLitShaders::TemplateSource().find("float3 light(CustomLitSurface s, CustomLitLight l)") != std::string_view::npos &&
          CustomLitShaders::TemplateSource().find("float3 ambient(CustomLitSurface s, CustomLitAmbient a)") != std::string_view::npos,
          "the new Material Shader template has both functions");
}

// The material graph (step 4): example graphs generated to Slang -- the parameter layout, the outputs connected, the type rules and the
// derivatives a Texture Sample gets. (NOX_MATERIAL_GRAPH_DUMP=<folder>: the generated code is written there, to compile it with Slang.)
static void TestMaterialGraph()
{
    using namespace Nox;
    std::printf("\n== Material graph: nodes to Slang\n");
    RegisterMaterialGraphNodeTypes();
    auto node = [](NodeGraph& graph, const char* type, std::vector<std::pair<std::string, NodeGraphValue>> properties = {})
    {
        GraphNode n;
        n.Id = graph.NextNodeId++;
        n.TypeName = type;
        for (auto& [name, value] : properties)
            n.Properties[name] = value;
        graph.Nodes.push_back(n);
        return n.Id;
    };
    auto link = [](NodeGraph& graph, uint32_t from, uint32_t fromPin, uint32_t to, uint32_t toPin) { graph.Links.push_back({ from, fromPin, to, toPin }); };

    // A dissolve-style graph: a tinted, panned texture times a colour parameter into Base Color, a scalar parameter into Roughness,
    // Fresnel into Emissive.
    NodeGraph graph = CreateEmptyMaterialGraph();
    const uint32_t output = graph.OutputNode;
    const uint32_t uv = node(graph, "Texture Coordinate", { { "Index", int32_t(0) }, { "Tiling", glm::vec2(4.0f, 4.0f) } });
    const uint32_t panner = node(graph, "Panner", { { "Speed", glm::vec2(0.1f, 0.0f) } });
    const uint32_t texture = node(graph, "Texture Sample", { { "Texture", uint64_t(1234) }, { "Parameter Name", std::string("Albedo") }, { "Normal Map", false } });
    const uint32_t tint = node(graph, "Vector Parameter", { { "Name", std::string("Tint") }, { "Default", glm::vec4(1.0f, 0.5f, 0.25f, 1.0f) } });
    const uint32_t multiply = node(graph, "Multiply");
    const uint32_t rough = node(graph, "Scalar Parameter", { { "Name", std::string("Roughness") }, { "Default", 0.6f } });
    const uint32_t fresnel = node(graph, "Fresnel", { { "Exponent", 3.0f }, { "Base Reflect Fraction", 0.04f } });
    link(graph, uv, 0, panner, 0);
    link(graph, panner, 0, texture, 0);
    link(graph, texture, 0, multiply, 0); // RGB
    link(graph, tint, 0, multiply, 1);    // RGB
    link(graph, multiply, 0, output, 0);  // Base Color
    link(graph, rough, 0, output, 2);     // Roughness
    link(graph, fresnel, 0, output, 4);   // Emissive (a float spread to rgb)
    const MaterialGraphProgram program = CompileMaterialGraph(graph);
    CHECK(program.Valid, "a graph with a panned texture, a colour and a scalar parameter and Fresnel compiles: '{}'", program.Error);
    CHECK(program.Outputs == (MaterialGraphOutputBaseColor | MaterialGraphOutputRoughness | MaterialGraphOutputEmissive),
          "only the connected Material Output pins are written (Base Color, Roughness, Emissive): {}", program.Outputs);
    CHECK(program.Parameters.size() == 3 && program.ParameterSlots == 2 && program.TextureSlots == 1, "two number slots and one texture slot: {} / {} / {}",
          program.Parameters.size(), program.ParameterSlots, program.TextureSlots);
    const auto albedo = std::find_if(program.Parameters.begin(), program.Parameters.end(), [](const MaterialGraphParameter& p) { return p.Name == "Albedo"; });
    CHECK(albedo != program.Parameters.end() && albedo->Type == MaterialGraphParameter::Kind::Texture && albedo->DefaultTexture == 1234,
          "the Texture Sample with a Parameter name is an overridable texture with its default");
    CHECK(program.Code.find("i.uv0Dx * float2(4.0, 4.0)") != std::string::npos && program.Code.find("material_graph_texture(i, 0,") != std::string::npos,
          "the texture's UV derivatives follow the tiling (its mip from the derivatives)");
    CHECK(program.Code.find("o.metallic") == std::string::npos && program.Code.find("o.baseColor = max(") != std::string::npos,
          "an unconnected pin keeps the material's value");

    // Type rules: a float2 and a float3 do not combine; an Append makes a float3 of a float2 and a float.
    NodeGraph bad = CreateEmptyMaterialGraph();
    const uint32_t badAdd = node(bad, "Add");
    link(bad, node(bad, "Texture Coordinate"), 0, badAdd, 0);
    link(bad, node(bad, "World Position"), 0, badAdd, 1);
    link(bad, badAdd, 0, bad.OutputNode, 0);
    const MaterialGraphProgram badProgram = CompileMaterialGraph(bad);
    CHECK(!badProgram.Valid && badProgram.Error.find("float2 and a float3") != std::string::npos, "a float2 + float3 is refused with which node: '{}'", badProgram.Error);
    NodeGraph append = CreateEmptyMaterialGraph();
    const uint32_t appendNode = node(append, "Append");
    link(append, node(append, "Texture Coordinate"), 0, appendNode, 0);
    link(append, node(append, "Time"), 0, appendNode, 1);
    link(append, appendNode, 0, append.OutputNode, 4);
    const MaterialGraphProgram appendProgram = CompileMaterialGraph(append);
    CHECK(appendProgram.Valid && appendProgram.Code.find("float3(") != std::string::npos, "Append: a float2 and a float make a float3: '{}'", appendProgram.Error);

    // The same parameter name twice: one slot; a name as both texture and number: refused.
    NodeGraph shared = CreateEmptyMaterialGraph();
    const uint32_t sharedAdd = node(shared, "Add");
    link(shared, node(shared, "Scalar Parameter", { { "Name", std::string("K") }, { "Default", 1.0f } }), 0, sharedAdd, 0);
    link(shared, node(shared, "Scalar Parameter", { { "Name", std::string("K") }, { "Default", 2.0f } }), 0, sharedAdd, 1);
    link(shared, sharedAdd, 0, shared.OutputNode, 1);
    const MaterialGraphProgram sharedProgram = CompileMaterialGraph(shared);
    CHECK(sharedProgram.Valid && sharedProgram.ParameterSlots == 1 && sharedProgram.Parameters[0].Default.x == 1.0f,
          "two Scalar Parameters named K share one slot (the first's default)");
    // Opacity Mask (stylized step 4b): its own function from only the nodes feeding that pin; its parameters take the first slots (the
    // mask is generated first); a graph connecting only the mask still counts as connecting something (it is built).
    NodeGraph masked = CreateEmptyMaterialGraph();
    const uint32_t maskTexture = node(masked, "Texture Sample", { { "Parameter Name", std::string("Leaves") } });
    const uint32_t maskMultiply = node(masked, "Multiply");
    link(masked, maskTexture, 4, maskMultiply, 0); // A
    link(masked, node(masked, "Scalar Parameter", { { "Name", std::string("Density") }, { "Default", 1.5f } }), 0, maskMultiply, 1);
    link(masked, maskMultiply, 0, masked.OutputNode, 7);
    link(masked, node(masked, "Color", { { "Value", glm::vec4(0.2f, 0.6f, 0.1f, 1.0f) } }), 0, masked.OutputNode, 0);
    const MaterialGraphProgram maskProgram = CompileMaterialGraph(masked);
    const size_t maskFunction = maskProgram.Code.find("float opacity_mask(MaterialGraphInputs i)");
    CHECK(maskProgram.Valid && maskProgram.Outputs == (MaterialGraphOutputBaseColor | MaterialGraphOutputOpacityMask) && maskFunction != std::string::npos &&
          maskProgram.Code.find("return saturate(", maskFunction) != std::string::npos,
          "Opacity Mask: its own function returning the mask, the output bit set: '{}'", maskProgram.Error);
    CHECK(maskProgram.Code.substr(0, maskFunction).find("material_graph_texture") == std::string::npos &&
          maskProgram.Code.find("material_graph_texture", maskFunction) != std::string::npos,
          "the mask's nodes are only in opacity_mask() (the surface computes none of them)");
    const auto density = std::find_if(maskProgram.Parameters.begin(), maskProgram.Parameters.end(), [](const MaterialGraphParameter& p) { return p.Name == "Density"; });
    CHECK(density != maskProgram.Parameters.end() && density->Default.x == 1.5f && maskProgram.TextureSlots == 1,
          "a parameter only the mask reads has a slot (and the texture its slot)");
    // A surface edit leaves the mask's code alone (every ray tracing pass's variant compiles MaskCode): the same graph with its Base
    // Color from a Vector Parameter and a texture -- two more parameters, one more texture -- generates the same MaskCode.
    NodeGraph maskedTinted = masked;
    maskedTinted.Links.erase(std::remove_if(maskedTinted.Links.begin(), maskedTinted.Links.end(), [&](const GraphLink& l)
    {
        return l.ToNode == maskedTinted.OutputNode && l.ToPin == 0;
    }), maskedTinted.Links.end());
    const uint32_t tintedColor = node(maskedTinted, "Multiply");
    link(maskedTinted, node(maskedTinted, "Vector Parameter", { { "Name", std::string("Albedo") }, { "Default", glm::vec4(1.0f) } }), 0, tintedColor, 0);
    link(maskedTinted, node(maskedTinted, "Texture Sample", { { "Parameter Name", std::string("Bark") } }), 0, tintedColor, 1);
    link(maskedTinted, tintedColor, 0, maskedTinted.OutputNode, 0);
    const MaterialGraphProgram maskTintedProgram = CompileMaterialGraph(maskedTinted);
    CHECK(maskTintedProgram.Valid && maskTintedProgram.ParameterSlots == 2 && maskTintedProgram.TextureSlots == 2 &&
          maskTintedProgram.MaskCode == maskProgram.MaskCode,
          "parameters and textures added to the surface leave MaskCode as it was (the mask's slots first): '{}'", maskTintedProgram.Error);
    CHECK(std::is_sorted(maskTintedProgram.Parameters.begin(), maskTintedProgram.Parameters.end(),
                         [](const MaterialGraphParameter& a, const MaterialGraphParameter& b) { return a.Name < b.Name; }),
          "the parameters are listed by name (Albedo, Bark, Density, Leaves)");
    NodeGraph maskOnly = CreateEmptyMaterialGraph();
    link(maskOnly, node(maskOnly, "Constant", { { "Value", 0.25f } }), 0, maskOnly.OutputNode, 7);
    const MaterialGraphProgram maskOnlyProgram = CompileMaterialGraph(maskOnly);
    CHECK(maskOnlyProgram.Valid && maskOnlyProgram.Outputs == MaterialGraphOutputOpacityMask && program.Code.find("    return 1.0;") != std::string::npos,
          "a graph connecting only Opacity Mask has an output (it is built); one without the mask returns 1 from opacity_mask()");

    // World Position Offset (stylized step 4b part 2): its own function, per vertex, from only the nodes feeding that pin (a wave along the
    // vertex normal: Vertex Normal x Sine(Time x Speed + World Position.x x 0.05) x Amplitude, plus a texture's red); the Material Output's
    // Max World Position Offset Displacement in the program; Screen Position refused there; graphs saved before the pin existed keep theirs.
    NodeGraph waving = CreateEmptyMaterialGraph();
    waving.Nodes[0].Properties["Max World Position Offset Displacement"] = 12.5f;
    const uint32_t waveSplit = node(waving, "Split");
    link(waving, node(waving, "World Position"), 0, waveSplit, 0);
    const uint32_t waveTime = node(waving, "Multiply");
    link(waving, node(waving, "Time"), 0, waveTime, 0);
    link(waving, node(waving, "Scalar Parameter", { { "Name", std::string("Speed") }, { "Default", 2.0f } }), 0, waveTime, 1);
    const uint32_t wavePhase = node(waving, "Multiply", { { "B", 0.05f } });
    link(waving, waveSplit, 0, wavePhase, 0); // X
    const uint32_t waveSum = node(waving, "Add");
    link(waving, waveTime, 0, waveSum, 0);
    link(waving, wavePhase, 0, waveSum, 1);
    const uint32_t waveSine = node(waving, "Sine");
    link(waving, waveSum, 0, waveSine, 0);
    const uint32_t waveAmplitude = node(waving, "Multiply");
    link(waving, waveSine, 0, waveAmplitude, 0);
    link(waving, node(waving, "Scalar Parameter", { { "Name", std::string("Amplitude") }, { "Default", 10.0f } }), 0, waveAmplitude, 1);
    const uint32_t waveTexture = node(waving, "Texture Sample", { { "Parameter Name", std::string("WindMask") } });
    const uint32_t waveMasked = node(waving, "Multiply");
    link(waving, waveAmplitude, 0, waveMasked, 0);
    link(waving, waveTexture, 1, waveMasked, 1); // R
    const uint32_t waveOffset = node(waving, "Multiply");
    link(waving, node(waving, "Vertex Normal"), 0, waveOffset, 0);
    link(waving, waveMasked, 0, waveOffset, 1);
    link(waving, waveOffset, 0, waving.OutputNode, 8);
    link(waving, node(waving, "Color", { { "Value", glm::vec4(0.1f, 0.3f, 0.8f, 1.0f) } }), 0, waving.OutputNode, 0);
    const MaterialGraphProgram waveProgram = CompileMaterialGraph(waving);
    const size_t offsetFunction = waveProgram.Code.find("float3 world_position_offset(MaterialGraphInputs i)");
    CHECK(waveProgram.Valid && waveProgram.Outputs == (MaterialGraphOutputBaseColor | MaterialGraphOutputWorldPositionOffset) && offsetFunction != std::string::npos &&
          waveProgram.Code.find("sin(", offsetFunction) != std::string::npos && waveProgram.Code.find("i.normal", offsetFunction) != std::string::npos,
          "World Position Offset: its own function returning the offset, the output bit set: '{}'", waveProgram.Error);
    CHECK(waveProgram.Code.substr(0, offsetFunction).find("sin(") == std::string::npos, "the offset's nodes are only in world_position_offset()");
    CHECK(waveProgram.MaxDisplacement == 12.5f, "the Material Output's Max World Position Offset Displacement is in the program: {}", waveProgram.MaxDisplacement);
    CHECK(waveProgram.ParameterSlots == 2 && waveProgram.TextureSlots == 1, "the offset's parameters and texture have slots: {} / {}", waveProgram.ParameterSlots,
          waveProgram.TextureSlots);
    CHECK(program.Code.find("    return float3(0.0);") != std::string::npos && program.MaxDisplacement == 0.0f,
          "a graph without the offset returns 0 from world_position_offset() and has no Max Displacement");
    // Ray tracing (step 4c part 2): the mask alone for MaterialGraphMasks.slang -- only graphs with a mask get a case, and no surface or
    // offset code is in it (a surface edit leaves the ray tracing variants' binaries alone).
    CHECK(!maskProgram.MaskCode.empty() && maskProgram.MaskCode.find("float opacity_mask(MaterialGraphInputs i)") == 0 &&
          maskProgram.MaskCode.find("surface(") == std::string::npos && waveProgram.MaskCode.empty() && program.MaskCode.empty(),
          "MaskCode: the mask function alone, empty without a mask");
    const std::string masksFile = CustomLitShaders::GenerateGraphMaskDispatcher(
        { "Graphs/Panned.nmatgraph", "Graphs/Leaves.nmatgraph", "Graphs/Waving.nmatgraph" },
        { { "Graphs/Panned.nmatgraph", program.Code, program.MaskCode }, { "Graphs/Leaves.nmatgraph", maskProgram.Code, maskProgram.MaskCode },
          { "Graphs/Waving.nmatgraph", waveProgram.Code, waveProgram.MaskCode } });
    CHECK(masksFile.find("case 1u: return MaterialGraph1::opacity_mask(i);") != std::string::npos && masksFile.find("case 0u") == std::string::npos &&
          masksFile.find("case 2u") == std::string::npos && masksFile.find("surface(") == std::string::npos &&
          masksFile.find("world_position_offset") == std::string::npos && masksFile.find("#define NOX_MATERIAL_GRAPH_MASK_ONLY 1") != std::string::npos,
          "MaterialGraphMasks.slang: a case for the masked graph only, no surface or offset code, the raster helpers left out");
    // The surfaces alone for the passes that shade ray hits (MaterialGraphSurfaces.slang): every graph's surface, no mask, no offset.
    CHECK(waveProgram.SurfaceCode.find("void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)") == 0 &&
          waveProgram.SurfaceCode.find("sin(") == std::string::npos && maskProgram.SurfaceCode.find("material_graph_texture") == std::string::npos,
          "SurfaceCode: the surface function alone (the offset's and the mask's nodes are not in it)");
    const std::string surfacesFile = CustomLitShaders::GenerateGraphSurfaceDispatcher(
        { "Graphs/Panned.nmatgraph", "Graphs/Leaves.nmatgraph", "Graphs/Waving.nmatgraph" },
        { { "Graphs/Panned.nmatgraph", program.Code, program.MaskCode, program.SurfaceCode },
          { "Graphs/Leaves.nmatgraph", maskProgram.Code, maskProgram.MaskCode, maskProgram.SurfaceCode },
          { "Graphs/Waving.nmatgraph", waveProgram.Code, waveProgram.MaskCode, waveProgram.SurfaceCode } });
    CHECK(surfacesFile.find("case 0u: MaterialGraphSurface0::surface(i, o); return;") != std::string::npos &&
          surfacesFile.find("case 2u: MaterialGraphSurface2::surface(i, o); return;") != std::string::npos &&
          surfacesFile.find("opacity_mask") == std::string::npos && surfacesFile.find("world_position_offset") == std::string::npos,
          "MaterialGraphSurfaces.slang: a case per graph, no mask or offset code");
    // Custom Light / Custom Ambient (step 4c part 5): the graph's own lighting for a Custom Lit material, as a Material Shader's light() /
    // ambient() -- a two-band toon light (Step of the light's N.L x its shadow) in the base colour; the Light node anywhere else refused.
    NodeGraph toonLit = CreateEmptyMaterialGraph();
    const uint32_t litSurface = node(toonLit, "Lit Surface");
    const uint32_t litLight = node(toonLit, "Light");
    const uint32_t litDot = node(toonLit, "Dot");
    link(toonLit, litSurface, 1, litDot, 0); // Normal
    link(toonLit, litLight, 0, litDot, 1);   // Direction
    const uint32_t litBand = node(toonLit, "Step", { { "Edge", 0.2f } });
    link(toonLit, litDot, 0, litBand, 1);
    const uint32_t litLit = node(toonLit, "Multiply");
    link(toonLit, litBand, 0, litLit, 0);
    link(toonLit, litLight, 2, litLit, 1);   // Shadow
    const uint32_t litColor = node(toonLit, "Multiply");
    link(toonLit, litLight, 1, litColor, 0); // Color
    link(toonLit, litLit, 0, litColor, 1);
    const uint32_t litOut = node(toonLit, "Multiply");
    link(toonLit, litColor, 0, litOut, 0);
    link(toonLit, litSurface, 0, litOut, 1); // Base Color
    link(toonLit, litOut, 0, toonLit.OutputNode, 9);
    link(toonLit, node(toonLit, "Vector Parameter", { { "Name", std::string("Shade") }, { "Default", glm::vec4(0.2f, 0.2f, 0.4f, 1.0f) } }), 0,
         toonLit.OutputNode, 10);
    const MaterialGraphProgram toonLitProgram = CompileMaterialGraph(toonLit);
    CHECK(toonLitProgram.Valid && (toonLitProgram.Outputs & (MaterialGraphOutputCustomLight | MaterialGraphOutputCustomAmbient)) ==
                                      (MaterialGraphOutputCustomLight | MaterialGraphOutputCustomAmbient) &&
              toonLitProgram.LightCode.find("float3 light(CustomLitSurface s, CustomLitLight l)") == 0 &&
              toonLitProgram.LightCode.find("l.shadow") != std::string::npos && toonLitProgram.LightCode.find("float3 ambient(CustomLitSurface s") != std::string::npos &&
              toonLitProgram.Code.find("l.direction") == std::string::npos,
          "Custom Light / Custom Ambient: light() and ambient() in LightCode, none of it in the surface code: '{}'", toonLitProgram.Error);
    CHECK(program.LightCode.empty(), "a graph without the lighting pins has no LightCode");
    NodeGraph lightMisused = CreateEmptyMaterialGraph();
    link(lightMisused, node(lightMisused, "Light"), 1, lightMisused.OutputNode, 0);
    const MaterialGraphProgram lightMisusedProgram = CompileMaterialGraph(lightMisused);
    CHECK(!lightMisusedProgram.Valid && lightMisusedProgram.Error.find("Light can only feed Custom Light") != std::string::npos,
          "the Light node feeding Base Color is refused: '{}'", lightMisusedProgram.Error);
    const std::string litDispatcher = CustomLitShaders::GenerateDispatcher({ "Graphs/ToonLit.nmatgraph" },
                                                                           { { "Graphs/ToonLit.nmatgraph", toonLitProgram.LightCode, std::string(), true } });
    CHECK(litDispatcher.find("MaterialGraphInputs material_graph_lit_inputs(CustomLitSurface s)") != std::string::npos &&
              litDispatcher.find("case 0u: return CustomLit0::light(s, l);") != std::string::npos,
          "a graph's lighting takes a slot of the Material Shaders' dispatcher, with the graph inputs' helper");
    // Vertex-stage inputs (step 4e part 1): Local Position (before skinning: a dissolve that stays on an animated character), Vertex
    // Tangent / Bitangent -- in the surface, the mask and World Position Offset; refused in the lighting (the G-buffer keeps none of them).
    NodeGraph localGraph = CreateEmptyMaterialGraph();
    const uint32_t localNoise = node(localGraph, "Noise", { { "Scale", 0.05f } });
    link(localGraph, node(localGraph, "Local Position"), 0, localNoise, 0);
    link(localGraph, localNoise, 0, localGraph.OutputNode, 7); // Opacity Mask
    link(localGraph, node(localGraph, "Vertex Tangent"), 0, localGraph.OutputNode, 0); // Base Color
    const uint32_t localWave = node(localGraph, "Multiply", { { "B", 2.0f } });
    link(localGraph, node(localGraph, "Vertex Bitangent"), 0, localWave, 0);
    link(localGraph, localWave, 0, localGraph.OutputNode, 8); // World Position Offset
    const MaterialGraphProgram localProgram = CompileMaterialGraph(localGraph);
    CHECK(localProgram.Valid && localProgram.MaskCode.find("i.localPosition") != std::string::npos &&
              localProgram.SurfaceCode.find("i.tangent") != std::string::npos && localProgram.Code.find("cross(i.normal, i.tangent.xyz)") != std::string::npos,
          "Local Position into the mask, Vertex Tangent into the surface, Vertex Bitangent into World Position Offset: '{}'", localProgram.Error);
    NodeGraph localLit = CreateEmptyMaterialGraph();
    link(localLit, node(localLit, "Local Position"), 0, localLit.OutputNode, 9);
    const MaterialGraphProgram localLitProgram = CompileMaterialGraph(localLit);
    CHECK(!localLitProgram.Valid && localLitProgram.Error.find("Local Position cannot feed Custom Light") != std::string::npos,
          "Local Position into Custom Light is refused (the lighting runs from the G-buffer): '{}'", localLitProgram.Error);
    // Vertex-stage inputs (step 4e part 2): Object Position, Object Bounds (Center / Extent / Radius), Transform (Local/World/View/Clip),
    // and Matrix (Model / View / Projection + inverses).
    NodeGraph objGraph = CreateEmptyMaterialGraph();
    const uint32_t objPos = node(objGraph, "Object Position");
    const uint32_t objBounds = node(objGraph, "Object Bounds");
    const uint32_t xform = node(objGraph, "Transform", { { "From", int32_t(1) }, { "To", int32_t(2) }, { "Kind", int32_t(0) } }); // World -> View Direction
    const uint32_t matNode = node(objGraph, "Matrix", { { "Matrix", int32_t(2) } }); // View matrix
    link(objGraph, node(objGraph, "Vertex Normal"), 0, xform, 0);
    link(objGraph, xform, 0, objGraph.OutputNode, 0);   // Base Color
    link(objGraph, objBounds, 2, objGraph.OutputNode, 2); // Roughness <- Radius
    link(objGraph, objPos, 0, objGraph.OutputNode, 4);    // Emissive <- Object Position
    link(objGraph, matNode, 0, objGraph.OutputNode, 8);   // WPO <- View Row 0
    const MaterialGraphProgram objProgram = CompileMaterialGraph(objGraph);
    CHECK(objProgram.Valid &&
              objProgram.SurfaceCode.find("material_graph_object_position(i)") != std::string::npos &&
              objProgram.SurfaceCode.find("material_graph_object_bounds_extent(i)") != std::string::npos &&
              objProgram.SurfaceCode.find("material_graph_transform(i,") != std::string::npos &&
              objProgram.Code.find("material_graph_matrix(i, 2u)") != std::string::npos,
          "Step 4e part 2 nodes compile into surface and WPO: '{}'", objProgram.Error);
    NodeGraph objLitRefused = CreateEmptyMaterialGraph();
    link(objLitRefused, node(objLitRefused, "Object Position"), 0, objLitRefused.OutputNode, 9);
    CHECK(!CompileMaterialGraph(objLitRefused).Valid, "Object Position into Custom Light is refused");
    NodeGraph localXformLit = CreateEmptyMaterialGraph();
    const uint32_t localXform = node(localXformLit, "Transform", { { "From", int32_t(0) }, { "To", int32_t(1) }, { "Kind", int32_t(0) } });
    link(localXformLit, localXform, 0, localXformLit.OutputNode, 9);
    CHECK(!CompileMaterialGraph(localXformLit).Valid, "Transform using Local space into Custom Light is refused");
    NodeGraph worldViewXformLit = CreateEmptyMaterialGraph();
    const uint32_t wvXform = node(worldViewXformLit, "Transform", { { "From", int32_t(1) }, { "To", int32_t(2) }, { "Kind", int32_t(0) } });
    link(worldViewXformLit, wvXform, 0, worldViewXformLit.OutputNode, 9);
    CHECK(CompileMaterialGraph(worldViewXformLit).Valid, "Transform World -> View works in Custom Light");
    // The stage of every Material Output pin in its tooltip (step 4d).
    const NodeTypeDesc* stageOutput = NodeTypeRegistry::Find(kMaterialGraphDomain, "Material Output");
    CHECK(stageOutput && stageOutput->Inputs[0].Tooltip.starts_with("Per pixel") && stageOutput->Inputs[8].Tooltip.starts_with("Per vertex") &&
              stageOutput->Inputs[9].Tooltip.starts_with("Per light"),
          "the Material Output's pins say their stage: Base Color per pixel, World Position Offset per vertex, Custom Light per light");
    const auto& rayVariants = CustomLitShaders::RayTracingMaskVariants();
    CHECK(std::count_if(rayVariants.begin(), rayVariants.end(), [](const CustomLitShaders::RayTracingMaskVariant& v) { return v.Surface; }) == 7 &&
          std::any_of(rayVariants.begin(), rayVariants.end(), [](const CustomLitShaders::RayTracingMaskVariant& v)
          {
              return v.Source == "assets/shaders/PathTracer/PathTracerHit.slang" && v.Surface;
          }), "seven of them shade their hits with the graphs' surfaces (the path tracer's closest hit among them)");
    CHECK(rayVariants.size() == 12 && std::any_of(rayVariants.begin(), rayVariants.end(), [](const CustomLitShaders::RayTracingMaskVariant& v)
          {
              return v.Source == "assets/shaders/ShadowMask.slang" && v.Variant.ends_with("/ShadowMaskGraphMask.slang");
          }), "twelve ray tracing passes get a mask variant (the sun's ray-traced shadows among them): {}", rayVariants.size());
    NodeGraph screenOffset = CreateEmptyMaterialGraph();
    link(screenOffset, node(screenOffset, "Screen Position"), 0, screenOffset.OutputNode, 8);
    const MaterialGraphProgram screenOffsetProgram = CompileMaterialGraph(screenOffset);
    CHECK(!screenOffsetProgram.Valid && screenOffsetProgram.Error.find("Screen Position cannot feed World Position Offset") != std::string::npos,
          "Screen Position into World Position Offset is refused, naming the node: '{}'", screenOffsetProgram.Error);
    NodeGraph screenSurface = CreateEmptyMaterialGraph();
    link(screenSurface, node(screenSurface, "Screen Position"), 0, screenSurface.OutputNode, 4);
    CHECK(CompileMaterialGraph(screenSurface).Valid, "Screen Position still feeds the surface pins");
    const NodeTypeDesc* outputType = NodeTypeRegistry::Find(kMaterialGraphDomain, "Material Output");
    CHECK(outputType && outputType->Inputs.size() == 31 && outputType->Inputs[7].Name == "Opacity Mask" && outputType->Inputs[8].Name == "World Position Offset" &&
              outputType->Inputs[9].Name == "Custom Light" && outputType->Inputs[10].Name == "Custom Ambient" &&
              outputType->Inputs[11].Name == "Vertex Normal" && outputType->Inputs[12].Name == "Vertex Color" &&
              outputType->Inputs[13].Name == "Specular" && outputType->Inputs[15].Name == "Clear Coat" &&
              outputType->Inputs[22].Name == "Transmission" && outputType->Inputs[28].Name == "Iridescence" &&
              outputType->Inputs[30].Name == "Dispersion",
          "the Material Output's pins: core 0..12 kept at their indices, lobe pins 13..30 appended (step 4g)");
    // Pixel-stage lobe outputs (step 4g): Clear Coat, Sheen, Iridescence, Anisotropy, Transmission, IOR, Dispersion.
    NodeGraph lobeGraph = CreateEmptyMaterialGraph();
    const uint32_t coatVal = node(lobeGraph, "Constant", { { "Value", 0.85f } });
    const uint32_t iorVal = node(lobeGraph, "Constant", { { "Value", 1.7f } });
    const uint32_t iridThick = node(lobeGraph, "Constant", { { "Value", 420.0f } });
    const uint32_t sheenCol = node(lobeGraph, "Color", { { "Value", glm::vec4(0.9f, 0.2f, 0.5f, 1.0f) } });
    link(lobeGraph, coatVal, 0, lobeGraph.OutputNode, 15);    // Clear Coat
    link(lobeGraph, sheenCol, 0, lobeGraph.OutputNode, 18);   // Sheen Color
    link(lobeGraph, iorVal, 0, lobeGraph.OutputNode, 23);     // IOR
    link(lobeGraph, coatVal, 0, lobeGraph.OutputNode, 28);    // Iridescence
    link(lobeGraph, iridThick, 0, lobeGraph.OutputNode, 29);  // Iridescence Thickness
    link(lobeGraph, coatVal, 0, lobeGraph.OutputNode, 30);    // Dispersion
    const MaterialGraphProgram lobeProgram = CompileMaterialGraph(lobeGraph);
    CHECK(lobeProgram.Valid &&
              (lobeProgram.Outputs & MaterialGraphOutputClearCoat) != 0 &&
              (lobeProgram.Outputs & MaterialGraphOutputSheenColor) != 0 &&
              (lobeProgram.Outputs & MaterialGraphOutputIOR) != 0 &&
              (lobeProgram.Outputs & MaterialGraphOutputIridescence) != 0 &&
              (lobeProgram.Outputs & MaterialGraphOutputIridescenceThickness) != 0 &&
              (lobeProgram.Outputs & MaterialGraphOutputDispersion) != 0 &&
              lobeProgram.SurfaceCode.find("o.clearCoat =") != std::string::npos &&
              lobeProgram.SurfaceCode.find("o.sheenColor =") != std::string::npos &&
              lobeProgram.SurfaceCode.find("o.ior =") != std::string::npos &&
              lobeProgram.SurfaceCode.find("o.iridescenceThickness =") != std::string::npos &&
              lobeProgram.SurfaceCode.find("o.dispersion =") != std::string::npos,
          "Step 4g lobe outputs compile cleanly into SurfaceCode: '{}'", lobeProgram.Error);
    // Vertex-stage outputs (step 4f): Vertex Interpolator (up to 4 slots), Vertex Normal override, Vertex Color override.
    NodeGraph interpGraph = CreateEmptyMaterialGraph();
    const uint32_t interp = node(interpGraph, "Vertex Interpolator");
    const uint32_t uvMul = node(interpGraph, "Multiply", { { "B", 2.0f } });
    link(interpGraph, node(interpGraph, "Texture Coordinate"), 0, uvMul, 0);
    link(interpGraph, uvMul, 0, interp, 0);
    link(interpGraph, interp, 0, interpGraph.OutputNode, 0);                        // Base Color <- interpolated float2
    link(interpGraph, node(interpGraph, "Vertex Normal"), 0, interpGraph.OutputNode, 11); // Vertex Normal override
    link(interpGraph, node(interpGraph, "Color"), 1, interpGraph.OutputNode, 12);         // Vertex Color override
    const MaterialGraphProgram interpProgram = CompileMaterialGraph(interpGraph);
    CHECK(interpProgram.Valid &&
              (interpProgram.Outputs & MaterialGraphOutputVertexInterpolators) != 0 &&
              (interpProgram.Outputs & MaterialGraphOutputVertexNormal) != 0 &&
              (interpProgram.Outputs & MaterialGraphOutputVertexColor) != 0 &&
              interpProgram.VertexCode.find("vo.interp[0u]") != std::string::npos &&
              interpProgram.VertexCode.find("vo.normal =") != std::string::npos &&
              interpProgram.VertexCode.find("vo.color =") != std::string::npos &&
              interpProgram.SurfaceCode.find("i.vertexInterp[0u].xy") != std::string::npos,
          "Step 4f Vertex Interpolator + Vertex Normal / Color overrides compile cleanly: '{}'", interpProgram.Error);
    NodeGraph interpScreen = CreateEmptyMaterialGraph();
    const uint32_t interpBad = node(interpScreen, "Vertex Interpolator");
    link(interpScreen, node(interpScreen, "Screen Position"), 0, interpBad, 0);
    link(interpScreen, interpBad, 0, interpScreen.OutputNode, 0);
    CHECK(!CompileMaterialGraph(interpScreen).Valid, "Screen Position feeding Vertex Interpolator is refused (runs per vertex)");
    NodeGraph interpLight = CreateEmptyMaterialGraph();
    const uint32_t interpToLight = node(interpLight, "Vertex Interpolator");
    link(interpLight, interpToLight, 0, interpLight.OutputNode, 9);
    CHECK(!CompileMaterialGraph(interpLight).Valid, "Vertex Interpolator feeding Custom Light is refused");

    // Pixel-stage inputs (step 4h): Pixel Normal (after Normal pin), Scene Depth, Pixel Depth, Depth Fade.
    NodeGraph pixelInputsGraph = CreateEmptyMaterialGraph();
    const uint32_t normalTex = node(pixelInputsGraph, "Texture Sample", { { "Normal Map", true } });
    const uint32_t pixNorm = node(pixelInputsGraph, "Pixel Normal");
    const uint32_t pixelFresnel = node(pixelInputsGraph, "Fresnel");
    const uint32_t depthFade = node(pixelInputsGraph, "Depth Fade", { { "Fade Distance", 50.0f } });
    const uint32_t sceneDepth = node(pixelInputsGraph, "Scene Depth");
    const uint32_t pixelDepth = node(pixelInputsGraph, "Pixel Depth");
    const uint32_t depthSub = node(pixelInputsGraph, "Subtract");
    link(pixelInputsGraph, normalTex, 0, pixelInputsGraph.OutputNode, 3);    // Normal
    link(pixelInputsGraph, pixNorm, 0, pixelFresnel, 2);                     // Fresnel.Normal <- Pixel Normal
    link(pixelInputsGraph, pixelFresnel, 0, pixelInputsGraph.OutputNode, 4); // Emissive
    link(pixelInputsGraph, depthFade, 0, pixelInputsGraph.OutputNode, 6);    // Opacity
    link(pixelInputsGraph, sceneDepth, 0, depthSub, 0);
    link(pixelInputsGraph, pixelDepth, 0, depthSub, 1);
    link(pixelInputsGraph, depthSub, 0, pixelInputsGraph.OutputNode, 2);     // Roughness
    const MaterialGraphProgram pixelInputsProgram = CompileMaterialGraph(pixelInputsGraph);
    CHECK(pixelInputsProgram.Valid &&
              pixelInputsProgram.SurfaceCode.find("const float3 _pixelNormal = material_graph_world_normal(") != std::string::npos &&
              pixelInputsProgram.SurfaceCode.find("i.sceneDepth") != std::string::npos &&
              pixelInputsProgram.SurfaceCode.find("i.pixelDepth") != std::string::npos,
          "Step 4h Pixel Normal (post-Normal pin), Scene Depth, Pixel Depth & Depth Fade compile cleanly: '{}'", pixelInputsProgram.Error);
    NodeGraph pixelNormCycle = CreateEmptyMaterialGraph();
    link(pixelNormCycle, node(pixelNormCycle, "Pixel Normal"), 0, pixelNormCycle.OutputNode, 3);
    CHECK(!CompileMaterialGraph(pixelNormCycle).Valid, "Pixel Normal feeding Normal pin is refused");
    NodeGraph depthInWpo = CreateEmptyMaterialGraph();
    link(depthInWpo, node(depthInWpo, "Scene Depth"), 0, depthInWpo.OutputNode, 8);
    CHECK(!CompileMaterialGraph(depthInWpo).Valid, "Scene Depth feeding World Position Offset is refused");

    // Step R1: Subsurface Color pin (pin 31, MaterialGraphOutput2SubsurfaceColor).
    NodeGraph sssGraph = CreateEmptyMaterialGraph();
    const uint32_t sssCol = node(sssGraph, "Color", { { "Value", glm::vec4(0.95f, 0.25f, 0.15f, 1.0f) } });
    link(sssGraph, sssCol, 0, sssGraph.OutputNode, 31);
    const MaterialGraphProgram sssProgram = CompileMaterialGraph(sssGraph);
    CHECK(sssProgram.Valid &&
              (sssProgram.Outputs2 & MaterialGraphOutput2SubsurfaceColor) != 0 &&
              sssProgram.SurfaceCode.find("o.subsurfaceColor =") != std::string::npos,
          "Step R1 Subsurface Color pin compiles cleanly into Outputs2 / SurfaceCode: '{}'", sssProgram.Error);

    // Step R2: AO Light Affect (pin 32) & Pixel Depth Offset (pin 33).
    NodeGraph r2Graph = CreateEmptyMaterialGraph();
    const uint32_t aoAffVal = node(r2Graph, "Constant", { { "Value", 0.8f } });
    const uint32_t pdoVal = node(r2Graph, "Constant", { { "Value", 25.0f } });
    link(r2Graph, aoAffVal, 0, r2Graph.OutputNode, 32);
    link(r2Graph, pdoVal, 0, r2Graph.OutputNode, 33);
    const MaterialGraphProgram r2Program = CompileMaterialGraph(r2Graph);
    CHECK(r2Program.Valid &&
              (r2Program.Outputs2 & MaterialGraphOutput2AOLightAffect) != 0 &&
              (r2Program.Outputs2 & MaterialGraphOutput2PixelDepthOffset) != 0 &&
              r2Program.SurfaceCode.find("o.aoLightAffect =") != std::string::npos &&
              r2Program.Code.find("float pixel_depth_offset(MaterialGraphInputs i)") != std::string::npos,
          "Step R2 AO Light Affect & Pixel Depth Offset compile cleanly: '{}'", r2Program.Error);

    // Step 5: hand-written surface() in a Material Shader (.slang).
    const std::string handSurface = R"(
#include "CustomLit.slang"
void surface(MaterialGraphInputs i, inout MaterialGraphOutputs o)
{
    o.baseColor = float3(0.2, 0.8, 0.4);
    o.roughness = 0.25;
    o.subsurfaceColor = float3(0.1, 0.9, 0.3);
}
)";
    uint32_t handOut1 = 0, handOut2 = 0;
    CustomLitShaders::DetectSurfaceOutputs(handSurface, handOut1, handOut2);
    const MaterialGraphSource handWrapped = CustomLitShaders::BuildShaderSurfaceSource({ "Shaders/Hand.slang", handSurface, "" }, handOut1);
    CHECK(CustomLitShaders::HasSurfaceFunction(handSurface) &&
              !CustomLitShaders::HasLightFunctions(handSurface) &&
              (handOut1 & MaterialGraphOutputBaseColor) != 0 &&
              (handOut1 & MaterialGraphOutputRoughness) != 0 &&
              (handOut1 & MaterialGraphOutputNormal) == 0 &&
              (handOut2 & MaterialGraphOutput2SubsurfaceColor) != 0 &&
              handWrapped.Code.find("void surface(") != std::string::npos,
          "Step 5 hand-written surface() detection, output bit scanning & wrapper work cleanly");

    // Every node is offered in the node graph editor's Add menu: a "Parameter" property marks a node reading a graph variable, which the
    // menu lists only as "Get <variable>" -- Texture Sample had one and was missing from the menu (the user's test).
    size_t offered = 0, hidden = 0;
    for (const NodeTypeDesc& type : NodeTypeRegistry::All())
    {
        if (type.Domain != kMaterialGraphDomain)
            continue;
        const bool readsVariable = std::any_of(type.DefaultProperties.begin(), type.DefaultProperties.end(),
                                               [](const auto& property) { return property.first == kParameterReferenceProperty; });
        (readsVariable ? hidden : offered) += 1;
    }
    CHECK(hidden == 0 && offered >= 45, "every material graph node is in the Add menu (none marked as a variable reader): {} offered, {} hidden",
          offered, hidden);
    CHECK(CompileMaterialGraph(CreateEmptyMaterialGraph()).Valid && CompileMaterialGraph(CreateEmptyMaterialGraph()).Outputs == 0,
          "an empty graph compiles and writes nothing (the material as it is)");

    if (const char* dump = std::getenv("NOX_MATERIAL_GRAPH_DUMP"))
    {
        std::ofstream file(std::string(dump) + "/graph0.slang");
        file << program.Code;
        std::ofstream file2(std::string(dump) + "/graph1.slang");
        file2 << appendProgram.Code;
        // The whole generated file with both graphs (compile the G-buffer and forward variants with it: NoxEditor/assets/shaders on the
        // include path, NOX_MATERIAL_GRAPH defined).
        std::ofstream generated(std::string(dump) + "/CustomLitShaders.slang");
        generated << CustomLitShaders::GenerateDispatcher({}, {});
        std::ofstream graphsFile(std::string(dump) + "/MaterialGraphs.slang");
        graphsFile << CustomLitShaders::GenerateGraphDispatcher({ "Graphs/Panned.nmatgraph", "Graphs/Append.nmatgraph" },
                                                                { { "Graphs/Panned.nmatgraph", program.Code }, { "Graphs/Append.nmatgraph", appendProgram.Code } });
        std::ofstream maskFile(std::string(dump) + "/MaterialGraphsMask.slang");
        maskFile << CustomLitShaders::GenerateGraphDispatcher({ "Graphs/Leaves.nmatgraph" }, { { "Graphs/Leaves.nmatgraph", maskProgram.Code } });
        // Every function in use: a surface graph, a masked one and a waving one (compile every graph variant with it).
        std::ofstream masksOut(std::string(dump) + "/MaterialGraphMasks.slang");
        masksOut << CustomLitShaders::GenerateGraphMaskDispatcher({ "Graphs/Panned.nmatgraph", "Graphs/Leaves.nmatgraph", "Graphs/Waving.nmatgraph",
                                                                    "Graphs/Local.nmatgraph" },
                                                                  { { "Graphs/Panned.nmatgraph", program.Code, program.MaskCode },
                                                                    { "Graphs/Leaves.nmatgraph", maskProgram.Code, maskProgram.MaskCode },
                                                                    { "Graphs/Waving.nmatgraph", waveProgram.Code, waveProgram.MaskCode },
                                                                    { "Graphs/Local.nmatgraph", localProgram.Code, localProgram.MaskCode } });
        std::ofstream litOut(std::string(dump) + "/CustomLitShadersGraph.slang");
        litOut << litDispatcher;
        std::ofstream surfacesOut(std::string(dump) + "/MaterialGraphSurfaces.slang");
        surfacesOut << CustomLitShaders::GenerateGraphSurfaceDispatcher(
            { "Graphs/Panned.nmatgraph", "Graphs/Leaves.nmatgraph", "Graphs/Waving.nmatgraph", "Graphs/Local.nmatgraph" },
            { { "Graphs/Panned.nmatgraph", program.Code, program.MaskCode, program.SurfaceCode },
              { "Graphs/Leaves.nmatgraph", maskProgram.Code, maskProgram.MaskCode, maskProgram.SurfaceCode },
              { "Graphs/Waving.nmatgraph", waveProgram.Code, waveProgram.MaskCode, waveProgram.SurfaceCode },
              { "Graphs/Local.nmatgraph", localProgram.Code, localProgram.MaskCode, localProgram.SurfaceCode } });
        std::ofstream allFile(std::string(dump) + "/MaterialGraphsAll.slang");
        allFile << CustomLitShaders::GenerateGraphDispatcher({ "Graphs/Panned.nmatgraph", "Graphs/Leaves.nmatgraph", "Graphs/Waving.nmatgraph", "Graphs/Local.nmatgraph" },
                                                             { { "Graphs/Panned.nmatgraph", program.Code }, { "Graphs/Leaves.nmatgraph", maskProgram.Code },
                                                               { "Graphs/Waving.nmatgraph", waveProgram.Code }, { "Graphs/Local.nmatgraph", localProgram.Code } });
    }
}

// An entity's material graph parameters (MaterialComponent::GraphParameters; stylized step 4c part 1, the C# MeshComponent.Set*Parameter
// API): applied by name over the slot material's own values, per slot or every slot, later entries winning.
static void TestEntityGraphParameters()
{
    MaterialData material;
    material.GraphParameters.push_back({ "Dissolve", false, glm::vec4(0.3f), {} });
    MaterialComponent component;
    CHECK(!component.HasParameters(), "an empty Material component sets no parameters");
    component.GraphParameters.push_back({ "Dissolve", false, glm::vec4(0.8f), {}, UINT32_MAX });
    component.GraphParameters.push_back({ "Tint", false, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), {}, 2 });
    component.GraphParameters.push_back({ "Albedo", true, glm::vec4(0.0f), "Textures/Rust.png", UINT32_MAX });
    CHECK(component.HasParameters(), "graph parameters make the entity draw with records of its own");
    MaterialData slot0 = material;
    component.ApplyParameters(slot0, 0);
    CHECK(slot0.GraphParameters.size() == 2 && slot0.GraphParameters[0].Value.x == 0.8f, "the entity's value replaces the material's own of that name");
    CHECK(slot0.GraphParameters[1].IsTexture && slot0.GraphParameters[1].Texture == "Textures/Rust.png", "a texture parameter is added by name and path");
    const auto hasTint = [](const MaterialData& data)
    {
        return std::any_of(data.GraphParameters.begin(), data.GraphParameters.end(), [](const MaterialGraphParameterValue& v) { return v.Name == "Tint"; });
    };
    MaterialData slot2 = material;
    component.ApplyParameters(slot2, 2);
    CHECK(hasTint(slot2) && !hasTint(slot0), "a value for slot 2 applies to slot 2 only");
    component.GraphParameters.push_back({ "Dissolve", false, glm::vec4(0.1f), {}, 0 });
    MaterialData later = material;
    component.ApplyParameters(later, 0);
    CHECK(later.GraphParameters[0].Value.x == 0.1f, "a later entry wins over an earlier one");
    const MaterialComponent::GraphParameter* found = component.FindGraphParameter("Dissolve", false, 0);
    CHECK(found && found->Value.x == 0.1f && !component.FindGraphParameter("Dissolve", true, 0) && !component.FindGraphParameter("Dissolve", false, 5),
          "FindGraphParameter matches name, kind and slot");
}

int main()
{
    Log::Init("NoxToolsTest.log");
    TestConsole();
    TestCustomLitShaders();
    TestMaterialGraph();
    TestEntityGraphParameters();
    TestVoxReader();
    TestVoxMesher();
#ifdef NOX_TOOLS_TEST_MCP
    TestMcp();
    TestMcpValidation();
    TestMcpInputPlayer();
    TestMcpAssets();
    TestMcpComponentShapes();
    TestMcpGraphs();
#endif
#ifdef NOX_TOOLS_TEST_MCP_SERVER
    TestMcpServer();
    TestMcpClient();
#endif
    std::printf("\n%d checks, %d failed\n", s_Checks, s_Failed);
    return s_Failed == 0 ? 0 : 1;
}
