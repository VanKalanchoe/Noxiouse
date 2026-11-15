#include "NoxCore/Core/Application.h"

#include "EditorV2Layer.h"

namespace Nox
{
    Application* CreateApplication(ApplicationCommandLineArgs args)
    {
        ApplicationSpecification spec;
        spec.Name = "NoxEditorV2 (Custom UI)";
        spec.WindowSpec.Width = 1920;
        spec.WindowSpec.Height = 1080;
        spec.CommandLineArgs = args;
        spec.isEditor = true;
        spec.UseImGui = false;

        auto* application = new Application(spec);
        application->PushLayer<EditorV2Layer>();
        return application;
    }
}
