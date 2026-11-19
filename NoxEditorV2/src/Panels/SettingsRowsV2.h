#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Core/Ref.h"

namespace Nox
{
    namespace UI
    {
        class UIContext;
        class UIElement;
        class UIStackBox;
    }

    // Rows of live settings in a panel (Renderer Settings, Mixer, Levels ...): each row reads its value through a getter every
    // frame and writes through a setter, so a value changed elsewhere (a preset, the console, MCP) shows at once -- the panel is
    // built once, not every frame. A row can be shown only while a condition holds (ShowWhen) and greyed out with the reason
    // next to it while another does not (EnableWhen) -- the old panels' BeginDisabled / "(why)" text.
    //
    // Sync() runs the rows' updates; call it once a frame while the panel is visible.
    class SettingsRowsV2
    {
    public:
        using Cond = std::function<bool()>;

        SettingsRowsV2(Ref<UI::UIStackBox> body, std::vector<std::function<void()>>* syncs, UI::UIContext* context);

        // A collapsible card (open state kept in `expanded` by title, so a rebuild keeps it); its rows go into the returned builder.
        SettingsRowsV2 Section(const std::string& title, bool openByDefault, std::unordered_map<std::string, bool>* expanded,
                               const std::string& tooltip = {});
        // Rows indented under the last one (a setting's details), in a box of their own the conditions can show or hide at once.
        SettingsRowsV2 Group();

        Ref<UI::UIElement> Check(const std::string& label, std::function<bool()> get, std::function<void(bool)> set, const std::string& tooltip = {});
        // A field with a fill bar between min and max (drag across it or click to type).
        Ref<UI::UIElement> Slider(const std::string& label, std::function<float()> get, std::function<void(float)> set, float min, float max,
                                  const char* format = "{:.2f}", const std::string& tooltip = {});
        Ref<UI::UIElement> Drag(const std::string& label, std::function<float()> get, std::function<void(float)> set, float speed,
                                float min = 0.0f, float max = 0.0f, const char* format = "{:.2f}", const std::string& tooltip = {});
        // applyOnRelease: `set` runs once when a drag is let go or a value typed, not on every step (a pool made again).
        Ref<UI::UIElement> Int(const std::string& label, std::function<int()> get, std::function<void(int)> set, int min, int max,
                               const std::string& tooltip = {}, bool applyOnRelease = false);
        Ref<UI::UIElement> Choice(const std::string& label, std::vector<std::string> items, std::function<int()> get, std::function<void(int)> set,
                                  const std::string& tooltip = {});
        // A colour and a text field over a value whose address stays put (a renderer's settings, a component edited in place).
        Ref<UI::UIElement> Color3(const std::string& label, glm::vec3* target, std::function<void()> onChange = {}, const std::string& tooltip = {});
        Ref<UI::UIElement> String(const std::string& label, std::string* target, const std::string& placeholder = {}, std::function<void()> onChange = {},
                                  const std::string& tooltip = {});
        // Text that follows a value (stats); `color` the text's.
        Ref<UI::UIElement> Text(std::function<std::string()> text, const glm::vec4& color = { 0.60f, 0.66f, 0.76f, 1.0f });
        Ref<UI::UIElement> Note(const std::string& text, const glm::vec4& color = { 0.60f, 0.66f, 0.76f, 1.0f });
        Ref<UI::UIElement> Button(const std::string& label, std::function<void()> onClick, const std::string& tooltip = {});
        Ref<UI::UIElement> Header(const std::string& title, const std::string& tooltip = {});
        Ref<UI::UIElement> Separator();

        // The element is shown only while `when` holds.
        void ShowWhen(const Ref<UI::UIElement>& element, Cond when);
        // The element is greyed out while `when` does not hold; `reason` (when given) says why, after the row's label.
        void EnableWhen(const Ref<UI::UIElement>& element, Cond when, std::function<std::string()> reason = {});

        const Ref<UI::UIStackBox>& GetBody() const { return m_Body; }
        UI::UIContext* GetContext() const { return m_Context; }

    private:
        Ref<UI::UIElement> AddRow(const std::string& label, const Ref<UI::UIElement>& editor, const std::string& tooltip);

        Ref<UI::UIStackBox> m_Body;
        std::vector<std::function<void()>>* m_Syncs = nullptr;
        UI::UIContext* m_Context = nullptr;
    };
}
