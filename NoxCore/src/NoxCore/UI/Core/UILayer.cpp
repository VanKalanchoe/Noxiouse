#include "UILayer.h"

#include <SDL3/SDL_keyboard.h>
#include <NoxUI/Backends/noxui_impl_sdl3.h>
#include <NoxUI/Core/UILog.h>
#include <NoxUI/Rendering/UIFont.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/Window.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include <NoxUI/Core/UIElement.h>
#include <NoxUI/Core/UIViewportManager.h>
#include <NoxUI/Layout/UIDockSpace.h>

namespace Nox
{
    UILayer::UILayer(Window& window, Renderer& renderer, std::string themePath)
        : Layer("UILayer")
        , m_Window(window)
        , m_Renderer(renderer)
        , m_ThemePath(std::move(themePath))
    {
        NoxUI::SetLogCallback([](NoxUI::LogLevel level, std::string_view message)
        {
            switch (level)
            {
            case NoxUI::LogLevel::Info:  NOX_CORE_INFO("{}", message); break;
            case NoxUI::LogLevel::Warn:  NOX_CORE_WARN("{}", message); break;
            case NoxUI::LogLevel::Error: NOX_CORE_ERROR("{}", message); break;
            }
        });

        NoxUI::UIFont::SetDefaultFontPath("assets/fonts/opensans/static/OpenSans-Regular.ttf");

        m_Theme = NoxUI::UITheme::CreateDefault();
        if (!m_ThemePath.empty())
        {
            m_Theme->LoadFromFile(m_ThemePath);
            m_Theme->SetWatchedFile(m_ThemePath);
            m_Renderer.GetFileWatcher().watch(std::filesystem::path(m_ThemePath),
                [theme = m_Theme](const std::filesystem::path&)
                {
                    if (theme)
                        theme->RequestHotReload();
                });
        }
        m_UI.SetTheme(m_Theme);

        if (m_Window.getHandle())
        {
            NoxUI_ImplSDL3_Init(m_Window.getHandle(), m_UI, nullptr);
            SDL_StartTextInput(m_Window.getHandle());
        }

        SetConfigFlags(m_ConfigFlags);
    }

    UILayer::~UILayer()
    {
        if (m_ViewportManager)
            m_ViewportManager->Shutdown();
        m_ViewportManager.reset();
        m_Renderer.ShutdownNoxUI();
        NoxUI_ImplSDL3_Shutdown();
    }

    void UILayer::SetConfigFlags(UIConfigFlags flags)
    {
        m_ConfigFlags = flags;
        if (HasFlag(m_ConfigFlags, UIConfigFlags::DockingEnable))
        {
            if (!m_DockSpace)
            {
                m_DockSpace = Ref<NoxUI::UIDockSpace>(new NoxUI::UIDockSpace());
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

    void UILayer::SetRoot(const Ref<NoxUI::UIElement>& root)
    {
        m_UI.SetRoot(root);
    }

    void UILayer::AttachDockSpace(NoxUI::UIDockSpace* dockSpace)
    {
        if (m_ViewportManager)
            m_ViewportManager->Shutdown();
        m_ViewportManager.reset();

        if (!dockSpace)
            return;

        dockSpace->SetContext(&m_UI);
        if (HasFlag(m_ConfigFlags, UIConfigFlags::ViewportsEnable) && m_Window.getHandle())
        {
            m_ViewportManager = std::make_unique<NoxUI::UIViewportManager>(m_UI, dockSpace);
            NoxUI_ImplSDL3_Init(m_Window.getHandle(), m_UI, m_ViewportManager.get());
            m_Renderer.InitNoxUI(m_ViewportManager.get());
            dockSpace->SetViewportManager(m_ViewportManager.get());
        }
        else
        {
            m_Renderer.InitNoxUI(nullptr);
            dockSpace->SetViewportManager(nullptr);
        }
    }

    Ref<NoxUI::UIDockNode> UILayer::AddPanel(
        const std::string& title,
        const Ref<NoxUI::UIElement>& content,
        NoxUI::UIDockDropZone zone,
        const std::string& targetTitle,
        float ratio,
        NoxUI::UIDockNodeFlags flags)
    {
        if (!m_DockSpace)
            SetConfigFlags(m_ConfigFlags | UIConfigFlags::DockingEnable);
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

    void UILayer::OnEvent(Event& /*event*/)
    {
        // Raw SDL3 events are dispatched directly via NoxUI_ImplSDL3_ProcessEvent in Application.cpp.
    }

    void UILayer::OnUpdate(Timestep ts)
    {
        m_LastDeltaSeconds = ts.GetSeconds();
        if (m_Theme)
            m_Theme->PollHotReload();
        NoxUI_ImplSDL3_NewFrame(m_UI, m_ViewportManager.get());
    }

    void UILayer::OnRender()
    {
        NoxUI::UIDrawList* draw = m_Renderer.GetUIDrawList();
        if (!draw)
            return;

        int width = 1920;
        int height = 1080;
        m_Window.getSizeInPixels(width, height);
        m_UI.UpdateAndRender(*draw, { static_cast<float>(width), static_cast<float>(height) }, m_LastDeltaSeconds);
    }
}
