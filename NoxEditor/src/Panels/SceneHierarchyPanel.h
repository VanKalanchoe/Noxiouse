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
        
        Entity GetSelectedEntity() const { return m_SelectionContext; };
        void SetSelectedEntity(Entity entity);
      
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
        Entity m_SelectionContext;
    };
}
