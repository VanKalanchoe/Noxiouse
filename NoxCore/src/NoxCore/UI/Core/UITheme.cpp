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

        theme->SetToken("Background",  { 0.082f, 0.086f, 0.102f, 1.00f });
        theme->SetToken("Surface0",    { 0.114f, 0.120f, 0.141f, 0.98f });
        theme->SetToken("Surface1",    { 0.145f, 0.153f, 0.180f, 0.98f });
        theme->SetToken("Surface2",    { 0.184f, 0.194f, 0.227f, 0.98f });
        theme->SetToken("Border",      { 0.235f, 0.251f, 0.298f, 0.85f });
        theme->SetToken("Accent",      { 0.259f, 0.529f, 0.961f, 1.00f });
        theme->SetToken("AccentHover", { 0.349f, 0.592f, 0.976f, 1.00f });
        theme->SetToken("Success",     { 0.235f, 0.678f, 0.482f, 1.00f });
        theme->SetToken("Warning",     { 0.851f, 0.616f, 0.224f, 1.00f });
        theme->SetToken("Danger",      { 0.835f, 0.310f, 0.333f, 1.00f });
        theme->SetToken("Text",        { 0.918f, 0.929f, 0.953f, 1.00f });
        theme->SetToken("TextMuted",   { 0.580f, 0.616f, 0.686f, 1.00f });

        UIStyleBox background{};
        background.FillColor = theme->GetToken("Background");
        background.FillColorBottom = background.FillColor;
        background.BorderThickness = 0.0f;
        background.ShadowBlur = 0.0f;
        background.CornerRadius = UICornerRadius::All(0.0f);
        background.Padding = UIThickness::All(0.0f);
        background.TextColor = theme->GetToken("Text");
        background.FontSize = 18.0f;
        theme->SetStyle("Background", background);

        UIStyleBox panel{};
        panel.FillColor = theme->GetToken("Surface0");
        panel.FillColorBottom = panel.FillColor;
        panel.BorderColor = theme->GetToken("Border");
        panel.BorderThickness = 1.0f;
        panel.ShadowColor = { 0.0f, 0.0f, 0.0f, 0.25f };
        panel.ShadowOffset = { 0.0f, 3.0f };
        panel.ShadowBlur = 8.0f;
        panel.CornerRadius = UICornerRadius::All(4.0f);
        panel.Padding = UIThickness::All(10.0f);
        panel.TextColor = theme->GetToken("Text");
        panel.FontSize = 18.0f;
        theme->SetStyle("Panel", panel);

        UIStyleBox topBar = panel;
        topBar.FillColor = theme->GetToken("Surface1");
        topBar.FillColorBottom = topBar.FillColor;
        topBar.CornerRadius = UICornerRadius::All(4.0f);
        topBar.ShadowBlur = 0.0f;
        topBar.Padding = UIThickness::Symmetric(14.0f, 10.0f);
        theme->SetStyle("TopBar", topBar);

        UIStyleBox label{};
        label.TextColor = theme->GetToken("Text");
        label.FontSize = 18.0f;
        theme->SetStyle("Label", label);

        UIStyleBox button{};
        button.FillColor = theme->GetToken("Surface1");
        button.FillColorBottom = button.FillColor;
        button.UseGradient = false;
        button.BorderColor = theme->GetToken("Border");
        button.BorderThickness = 1.0f;
        button.CornerRadius = UICornerRadius::All(3.0f);
        button.Padding = UIThickness::Symmetric(10.0f, 5.0f);
        button.TextColor = theme->GetToken("Text");
        button.FontSize = 18.0f;
        button.TransitionSeconds = 0.06f;
        theme->SetStyle("Button", button);

        UIStyleBox buttonHover = button;
        buttonHover.FillColor = theme->GetToken("Surface2");
        buttonHover.FillColorBottom = buttonHover.FillColor;
        buttonHover.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("Button:hover", buttonHover);

        UIStyleBox buttonPressed = button;
        buttonPressed.FillColor = { 0.196f, 0.392f, 0.710f, 1.0f };
        buttonPressed.FillColorBottom = buttonPressed.FillColor;
        buttonPressed.BorderColor = theme->GetToken("AccentHover");
        theme->SetStyle("Button:pressed", buttonPressed);

        UIStyleBox buttonPrimary = button;
        buttonPrimary.FillColor = theme->GetToken("Accent");
        buttonPrimary.FillColorBottom = buttonPrimary.FillColor;
        buttonPrimary.BorderColor = theme->GetToken("AccentHover");
        buttonPrimary.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        theme->SetStyle("Button.primary", buttonPrimary);

        UIStyleBox buttonPrimaryHover = buttonPrimary;
        buttonPrimaryHover.FillColor = theme->GetToken("AccentHover");
        buttonPrimaryHover.FillColorBottom = buttonPrimaryHover.FillColor;
        theme->SetStyle("Button.primary:hover", buttonPrimaryHover);

        // Inputs & Controls (ImGui-style crisp 3px corners)
        UIStyleBox textInput{};
        textInput.FillColor = { 0.075f, 0.080f, 0.098f, 1.0f };
        textInput.FillColorBottom = textInput.FillColor;
        textInput.BorderColor = theme->GetToken("Border");
        textInput.BorderThickness = 1.0f;
        textInput.CornerRadius = UICornerRadius::All(3.0f);
        textInput.Padding = UIThickness::Symmetric(8.0f, 5.0f);
        textInput.TextColor = theme->GetToken("Text");
        textInput.FontSize = 18.0f;
        theme->SetStyle("TextInput", textInput);

        UIStyleBox textInputHover = textInput;
        textInputHover.BorderColor = theme->GetToken("AccentHover");
        theme->SetStyle("TextInput:hover", textInputHover);

        UIStyleBox textInputFocus = textInput;
        textInputFocus.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("TextInput:focus", textInputFocus);

        UIStyleBox checkBox = textInput;
        checkBox.Padding = UIThickness::All(0.0f);
        theme->SetStyle("CheckBox", checkBox);

        UIStyleBox checkBoxHover = checkBox;
        checkBoxHover.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("CheckBox:hover", checkBoxHover);

        UIStyleBox slider = textInput;
        slider.TextColor = theme->GetToken("TextMuted");
        slider.FontSize = 18.0f;
        theme->SetStyle("Slider", slider);

        UIStyleBox dragScalar = textInput;
        dragScalar.Padding = UIThickness::Symmetric(6.0f, 4.0f);
        dragScalar.FontSize = 18.0f;
        theme->SetStyle("DragScalar", dragScalar);

        UIStyleBox dragScalarHover = dragScalar;
        dragScalarHover.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("DragScalar:hover", dragScalarHover);

        UIStyleBox dragScalarFocus = dragScalar;
        dragScalarFocus.BorderColor = theme->GetToken("AccentHover");
        theme->SetStyle("DragScalar:focus", dragScalarFocus);

        UIStyleBox sectionHeader = button;
        sectionHeader.FillColor = theme->GetToken("Surface1");
        sectionHeader.FillColorBottom = sectionHeader.FillColor;
        sectionHeader.Padding = UIThickness::Symmetric(10.0f, 5.0f);
        theme->SetStyle("SectionHeader", sectionHeader);

        UIStyleBox sectionHeaderHover = sectionHeader;
        sectionHeaderHover.FillColor = theme->GetToken("Surface2");
        sectionHeaderHover.BorderColor = theme->GetToken("Accent");
        theme->SetStyle("SectionHeader:hover", sectionHeaderHover);

        UIStyleBox treeView = textInput;
        treeView.CornerRadius = UICornerRadius::All(3.0f);
        theme->SetStyle("TreeView", treeView);

        UIStyleBox treeRow = button;
        treeRow.FillColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        treeRow.FillColorBottom = treeRow.FillColor;
        treeRow.BorderThickness = 0.0f;
        treeRow.CornerRadius = UICornerRadius::All(2.0f);
        theme->SetStyle("TreeRow", treeRow);

        UIStyleBox treeRowHover = treeRow;
        treeRowHover.FillColor = theme->GetToken("Surface2");
        treeRowHover.FillColorBottom = treeRowHover.FillColor;
        theme->SetStyle("TreeRow:hover", treeRowHover);

        // ImGui-style Tabs (flat bottom, subtle top corners, unmistakable active tab)
        UIStyleBox tabBar{};
        tabBar.FillColor = theme->GetToken("Surface1");
        tabBar.FillColorBottom = tabBar.FillColor;
        tabBar.BorderColor = theme->GetToken("Border");
        tabBar.BorderThickness = 1.0f;
        tabBar.CornerRadius = UICornerRadius::All(0.0f);
        tabBar.Padding = { 6.0f, 4.0f, 6.0f, 0.0f };
        theme->SetStyle("TabBar", tabBar);

        UIStyleBox tab{};
        tab.FillColor = { 0.118f, 0.125f, 0.150f, 1.0f };
        tab.FillColorBottom = tab.FillColor;
        tab.BorderColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        tab.BorderThickness = 1.0f;
        tab.CornerRadius = { 3.0f, 3.0f, 0.0f, 0.0f };
        tab.Padding = UIThickness::Symmetric(12.0f, 5.0f);
        tab.TextColor = theme->GetToken("TextMuted");
        tab.FontSize = 18.0f;
        tab.TransitionSeconds = 0.05f;
        theme->SetStyle("Tab", tab);

        UIStyleBox tabHover = tab;
        tabHover.FillColor = theme->GetToken("Surface2");
        tabHover.FillColorBottom = tabHover.FillColor;
        tabHover.TextColor = theme->GetToken("Text");
        theme->SetStyle("Tab:hover", tabHover);

        UIStyleBox tabActive = tab;
        tabActive.FillColor = { 0.165f, 0.285f, 0.460f, 1.0f };
        tabActive.FillColorBottom = tabActive.FillColor;
        tabActive.BorderColor = theme->GetToken("Accent");
        tabActive.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        theme->SetStyle("Tab.active", tabActive);

        // ImGui-style MenuBar & Dropdown Popups
        UIStyleBox menuBar{};
        menuBar.FillColor = { 0.094f, 0.100f, 0.118f, 1.0f };
        menuBar.FillColorBottom = menuBar.FillColor;
        menuBar.BorderColor = theme->GetToken("Border");
        menuBar.BorderThickness = 1.0f;
        menuBar.CornerRadius = UICornerRadius::All(0.0f);
        menuBar.Padding = UIThickness::Symmetric(6.0f, 2.0f);
        theme->SetStyle("MenuBar", menuBar);

        UIStyleBox menuTrigger{};
        menuTrigger.FillColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        menuTrigger.FillColorBottom = menuTrigger.FillColor;
        menuTrigger.BorderThickness = 0.0f;
        menuTrigger.CornerRadius = UICornerRadius::All(2.0f);
        menuTrigger.Padding = UIThickness::Symmetric(10.0f, 4.0f);
        menuTrigger.TextColor = theme->GetToken("Text");
        menuTrigger.FontSize = 18.0f;
        menuTrigger.TransitionSeconds = 0.04f;
        theme->SetStyle("MenuTrigger", menuTrigger);

        UIStyleBox menuTriggerHover = menuTrigger;
        menuTriggerHover.FillColor = theme->GetToken("Surface2");
        menuTriggerHover.FillColorBottom = menuTriggerHover.FillColor;
        menuTriggerHover.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        theme->SetStyle("MenuTrigger:hover", menuTriggerHover);

        UIStyleBox menuPopup{};
        menuPopup.FillColor = { 0.133f, 0.141f, 0.169f, 0.99f };
        menuPopup.FillColorBottom = menuPopup.FillColor;
        menuPopup.BorderColor = { 0.300f, 0.325f, 0.390f, 1.0f };
        menuPopup.BorderThickness = 1.0f;
        menuPopup.ShadowColor = { 0.0f, 0.0f, 0.0f, 0.55f };
        menuPopup.ShadowOffset = { 0.0f, 6.0f };
        menuPopup.ShadowBlur = 14.0f;
        menuPopup.CornerRadius = UICornerRadius::All(3.0f);
        menuPopup.Padding = UIThickness::All(4.0f);
        menuPopup.TextColor = theme->GetToken("Text");
        menuPopup.FontSize = 18.0f;
        theme->SetStyle("MenuPopup", menuPopup);

        UIStyleBox menuItem{};
        menuItem.FillColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        menuItem.FillColorBottom = menuItem.FillColor;
        menuItem.BorderThickness = 0.0f;
        menuItem.CornerRadius = UICornerRadius::All(2.0f);
        menuItem.Padding = UIThickness::Symmetric(10.0f, 5.0f);
        menuItem.TextColor = theme->GetToken("Text");
        menuItem.FontSize = 18.0f;
        menuItem.TransitionSeconds = 0.03f;
        theme->SetStyle("MenuItem", menuItem);

        UIStyleBox menuItemHover = menuItem;
        menuItemHover.FillColor = theme->GetToken("Accent");
        menuItemHover.FillColorBottom = menuItemHover.FillColor;
        menuItemHover.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        theme->SetStyle("MenuItem:hover", menuItemHover);

        UIStyleBox menuItemPrimary = menuItem;
        menuItemPrimary.FillColor = { 0.180f, 0.340f, 0.580f, 1.0f };
        menuItemPrimary.FillColorBottom = menuItemPrimary.FillColor;
        menuItemPrimary.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        theme->SetStyle("MenuItem.primary", menuItemPrimary);

        UIStyleBox separator{};
        separator.FillColor = theme->GetToken("Border");
        separator.FillColorBottom = separator.FillColor;
        separator.BorderThickness = 0.0f;
        separator.CornerRadius = UICornerRadius::All(0.0f);
        theme->SetStyle("Separator", separator);

        UIStyleBox tooltip = menuPopup;
        tooltip.Padding = UIThickness::Symmetric(10.0f, 6.0f);
        tooltip.FontSize = 18.0f;
        theme->SetStyle("Tooltip", tooltip);

        UIStyleBox titleClose{};
        titleClose.FillColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        titleClose.FillColorBottom = titleClose.FillColor;
        titleClose.BorderThickness = 0.0f;
        titleClose.CornerRadius = UICornerRadius::All(0.0f);
        titleClose.Padding = UIThickness::All(0.0f);
        titleClose.TextColor = theme->GetToken("TextMuted");
        titleClose.FontSize = 16.0f;
        titleClose.TransitionSeconds = 0.05f;
        theme->SetStyle("TitleClose", titleClose);

        UIStyleBox titleCloseHover = titleClose;
        titleCloseHover.FillColor = theme->GetToken("Danger");
        titleCloseHover.FillColorBottom = titleCloseHover.FillColor;
        titleCloseHover.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        theme->SetStyle("TitleClose:hover", titleCloseHover);

        UIStyleBox titleClosePressed = titleCloseHover;
        titleClosePressed.FillColor = { 0.68f, 0.22f, 0.25f, 1.0f };
        titleClosePressed.FillColorBottom = titleClosePressed.FillColor;
        theme->SetStyle("TitleClose:pressed", titleClosePressed);

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
                    UIStyleBox box{};
                    if (const size_t colonPos = key.find(':'); colonPos != std::string::npos)
                    {
                        const std::string baseKey = key.substr(0, colonPos);
                        if (m_Styles.contains(baseKey))
                            box = m_Styles[baseKey];
                        else if (const size_t dotPos = baseKey.find('.'); dotPos != std::string::npos && m_Styles.contains(baseKey.substr(0, dotPos)))
                            box = m_Styles[baseKey.substr(0, dotPos)];
                        else if (m_Styles.contains(key))
                            box = m_Styles[key];
                    }
                    else if (const size_t dotPos = key.find('.'); dotPos != std::string::npos)
                    {
                        const std::string baseKey = key.substr(0, dotPos);
                        if (m_Styles.contains(baseKey))
                            box = m_Styles[baseKey];
                        else if (m_Styles.contains(key))
                            box = m_Styles[key];
                    }
                    else if (m_Styles.contains(key))
                    {
                        box = m_Styles[key];
                    }
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

        const UIThickness basePadding = result.Padding;
        const float baseFontSize = result.FontSize;

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

        // Keep layout metrics stable across hover/focus/pressed/disabled states.
        result.Padding = basePadding;
        result.FontSize = baseFontSize;

        return result;
    }
}
