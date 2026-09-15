#pragma once
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Core/Ref.h"
#include "Profiler.h"

namespace Nox
{
    class Font;
    class Renderer;
    class Renderer2D;

    // Nox Stats overlay (NOX_PROFILE_STATS), Minecraft F3 style: FPS, CPU and GPU frame time (last/avg/min/max) and
    // RAM/VRAM used / total. Per-scope tables, heaps and counters are the "detailed" mode (off by default; Tracy
    // covers per-pass analysis for now).
    // Drawn with the existing Renderer2D DrawQuad/DrawString, placed on a plane just in front of the camera so it
    // covers the top-left corner of the output image (editor viewport or game), pixel-exact for any camera.
    class StatsOverlay
    {
    public:
        void Draw(Renderer& renderer);
        bool IsDetailed() const { return m_Detailed; }
        void SetDetailed(bool detailed) { m_Detailed = detailed; }

    private:
        void BeginPlane(Renderer& renderer);
        glm::mat4 PixelTransform(const glm::vec2& pixel, const glm::vec2& pixelScale) const;
        float MeasureText(std::string_view text) const;
        void DrawPanel(const glm::vec2& pixel, const glm::vec2& size, const glm::vec4& color);
        void DrawLabel(std::string_view text, const glm::vec2& pixel, const glm::vec4& color);
        void DrawLabelLine(std::string_view text, const glm::vec4& color);

        void DrawTimingHeader(std::string_view title);
        void DrawTimingRow(std::string_view name, uint32_t depth, const ProfileTiming& timing);

        void DrawBasic(Renderer& renderer);
        void DrawDetailed();

        template <typename... Args>
        const std::string& Format(std::format_string<Args...> format, Args&&... args);

    private:
        Renderer2D* m_Renderer2D = nullptr;
        Ref<Font> m_Font;
        bool m_Detailed = false;

        // Camera-facing plane for this frame: world position of pixel (0,0) and world offsets per pixel.
        glm::vec3 m_PlaneOrigin{ 0.0f };
        glm::vec3 m_PixelRight{ 0.0f };
        glm::vec3 m_PixelDown{ 0.0f };
        glm::vec3 m_PlaneNormal{ 0.0f };
        glm::vec2 m_Cursor{ 0.0f };

        // Reused every frame: formatted text and the rows read from Profiler.
        std::string m_Text;
        std::vector<ProfileScopeStats> m_CpuScopes;
        std::vector<ProfileScopeStats> m_GpuScopes;
        std::vector<ProfileCounterStats> m_Counters;
    };
}
