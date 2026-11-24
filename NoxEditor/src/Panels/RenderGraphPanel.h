#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "NoxCore/Core/Ref.h"

namespace NoxUI
{
        class UIContext;
        class UIElement;
    }

namespace Nox
{
    class Renderer;

    // Window > Render Graph: the frame's passes (groups, command buffer, CPU / GPU ms, mesh invocations, flags; click a pass for what
    // it reads and writes), its resources (kind, size, format, memory, lifetime over the passes, the pooled GPU object; Inspect shows a
    // texture), validation messages and the texture inspector (mip, depth curve, exposure, channels, the value under the pointer).
    // Also the culling and LOD switches and the frame's counts. The graph's report is made only while the window is open.
    class RenderGraphPanel
    {
    public:
        explicit RenderGraphPanel(Renderer* renderer = nullptr) : m_Renderer(renderer) {}
        void SetRenderer(Renderer* renderer) { m_Renderer = renderer; }
        const Ref<NoxUI::UIElement>& BuildUI(NoxUI::UIContext* context);
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }
        // Every frame: `shown` while the window is open (closed, the report and any inspection stop).
        void Update(bool shown);

    private:
        Renderer* m_Renderer = nullptr;
        Ref<NoxUI::UIElement> m_Root;
        uint32_t m_SelectedPass = ~0u;
        bool m_ShowUnusedResources = false;
        float m_ExposureStops = 0.0f;
        bool m_WasShown = false;
    };
}
