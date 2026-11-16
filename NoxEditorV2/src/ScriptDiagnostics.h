#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace Nox
{
    struct ScriptDiagnostic
    {
        std::string Severity; // "error" or "warning"
        std::string Code;     // "CS1002", "MSB1009", "NU1101" (empty when the line has none)
        std::string File;     // as the compiler wrote it (absolute on the build machine); empty for a project-wide message
        int Line = 0;         // 1-based; 0 when the message has no place
        int Column = 0;
        std::string Message;
    };

    std::vector<ScriptDiagnostic> ParseScriptBuildOutput(std::string_view output);
}
