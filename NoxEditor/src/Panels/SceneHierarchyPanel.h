#pragma once
#include <imgui.h>

#include "NoxCore/Core/core.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"

namespace Nox
{
    class SceneHierarchyPanel
    {
    public:
        SceneHierarchyPanel() = default;
        SceneHierarchyPanel(const Ref<Scene>& context);
        
        void SetContext(const Ref<Scene>& context);
        
        void OnImGuiRender();
        
        const std::vector<Entity>& GetSelectedEntities() const { return m_SelectionContexts; };
        Entity GetSelectedEntity() const { return m_SelectionContexts.empty() ? Entity{} : m_SelectionContexts.back(); }
        void SetSelectedEntity(Entity entity);
        
        bool IsSelected(Entity entity) const;
        void ToggleSelectedEntity(Entity entity);
        void ClearSelection();
        void SelectRange(Entity entity);
      
        ImVec2 left;
        bool leftFocused;
        bool leftHovered;
        bool leftPropFocused;
        bool leftPropHovered;
    private:
        template<typename T>
        void DisplayAddComponentEntry(const std::string& entryName);
        
        void DrawEntityNode(Entity entity);
        void DrawComponents(Entity entity);
    private:
        Ref<Scene> m_Context;
        std::vector<Entity> m_SelectionContexts;
        Entity m_SelectionAnchor;

        // Deletes requested while drawing. Destroying entities inside the registry view being
        // iterated invalidates it, so they're applied after the loop, resolved by UUID so an entity
        // already destroyed as a child of another pending delete is skipped.
        std::vector<UUID> m_PendingDestroy;
        bool m_MaterialShowAll = false; // Material component: show every slot instead of just this entity's own submesh
    };
}
