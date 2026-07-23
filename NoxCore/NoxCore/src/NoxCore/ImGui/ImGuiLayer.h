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
        
        void OnUpdate(Timestep ts) override;
        void OnRender() override;
        void OnImGuiRender() override;
        
        void Begin();
        void End();
        
        //dark theme ? 
        
        uint32_t GetActiveWidgetID() const;
        
    private:
        Renderer& m_renderer;
    };

}