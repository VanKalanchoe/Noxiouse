#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

union SDL_Event;

namespace NRI
{
    class CommandAllocator;
    class CommandBuffer;
    class Swapchain;
}

namespace Nox
{
    class Renderer;
    class Window;
}

namespace Nox::UI
{
    class UIDockSpace;

    struct UIViewport
    {
        uint32_t WindowID = 0;
        std::unique_ptr<Nox::Window> Window;
        std::unique_ptr<NRI::Swapchain> Swapchain;
        std::array<std::unique_ptr<NRI::CommandAllocator>, 2> Allocators;
        std::array<std::unique_ptr<NRI::CommandBuffer>, 2> CmdBuffers;

        UIContext Context;
        UIDrawList DrawList;
        std::string Title;
        Ref<UIElement> Content;
        Ref<UIElement> ContentHost;

        uint32_t PixelWidth = 0;
        uint32_t PixelHeight = 0;
        bool PendingClose = false;
        bool NeedsSwapchainResize = false;
    };

    // Manages secondary OS windows (SDL3 Window + NRI Swapchain + UIContext)
    // for ImGui-style multi-viewport tearing & re-docking across monitors.
    class UIViewportManager
    {
    public:
        UIViewportManager(Nox::Window& mainWindow, Renderer& renderer, UIContext& mainContext, UIDockSpace* dockSpace = nullptr);
        ~UIViewportManager();

        void SetDockSpace(UIDockSpace* dockSpace) { m_DockSpace = dockSpace; }

        // Promotes a panel/floating window into its own OS window at the global cursor position.
        UIViewport* PromoteToViewport(
            const std::string& title,
            const Ref<UIElement>& content,
            UIVec2 size,
            UIVec2 grabOffset,
            bool startDragging = true
        );

        void BeginDragViewport(UIViewport* viewport, UIVec2 grabOffset);
        void BeginResizeViewport(UIViewport* viewport);

        // Routes SDL3 events belonging to secondary OS windows; returns true if consumed.
        bool ProcessSDLEvent(const SDL_Event& e);

        // Updates active OS-window drags/resizes, updates UIContexts, and renders secondary swapchains.
        void UpdateAndRender(float dt);

        void Shutdown();

        size_t GetViewportCount() const { return m_Viewports.size(); }

        static UIViewportManager* GetActive() { return s_Active; }

    private:
        UIViewport* FindByWindowID(uint32_t windowID) const;

        Nox::Window& m_MainWindow;
        Renderer& m_Renderer;
        UIContext& m_MainContext;
        UIDockSpace* m_DockSpace = nullptr;

        std::vector<std::unique_ptr<UIViewport>> m_Viewports;
        UIViewport* m_ActiveDragViewport = nullptr;
        UIVec2 m_DragGrabOffset{ 0.0f, 0.0f };
        UIViewport* m_ActiveResizeViewport = nullptr;

        inline static UIViewportManager* s_Active = nullptr;
    };
}
