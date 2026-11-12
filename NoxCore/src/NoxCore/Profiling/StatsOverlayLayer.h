#pragma once
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "StatsOverlay.h"
#include "StatsWindow.h"

namespace Nox
{
    class Renderer;

    // Engine-level layer pushed by Application for every app (editor and runtime) when NOX_PROFILE_STATS is on.
    // F3 toggles Nox Stats -- an ImGui window in the editor (StatsWindow: docks, scrolls), the text overlay in a game without ImGui
    // (StatsOverlay) --, Shift+F3 resets the statistics window, Ctrl+F3 saves a stats report file.
    class StatsOverlayLayer final : public Layer
    {
    public:
        StatsOverlayLayer(Renderer& renderer, bool imguiWindow);

        void OnEvent(Event& event) override;
        void OnRender() override;
        void OnImGuiRender() override;

        bool IsVisible() const { return m_Visible; }
        void SetVisible(bool visible) { m_Visible = visible; }
        bool IsDetailed() const { return m_Overlay.IsDetailed(); }
        void SetDetailed(bool detailed) { m_Overlay.SetDetailed(detailed); }

    private:
        bool OnKeyPressed(KeyPressedEvent& event);

    private:
        Renderer& m_Renderer;
        StatsOverlay m_Overlay;
        StatsWindow m_Window;
        bool m_UseWindow = false; // the editor: an ImGui window instead of the overlay
        bool m_Visible = false;
    };
}
