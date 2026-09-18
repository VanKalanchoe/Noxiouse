#include "StatsOverlay.h"

#include <array>
#include <iterator>

#include <SDL3/SDL_cpuinfo.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/MSDFData.h"
#include "NoxCore/Renderer/Renderer.h"

#if !defined(NOX_BUILD_CONFIG)
    #define NOX_BUILD_CONFIG "Unknown"
#endif

namespace Nox
{
    namespace
    {
        // All layout sizes derive from the text height, so the overlay scales with one value.
        constexpr float TextPixelHeight = 22.0f;
        constexpr glm::vec2 Origin{ 0.6f * TextPixelHeight, 0.6f * TextPixelHeight };
        constexpr float LineStep = 1.3f * TextPixelHeight;
        constexpr float SectionGap = 0.4f * TextPixelHeight;
        constexpr float BackgroundPadding = 0.35f * TextPixelHeight;
        constexpr float DepthIndent = 0.8f * TextPixelHeight;
        constexpr float BasicNameColumnWidth = 4.0f * TextPixelHeight;
        constexpr float DetailedNameColumnWidth = 15.0f * TextPixelHeight;
        constexpr float ValueColumnWidth = 4.2f * TextPixelHeight;
        constexpr int ValueColumnCount = 4;

        // NDC depth of the overlay plane. Reverse-Z: 0.5 is twice the near-plane distance, so the overlay passes the
        // 2D pass depth test (GreaterOrEqual) against everything except geometry closer than that.
        constexpr double OverlayDepth = 0.5;

        // Dark enough to keep white text readable over bright scenes.
        constexpr glm::vec4 BackgroundColor{ 0.0f, 0.0f, 0.0f, 0.75f };
        constexpr glm::vec4 TextColor{ 0.95f, 0.95f, 0.95f, 1.0f };
        constexpr glm::vec4 TitleColor{ 1.0f, 0.85f, 0.35f, 1.0f };
        constexpr glm::vec4 DimColor{ 0.65f, 0.65f, 0.65f, 1.0f };
        constexpr glm::vec4 WarningColor{ 1.0f, 0.4f, 0.4f, 1.0f };

        constexpr double BytesPerMegabyte = 1024.0 * 1024.0;

        double ToMegabytes(uint64_t bytes)
        {
            return static_cast<double>(bytes) / BytesPerMegabyte;
        }

        // Output pixel -> world position on the overlay plane. The 2D overlay pass uses the viewport {0, h, w, -h},
        // so pixel y = 0 (top) is NDC y = +1.
        glm::dvec3 UnprojectPixel(const glm::dmat4& inverseViewProjection, const glm::dvec2& pixel, const glm::dvec2& outputSize)
        {
            const glm::dvec4 ndc(pixel.x / outputSize.x * 2.0 - 1.0, 1.0 - pixel.y / outputSize.y * 2.0, OverlayDepth, 1.0);
            const glm::dvec4 world = inverseViewProjection * ndc;
            return glm::dvec3(world) / world.w;
        }
    }

    template <typename... Args>
    const std::string& StatsOverlay::Format(std::format_string<Args...> format, Args&&... args)
    {
        // Reuses the string's capacity; the reference is valid until the next Format call.
        m_Text.clear();
        std::format_to(std::back_inserter(m_Text), format, std::forward<Args>(args)...);
        return m_Text;
    }

    void StatsOverlay::Draw(Renderer& renderer)
    {
        const NRI::Extent2D outputExtent = renderer.getOutputSize();
        if (outputExtent.width == 0 || outputExtent.height == 0)
            return;

        if (!m_Font)
            m_Font = Font::GetDefault();
        m_Renderer2D = renderer.getRenderer2D();

        BeginPlane(renderer);
        m_Cursor = Origin;

        DrawBasic(renderer);
        if (m_Detailed)
            DrawDetailed();
    }

