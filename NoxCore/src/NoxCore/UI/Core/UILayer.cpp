#include "UILayer.h"

#include <SDL3/SDL_keyboard.h>

#include "NoxCore/Core/Window.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Core/UIViewportManager.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"

namespace Nox
{
    UILayer::UILayer(Window& window, Renderer& renderer, std::string themePath)
        : Layer("UILayer")
        , m_Window(window)
        , m_Renderer(renderer)
        , m_ThemePath(std::move(themePath))
    {
        m_Theme = UI::UITheme::CreateDefault();
        if (!m_ThemePath.empty())
        {
            m_Theme->LoadFromFile(m_ThemePath);
            m_Theme->WatchFile(m_Renderer.GetFileWatcher(), m_ThemePath);
        }
        m_UI.SetTheme(m_Theme);

        if (m_Window.getHandle())
            SDL_StartTextInput(m_Window.getHandle());

        SetConfigFlags(m_ConfigFlags);
    }

    UILayer::~UILayer()
    {
        if (m_ViewportManager)
            m_ViewportManager->Shutdown();
        m_ViewportManager.reset();
    }

    void UILayer::SetConfigFlags(UI::UIConfigFlags flags)
    {
        m_ConfigFlags = flags;
        if (UI::HasFlag(m_ConfigFlags, UI::UIConfigFlags::DockingEnable))
        {
            if (!m_DockSpace)
            {
                m_DockSpace = Ref<UI::UIDockSpace>(new UI::UIDockSpace());
                if (!m_UI.GetRoot())
                    m_UI.SetRoot(m_DockSpace);
            }
            AttachDockSpace(m_DockSpace.get());
        }
        else
        {
            if (m_ViewportManager)
                m_ViewportManager->Shutdown();
            m_ViewportManager.reset();
            if (m_DockSpace && m_UI.GetRoot() == m_DockSpace)
                m_UI.SetRoot(nullptr);
            m_DockSpace = nullptr;
        }
    }

    void UILayer::SetRoot(const Ref<UI::UIElement>& root)
    {
        m_UI.SetRoot(root);
    }

    void UILayer::AttachDockSpace(UI::UIDockSpace* dockSpace)
    {
        if (m_ViewportManager)
            m_ViewportManager->Shutdown();
        m_ViewportManager.reset();

        if (!dockSpace)
            return;

        dockSpace->SetContext(&m_UI);
        if (UI::HasFlag(m_ConfigFlags, UI::UIConfigFlags::ViewportsEnable) && m_Window.getHandle())
        {
            m_ViewportManager = std::make_unique<UI::UIViewportManager>(
                m_Window,
                m_Renderer,
                m_UI,
                dockSpace
            );
            dockSpace->SetViewportManager(m_ViewportManager.get());
        }
        else
        {
            dockSpace->SetViewportManager(nullptr);
        }
    }

    Ref<UI::UIDockNode> UILayer::AddPanel(
        const std::string& title,
        const Ref<UI::UIElement>& content,
        UI::UIDockDropZone zone,
        const std::string& targetTitle,
        float ratio,
        UI::UIDockNodeFlags flags)
    {
        if (!m_DockSpace)
            SetConfigFlags(m_ConfigFlags | UI::UIConfigFlags::DockingEnable);
        return m_DockSpace ? m_DockSpace->AddPanel(title, content, zone, targetTitle, ratio, flags) : nullptr;
    }

    void UILayer::ReloadTheme(const std::string& themePath)
    {
        if (!m_Theme)
            return;
        const std::string& path = themePath.empty() ? m_ThemePath : themePath;
        if (!path.empty())
            m_Theme->LoadFromFile(path);
    }

    void UILayer::OnEvent(Event& event)
    {
        if (m_BlockEvents)
            return;
        m_UI.OnEvent(event);
    }

    void UILayer::OnUpdate(Timestep ts)
    {
        m_LastDeltaSeconds = ts.GetSeconds();
    }

    void UILayer::OnRender()
    {
        UI::UIDrawList* draw = m_Renderer.GetUIDrawList();
        if (!draw)
            return;

        int width = 1920;
        int height = 1080;
        m_Window.getSizeInPixels(width, height);
        m_UI.UpdateAndRender(*draw, { static_cast<float>(width), static_cast<float>(height) }, m_LastDeltaSeconds);
    }
}
