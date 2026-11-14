#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace Nox
{
    // One compiler or MSBuild message from a script build (`dotnet build` output), as data: what the Output Log shows and what the MCP
    // tool build_scripts returns. Plain text in, no engine dependencies (tested in NoxToolsTest).
    struct ScriptDiagnostic
    {
        std::string Severity; // "error" or "warning"
        std::string Code;     // "CS1002", "MSB1009", "NU1101" (empty when the line has none)
        std::string File;     // as the compiler wrote it (absolute on the build machine); empty for a project-wide message
        int Line = 0;         // 1-based; 0 when the message has no place
        int Column = 0;
        std::string Message;
    };

    // The errors and warnings in a build's output, each once (MSBuild repeats them in its summary), in the order they first appear:
    //   C:\Game\Scripts\Player.cs(12,5): error CS1002: ; expected [C:\Game\Scripts\Game.csproj]
    //   C:\Game\Scripts\Game.csproj : warning NU1603: ...
    //   MSBUILD : error MSB1009: Project file does not exist.
    std::vector<ScriptDiagnostic> ParseScriptBuildOutput(std::string_view output);
}
