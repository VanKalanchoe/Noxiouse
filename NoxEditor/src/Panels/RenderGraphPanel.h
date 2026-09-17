#pragma once
#include <cstdint>

#include <imgui.h>

namespace Nox
{
    class Renderer;
    struct RGFrameReport;

    // Render graph tooling (§5.4.7): passes (culled, command buffer, CPU/GPU time, accesses), resources (lifetime, memory,
    // aliasing), validation messages and texture inspection of any graph texture. Opened from Window > Render Graph; while
    // closed the renderer produces no report and inspects nothing.
    class RenderGraphPanel
    {
    public:
        explicit RenderGraphPanel(Renderer* renderer) : m_Renderer(renderer) {}

        void OnImGuiRender();

        bool IsOpen() const { return m_Open; }
        void SetOpen(bool open) { m_Open = open; }

    private:
        void DrawHeader(const RGFrameReport& report);
        void DrawPasses(const RGFrameReport& report);
        void DrawResources(const RGFrameReport& report);
        void DrawValidation(const RGFrameReport& report);
        void DrawInspector();
        void Inspect(const char* name, uint32_t occurrence);
        void StopInspection();

    private:
        Renderer* m_Renderer = nullptr;
        bool m_Open = false;
        uint32_t m_SelectedPass = ~0u;
        bool m_ShowUnusedResources = false;
        float m_ExposureStops = 0.0f;
    };
}
