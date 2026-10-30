#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Nox
{
    class Entity;
    class Scene;

    // glTF material variants (KHR_materials_variants; three.js / Babylon's selectVariant, a Variant Manager switch in Unreal): one
    // model, several named material sets. Selecting a variant writes the entity's MaterialComponent slots, so it is saved, undone,
    // overridden per model-instance node and drawn like any material override. Each function covers the entity and everything
    // below it (a variant is the whole file's: a model instance's root selects it for all its nodes). Main thread; the meshes
    // must be loaded.
    namespace MaterialVariants
    {
        // The variants of the meshes drawn by the entity and its descendants, by name, each once, in the files' order.
        std::vector<std::string> List(Scene& scene, Entity root);

        // Every slot of those meshes takes the variant's material, or the mesh's own where the variant has none for it (an empty
        // name: the mesh's own everywhere). Other overrides of those slots are replaced. False when no mesh there has the variant.
        bool Select(Scene& scene, Entity root, std::string_view variant);

        // The variant the slots show now: its name, "" for the meshes' own materials, nullopt for anything else (overrides set by
        // hand, or a mix).
        std::optional<std::string> Current(Scene& scene, Entity root);
    }
}
