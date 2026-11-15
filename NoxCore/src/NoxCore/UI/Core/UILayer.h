#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UIDockSpace.h"

namespace Nox
{
    class Renderer;
    class Window;

    namespace UI
    {
        class UIElement;
        class UIViewportManager;

        enum class UIConfigFlags : uint32_t
        {
            None            = 0,
            DockingEnable   = 1 << 0,
            ViewportsEnable = 1 << 1
        };

        inline UIConfigFlags operator|(UIConfigFlags a, UIConfigFlags b)
        {
            return static_cast<UIConfigFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
        }

        inline bool HasFlag(UIConfigFlags flags, UIConfigFlags test)
        {
            return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
        }
    }

    class UILayer : public Layer
    {
    public:
        UILayer(Window& window, Renderer& renderer, std::string themePath = "assets/themes/DefaultDark.nstyle");
        ~UILayer() override;

        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;

        void SetConfigFlags(UI::UIConfigFlags flags);
        UI::UIConfigFlags GetConfigFlags() const { return m_ConfigFlags; }

        UI::UIContext& GetContext() { return m_UI; }
        const UI::UIContext& GetContext() const { return m_UI; }
        const Ref<UI::UITheme>& GetTheme() const { return m_Theme; }
        UI::UIDockSpace* GetDockSpace() { return m_DockSpace.get(); }
        const Ref<UI::UIDockSpace>& GetDockSpaceRef() const { return m_DockSpace; }
        UI::UIViewportManager* GetViewportManager() { return m_ViewportManager.get(); }

        void SetRoot(const Ref<UI::UIElement>& root);
        void AttachDockSpace(UI::UIDockSpace* dockSpace);
        Ref<UI::UIDockNode> AddPanel(
            const std::string& title,
            const Ref<UI::UIElement>& content,
            UI::UIDockDropZone zone = UI::UIDockDropZone::Center,
            const std::string& targetTitle = {},
            float ratio = 0.5f,
            UI::UIDockNodeFlags flags = UI::UIDockNodeFlags::None);
        void ReloadTheme(const std::string& themePath = {});

        void BlockEvents(bool block) { m_BlockEvents = block; }

    private:
        Window& m_Window;
        Renderer& m_Renderer;
        std::string m_ThemePath;
        UI::UIConfigFlags m_ConfigFlags = UI::UIConfigFlags::DockingEnable | UI::UIConfigFlags::ViewportsEnable;

        UI::UIContext m_UI;
        Ref<UI::UITheme> m_Theme;
        Ref<UI::UIDockSpace> m_DockSpace;
        std::unique_ptr<UI::UIViewportManager> m_ViewportManager;

        float m_LastDeltaSeconds = 0.016f;
        bool m_BlockEvents = false;
    };
}
