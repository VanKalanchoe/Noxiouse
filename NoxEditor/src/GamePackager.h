#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Nox
{
    // File > Package Game (docs/Runtime_And_Packaging_Plan_2026-10.md R3, R6 and a first R7): a folder that runs the game on its own.
    //
    //   <Output>/<Project>.exe        NoxRuntime, renamed
    //   <Output>/*.dll, Scripts/, ... everything else next to the editor's executable (engine libraries, .NET host, managed
    //                                 scripts, shader cache) except the editor itself and debug files
    //   <Output>/assets/              the engine's shaders, fonts and textures
    //   <Output>/Game/<Project>.nproj the project, and under Game/<AssetDirectory>/ only the assets its scenes need: cooked files
    //                                 without their sources (the importers take a cooked file as it is when its source is gone),
    //                                 with a registry of just those
    //
    // The scenes are the project's start scene (or every scene); scripts naming prefabs by path pull them in too.
    struct PackageSettings
    {
        std::filesystem::path OutputDirectory;
        bool AllScenes = false;
    };

    struct PackageReport
    {
        bool Success = false;
        std::string Error;
        size_t Assets = 0;
        size_t Files = 0;
        uint64_t Bytes = 0;
        std::filesystem::path Executable;
        std::vector<std::string> Warnings;
    };

    PackageReport PackageGame(const PackageSettings& settings);
}
