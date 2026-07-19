#include "EditorLayer.h"

#include <imgui.h>

#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Input.h"
#include "NoxCore/Events/InputEvents.h"
#include "NoxCore/ImGui/ImGuiLayer.h"

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
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>([this](KeyPressedEvent& e) { return OnKeyPressed(e); });
        dispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent& e) { return OnMouseButtonPressed(e); });
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
        
        m_SceneHierarchyPanel.OnImGuiRender();
    }
    
    bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
    {
        // Shortcuts
        if (e.IsRepeat())
        {
            return false;
        }

        bool control = (Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL));
        bool shift = (Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT));

        switch (e.GetKeyCode())
        {
        case SDL_SCANCODE_N:
            {
                if (control)
                {
                    /*NewScene();*/
                }
                break;
            }
        case SDL_SCANCODE_O:
            {
                if (control)
                {
                    /*OpenProject();*/
                }
                break;
            }
        case SDL_SCANCODE_S:
            {
                if (control)
                {
                    /*if (shift)
                        SaveSceneAs();
                    else
                        SaveScene();*/
                }
                break;
            }

        // Scene Commands
        case SDL_SCANCODE_D:
            {
                if (control)
                {
                    /*OnDuplicateEntity();*/
                }
                break;
            }


        /*// Gizmos
        case SDL_SCANCODE_Q:
            m_GizmoType = -1;
            break;
        case SDL_SCANCODE_W:
            m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
            break;
        case SDL_SCANCODE_E:
            m_GizmoType = ImGuizmo::OPERATION::ROTATE;
            break;
        case SDL_SCANCODE_R:
            if (control)
            {
                /*ScriptEngine::ReloadAssembly();#1#
            }
            else
            {
                m_GizmoType = ImGuizmo::OPERATION::SCALE;
            }
            break;*/
        case SDL_SCANCODE_DELETE:
            {
                if (Application::Get().GetLayer<ImGuiLayer>()->GetActiveWidgetID() == 0)
                {
                    Entity selectedEntity = m_SceneHierarchyPanel.GetSelectedEntity();
                    if (selectedEntity)
                    {
                        m_SceneHierarchyPanel.SetSelectedEntity({});
                        /*m_ActiveScene->DestroyEntity(selectedEntity);*/
                    }
                }
                break;
            }
        default:
            break;
        }

        return false;
    }

    bool EditorLayer::IsButtonHovered() const
    {
        return true; //maybe useful for imgui ImGui::IsItemHovered()
    }

    bool EditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent& event)
    {
        if (event.GetMouseButton() == SDL_BUTTON_LEFT)
        {
            /*if (m_ViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(SDL_SCANCODE_LALT))
            {
                m_SceneHierarchyPanel.SetSelectedEntity(m_HoveredEntity);
            }*/
        }

        return false;
    }
}
