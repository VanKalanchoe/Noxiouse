#pragma once
#include "NoxCore/Core/Layer.h"
#include "NoxCore/Events/InputEvents.h"
#include "StatsOverlay.h"

namespace Nox
{
    class Renderer;

    // Engine-level layer pushed by Application for every app (editor and runtime) when NOX_PROFILE_STATS is on.
    // F3 toggles the Nox Stats overlay, Shift+F3 resets the statistics window, Ctrl+F3 saves a stats report file.
    class StatsOverlayLayer final : public Layer
    {
    public:
        explicit StatsOverlayLayer(Renderer& renderer);

        void OnEvent(Event& event) override;
        void OnRender() override;

        bool IsVisible() const { return m_Visible; }
        void SetVisible(bool visible) { m_Visible = visible; }
        bool IsDetailed() const { return m_Overlay.IsDetailed(); }
        void SetDetailed(bool detailed) { m_Overlay.SetDetailed(detailed); }

    private:
        bool OnKeyPressed(KeyPressedEvent& event);

    private:
        Renderer& m_Renderer;
        StatsOverlay m_Overlay;
        bool m_Visible = false;
    };
}
