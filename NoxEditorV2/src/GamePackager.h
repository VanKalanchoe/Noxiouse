#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Nox
{
    struct PackageSettings
    {
        std::filesystem::path OutputDirectory;
        bool AllScenes = false;
        bool PackFiles = true;
        bool PrecompiledShadersOnly = true;
        bool SelfContainedDotNet = false;
        bool Server = false;
    };

    struct PackageReport
    {
        bool Success = false;
        std::string Error;
        size_t Assets = 0;
        size_t Files = 0;
        size_t Cooked = 0;
        uint64_t Bytes = 0;
        std::filesystem::path Executable;
        std::vector<std::string> Warnings;
    };

    PackageReport PackageGame(const PackageSettings& settings);
}
