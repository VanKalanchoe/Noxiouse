#include "NoxCore/Core/Application.h"

#include "RuntimeLayer.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace Nox
{
    // The standalone player (docs/Runtime_And_Packaging_Plan_2026-10.md, R1): the engine without the editor -- no ImGui, no
    // panels, the scene rendered straight to the window. Usage: NoxRuntime <project.nproj>
    Application* CreateApplication(ApplicationCommandLineArgs args)
    {
        ApplicationSpecification specification;
        specification.Name = "NoxRuntime";
        specification.WindowSpec.Width = 1920;
        specification.WindowSpec.Height = 1080;
        specification.CommandLineArgs = args;
        specification.isEditor = false;
        // "--server": a dedicated server (docs/Networking_Plan_2026-11.md, NET0) -- no window, GPU or sound; "--tickrate=N" its main
        // loop's updates a second (60 by default; the simulation's own tick stays Scene::TickRate).
        if (args.HasSwitch("--server"))
        {
            specification.Name = "NoxServer";
            specification.Headless = true;
            for (int index = 1; index < args.Count; ++index)
            {
                const std::string_view argument(args[index]);
                constexpr std::string_view prefix = "--tickrate=";
                if (argument.substr(0, prefix.size()) == prefix)
                {
                    const int rate = std::atoi(std::string(argument.substr(prefix.size())).c_str());
                    if (rate > 0 && rate <= 1000)
                        specification.TickRate = static_cast<uint32_t>(rate);
                }
            }
        }

        auto application = new Application(specification);
        application->PushLayer<RuntimeLayer>();
        return application;
    }
}
