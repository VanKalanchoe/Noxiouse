#pragma once

#include "NoxCore/Core/Layer.h"

namespace Nox
{
    class Renderer;
    
    class ImGuiLayer : public Layer
    {
    public:
        ImGuiLayer(Renderer& renderer);
        ~ImGuiLayer() override;
        
        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;
        void OnImGuiRender() override;
        
        void Begin();
        void End();
        
        void BlockEvents(bool block) { m_BlockEvents = block; }
        
        //dark theme ? 
       
        uint32_t GetActiveWidgetID() const;
        void SetImGuizmoTheme();

    private:
        Renderer& m_renderer;
        bool m_BlockEvents = true;
    };

}