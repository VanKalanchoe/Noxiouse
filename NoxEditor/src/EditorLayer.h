#pragma once
#include "NoxCore/Core/Layer.h"
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
        
    private:
        // Panels
        SceneHierarchyPanel m_SceneHierarchyPanel;
    };
}
