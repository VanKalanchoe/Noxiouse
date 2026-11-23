#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Core/UITheme.h>
#include <NoxUI/Core/UIViewportManager.h>
#include <NoxUI/Layout/UIDockSpace.h>
#include <NoxUI/Rendering/UIFont.h>

namespace Nox
{
    class Renderer;
    class Window;

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

    class UILayer : public Layer
    {
    public:
        UILayer(Window& window, Renderer& renderer, std::string themePath = "assets/themes/DefaultDark.nstyle");
        ~UILayer() override;

        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;

        void SetConfigFlags(UIConfigFlags flags);
        UIConfigFlags GetConfigFlags() const { return m_ConfigFlags; }

        NoxUI::UIContext& GetContext() { return m_UI; }
        const NoxUI::UIContext& GetContext() const { return m_UI; }
        const Ref<NoxUI::UITheme>& GetTheme() const { return m_Theme; }
        NoxUI::UIDockSpace* GetDockSpace() { return m_DockSpace.get(); }
        const Ref<NoxUI::UIDockSpace>& GetDockSpaceRef() const { return m_DockSpace; }
        NoxUI::UIViewportManager* GetViewportManager() { return m_ViewportManager.get(); }

        void SetRoot(const Ref<NoxUI::UIElement>& root);
        void AttachDockSpace(NoxUI::UIDockSpace* dockSpace);
        Ref<NoxUI::UIDockNode> AddPanel(
            const std::string& title,
            const Ref<NoxUI::UIElement>& content,
            NoxUI::UIDockDropZone zone = NoxUI::UIDockDropZone::Center,
            const std::string& targetTitle = {},
            float ratio = 0.5f,
            NoxUI::UIDockNodeFlags flags = NoxUI::UIDockNodeFlags::None);
        void ReloadTheme(const std::string& themePath = {});

        void BlockEvents(bool block) { m_BlockEvents = block; }

    private:
        Window& m_Window;
        Renderer& m_Renderer;
        std::string m_ThemePath;
        UIConfigFlags m_ConfigFlags = UIConfigFlags::DockingEnable | UIConfigFlags::ViewportsEnable;

        NoxUI::UIContext m_UI;
        Ref<NoxUI::UITheme> m_Theme;
        Ref<NoxUI::UIDockSpace> m_DockSpace;
        std::unique_ptr<NoxUI::UIViewportManager> m_ViewportManager;

        float m_LastDeltaSeconds = 0.016f;
        bool m_BlockEvents = false;
    };
}
