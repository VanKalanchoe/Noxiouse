#pragma once

#include <algorithm>
#include <memory>
#include <utility>

#include "NoxCore/Events/Event.h"
#include "Timestep.h"

namespace Nox
{
    class Layer
    {
    public:
        Layer(const std::string& name = "Layer");
        virtual ~Layer() = default;

        virtual void OnEvent(Event& event) {}

        virtual void OnUpdate(Timestep ts) {}
        virtual void OnRender() {}
        virtual void OnImGuiRender() {}
        
        const std::string& GetName() const { return m_Name; }

        template<std::derived_from<Layer> T, typename ... Args>
        void TransitionTo(Args&& ... args)
        {
            QueueTransition(std::move(std::make_unique<T>(std::forward<Args>(args)...)));
        }
        
    protected:
        std::string m_Name;
        
    private:
        void QueueTransition(std::unique_ptr<Layer> layer);
        Layer() = default;
    };
}