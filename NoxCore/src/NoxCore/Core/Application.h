#pragma once
#include <string>

#include "Log.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/Event.h"

namespace Nox
{
    class Renderer;
    class ImGuiLayer;
    class Application;

    struct ApplicationCommandLineArgs
    {
        int Count = 0;
        char** Args = nullptr;

        const char* operator[](int index) const
        {
            /*VK_CORE_ASSERT(index < Count, "index out of range");*/
            return Args[index];
        }
    };

    struct ApplicationSpecification
    {
        std::string Name = "Application";
        std::string WorkingDirectory;
        //app args ApplicationCommandLineArgs CommandLineArgs;
        ApplicationCommandLineArgs CommandLineArgs;
        WindowSpecification WindowSpec;
        bool isEditor = false;

        using EventCallbackFn = std::function<void(Event&)>;
        EventCallbackFn EventCallback;
    };

    struct AppState
    {
        Application* app;
        float lastTime;
    };

    class Application
    {
    public:
        Application(ApplicationSpecification specification = ApplicationSpecification());
        ~Application();

        void Run(const AppState& applicationState) const;
        static void Shutdown();
        
        Renderer* GetRenderer() { return renderer.get(); }

        void RaiseEvent(Event& event);

        /*template<typename TLayer>
        requires(std::is_base_of_v<Layer, TLayer>)
        void PushLayer()
        {
            m_LayerStack.push_back(std::make_unique<TLayer>());    
        }*/

        template <typename TLayer, typename... Args>
        requires(std::is_base_of_v<Layer, TLayer>)
        TLayer* PushLayer(Args&&... args)
        {
            // Create the layer, passing arguments (like the renderer) to the constructor
            auto layer = std::make_unique<TLayer>(std::forward<Args>(args)...);
            TLayer* rawPtr = layer.get();
            m_LayerStack.push_back(std::move(layer));
            return rawPtr;
        }

        template <typename TLayer>
            requires(std::is_base_of_v<Layer, TLayer>)
        void PopLayer()
        {
            auto it = std::find_if(m_LayerStack.begin(), m_LayerStack.end(), [](const std::unique_ptr<Layer>& layer)
            {
                return dynamic_cast<TLayer*>(layer.get()) != nullptr;
            });

            if (it != m_LayerStack.end())
                m_LayerStack.erase(it); // layer is destroyed here
        }

        template <typename TLayer>
            requires(std::is_base_of_v<Layer, TLayer>)
        TLayer* GetLayer()
        {
            for (const auto& layer : m_LayerStack)
            {
                if (auto casted = dynamic_cast<TLayer*>(layer.get()))
                    return casted;
            }
            return nullptr;
        }

        static Application& Get();
        const ApplicationSpecification& GetSpecification() const { return m_Specification; }
        std::shared_ptr<Window> getWindow() { return m_Window; }
        ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

        static float GetTime();
        static std::string GetExecutableRootPath();

        void setBlockEvents(bool block) { m_BlockEvents = block; }
        bool getBlockEvents() { return m_BlockEvents; }
    private:
        ApplicationSpecification m_Specification;
        bool m_BlockEvents = false;
        std::shared_ptr<Window> m_Window;
        ImGuiLayer* m_ImGuiLayer = nullptr;
        /* We will use this renderer to draw into this window every frame. */
        std::unique_ptr<Renderer> renderer;
        // has to be last otherwise renderer cant free resoruces
        std::vector<std::unique_ptr<Layer>> m_LayerStack;
    };

    extern Application* CreateApplication(ApplicationCommandLineArgs args);
}
