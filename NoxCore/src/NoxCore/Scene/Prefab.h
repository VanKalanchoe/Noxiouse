#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "NoxCore/Asset/Asset.h"
#include "Components.h"

namespace YAML
{
    class Node;
}

namespace Nox
{
    // A prefab asset (.nprefab): a saved recipe of entities. It is a scene-format file (the same entity/component YAML), kept
    // as parsed nodes; PrefabInstance::SpawnPending builds the entities of every instance from it.
    class Prefab : public Asset
    {
    public:
        static AssetType GetStaticType() { return AssetType::Prefab; }
        virtual AssetType GetType() const override { return GetStaticType(); }

        std::string Name;
        // The file's id of the root entity (ids are local to the file; spawned entities get ids derived from the instance).
        uint64_t Root = 0;
        // The file's "Entities" sequence. For a variant: only the entities the variant adds to its base (their parent ids may be the
        // base's).
        std::shared_ptr<const YAML::Node> Entities;

        // A variant (P6c) is a prefab based on another: it holds the changes to the base (the same kinds an instance holds) and the
        // entities it adds. The full content is worked out from the base when it is used (PrefabInstance, effectiveNodes).
        AssetHandle Base = 0;
        std::vector<PrefabPropertyOverride> Overrides;
        std::vector<PrefabStructureChange> Structure;
        std::vector<PrefabNestedChange> Nested;
    };
}
