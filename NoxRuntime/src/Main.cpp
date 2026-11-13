#include "NoxCore/Core/Application.h"

#include "RuntimeLayer.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace Nox
{
    // The standalone player (docs/Runtime_And_Packaging_Plan_2026-10.md, R1): the engine without the editor -- no ImGui, no
    // panels, the scene rendered straight to the window. Usage: NoxRuntime [switches] [--project=]<project.nproj>
    // Networking (docs/Networking_Plan_2026-11.md, NET1; RuntimeLayer::ParseNetworkOptions):
    //   --server                 a dedicated server: no window, GPU or sound, listening for clients (--port=7777)
    //   --listen                 a listen server: plays in a window and accepts clients too
    //   --connect=<address>      a client: joins the server at 192.168.1.5:7777 / myserver.lan / loopback
    //   --tickrate=N             (server) main loop updates a second (60; the simulation's tick stays Scene::TickRate)
    // A value follows its switch with '=' or as the next argument ("--connect localhost:7777": Rider's argument presets);
    // ApplicationCommandLineArgs::TakesValue lists the switches that take one.
    Application* CreateApplication(ApplicationCommandLineArgs args)
    {
        ApplicationSpecification specification;
        specification.Name = "NoxRuntime";
        specification.WindowSpec.Width = 1920;
        specification.WindowSpec.Height = 1080;
        specification.CommandLineArgs = args;
        specification.isEditor = false;
        if (args.HasSwitch("--server"))
        {
            specification.Name = "NoxServer";
            specification.Headless = true;
            if (const char* rate = args.Value("--tickrate"))
            {
                const int value = std::atoi(rate);
                if (value > 0 && value <= 1000)
                    specification.TickRate = static_cast<uint32_t>(value);
            }
        }

        auto application = new Application(specification);
        application->PushLayer<RuntimeLayer>();
        return application;
    }
}
