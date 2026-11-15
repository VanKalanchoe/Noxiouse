#include "UITheme.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Utils/NOXWatcher.h"

namespace Nox::UI
{
    namespace
    {
        float LerpF(float a, float b, float t) { return a + (b - a) * t; }

        bool ParseHexColor(std::string_view hex, UIColor& out)
        {
            if (hex.empty() || hex[0] != '#')
                return false;
            hex.remove_prefix(1);
            if (hex.size() != 6 && hex.size() != 8)
                return false;

            auto byteAt = [&](size_t idx) -> uint8_t
            {
                auto hexVal = [](char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                    return -1;
                };
                int hi = hexVal(hex[idx]);
                int lo = hexVal(hex[idx + 1]);
                if (hi < 0 || lo < 0) return 0;
                return static_cast<uint8_t>((hi << 4) | lo);
            };

            out.r = static_cast<float>(byteAt(0)) / 255.0f;
            out.g = static_cast<float>(byteAt(2)) / 255.0f;
            out.b = static_cast<float>(byteAt(4)) / 255.0f;
            out.a = (hex.size() == 8) ? static_cast<float>(byteAt(6)) / 255.0f : 1.0f;
            return true;
        }

        UIColor ReadColorNode(const YAML::Node& node, const UITheme& theme, const UIColor& fallback)
        {
            if (!node)
                return fallback;
            if (node.IsScalar())
            {
                const std::string s = node.as<std::string>();
                if (!s.empty() && s[0] == '$')
                    return theme.GetToken(std::string_view(s).substr(1), fallback);
                UIColor hexColor{};
                if (ParseHexColor(s, hexColor))
                    return hexColor;
            }
            if (node.IsSequence() && (node.size() == 3 || node.size() == 4))
            {
                UIColor c(1.0f);
                c.r = node[0].as<float>(1.0f);
                c.g = node[1].as<float>(1.0f);
                c.b = node[2].as<float>(1.0f);
                c.a = node.size() == 4 ? node[3].as<float>(1.0f) : 1.0f;
                return c;
            }
            return fallback;
        }

        UICornerRadius ReadCornerRadius(const YAML::Node& node, const UICornerRadius& fallback)
        {
            if (!node)
                return fallback;
            if (node.IsScalar())
                return UICornerRadius::All(node.as<float>(fallback.TopLeft));
            if (node.IsSequence() && node.size() == 4)
            {
                return {
                    node[0].as<float>(0.0f),
                    node[1].as<float>(0.0f),
                    node[2].as<float>(0.0f),
                    node[3].as<float>(0.0f)
                };
            }
            return fallback;
        }

        UIThickness ReadThickness(const YAML::Node& node, const UIThickness& fallback)
        {
            if (!node)
                return fallback;
            if (node.IsScalar())
                return UIThickness::All(node.as<float>(fallback.Left));
            if (node.IsSequence() && node.size() == 2)
                return UIThickness::Symmetric(node[0].as<float>(0.0f), node[1].as<float>(0.0f));
            if (node.IsSequence() && node.size() == 4)
            {
                return {
                    node[0].as<float>(0.0f),
                    node[1].as<float>(0.0f),
                    node[2].as<float>(0.0f),
                    node[3].as<float>(0.0f)
                };
            }
            return fallback;
        }

