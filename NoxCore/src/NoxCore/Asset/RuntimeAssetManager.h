#pragma once

#include "EditorAssetManager.h"

namespace Nox
{
    // The asset manager of the runtime player and packaged games (docs/Runtime_And_Packaging_Plan_2026-10.md, R2 / item 7):
    // the editor's loading pipeline (background loads, uploads, texture streaming, the unused-asset sweep, memory assets) with
    // every editor duty turned off. No file watcher and no reimports, no source hashing, no folder scans, no registry, .nmat
    // or .bak writes, no texture recooks: it reads the cooked files and the registry it was given and never changes the
    // project on disk (a packaged game's folder may be read-only). The pack-file reader (R4) replaces its file reads later.
    class RuntimeAssetManager final : public EditorAssetManager
    {
    public:
        RuntimeAssetManager() : EditorAssetManager(ReadOnlyTag{}) {}
    };
}
