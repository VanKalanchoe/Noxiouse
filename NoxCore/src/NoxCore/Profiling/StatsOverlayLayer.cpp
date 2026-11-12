#include "StatsOverlayLayer.h"

#include <SDL3/SDL_keyboard.h>

#include "NoxCore/Core/core.h"
#include "NoxCore/Renderer/Renderer.h"
#include "StatsReport.h"

namespace Nox
{
    StatsOverlayLayer::StatsOverlayLayer(Renderer& renderer, bool imguiWindow)
        : Layer("StatsOverlayLayer"), m_Renderer(renderer), m_UseWindow(imguiWindow)
    {
    }

    void StatsOverlayLayer::OnEvent(Event& event)
    {
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<KeyPressedEvent>(Nox_BIND_EVENT_FN(StatsOverlayLayer::OnKeyPressed));
    }

    void StatsOverlayLayer::OnRender()
    {
        if (!m_Visible || m_UseWindow)
            return;

        // The overlay measures its own CPU cost with this row (GPU cost is part of the "2D Overlay" pass).
        NOX_PROFILE_SCOPE("Stats Overlay");
        m_Overlay.Draw(m_Renderer);
    }

    void StatsOverlayLayer::OnImGuiRender()
    {
        if (!m_Visible || !m_UseWindow)
            return;
        NOX_PROFILE_SCOPE("Stats Window");
        m_Window.Draw(m_Renderer, &m_Visible);
    }

    bool StatsOverlayLayer::OnKeyPressed(KeyPressedEvent& event)
    {
        if (event.IsRepeat() || event.GetKeyCode() != SDL_SCANCODE_F3)
            return false;

        const uint16_t modifiers = event.GetModifiers(); // held when F3 went down (see KeyPressedEvent::GetModifiers)
        if (modifiers & SDL_KMOD_CTRL)
            SaveStatsReport(m_Renderer);
        else if (modifiers & SDL_KMOD_SHIFT)
            Profiler::Get().ResetStats();
        else
            m_Visible = !m_Visible;

        return true;
    }
}