        void ApplyStyleNode(const YAML::Node& node, const UITheme& theme, UIStyleBox& style)
        {
            if (!node || !node.IsMap())
                return;
            if (node["fill"])
            {
                style.FillColor = ReadColorNode(node["fill"], theme, style.FillColor);
                if (!node["fill_bottom"])
                    style.FillColorBottom = style.FillColor;
            }
            if (node["fill_bottom"])
            {
                style.FillColorBottom = ReadColorNode(node["fill_bottom"], theme, style.FillColorBottom);
                style.UseGradient = true;
            }
            if (node["gradient"])
                style.UseGradient = node["gradient"].as<bool>(style.UseGradient);
            if (node["border_color"])
                style.BorderColor = ReadColorNode(node["border_color"], theme, style.BorderColor);
            if (node["border_thickness"])
                style.BorderThickness = node["border_thickness"].as<float>(style.BorderThickness);
            if (node["shadow_color"])
                style.ShadowColor = ReadColorNode(node["shadow_color"], theme, style.ShadowColor);
            if (node["shadow_offset"] && node["shadow_offset"].IsSequence() && node["shadow_offset"].size() == 2)
                style.ShadowOffset = { node["shadow_offset"][0].as<float>(0.0f), node["shadow_offset"][1].as<float>(4.0f) };
            if (node["shadow_blur"])
                style.ShadowBlur = node["shadow_blur"].as<float>(style.ShadowBlur);
            if (node["corner_radius"])
                style.CornerRadius = ReadCornerRadius(node["corner_radius"], style.CornerRadius);
            if (node["padding"])
                style.Padding = ReadThickness(node["padding"], style.Padding);
            if (node["text_color"])
                style.TextColor = ReadColorNode(node["text_color"], theme, style.TextColor);
            if (node["font_size"])
                style.FontSize = node["font_size"].as<float>(style.FontSize);
            if (node["transition_ms"])
                style.TransitionSeconds = std::max(0.0f, node["transition_ms"].as<float>(100.0f) / 1000.0f);
        }
    }

    UIStyleBox UIStyleBox::Lerp(const UIStyleBox& a, const UIStyleBox& b, float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        UIStyleBox out = b;
        out.FillColor = glm::mix(a.FillColor, b.FillColor, t);
        out.FillColorBottom = glm::mix(a.FillColorBottom, b.FillColorBottom, t);
        out.BorderColor = glm::mix(a.BorderColor, b.BorderColor, t);
        out.ShadowColor = glm::mix(a.ShadowColor, b.ShadowColor, t);
        out.ShadowOffset = glm::mix(a.ShadowOffset, b.ShadowOffset, t);
        out.ShadowBlur = LerpF(a.ShadowBlur, b.ShadowBlur, t);
        out.BorderThickness = LerpF(a.BorderThickness, b.BorderThickness, t);
        out.CornerRadius = {
            LerpF(a.CornerRadius.TopLeft, b.CornerRadius.TopLeft, t),
            LerpF(a.CornerRadius.TopRight, b.CornerRadius.TopRight, t),
            LerpF(a.CornerRadius.BottomRight, b.CornerRadius.BottomRight, t),
            LerpF(a.CornerRadius.BottomLeft, b.CornerRadius.BottomLeft, t)
        };
        out.TextColor = glm::mix(a.TextColor, b.TextColor, t);
        out.FontSize = LerpF(a.FontSize, b.FontSize, t);
        return out;
    }

    Ref<UITheme> UITheme::CreateDefault()
    {
        Ref<UITheme> theme(new UITheme());

        theme->SetToken("Background", { 0.070f, 0.078f, 0.102f, 1.0f });
        theme->SetToken("Surface0",   { 0.102f, 0.114f, 0.149f, 0.96f });
        theme->SetToken("Surface1",   { 0.133f, 0.149f, 0.196f, 0.96f });
        theme->SetToken("Border",     { 0.220f, 0.250f, 0.330f, 0.90f });
        theme->SetToken("Accent",     { 0.240f, 0.560f, 0.960f, 1.00f });
        theme->SetToken("Text",       { 0.920f, 0.940f, 0.980f, 1.00f });
        theme->SetToken("TextMuted",  { 0.620f, 0.670f, 0.760f, 1.00f });

        UIStyleBox panel{};
        panel.FillColor = theme->GetToken("Surface0");
        panel.FillColorBottom = panel.FillColor;
        panel.BorderColor = theme->GetToken("Border");
        panel.BorderThickness = 1.0f;
        panel.ShadowColor = { 0.0f, 0.0f, 0.0f, 0.45f };
        panel.ShadowOffset = { 0.0f, 6.0f };
        panel.ShadowBlur = 14.0f;
        panel.CornerRadius = UICornerRadius::All(10.0f);
        panel.Padding = UIThickness::All(12.0f);
        theme->SetStyle("Panel", panel);

        UIStyleBox button{};
        button.FillColor = theme->GetToken("Surface1");
        button.FillColorBottom = { 0.105f, 0.118f, 0.156f, 1.0f };
        button.UseGradient = true;
        button.BorderColor = theme->GetToken("Border");
        button.BorderThickness = 1.0f;
        button.CornerRadius = UICornerRadius::All(7.0f);
        button.Padding = UIThickness::Symmetric(14.0f, 8.0f);
        button.TextColor = theme->GetToken("Text");
        button.FontSize = 15.0f;
        theme->SetStyle("Button", button);

        UIStyleBox buttonHover = button;
        buttonHover.FillColor = { 0.18f, 0.21f, 0.28f, 1.0f };
        buttonHover.FillColorBottom = { 0.14f, 0.16f, 0.22f, 1.0f };
        buttonHover.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("Button:hover", buttonHover);

        UIStyleBox buttonPressed = button;
        buttonPressed.FillColor = { 0.16f, 0.40f, 0.72f, 1.0f };
        buttonPressed.FillColorBottom = { 0.12f, 0.30f, 0.56f, 1.0f };
        buttonPressed.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("Button:pressed", buttonPressed);

        return theme;
    }