    void StatsOverlay::BeginPlane(Renderer& renderer)
    {
        // Built from the far corners (not 1-pixel steps) and in double precision, so the per-pixel axes stay accurate
        // when the camera is far from the world origin.
        const NRI::Extent2D outputExtent = renderer.getOutputSize();
        const glm::dvec2 outputSize(outputExtent.width, outputExtent.height);
        const glm::dmat4 inverseViewProjection = glm::inverse(glm::dmat4(renderer.getViewProjection()));
        const glm::dvec3 origin = UnprojectPixel(inverseViewProjection, glm::dvec2(0.0), outputSize);
        const glm::dvec3 right = (UnprojectPixel(inverseViewProjection, glm::dvec2(outputSize.x, 0.0), outputSize) - origin) / outputSize.x;
        const glm::dvec3 down = (UnprojectPixel(inverseViewProjection, glm::dvec2(0.0, outputSize.y), outputSize) - origin) / outputSize.y;

        m_PlaneOrigin = glm::vec3(origin);
        m_PixelRight = glm::vec3(right);
        m_PixelDown = glm::vec3(down);
        m_PlaneNormal = glm::vec3(glm::normalize(glm::cross(right, down)));
    }

    void StatsOverlay::DrawBasic(Renderer& renderer)
    {
        const Profiler& profiler = Profiler::Get();

        DrawLabelLine(Format("Nox Stats | {} | VSync {}", NOX_BUILD_CONFIG, renderer.getVSync() ? "ON" : "OFF"), TextColor);
        if (std::string_view(NOX_BUILD_CONFIG) == "Debug")
            DrawLabelLine("Debug build: not for performance numbers", WarningColor);

        const ProfileTiming frame = profiler.GetFrameTime();
        DrawLabelLine(Format("FPS {:.0f}", frame.Avg > 0.0 ? 1000.0 / frame.Avg : 0.0), TextColor);

        DrawTimingHeader("ms");
        DrawTimingRow("CPU", 0, profiler.GetCpuFrameTime());
        if (profiler.IsGpuTimingAvailable())
            DrawTimingRow("GPU", 0, profiler.GetGpuFrameTime());

        // RAM: this process / installed (SDL3). VRAM: device-local heaps, usage / budget the OS grants the process.
        const ProfileMemoryStats& memory = profiler.GetMemoryStats();
        uint64_t vramUsage = 0;
        uint64_t vramBudget = 0;
        for (const NRI::MemoryHeapStats& heap : memory.Heaps)
        {
            if (!heap.deviceLocal)
                continue;
            vramUsage += heap.usage;
            vramBudget += heap.budget;
        }

        m_Cursor.y += SectionGap;
        DrawLabelLine(Format("RAM   {:.0f} / {} MB", ToMegabytes(memory.Process.WorkingSetBytes), SDL_GetSystemRAM()), TextColor);
        DrawLabelLine(Format("VRAM  {:.0f} / {:.0f} MB", ToMegabytes(vramUsage), ToMegabytes(vramBudget)), TextColor);
    }

