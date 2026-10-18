#include "NoxCore/Core/Application.h"

#include "RuntimeLayer.h"

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

        auto application = new Application(specification);
        application->PushLayer<RuntimeLayer>();
        return application;
    }
}
