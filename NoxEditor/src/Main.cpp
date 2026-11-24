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

#include "EditorLayer.h"
#include "ProjectManager/ProjectLibrary.h"
#include "ProjectManager/ProjectManagerLayer.h"

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

        // Which project: one on the command line (a .nproj; switches like --vk / --dx12 are not one) opens straight away; else the
        // most recent one when the Project Manager's "Always open the last project" is on (--project-manager, from File > Project
        // Manager..., skips that); else the Project Manager, which hands over to the editor once a project is picked.
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
            application->PushLayer<ProjectManagerLayer>();
        else
            application->PushLayer<EditorLayer>(project);

        return application;
    }
}
