#include "NoxCore/Core/Application.h"

#include "EditorLayer.h"

namespace Nox
{
    Application* CreateApplication(ApplicationCommandLineArgs args)
    {
        ApplicationSpecification EditorLayerSpec;
        EditorLayerSpec.Name = "NoxEditor";
        EditorLayerSpec.WindowSpec.Width = 1920;
        EditorLayerSpec.WindowSpec.Height = 1080;
        EditorLayerSpec.CommandLineArgs = args;
        EditorLayerSpec.isEditor = true;
        
        auto application = new Application(EditorLayerSpec);
        application->PushLayer<EditorLayer>();
        
        return application;
    }
}
