#include "NoxCore/Core/Application.h"

#include <filesystem>

#include "EditorV2Layer.h"
#include "ProjectManager/ProjectLibrary.h"
#include "ProjectManager/ProjectManagerV2Layer.h"

namespace Nox
{
    Application* CreateApplication(ApplicationCommandLineArgs args)
    {
        ApplicationSpecification spec;
        spec.Name = "NoxEditorV2";
        spec.WindowSpec.Width = 1920;
        spec.WindowSpec.Height = 1080;
        spec.CommandLineArgs = args;
        spec.isEditor = true;
        spec.UseImGui = false;

        auto* application = new Application(spec);
        std::filesystem::path project;
        if (const char* projectFile = args.ProjectFile())
            project = projectFile;
        else if (!args.HasSwitch("--project-manager") && ProjectLibrary::GetOpenLastProjectAtStartup())
        {
            const std::vector<std::filesystem::path> recent = ProjectLibrary::LoadRecentProjects();
            std::error_code error;
            if (!recent.empty() && std::filesystem::exists(recent.front(), error))
                project = recent.front();
        }

        if (project.empty())
            application->PushLayer<ProjectManagerV2Layer>();
        else
            application->PushLayer<EditorV2Layer>(project);
        return application;
    }
}
