#pragma once
#include <string>

#include "McpProtocol.h"
#include "NoxCore/Renderer/DataTypes.h"

// A material (.nmat, MaterialData) as JSON for the MCP tools get_material / create_material / set_material: the fields by their C++
// names (BaseColorFactor, RoughnessFactor, ClearcoatFactor ...), vectors as number lists, Mode as "Opaque" / "Mask" / "Blend", the lobe
// textures as { Path, Set }, the UV channels as a list of four { Source, Offset, Rotation, Scale }. One table drives both directions.
// Plain data (tested in NoxToolsTest).
namespace Nox::Mcp
{
    Json MaterialToJson(const MaterialData& material);
    // Reads every field `value` has into `material` (the others stay); "" or why not (a field of the wrong kind, an unknown alpha mode).
    // Check the value against MaterialToJson(defaults) first (CheckAgainstExample) for unknown fields.
    std::string MaterialFromJson(const Json& value, MaterialData& material);
    // The texture paths a material names, with the field each is in ("BaseColorTexturePath", "ClearcoatTexture/Path").
    std::vector<std::pair<std::string, std::string>> MaterialTexturePaths(const MaterialData& material);
}
