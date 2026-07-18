#include "ImGuiLayer.h"

#include <imgui.h>
#include "imgui_internal.h"  // For Docking
#include <imgui_impl_sdl3.h>

#include <SDL3/SDL_video.h>

#include "NoxCore/Renderer.h"
#include "NoxCore/Core/Application.h"

namespace Nox
{
    ImGuiLayer::ImGuiLayer(Renderer& renderer) : Layer("ImGuiLayer"), m_renderer(renderer)
    {
        float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
        //io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows broken right now
        //io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoTaskBarIcons;
        //io.ConfigFlags |= ImGuiConfigFlags_ViewportsNoMerge;

        // Setup Dear ImGui style
        ImGui::StyleColorsDark();
        //ImGui::StyleColorsLight();

        // Setup scaling
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(main_scale); // Bake a fixed style scale. (until we have a solution for dynamic style scaling, changing this requires resetting Style + calling this again)
        style.FontScaleDpi = main_scale;
        // Set initial font scale. (in docking branch: using io.ConfigDpiScaleFonts=true automatically overrides this for every window depending on the current monitor)
        io.ConfigDpiScaleFonts = true; // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
        io.ConfigDpiScaleViewports = true; // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }

        m_renderer.initImGui();

        // Load Fonts
        // - If fonts are not explicitly loaded, Dear ImGui will select an embedded font: either AddFontDefaultVector() or AddFontDefaultBitmap().
        //   This selection is based on (style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi) reaching a small threshold.
        // - You can load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
        // - If a file cannot be loaded, AddFont functions will return a nullptr. Please handle those errors in your code (e.g. use an assertion, display an error and quit).
        // - Read 'docs/FONTS.md' for more instructions and details.
        // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use FreeType for higher quality font rendering.
        // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
        //style.FontSizeBase = 20.0f;
        //io.Fonts->AddFontDefaultVector();
        //io.Fonts->AddFontDefaultBitmap();
        //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
        //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf");
        //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf");
        //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf");
        //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf");
        //IM_ASSERT(font != nullptr);
    }

    ImGuiLayer::~ImGuiLayer()
    {
        m_renderer.shutdownImGui();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    void ImGuiLayer::OnUpdate(Timestep ts)
    {
    }

    void ImGuiLayer::OnRender()
    {
    }

    void ImGuiLayer::OnImGuiRender()
    {
    }

    void ImGuiLayer::Begin()
    {
        m_renderer.beginImGui();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        
        /*--
       * IMGUI Docking
       * Create a dockspace and dock the viewport and settings window.
       * The central node is named "Viewport", which can be used later with Begin("Viewport")
       * to render the final image.
      -*/
        
        const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
        
        // 1. Grab the style and save the default minimum size
        ImGuiStyle& style = ImGui::GetStyle();
        float minWinSizeX = style.WindowMinSize.x;
        float minWinSizeY = style.WindowMinSize.y;
        
        // 2. Enforce the new minimum size globally for the DockSpace
        style.WindowMinSize.x = 370.0f;
        style.WindowMinSize.y = 370.0f;
        
        // 3. Submit the DockSpace (It will inherit the 370x350 constraint)
        ImGuiID dockID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);
        
        // 4. Restore the original minimum size for standard floating windows
        style.WindowMinSize.x = minWinSizeX;
        style.WindowMinSize.y = minWinSizeY;
        
        // Docking layout, must be done only if it doesn't exist
        if(!ImGui::DockBuilderGetNode(dockID)->IsSplitNode() && !ImGui::FindWindowByName("Viewport"))
        {
            ImGui::DockBuilderDockWindow("Viewport", dockID);  // Dock "Viewport" to  central node
            ImGui::DockBuilderGetCentralNode(dockID)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;  // Remove "Tab" from the central node
            ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID);  // Split the central node
            ImGui::DockBuilderDockWindow("Settings", leftID);  // Dock "Settings" to the left node
        }
        
        // [optional] Show the menu bar
        if(ImGui::BeginMainMenuBar())
        {
            if(ImGui::BeginMenu("File"))
            {
                auto& app = Application::Get();
                if (auto* renderer = app.GetRenderer())
                {
                    bool currentVSync = renderer->getVSync();
                    if(ImGui::MenuItem("vSync", "", &currentVSync))
                        renderer->setVSync(currentVSync); // Recreate the swapchain with the new vSync setting
                }
                ImGui::Separator();
                if(ImGui::MenuItem("Exit"))
                    Application::Shutdown();
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        
        /* END Docking */
        
        // We define "viewport" with no padding an retrieve the rendering area
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Viewport");
        ImVec2 windowSize = ImGui::GetContentRegionAvail();
        ImGui::End();
        ImGui::PopStyleVar();

        // Verify if the viewport has a new size and resize the RenderTarget accordingly.
        auto& app = Application::Get();
        if (auto* renderer = app.GetRenderer())
        {
            NRI::Extent2D m_viewportSize = renderer->getViewPortSize();
            const NRI::Extent2D viewportSize = {static_cast<uint32_t>(windowSize.x), static_cast<uint32_t>(windowSize.y)};
            if(m_viewportSize.width != viewportSize.width || m_viewportSize.height != viewportSize.height)
            {
                renderer->onViewportSizeChange(viewportSize);
            }
        }
        
        // Extra ImGui windows can be added in OnImGuiRender() layer, like the demo window.
        // ImGui::ShowDemoWindow();
    }

    void ImGuiLayer::End()
    {
        m_renderer.endImGui();
    }

    uint32_t ImGuiLayer::GetActiveWidgetID() const
    {
        return GImGui->ActiveId;
    }
}
