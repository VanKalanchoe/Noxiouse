#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "McpEditorTools.h"
#include "McpSceneHelpers.h"
#include "McpValidation.h"
#include "NoxCore/Asset/Asset.h"

// Shared by the editor's MCP tool files: the shared scene helpers (McpSceneHelpers, NoxMcp) and assets by uuid or path.
namespace Nox::Mcp
{
    // "StaticMesh" for AssetType::StaticMesh: how the tools name asset types (AssetTypeToString's text without "AssetType::").
    std::string ShortTypeName(AssetType type);

    // An asset of `type` named by its uuid (a digit string) or by its path under the asset directory ("Materials/Red.nmat"); 0 and
    // `error` when there is none (or it is another kind of asset).
    AssetHandle ResolveAsset(const Json& value, AssetType type, std::string& error);
    // Its path under the asset directory, as the registry has it.
    std::string AssetPath(AssetHandle handle);
    // A path under the asset directory from what a caller wrote: relative, inside it (no "..", not absolute); empty and `error` when not.
    std::filesystem::path AssetRelativePath(const std::string& text, std::string& error);
}
