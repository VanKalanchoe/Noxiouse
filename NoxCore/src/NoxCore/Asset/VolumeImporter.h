#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Asset/AssetMetadata.h"
#include "NoxCore/Asset/VolumeAsset.h"

namespace Nox
{
    struct VolumeCookOptions
    {
        bool IncludeSequence = true;   // auto-detect numbered siblings (e.g. tornado_0000.vdb .. tornado_0199.vdb)
        uint32_t MaxDimension = 96u;   // max voxels along the longest axis per frame (keeps cooking & streaming fast)
        uint32_t MaxFrames = 0u;       // 0 = cook every frame in the sequence (preserves seamless looping!)
        float Fps = 24.0f;
    };

    class VolumeImporter
    {
    public:
        static Ref<VolumeAsset> ImportVolume(AssetHandle handle, const AssetMetadata& metadata);
        static Ref<VolumeAsset> LoadVolume(const std::filesystem::path& path);

        // Detects numbered .vdb sequence files in the same folder (e.g. name_0000.vdb -> name_0001.vdb ...).
        static std::vector<std::filesystem::path> FindVdbSequenceFiles(const std::filesystem::path& firstVdbPath);

        // Cooks a single .vdb or numbered .vdb sequence into a compact FP16 .nvdb binary asset (requires NOX_HAS_OPENVDB=1).
        static bool CookVdbToNvdb(const std::filesystem::path& vdbPath, const std::filesystem::path& outNvdbPath,
                                  const VolumeCookOptions& options = {});

        // Generates a procedural animated fire/smoke vortex .nvdb (zero external deps, useful for testing Step N2).
        static bool GenerateProceduralTestNvdb(const std::filesystem::path& outNvdbPath, uint32_t frameCount = 32u, uint32_t dim = 64u);

        static bool IsOpenVdbEnabled();
    };
}
