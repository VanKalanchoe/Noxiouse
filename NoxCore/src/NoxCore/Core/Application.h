#pragma once
#include <functional>
#include <vector>
#include <string>
#include <string_view>

#include "Log.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/Event.h"

namespace Nox
{
    class Renderer;
    class ImGuiLayer;
    class JobSystem;
    class Application;

    // The command line: switches start with "--" and may come anywhere (--vk, --dx12, --windowed); everything else is positional (the
    // project file). Code asks for a switch or for the first positional argument -- never for args[1], which may be a switch.
    struct ApplicationCommandLineArgs
    {
        int Count = 0;
        char** Args = nullptr;

        const char* operator[](int index) const
        {
            /*VK_CORE_ASSERT(index < Count, "index out of range");*/
            return Args[index];
        }

        static bool IsSwitch(std::string_view argument) { return argument.size() > 2 && argument.substr(0, 2) == "--"; }
        bool HasSwitch(std::string_view name) const
        {
            for (int index = 1; index < Count; ++index)
                if (std::string_view(Args[index]) == name)
                    return true;
            return false;
        }
        // The first argument that is not a switch (the project file), or null.
        const char* FirstPositional() const
        {
            for (int index = 1; index < Count; ++index)
                if (!IsSwitch(Args[index]))
                    return Args[index];
            return nullptr;
        }
        // The graphics API asked for: --dx12 / --d3d12 (Windows) or --vk / --vulkan; the last one given wins; Vulkan by default.
        bool WantsD3D12() const
        {
            bool d3d12 = false;
            for (int index = 1; index < Count; ++index)
            {
                const std::string_view argument(Args[index]);
                if (argument == "--dx12" || argument == "--d3d12")
                    d3d12 = true;
                else if (argument == "--vk" || argument == "--vulkan")
                    d3d12 = false;
            }
            return d3d12;
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
    };

    class Application
    {
    public:
        Application(ApplicationSpecification specification = ApplicationSpecification());
        ~Application();

        void Run(AppState& applicationState);
        static void Shutdown();
        
        Renderer* GetRenderer() { return renderer.get(); }

        void RaiseEvent(Event& event);

        /*template<typename TLayer>
        requires(std::is_base_of_v<Layer, TLayer>)
        void PushLayer()
        {
            m_LayerStack.push_back(std::make_unique<TLayer>());    
        }*/

        // Runs `action` once this frame is done (after it is drawn): for what must not happen while the layers are being walked, such
        // as replacing one layer by another (the Project Manager handing over to the editor).
        void QueueAfterFrame(std::function<void()> action) { m_AfterFrame.push_back(std::move(action)); }

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

        static double GetTime();
        static std::string GetExecutableRootPath();

        void setBlockEvents(bool block) { m_BlockEvents = block; }
        bool getBlockEvents() { return m_BlockEvents; }
    private:
        // First member: created before and destroyed after everything that schedules tasks.
        std::unique_ptr<JobSystem> m_JobSystem;
        ApplicationSpecification m_Specification;
        bool m_BlockEvents = false;
        std::shared_ptr<Window> m_Window;
        ImGuiLayer* m_ImGuiLayer = nullptr;
        /* We will use this renderer to draw into this window every frame. */
        std::unique_ptr<Renderer> renderer;
        // has to be last otherwise renderer cant free resoruces
        std::vector<std::unique_ptr<Layer>> m_LayerStack;
        std::vector<std::function<void()>> m_AfterFrame;
        double m_LastFrameTime = 0.0;
    };

    extern Application* CreateApplication(ApplicationCommandLineArgs args);
}
