#include "ImGuiLayer.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include "imgui_internal.h"  // For Docking
#include <imgui_impl_sdl3.h>

#include "NoxCore/Renderer/Renderer.h"
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
        
        SetImGuizmoTheme();

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
        
        float fontSize = 18.0f;// *2.0f;
        io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/static/OpenSans-Bold.ttf", fontSize);
        io.FontDefault = io.Fonts->AddFontFromFileTTF("assets/fonts/opensans/static/OpenSans-Regular.ttf", fontSize);
    }

    ImGuiLayer::~ImGuiLayer()
    {
        m_renderer.shutdownImGui();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    void ImGuiLayer::OnEvent(Event& event)
    {
        if (m_BlockEvents)
        {
            ImGuiIO& io = ImGui::GetIO();
            event.Handled |= event.IsInCategory(EventCategoryMouse) & io.WantCaptureMouse;
            event.Handled |= event.IsInCategory(EventCategoryKeyboard) & io.WantCaptureKeyboard;
        }
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
        ImGuizmo::BeginFrame();
    }

    void ImGuiLayer::End()
    {
        m_renderer.endImGui();
        ImGui::Render();
    }

    uint32_t ImGuiLayer::GetActiveWidgetID() const
    {
        return GImGui->ActiveId;
    }
    
    void ImGuiLayer::SetImGuizmoTheme()
    {
        ImGuizmo::Style& style = ImGuizmo::GetStyle();
        
        ImGuizmo::SetGizmoSizeClipSpace(0.15f);
        
        // const float screenRotateSize = 0.04f;
        
        style.TranslationLineThickness   = 6.0f; // lineThickness = 6.0f
        style.TranslationLineArrowSize   = 12.0f; // arrowSize = 12.0f
    
        style.RotationLineThickness      = 6.0f; // circleLineThickness = 6.0f
        style.RotationOuterLineThickness = 6.0f;

        style.ScaleLineThickness         = 6.0f; // lineThickness = 6.0f
        style.ScaleLineCircleSize        = 12.0f; // circleSize = 12.0f
        
        style.Colors[ImGuizmo::DIRECTION_X] = ImGui::ColorConvertU32ToFloat4(0xFF715ED8);
        style.Colors[ImGuizmo::DIRECTION_Y] = ImGui::ColorConvertU32ToFloat4(0xFF25AA25);
        style.Colors[ImGuizmo::DIRECTION_Z] = ImGui::ColorConvertU32ToFloat4(0xFFCC532C);
        
        style.Colors[ImGuizmo::PLANE_X] = ImGui::ColorConvertU32ToFloat4(0xFF7A68D8);
        style.Colors[ImGuizmo::PLANE_Y] = ImGui::ColorConvertU32ToFloat4(0xFF55AB55);
        style.Colors[ImGuizmo::PLANE_Z] = ImGui::ColorConvertU32ToFloat4(0xFFD96742);
    
        style.Colors[ImGuizmo::SELECTION]   = ImGui::ColorConvertU32ToFloat4(0xFF20AACC);
    }
}
