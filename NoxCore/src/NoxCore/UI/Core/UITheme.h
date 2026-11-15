#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UITypes.h"

namespace Utils
{
    class NOXWatcher;
}

namespace Nox::UI
{
    struct UIStyleBox
    {
        UIColor FillColor{ 0.11f, 0.12f, 0.16f, 1.0f };
        UIColor FillColorBottom{ 0.11f, 0.12f, 0.16f, 1.0f };
        bool UseGradient = false;

        UIColor BorderColor{ 0.22f, 0.25f, 0.33f, 0.9f };
        float BorderThickness = 0.0f;

        UIColor ShadowColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        UIVec2 ShadowOffset{ 0.0f, 4.0f };
        float ShadowBlur = 0.0f;

        UICornerRadius CornerRadius = UICornerRadius::All(6.0f);
        UIThickness Padding = UIThickness::All(8.0f);

        UIColor TextColor{ 0.92f, 0.94f, 0.98f, 1.0f };
        float FontSize = 15.0f;

        float TransitionSeconds = 0.10f;

        static UIStyleBox Lerp(const UIStyleBox& a, const UIStyleBox& b, float t);
    };

    class UITheme : public RefCounted
    {
    public:
        UITheme() = default;

        static Ref<UITheme> CreateDefault();

        // Loads a .nstyle YAML file. Falls back safely to default values on parse errors.
        bool LoadFromFile(const std::filesystem::path& path);

        // Watches a .nstyle file for live hot-reloading.
        void WatchFile(::Utils::NOXWatcher& watcher, const std::filesystem::path& path);

        // Polls any pending hot reload on the main thread. Returns true if reloaded.
        bool PollHotReload();

        void SetToken(const std::string& name, const UIColor& color);
        UIColor GetToken(std::string_view name, const UIColor& fallback = { 1.0f, 1.0f, 1.0f, 1.0f }) const;

        void SetStyle(const std::string& selector, const UIStyleBox& style);

        // Resolves style for (className, variant, state), layering:
        // Base -> className -> className.variant -> :hover/:pressed/:focused/:disabled.
        UIStyleBox Resolve(std::string_view className, std::string_view variant, UIState state) const;

    private:
        std::unordered_map<std::string, UIColor> m_Tokens;
        std::unordered_map<std::string, UIStyleBox> m_Styles;
        std::filesystem::path m_WatchedPath;
        bool m_PendingReload = false;
    };
}
