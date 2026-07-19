#pragma once
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "Panels/SceneHierarchyPanel.h"

namespace Nox
{
    class EditorLayer : public Layer
    {
    public:
        EditorLayer();
        ~EditorLayer() override;
        
        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;
        void OnImGuiRender() override;
        bool OnKeyPressed(KeyPressedEvent& e);
        bool IsButtonHovered() const;
        bool OnMouseButtonPressed(MouseButtonPressedEvent& event);

    private:
        Ref<Scene> m_ActiveScene;
        
        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
    };
}