    void StatsOverlay::DrawDetailed()
    {
        Profiler& profiler = Profiler::Get();

        // Two columns so both tables fit on screen: GPU passes, heaps and counters continue below the basic view;
        // the (longer) CPU scope table starts at the top, right of the first column.
        constexpr float TableWidth = DetailedNameColumnWidth + ValueColumnCount * ValueColumnWidth;
        const glm::vec2 secondColumn(Origin.x + TableWidth + 2.0f * BackgroundPadding + 2.0f * SectionGap, Origin.y);

        m_Cursor.y += SectionGap;
        if (profiler.IsGpuTimingAvailable())
        {
            profiler.GetGpuScopes(m_GpuScopes);
            DrawTimingHeader("GPU passes (ms)");
            for (const ProfileScopeStats& scope : m_GpuScopes)
                DrawTimingRow(scope.Name, scope.Depth, scope.Timing);
        }
        else
        {
            DrawLabelLine("GPU timestamp queries are not supported on this queue", DimColor);
        }

        const ProfileMemoryStats& memory = profiler.GetMemoryStats();
        m_Cursor.y += SectionGap;
        for (size_t heapIndex = 0; heapIndex < memory.Heaps.size(); ++heapIndex)
        {
            const NRI::MemoryHeapStats& heap = memory.Heaps[heapIndex];
            DrawLabelLine(Format("{} heap {}: {:.0f} / {:.0f} MB, {} allocations ({:.0f} in {:.0f} MB of blocks)",
                                 heap.deviceLocal ? "VRAM" : "Host", heapIndex, ToMegabytes(heap.usage), ToMegabytes(heap.budget),
                                 heap.allocationCount, ToMegabytes(heap.allocationBytes), ToMegabytes(heap.blockBytes)), TextColor);
        }
        if (!memory.Heaps.empty() && !memory.BudgetFromDriver)
            DrawLabelLine("VK_EXT_memory_budget unavailable: estimated", DimColor);

        // What the engine holds of that budget: committed is what the category occupies, used what is live inside it
        // (a geometry stream keeps capacity beyond its ranges). Over budget is shown, never enforced (soft budget).
        if (!memory.Categories.empty())
        {
            m_Cursor.y += SectionGap;
            DrawLabelLine("VRAM by category (committed / used / budget MB)", DimColor);
            for (const MemoryCategoryStats& category : memory.Categories)
            {
                const bool overBudget = category.Budget > 0 && category.Committed > category.Budget;
                DrawLabelLine(Format("  {:<12} {:6.0f} {:6.0f} {:6.0f}{}", category.Name, ToMegabytes(category.Committed),
                                     ToMegabytes(category.Used), ToMegabytes(category.Budget), overBudget ? "  OVER" : ""),
                              overBudget ? WarningColor : TextColor);
            }
        }

        profiler.GetCounters(m_Counters);
        if (!m_Counters.empty())
        {
            m_Cursor.y += SectionGap;
            for (const ProfileCounterStats& counter : m_Counters)
                DrawLabelLine(Format("{}   {:.0f}", counter.Name, counter.Value), TextColor);
        }

        m_Cursor = secondColumn;
        profiler.GetCpuScopes(m_CpuScopes);
        DrawTimingHeader("CPU scopes (ms)");
        for (const ProfileScopeStats& scope : m_CpuScopes)
            DrawTimingRow(scope.Name, scope.Depth, scope.Timing);
    }

    glm::mat4 StatsOverlay::PixelTransform(const glm::vec2& pixel, const glm::vec2& pixelScale) const
    {
        // Local x maps to screen right, local y to screen up (Renderer2D quads and glyphs are y-up).
        return glm::mat4(glm::vec4(m_PixelRight * pixelScale.x, 0.0f),
                         glm::vec4(-m_PixelDown * pixelScale.y, 0.0f),
                         glm::vec4(m_PlaneNormal, 0.0f),
                         glm::vec4(m_PlaneOrigin + m_PixelRight * pixel.x + m_PixelDown * pixel.y, 1.0f));
    }

