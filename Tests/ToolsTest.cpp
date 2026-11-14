// NoxToolsTest: the engine console's registry (docs/MCP_Plan_2026-11.md §4) and, from MCP0, the MCP protocol layer -- without a
// window, GPU or editor. Exit code 0 when every check passed.
#include "NoxCore/Console/Console.h"
#include "NoxCore/Core/Log.h"

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

int main()
{
    Log::Init("NoxToolsTest.log");
    TestConsole();
#ifdef NOX_TOOLS_TEST_MCP
    TestMcp();
    TestMcpValidation();
#endif
#ifdef NOX_TOOLS_TEST_MCP_SERVER
    TestMcpServer();
#endif
    std::printf("\n%d checks, %d failed\n", s_Checks, s_Failed);
    return s_Failed == 0 ? 0 : 1;
}
