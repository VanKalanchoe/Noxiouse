#include "EditorLayer.h"

#include <imgui.h>

#include "NoxCore/Renderer.h"
#include "NoxCore/Core/Application.h"

namespace Nox
{
    EditorLayer::EditorLayer() : Layer("EditorLayer")
    {
    }

    EditorLayer::~EditorLayer()
    {
    }

    void EditorLayer::OnEvent(Event& event)
    {
    }

    void EditorLayer::OnUpdate(Timestep ts)
    {
    }

    void EditorLayer::OnRender()
    {
    }

    void EditorLayer::OnImGuiRender()
    {
        /*-- 
         * The rendering of the scene is done using dynamic rendering into the RenderTarget (see recordGraphicsCommands).
         * The target image will be rendered/displayed using ImGui.
         * Its placement will cover the entire viewport (ImGui draws a quad with the texture we provide),
         * and the image will be displayed in the viewport.
         * There are multiple ways to display the image, but this method is the most flexible.
         * Other methods include:
         *  - Blitting the image to the swapchain image, with the UI drawn on top. However, this makes it harder 
         *    to fit the image within a specific area of the window.
         *  - Using the image as a texture in a quad and rendering it to the swapchain image. This is what ImGui 
         *    does, but we don't need to add a quad to the scene, as ImGui handles it for us.
        -*/
        // Using the dock "Viewport", this sets the window to cover the entire central viewport
        if (ImGui::Begin("Viewport"))
        {
            auto& app = Application::Get();
            auto* renderer = app.GetRenderer();
            if (renderer)
            {
                auto* texture = renderer->GetSceneResource();
                if (texture)
                {
                    auto textureID = texture->getImTextureID();
                    if (textureID)
                    {
                        // !!! This is where the RenderTarget image is displayed !!!
                        ImGui::Image(textureID, ImGui::GetContentRegionAvail());
                    }
                    // Adding overlay text on the upper left corner
                    ImGui::SetCursorPos(ImVec2(0, 0));
                    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
                }
            }
            ImGui::End();
        }
    }
}