    float StatsOverlay::MeasureText(std::string_view text) const
    {
        // Same advance/kerning rules as Renderer2D::DrawString for single-line text.
        const auto& fontGeometry = m_Font->GetMSDFData()->FontGeometry;
        const auto& metrics = fontGeometry.getMetrics();
        const double fsScale = 1.0 / (metrics.ascenderY - metrics.descenderY);

        double width = 0.0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char character = text[i];
            auto glyph = fontGeometry.getGlyph(character);
            if (!glyph)
                glyph = fontGeometry.getGlyph('?');
            if (!glyph)
                break;

            double advance = glyph->getAdvance();
            if (i + 1 < text.size())
                fontGeometry.getAdvance(advance, character, text[i + 1]);
            width += fsScale * advance;
        }
        return static_cast<float>(width) * TextPixelHeight;
    }

    void StatsOverlay::DrawPanel(const glm::vec2& pixel, const glm::vec2& size, const glm::vec4& color)
    {
        // Renderer2D quads are centered unit quads.
        m_Renderer2D->DrawQuad(PixelTransform(pixel + size * 0.5f, size), color);
    }

    void StatsOverlay::DrawLabel(std::string_view text, const glm::vec2& pixel, const glm::vec4& color)
    {
        // DrawString places the first baseline at the transform origin; 1 glyph unit = ascender-to-descender height.
        const auto& metrics = m_Font->GetMSDFData()->FontGeometry.getMetrics();
        const float ascent = static_cast<float>(metrics.ascenderY / (metrics.ascenderY - metrics.descenderY));
        const glm::vec2 baseline = pixel + glm::vec2(0.0f, ascent * TextPixelHeight);

        m_Renderer2D->DrawString(std::string(text), m_Font, PixelTransform(baseline, glm::vec2(TextPixelHeight)), Renderer2D::TextParams{ color });
    }

    void StatsOverlay::DrawLabelLine(std::string_view text, const glm::vec4& color)
    {
        DrawPanel(m_Cursor - glm::vec2(BackgroundPadding, 0.0f), glm::vec2(MeasureText(text) + 2.0f * BackgroundPadding, LineStep), BackgroundColor);
        DrawLabel(text, m_Cursor + glm::vec2(0.0f, (LineStep - TextPixelHeight) * 0.5f), color);
        m_Cursor.y += LineStep;
    }

    void StatsOverlay::DrawTimingHeader(std::string_view title)
    {
        constexpr std::array<std::string_view, ValueColumnCount> ColumnNames = { "last", "avg", "min", "max" };
        const float nameColumnWidth = m_Detailed ? DetailedNameColumnWidth : BasicNameColumnWidth;
        const float tableWidth = nameColumnWidth + ValueColumnCount * ValueColumnWidth;
        const float textOffsetY = (LineStep - TextPixelHeight) * 0.5f;

        DrawPanel(m_Cursor - glm::vec2(BackgroundPadding, 0.0f), glm::vec2(tableWidth + 2.0f * BackgroundPadding, LineStep), BackgroundColor);
        DrawLabel(title, m_Cursor + glm::vec2(0.0f, textOffsetY), TitleColor);
        for (int column = 0; column < ValueColumnCount; ++column)
        {
            const float columnRight = m_Cursor.x + nameColumnWidth + static_cast<float>(column + 1) * ValueColumnWidth;
            DrawLabel(ColumnNames[column], glm::vec2(columnRight - MeasureText(ColumnNames[column]), m_Cursor.y + textOffsetY), TitleColor);
        }
        m_Cursor.y += LineStep;
    }

    void StatsOverlay::DrawTimingRow(std::string_view name, uint32_t depth, const ProfileTiming& timing)
    {
        const float nameColumnWidth = m_Detailed ? DetailedNameColumnWidth : BasicNameColumnWidth;
        const float tableWidth = nameColumnWidth + ValueColumnCount * ValueColumnWidth;
        const float textOffsetY = (LineStep - TextPixelHeight) * 0.5f;

        // Rows share one background width so columns read as a table; values are right-aligned per column.
        DrawPanel(m_Cursor - glm::vec2(BackgroundPadding, 0.0f), glm::vec2(tableWidth + 2.0f * BackgroundPadding, LineStep), BackgroundColor);
        DrawLabel(name, m_Cursor + glm::vec2(static_cast<float>(depth) * DepthIndent, textOffsetY), TextColor);

        const std::array<double, ValueColumnCount> values = { timing.Last, timing.Avg, timing.Min, timing.Max };
        for (int column = 0; column < ValueColumnCount; ++column)
        {
            const std::string& text = Format("{:.2f}", values[column]);
            const float columnRight = m_Cursor.x + nameColumnWidth + static_cast<float>(column + 1) * ValueColumnWidth;
            DrawLabel(text, glm::vec2(columnRight - MeasureText(text), m_Cursor.y + textOffsetY), TextColor);
        }
        m_Cursor.y += LineStep;
    }
}
