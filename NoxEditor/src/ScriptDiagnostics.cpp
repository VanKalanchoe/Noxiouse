#include "ScriptDiagnostics.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace Nox
{
    namespace
    {
        std::string Trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }
    }

    std::vector<ScriptDiagnostic> ParseScriptBuildOutput(std::string_view output)
    {
        // <origin>[(line,col[,endLine,endCol])] : error|warning [CODE] : message [ [project] ]
        static const std::regex pattern(
            R"(^(.*?)(?:\((\d+)(?:,(\d+))?(?:,\d+,\d+)?\))?\s*:\s*(error|warning)\s*([A-Za-z]+[0-9]+)?\s*:\s*(.*)$)",
            std::regex::icase | std::regex::optimize);
        static const std::regex project(R"(\s*\[[^\[\]]*\]\s*$)");

        std::vector<ScriptDiagnostic> diagnostics;
        size_t start = 0;
        while (start <= output.size())
        {
            size_t end = output.find('\n', start);
            if (end == std::string_view::npos)
                end = output.size();
            std::string line(output.substr(start, end - start));
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::smatch match;
            if (!std::regex_match(line, match, pattern))
                continue;
            ScriptDiagnostic diagnostic;
            diagnostic.File = Trim(match[1].str());
            if (diagnostic.File == "MSBUILD" || diagnostic.File == "CSC" || diagnostic.File.empty())
                diagnostic.File.clear(); // (a message about the build itself, not a file)
            diagnostic.Line = match[2].matched ? std::stoi(match[2].str()) : 0;
            diagnostic.Column = match[3].matched ? std::stoi(match[3].str()) : 0;
            diagnostic.Severity = match[4].str();
            std::transform(diagnostic.Severity.begin(), diagnostic.Severity.end(), diagnostic.Severity.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            diagnostic.Code = match[5].str();
            diagnostic.Message = Trim(std::regex_replace(match[6].str(), project, ""));

            const bool seen = std::any_of(diagnostics.begin(), diagnostics.end(), [&](const ScriptDiagnostic& other)
            {
                return other.Severity == diagnostic.Severity && other.Code == diagnostic.Code && other.File == diagnostic.File &&
                       other.Line == diagnostic.Line && other.Column == diagnostic.Column && other.Message == diagnostic.Message;
            });
            if (!seen)
                diagnostics.push_back(std::move(diagnostic));
        }
        return diagnostics;
    }
}
