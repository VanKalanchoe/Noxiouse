#pragma once
#include <filesystem>

namespace Nox
{
    class Renderer;

    // Writes everything Nox Stats knows (renderer settings, frame times, GPU passes, CPU scopes, memory, counters) as a
    // plain-text table into NoxStats_<date>_<time>.txt next to Nox.log, for sharing a capture without retyping it.
    // Main thread. Returns the written file, or an empty path on failure (logged).
    // Non-const: several renderer settings are only exposed through the editor's by-reference getters.
    std::filesystem::path SaveStatsReport(Renderer& renderer);
}
