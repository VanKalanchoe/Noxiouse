#include "RGFrameReport.h"

#include <fstream>
#include <string_view>
#include <unordered_map>

#include "NoxCore/Core/Log.h"

namespace Nox
{
    bool WriteRenderGraphDot(const RGFrameReport& report, const std::filesystem::path& path)
    {
        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            NOX_CORE_ERROR("Render graph DOT: could not write {}", path.string());
            return false;
        }

        file << "digraph RenderGraph {\n";
        file << "  rankdir=LR;\n  node [fontname=\"Segoe UI\", fontsize=10];\n";

        for (uint32_t index = 0; index < report.Passes.size(); ++index)
        {
            const RGFrameReport::Pass& pass = report.Passes[index];
            file << "  p" << index << " [shape=box, label=\"" << pass.Name;
            if (!pass.Culled)
                file << "\\nchunk " << pass.Chunk;
            file << "\"" << (pass.Culled ? ", style=dashed, fontcolor=gray" : ", style=filled, fillcolor=\"#dbe8f7\"") << "];\n";
        }

        auto writeResources = [&file](const std::vector<RGFrameReport::Resource>& resources, const char* prefix)
        {
            // History slots share their key as name: labeled like the render graph panel, "Name [slot]".
            std::unordered_map<std::string_view, uint32_t> occurrences;
            for (uint32_t index = 0; index < resources.size(); ++index)
            {
                const RGFrameReport::Resource& resource = resources[index];
                const uint32_t occurrence = occurrences[resource.Name]++;
                if (resource.FirstPass == RGInvalidIndex)
                    continue;
                const char* color = resource.Kind == RGResourceKind::Transient ? "#e8f5e0" : resource.Kind == RGResourceKind::History ? "#fbeede" : "#eeeeee";
                file << "  " << prefix << index << " [shape=ellipse, style=filled, fillcolor=\"" << color << "\", label=\"" << resource.Name;
                if (resource.Kind == RGResourceKind::History)
                    file << " [" << occurrence << "]";
                file << "\"];\n";
            }
        };
        writeResources(report.Textures, "t");
        writeResources(report.Buffers, "b");

        for (uint32_t index = 0; index < report.Passes.size(); ++index)
        {
            const RGFrameReport::Pass& pass = report.Passes[index];
            if (pass.Culled)
                continue;

            for (uint32_t accessIndex = pass.FirstAccess; accessIndex < pass.FirstAccess + pass.AccessCount; ++accessIndex)
            {
                const RGFrameReport::Access& access = report.Accesses[accessIndex];
                const char* resource = access.IsTexture ? "t" : "b";
                if (access.IsWrite)
                    file << "  p" << index << " -> " << resource << access.Resource << ";\n";
                else
                    file << "  " << resource << access.Resource << " -> p" << index << ";\n";
            }
        }

        file << "}\n";
        return static_cast<bool>(file);
    }
}
