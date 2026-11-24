#pragma once

#include <cstdint>
#include <vector>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Scene/Entity.h"

namespace Nox
{
    class Scene;

    // The entities selected in the editor -- one service the hierarchy, the inspector, the viewports, the gizmo and the shortcuts
    // read and change (Unreal's USelection), instead of a list inside the hierarchy panel that everything else asks.
    //
    // It holds UUIDs, not entity handles: a scene put back by undo / redo or copied for Play has new handles for the same entities,
    // and the selection carries over to it (SetScene). Ids the scene does not have (deleted, another scene opened) are dropped.
    // Order matters: the last one is the primary (the gizmo's pivot, the inspector's entity). Locked entities are not selected.
    //
    // Readers notice a change by GetVersion() (it grows with every change and with SetScene: the handles changed then).
    class EditorSelection
    {
    public:
        // The scene the ids refer to (null: none). Keeps the ids that scene has.
        void SetScene(const Ref<Scene>& scene);
        const Ref<Scene>& GetScene() const { return m_Scene; }

        // Replace the selection with `entity` (a null or locked entity clears it).
        void Select(Entity entity);
        // Replace it with these, in order (locked ones left out).
        void Set(const std::vector<Entity>& entities);
        void Add(Entity entity);
        // In: out; out: in (and primary).
        void Toggle(Entity entity);
        void Remove(UUID id);
        void Clear();
        // Drops ids the scene no longer has (the editor calls it once a frame; entities are destroyed in many places).
        void Prune();

        bool IsEmpty() const { return m_Ids.empty(); }
        size_t Count() const { return m_Ids.size(); }
        bool Contains(UUID id) const;
        bool Contains(Entity entity) const;
        const std::vector<UUID>& GetIds() const { return m_Ids; }
        // The selected entities of the scene, in selection order (resolved once per change).
        const std::vector<Entity>& GetEntities() const;
        // The last selected (null when empty).
        Entity GetPrimary() const;
        uint64_t GetVersion() const { return m_Version; }

    private:
        bool Selectable(Entity entity) const;
        void Changed();

        Ref<Scene> m_Scene;
        std::vector<UUID> m_Ids;
        uint64_t m_Version = 1;
        mutable std::vector<Entity> m_Entities;
        mutable uint64_t m_ResolvedVersion = 0;
    };
}
