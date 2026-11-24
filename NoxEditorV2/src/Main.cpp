#include "NoxCore/Core/Application.h"

#include <cstdint>
#include <filesystem>

#if defined(_WIN32)
// Export D3D12 Agility SDK 1.619 parameters from the executable so Windows loads AgilitySDK\D3D12Core.dll
// and d3d12SDKLayers.dll (providing ID3D12Device15 required by NVIDIA NRI/NRD).
extern "C" {
    __declspec(dllexport) extern const uint32_t D3D12SDKVersion = 619;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\AgilitySDK\\";
}
#endif

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
