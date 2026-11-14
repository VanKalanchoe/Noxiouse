// NoxToolsTest: the engine console's registry (docs/MCP_Plan_2026-11.md §4) and, from MCP0, the MCP protocol layer -- without a
// window, GPU or editor. Exit code 0 when every check passed.
#include "NoxCore/Console/Console.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Renderer/CustomLitShaders.h"
#include "NoxCore/Renderer/MaterialGraph.h"
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
    // Opacity Mask (stylized step 4b): its own function from only the nodes feeding that pin; a parameter it alone reads gets a slot after
    // the surface's; a graph connecting only the mask still counts as connecting something (it is built).
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
    NodeGraph maskOnly = CreateEmptyMaterialGraph();
    link(maskOnly, node(maskOnly, "Constant", { { "Value", 0.25f } }), 0, maskOnly.OutputNode, 7);
    const MaterialGraphProgram maskOnlyProgram = CompileMaterialGraph(maskOnly);
    CHECK(maskOnlyProgram.Valid && maskOnlyProgram.Outputs == MaterialGraphOutputOpacityMask && program.Code.find("    return 1.0;") != std::string::npos,
          "a graph connecting only Opacity Mask has an output (it is built); one without the mask returns 1 from opacity_mask()");

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
    }
}

int main()
{
    Log::Init("NoxToolsTest.log");
    TestConsole();
    TestCustomLitShaders();
    TestMaterialGraph();
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