    bool UITheme::LoadFromFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            NOX_CORE_WARN("UITheme: Could not open '{}', keeping current styles", path.string());
            return false;
        }

        try
        {
            std::stringstream ss;
            ss << stream.rdbuf();
            YAML::Node root = YAML::Load(ss.str());
            if (!root || !root.IsMap())
            {
                NOX_CORE_ERROR("UITheme: Invalid root map in '{}'", path.string());
                return false;
            }

            if (const YAML::Node tokens = root["tokens"]; tokens && tokens.IsMap())
            {
                for (const auto& kv : tokens)
                    SetToken(kv.first.as<std::string>(), ReadColorNode(kv.second, *this, UIColor(1.0f)));
            }

            if (const YAML::Node styles = root["styles"]; styles && styles.IsMap())
            {
                for (const auto& kv : styles)
                {
                    const std::string key = kv.first.as<std::string>();
                    UIStyleBox box = m_Styles.contains(key) ? m_Styles[key] : UIStyleBox{};
                    ApplyStyleNode(kv.second, *this, box);
                    m_Styles[key] = box;
                }
            }

            NOX_CORE_INFO("UITheme: Loaded '{}'", path.string());
            return true;
        }
        catch (const std::exception& e)
        {
            NOX_CORE_ERROR("UITheme: Failed to parse '{}': {}", path.string(), e.what());
            return false;
        }
    }

    void UITheme::WatchFile(::Utils::NOXWatcher& watcher, const std::filesystem::path& path)
    {
        m_WatchedPath = path;
        watcher.watch(path, [this](const std::filesystem::path&)
        {
            m_PendingReload = true;
        });
    }

    bool UITheme::PollHotReload()
    {
        if (!m_PendingReload || m_WatchedPath.empty())
            return false;
        m_PendingReload = false;
        return LoadFromFile(m_WatchedPath);
    }

    void UITheme::SetToken(const std::string& name, const UIColor& color)
    {
        m_Tokens[name] = color;
    }

    UIColor UITheme::GetToken(std::string_view name, const UIColor& fallback) const
    {
        if (auto it = m_Tokens.find(std::string(name)); it != m_Tokens.end())
            return it->second;
        return fallback;
    }

    void UITheme::SetStyle(const std::string& selector, const UIStyleBox& style)
    {
        m_Styles[selector] = style;
    }

    UIStyleBox UITheme::Resolve(std::string_view className, std::string_view variant, UIState state) const
    {
        UIStyleBox result{};

        auto applyIfFound = [&](const std::string& key)
        {
            if (auto it = m_Styles.find(key); it != m_Styles.end())
                result = it->second;
        };

        const std::string cls(className);
        applyIfFound(cls);

        std::string base = cls;
        if (!variant.empty())
        {
            base += ".";
            base += variant;
            applyIfFound(base);
        }

        auto applyStateSuffix = [&](std::string_view suffix)
        {
            applyIfFound(cls + std::string(suffix));
            if (!variant.empty())
                applyIfFound(base + std::string(suffix));
        };

        if (HasState(state, UIState::Disabled))
            applyStateSuffix(":disabled");
        else
        {
            if (HasState(state, UIState::Focused))
                applyStateSuffix(":focus");
            if (HasState(state, UIState::Hovered))
                applyStateSuffix(":hover");
            if (HasState(state, UIState::Pressed))
                applyStateSuffix(":active");
            if (HasState(state, UIState::Pressed))
                applyStateSuffix(":pressed");
        }

        return result;
    }
}
