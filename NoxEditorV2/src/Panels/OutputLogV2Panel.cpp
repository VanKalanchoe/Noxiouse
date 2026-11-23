#include "OutputLogV2Panel.h"
#include <NoxUI/Rendering/UIFont.h>

#include <algorithm>
#include <cctype>
#include <format>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Console/Console.h"
#include "NoxCore/Renderer/Font.h"
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Rendering/UIDrawList.h>
#include <NoxUI/Widgets/UIButton.h>
#include <NoxUI/Widgets/UILabel.h>
#include <NoxUI/Widgets/UIMenu.h>
#include <NoxUI/Widgets/UIScrollBar.h>
#include <NoxUI/Widgets/UISearchBar.h>
#include <NoxUI/Widgets/UITextInput.h>

namespace Nox
{
    namespace
    {
        using NoxUI::UIColor;
        using NoxUI::UIVec2;

        // The four verbosity groups, Unreal's: Error (error, critical), Warning, Display / Log (info), Verbose (debug, trace).
        int LevelGroup(spdlog::level::level_enum level)
        {
            switch (level)
            {
            case spdlog::level::err:
            case spdlog::level::critical: return 0;
            case spdlog::level::warn:     return 1;
            case spdlog::level::info:     return 2;
            default:                      return 3;
            }
        }

        constexpr const char* kGroupNames[] = { "Errors", "Warnings", "Messages", "Verbose" };
        constexpr NoxUI::UIThemeToken kGroupColors[] = {
            { "LogError", { 0.94f, 0.36f, 0.36f, 1.0f } },
            { "LogWarning", { 0.95f, 0.72f, 0.28f, 1.0f } },
            { "LogMessage", { 0.86f, 0.88f, 0.92f, 1.0f } },
            { "LogVerbose", { 0.56f, 0.60f, 0.68f, 1.0f } },
        };

        std::string ToLower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return lower;
        }

        std::string Format(const LogEntry& entry)
        {
            return std::format("[{}] {}: {}", entry.Clock, entry.Logger, entry.Text);
        }
    }

    // ------------------------------------------------------------------------------------------------------------------------------

    // A filter chip: the level's dot, its name and how many lines it has; filled while that level is shown.
    class LogFilterChip final : public NoxUI::UIElement
    {
    public:
        LogFilterChip(int group, std::function<void()> onToggle)
            : m_Group(group), m_OnToggle(std::move(onToggle))
        {
            HitTestMode = NoxUI::UIHitTestMode::Self;
            VAlign = NoxUI::UIAlignment::Center;
        }

        bool On = true;
        size_t Count = 0;

        bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
        {
            if (e.Button != 1)
                return false;
            On = !On;
            if (m_OnToggle)
                m_OnToggle();
            return true;
        }
        bool OnPointerEnter(const NoxUI::UIPointerEvent&) override { m_Hovered = true; return false; }
        bool OnPointerLeave(const NoxUI::UIPointerEvent&) override { m_Hovered = false; return false; }

    protected:
        UIVec2 ComputeDesiredSize() override
        {
            const UIVec2 text = NoxUI::UILabel::MeasureText(Label(), nullptr, kFont);
            return { 10.0f + 8.0f + 6.0f + text.x + 12.0f, 26.0f };
        }

        void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            const NoxUI::UIRect& b = GetBounds();
            const UIColor color = kGroupColors[m_Group];
            const UIColor border = theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f });
            const UIColor text = theme.GetToken("Text", { 0.92f, 0.93f, 0.95f, 1.0f });
            const UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
            const float radius = b.Height() * 0.5f;
            const UIColor fill = On ? UIColor{ color.r * 0.22f + 0.06f, color.g * 0.22f + 0.07f, color.b * 0.22f + 0.09f, 1.0f }
                                    : UIColor{ 0.0f, 0.0f, 0.0f, 0.0f };
            d.AddRoundedRect(b.Min, b.Max, m_Hovered ? UIColor{ fill.r + 0.04f, fill.g + 0.04f, fill.b + 0.05f, 1.0f } : fill,
                             glm::vec4(radius), 1.0f, On ? UIColor{ color.r, color.g, color.b, 0.55f } : border);
            const UIVec2 dot{ b.Min.x + 14.0f, b.Center().y };
            d.AddRoundedRect(dot - UIVec2{ 4.0f }, dot + UIVec2{ 4.0f }, On ? color : UIColor{ color.r, color.g, color.b, 0.35f }, glm::vec4(4.0f));
            const std::string label = Label();
            const UIVec2 size = NoxUI::UILabel::MeasureText(label, nullptr, kFont);
            d.AddText(label, nullptr, { dot.x + 10.0f, b.Center().y - size.y * 0.5f }, kFont, On ? text : muted);
        }

    private:
        std::string Label() const { return std::string(kGroupNames[m_Group]) + "  " + std::to_string(Count); }

        static constexpr float kFont = 14.5f;
        int m_Group;
        std::function<void()> m_OnToggle;
        bool m_Hovered = false;
    };

    // ------------------------------------------------------------------------------------------------------------------------------

    // The lines: virtualised rows (time, category, text), coloured by level, selectable, following the end while at the bottom.
    class LogLinesView final : public NoxUI::UIElement
    {
    public:
        explicit LogLinesView(OutputLogV2Panel& panel)
            : m_Panel(panel)
        {
            HitTestMode = NoxUI::UIHitTestMode::Self;
            Focusable = true;
            ClipChildren = true;
            WidthMode = NoxUI::UISizeMode::Fill;
            HeightMode = NoxUI::UISizeMode::Fill;
        }

        void PinToBottom() { m_PinToBottom = true; }

        bool OnMouseWheel(const NoxUI::UIPointerEvent& e) override
        {
            Sync();
            m_Scroll = std::clamp(m_Scroll - e.ScrollDelta.y * kRow * 3.0f, 0.0f, m_ScrollBar.GetMaxScroll());
            m_PinToBottom = m_Scroll >= m_ScrollBar.GetMaxScroll() - 2.0f;
            m_ScrollBar.NotifyScrolled();
            return true;
        }

        bool OnPointerDown(const NoxUI::UIPointerEvent& e) override
        {
            Sync();
            if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                ctx->SetFocus(this);
            if (e.Button == 1 && m_ScrollBar.OnPointerDown(e.Position, m_Scroll))
            {
                m_PinToBottom = m_Scroll >= m_ScrollBar.GetMaxScroll() - 2.0f;
                if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                    ctx->CancelPendingDrag();
                return true;
            }
            const int row = RowAt(e.Position);
            std::vector<uint64_t>& selected = m_Panel.m_Selected;
            if (e.Button == 3)
            {
                if (row >= 0)
                {
                    const uint64_t sequence = SequenceOf(row);
                    if (!std::binary_search(selected.begin(), selected.end(), sequence))
                    {
                        selected = { sequence };
                        m_Panel.m_SelectionAnchor = sequence;
                    }
                }
                if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                {
                    std::vector<NoxUI::UIMenuItem> items;
                    items.push_back({ "Copy", "Ctrl+C", [this]() { m_Panel.CopyLines(true); }, false, !selected.empty() });
                    items.push_back({ "Copy All Shown", "", [this]() { m_Panel.CopyLines(false); } });
                    items.push_back({ "", "", {}, true });
                    items.push_back({ "Clear", "", [this]() { m_Panel.Clear(); }, false, true, false, false, "Hides what is listed now (Nox.log keeps it)." });
                    NoxUI::UIMenuPopup::ShowPopupMenu(*ctx, { e.Position, e.Position + UIVec2{ 1.0f } }, std::move(items));
                }
                return true;
            }
            if (e.Button != 1)
                return false;
            if (row < 0)
            {
                selected.clear();
                return true;
            }
            const uint64_t sequence = SequenceOf(row);
            const uint16_t mods = e.Modifiers;
            if ((mods & NoxUI::UIMod::Shift) != 0 && m_Panel.m_SelectionAnchor != 0)
            {
                const uint64_t from = std::min(sequence, m_Panel.m_SelectionAnchor), to = std::max(sequence, m_Panel.m_SelectionAnchor);
                selected.clear();
                for (const auto& r : m_Panel.m_Rows)
                {
                    const uint64_t s = m_Panel.m_Entries[r.Entry].Sequence;
                    if (s >= from && s <= to && (selected.empty() || selected.back() != s))
                        selected.push_back(s);
                }
            }
            else if ((mods & NoxUI::UIMod::Ctrl) != 0)
            {
                const auto it = std::lower_bound(selected.begin(), selected.end(), sequence);
                if (it != selected.end() && *it == sequence)
                    selected.erase(it);
                else
                    selected.insert(it, sequence);
                m_Panel.m_SelectionAnchor = sequence;
            }
            else
            {
                selected = { sequence };
                m_Panel.m_SelectionAnchor = sequence;
            }
            return true;
        }

        bool OnPointerMove(const NoxUI::UIPointerEvent& e) override
        {
            if (m_ScrollBar.OnPointerMove(e.Position, m_Scroll))
            {
                m_PinToBottom = m_Scroll >= m_ScrollBar.GetMaxScroll() - 2.0f;
                return true;
            }
            m_HoveredRow = m_ScrollBar.Contains(e.Position) ? -1 : RowAt(e.Position);
            return false;
        }

        bool OnPointerUp(const NoxUI::UIPointerEvent& e) override
        {
            return e.Button == 1 && m_ScrollBar.OnPointerUp();
        }

        bool OnPointerLeave(const NoxUI::UIPointerEvent&) override
        {
            m_HoveredRow = -1;
            return false;
        }

        bool OnKeyDown(const NoxUI::UIKeyEvent& e) override
        {
            const bool ctrl = (e.Modifiers & NoxUI::UIMod::Ctrl) != 0;
            if (ctrl && e.KeyCode == SDL_SCANCODE_C)
            {
                m_Panel.CopyLines(true);
                return true;
            }
            if (ctrl && e.KeyCode == SDL_SCANCODE_A)
            {
                m_Panel.m_Selected.clear();
                for (const auto& r : m_Panel.m_Rows)
                {
                    const uint64_t s = m_Panel.m_Entries[r.Entry].Sequence;
                    if (m_Panel.m_Selected.empty() || m_Panel.m_Selected.back() != s)
                        m_Panel.m_Selected.push_back(s);
                }
                return true;
            }
            if (e.KeyCode == SDL_SCANCODE_END)
            {
                m_PinToBottom = true;
                return true;
            }
            if (e.KeyCode == SDL_SCANCODE_HOME)
            {
                m_PinToBottom = false;
                m_Scroll = 0.0f;
                return true;
            }
            return false;
        }

    protected:
        UIVec2 ComputeDesiredSize() override { return { 240.0f, 80.0f }; }

        void OnUpdate(float dt) override
        {
            Sync();
            bool hovered = false;
            if (const NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent())
                hovered = ctx->GetHovered() == this && m_ScrollBar.Contains(ctx->GetLastMousePos());
            m_ScrollBar.Update(dt, hovered);
        }

        void OnPaint(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            Sync();
            const NoxUI::UIRect& b = GetBounds();
            d.AddRoundedRect(b.Min, b.Max, theme.GetToken("Background", { 0.063f, 0.075f, 0.106f, 1.0f }), glm::vec4(4.0f), 1.0f,
                             theme.GetToken("Border", { 0.20f, 0.235f, 0.32f, 1.0f }));
            if (m_PinToBottom)
                m_Scroll = m_ScrollBar.GetMaxScroll();
            m_Scroll = std::clamp(m_Scroll, 0.0f, m_ScrollBar.GetMaxScroll());

            const auto& rows = m_Panel.m_Rows;
            const UIColor muted = theme.GetToken("TextMuted", { 0.58f, 0.62f, 0.69f, 1.0f });
            const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });
            if (rows.empty())
            {
                const std::string message = m_Panel.m_Entries.empty() ? "Nothing logged yet" : "No lines match the filters";
                d.AddText(message, nullptr, { b.Min.x + 12.0f, b.Min.y + 10.0f }, kFont, muted);
                return;
            }

            const float clockW = NoxUI::UILabel::MeasureText("00:00:00", nullptr, kFont).x + 12.0f;
            const float loggerW = NoxUI::UILabel::MeasureText("APP", nullptr, kFont).x + 14.0f;
            d.PushClipRect({ b.Min.x + 1.0f, b.Min.y + 1.0f }, { b.Max.x - m_ScrollBar.GetReservedWidth() - 1.0f, b.Max.y - 1.0f });
            const int first = std::max(0, static_cast<int>((m_Scroll - kTop) / kRow));
            const int last = std::min(static_cast<int>(rows.size()) - 1, static_cast<int>((m_Scroll + b.Height()) / kRow) + 1);
            for (int i = first; i <= last; ++i)
            {
                const auto& row = rows[static_cast<size_t>(i)];
                const LogEntry& entry = m_Panel.m_Entries[row.Entry];
                const float y = b.Min.y + kTop + static_cast<float>(i) * kRow - m_Scroll;
                const bool selected = std::binary_search(m_Panel.m_Selected.begin(), m_Panel.m_Selected.end(), entry.Sequence);
                const int group = LevelGroup(entry.Level);
                if (selected)
                    d.AddRoundedRect({ b.Min.x + 2.0f, y }, { b.Max.x - 2.0f, y + kRow }, { accent.r, accent.g, accent.b, 0.26f });
                else if (i == m_HoveredRow)
                    d.AddRoundedRect({ b.Min.x + 2.0f, y }, { b.Max.x - 2.0f, y + kRow }, NoxUI::ThemeColor("LogRowHover", { 1.0f, 1.0f, 1.0f, 0.04f }));
                else if (group <= 1)
                    d.AddRoundedRect({ b.Min.x + 2.0f, y }, { b.Max.x - 2.0f, y + kRow }, { kGroupColors[group].Get().r, kGroupColors[group].Get().g, kGroupColors[group].Get().b, 0.07f });

                const float textY = y + (kRow - kFont * 1.2f) * 0.5f;
                const bool firstLine = row.Begin == 0;
                float x = b.Min.x + 8.0f;
                if (firstLine)
                {
                    d.AddText(entry.Clock, nullptr, { x, textY }, kFont, muted);
                    d.AddText(entry.Logger, nullptr, { x + clockW, textY }, kFont, muted);
                }
                x += clockW + loggerW;
                const std::string_view text = std::string_view(entry.Text).substr(row.Begin, row.End - row.Begin);
                d.AddText(std::string(text), nullptr, { x, textY }, kFont, kGroupColors[group]);
            }
            d.PopClipRect();
        }

        void OnPaintOverlay(NoxUI::UIDrawList& d, const NoxUI::UITheme& theme) override
        {
            m_ScrollBar.Paint(d, theme, m_Scroll);
        }

    private:
        static constexpr float kRow = 22.0f;
        static constexpr float kTop = 3.0f;
        static constexpr float kFont = 15.0f;

        void Sync()
        {
            const NoxUI::UIRect& b = GetBounds();
            m_ScrollBar.SetGeometry(b, kTop * 2.0f + static_cast<float>(m_Panel.m_Rows.size()) * kRow, std::max(0.0f, b.Height()));
        }

        int RowAt(const UIVec2& p) const
        {
            const NoxUI::UIRect& b = GetBounds();
            if (!b.Contains(p))
                return -1;
            const int row = static_cast<int>((p.y - b.Min.y - kTop + m_Scroll) / kRow);
            return (row >= 0 && row < static_cast<int>(m_Panel.m_Rows.size())) ? row : -1;
        }

        uint64_t SequenceOf(int row) const { return m_Panel.m_Entries[m_Panel.m_Rows[static_cast<size_t>(row)].Entry].Sequence; }

        OutputLogV2Panel& m_Panel;
        NoxUI::UIScrollBar m_ScrollBar;
        float m_Scroll = 0.0f;
        bool m_PinToBottom = true;
        int m_HoveredRow = -1;
    };

    // ------------------------------------------------------------------------------------------------------------------------------

    // The Cmd line: Enter runs, Tab completes the first word, Up / Down walk the history.
    class ConsoleLine final : public NoxUI::UITextInput
    {
    public:
        explicit ConsoleLine(OutputLogV2Panel& panel)
            : NoxUI::UITextInput({}, "Console command (Tab completes, Up / Down history, 'help' lists them)"), m_Panel(panel)
        {
            CapturesTab = true;
            WidthMode = NoxUI::UISizeMode::Fill;
            OnSubmit = [this](const std::string& line)
            {
                m_Panel.RunCommand(line);
                Text.clear();
                m_Panel.m_HistoryIndex = -1;
                m_Panel.UpdateSuggestions();
            };
            OnChanged = [this](const std::string&) { m_Panel.UpdateSuggestions(); };
        }

        bool OnKeyDown(const NoxUI::UIKeyEvent& e) override
        {
            auto& history = m_Panel.m_History;
            int& index = m_Panel.m_HistoryIndex;
            if ((e.KeyCode == SDL_SCANCODE_UP || e.KeyCode == SDL_SCANCODE_DOWN) && !history.empty())
            {
                const int count = static_cast<int>(history.size());
                if (e.KeyCode == SDL_SCANCODE_UP)
                    index = index < 0 ? count - 1 : std::max(0, index - 1);
                else
                    index = index < 0 ? -1 : (index + 1 < count ? index + 1 : -1);
                SetLine(index >= 0 ? history[static_cast<size_t>(index)] : std::string());
                return true;
            }
            if (e.KeyCode == SDL_SCANCODE_TAB)
            {
                // The first word: completed to the longest prefix the names share (one name: completed with a space after it).
                if (Text.find(' ') == std::string::npos && !Text.empty())
                {
                    const std::vector<std::string> names = Console::Get().Complete(Text);
                    if (!names.empty())
                    {
                        std::string common = names.front();
                        for (const std::string& name : names)
                        {
                            size_t length = 0;
                            while (length < common.size() && length < name.size() &&
                                   std::tolower(static_cast<unsigned char>(common[length])) == std::tolower(static_cast<unsigned char>(name[length])))
                                ++length;
                            common.resize(length);
                        }
                        if (names.size() == 1)
                            common += ' ';
                        if (common.size() >= Text.size())
                            SetLine(common);
                    }
                }
                return true;
            }
            return NoxUI::UITextInput::OnKeyDown(e);
        }

    private:
        void SetLine(const std::string& line)
        {
            Text = line;
            // The cursor goes to the end: End is what the field does with it.
            NoxUI::UITextInput::OnKeyDown({ SDL_SCANCODE_END, 0, false, false });
            m_Panel.UpdateSuggestions();
        }

        OutputLogV2Panel& m_Panel;
    };

    // ------------------------------------------------------------------------------------------------------------------------------

    OutputLogV2Panel::OutputLogV2Panel() = default;
    OutputLogV2Panel::~OutputLogV2Panel() = default;

    Ref<NoxUI::UIElement> OutputLogV2Panel::BuildUI()
    {
        Ref<NoxUI::UIStackBox> toolbar(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 6.0f));
        toolbar->WidthMode = NoxUI::UISizeMode::Fill;
        toolbar->Padding = NoxUI::UIThickness::Symmetric(6.0f, 5.0f);
        toolbar->VAlign = NoxUI::UIAlignment::Center;

        Ref<NoxUI::UISearchBar> search(new NoxUI::UISearchBar("Search the log (every word must appear)", [this](const std::string& query)
        {
            m_SearchWords.clear();
            std::string word;
            for (const char ch : ToLower(query) + " ")
            {
                if (ch == ' ')
                {
                    if (!word.empty())
                        m_SearchWords.push_back(std::move(word));
                    word.clear();
                }
                else
                {
                    word.push_back(ch);
                }
            }
            Refilter();
        }));
        search->WidthMode = NoxUI::UISizeMode::Fill;
        search->HeightMode = NoxUI::UISizeMode::Fixed;
        search->FixedSize = { 0.0f, 26.0f };
        search->VAlign = NoxUI::UIAlignment::Center;
        toolbar->AddChild(search);

        for (int group = 0; group < 4; ++group)
        {
            m_Chips[group] = Ref<LogFilterChip>(new LogFilterChip(group, [this, group]()
            {
                m_Show[group] = m_Chips[group]->On;
                Refilter();
            }));
            m_Chips[group]->On = m_Show[group];
            const size_t counts[4] = { m_ErrorCount, m_WarningCount, m_InfoCount, m_VerboseCount };
            m_Chips[group]->Count = counts[group];
            m_Chips[group]->Tooltip = std::string("Show or hide ") + kGroupNames[group] + " (the number counts them since Clear)";
            toolbar->AddChild(m_Chips[group]);
        }

        auto button = [](const std::string& text, std::function<void()> onClick, const std::string& tooltip)
        {
            Ref<NoxUI::UIButton> b(new NoxUI::UIButton(text, std::move(onClick)));
            b->Padding = NoxUI::UIThickness::Symmetric(10.0f, 2.0f);
            b->HeightMode = NoxUI::UISizeMode::Fixed;
            b->FixedSize = { 0.0f, 26.0f };
            b->Tooltip = tooltip;
            b->VAlign = NoxUI::UIAlignment::Center;
            return b;
        };
        toolbar->AddChild(button("Copy", [this]() { CopyLines(!m_Selected.empty()); }, "Copies the selected lines (all shown lines when none is selected)"));
        toolbar->AddChild(button("Clear", [this]() { Clear(); }, "Hides what is listed now; Nox.log keeps everything"));
        if (m_ToolbarExtra)
        {
            m_ToolbarExtra->VAlign = NoxUI::UIAlignment::Center;
            toolbar->AddChild(m_ToolbarExtra);
        }

        m_Lines = Ref<LogLinesView>(new LogLinesView(*this));
        m_Lines->Margin = NoxUI::UIThickness{ 6.0f, 0.0f, 6.0f, 0.0f };

        m_Suggestions = Ref<NoxUI::UILabel>(new NoxUI::UILabel("", 14.0f));
        m_Suggestions->UseColorOverride = true;
        m_Suggestions->ColorOverride = NoxUI::ThemeColor("LogSuggestions", { 0.62f, 0.68f, 0.78f, 1.0f });
        m_Suggestions->Margin = NoxUI::UIThickness{ 50.0f, 4.0f, 8.0f, 0.0f };
        m_Suggestions->Visible = false;

        Ref<NoxUI::UIStackBox> commandRow(new NoxUI::UIStackBox(NoxUI::UIOrientation::Horizontal, 8.0f));
        commandRow->WidthMode = NoxUI::UISizeMode::Fill;
        commandRow->Padding = NoxUI::UIThickness{ 6.0f, 5.0f, 6.0f, 6.0f };
        commandRow->VAlign = NoxUI::UIAlignment::Center;
        Ref<NoxUI::UILabel> cmd(new NoxUI::UILabel("Cmd", 15.0f));
        cmd->VAlign = NoxUI::UIAlignment::Center;
        cmd->Tooltip = "Engine console commands and variables: 'help' lists them, 'help <name>' explains one";
        commandRow->AddChild(cmd);
        m_Command = Ref<ConsoleLine>(new ConsoleLine(*this));
        commandRow->AddChild(m_Command);

        Ref<NoxUI::UIStackBox> root(new NoxUI::UIStackBox(NoxUI::UIOrientation::Vertical, 0.0f));
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->AddChild(toolbar);
        root->AddChild(m_Lines);
        root->AddChild(m_Suggestions);
        root->AddChild(commandRow);
        m_Root = root;
        Refilter();
        return m_Root;
    }

    void OutputLogV2Panel::Update(float)
    {
        std::vector<LogEntry> fresh;
        const uint64_t latest = LogBuffer::Get().CopySince(m_LastSequence, fresh);
        if (fresh.empty())
            return;
        m_LastSequence = latest;
        for (LogEntry& entry : fresh)
        {
            switch (LevelGroup(entry.Level))
            {
            case 0: ++m_ErrorCount; break;
            case 1: ++m_WarningCount; break;
            case 2: ++m_InfoCount; break;
            default: ++m_VerboseCount; break;
            }
            m_Entries.push_back(std::move(entry));
        }
        if (m_Entries.size() > LogBuffer::Capacity)
        {
            m_Entries.erase(m_Entries.begin(), m_Entries.begin() + static_cast<std::ptrdiff_t>(m_Entries.size() - LogBuffer::Capacity));
            Refilter();
        }
        else
        {
            for (size_t i = m_Entries.size() - fresh.size(); i < m_Entries.size(); ++i)
                AddRows(static_cast<uint32_t>(i));
        }
        const size_t counts[4] = { m_ErrorCount, m_WarningCount, m_InfoCount, m_VerboseCount };
        for (int group = 0; group < 4; ++group)
        {
            if (m_Chips[group])
                m_Chips[group]->Count = counts[group];
        }
    }

    bool OutputLogV2Panel::Passes(const LogEntry& entry) const
    {
        if (entry.Sequence <= m_ClearedAt || !m_Show[LevelGroup(entry.Level)])
            return false;
        if (m_SearchWords.empty())
            return true;
        const std::string line = ToLower(entry.Logger + " " + entry.Text);
        return std::all_of(m_SearchWords.begin(), m_SearchWords.end(), [&line](const std::string& word) { return line.find(word) != std::string::npos; });
    }

    void OutputLogV2Panel::AddRows(uint32_t entryIndex)
    {
        const LogEntry& entry = m_Entries[entryIndex];
        if (!Passes(entry))
            return;
        uint32_t begin = 0;
        const uint32_t size = static_cast<uint32_t>(entry.Text.size());
        while (true)
        {
            const size_t newline = entry.Text.find('\n', begin);
            const uint32_t end = newline == std::string::npos ? size : static_cast<uint32_t>(newline);
            m_Rows.push_back({ entryIndex, begin, end });
            if (newline == std::string::npos || end + 1 >= size)
                break;
            begin = end + 1;
        }
    }

    void OutputLogV2Panel::Refilter()
    {
        m_Rows.clear();
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_Entries.size()); ++i)
            AddRows(i);
        // Selected lines that were dropped from the list stay out of a copy.
        std::erase_if(m_Selected, [this](uint64_t sequence)
        {
            return std::none_of(m_Rows.begin(), m_Rows.end(), [&](const Row& row) { return m_Entries[row.Entry].Sequence == sequence; });
        });
    }

    void OutputLogV2Panel::Clear()
    {
        m_ClearedAt = m_LastSequence;
        m_ErrorCount = m_WarningCount = m_InfoCount = m_VerboseCount = 0;
        for (const Ref<LogFilterChip>& chip : m_Chips)
        {
            if (chip)
                chip->Count = 0;
        }
        m_Selected.clear();
        m_SelectionAnchor = 0;
        Refilter();
        if (m_Lines)
            m_Lines->PinToBottom();
    }

    void OutputLogV2Panel::CopyLines(bool selectedOnly) const
    {
        std::string text;
        uint32_t lastEntry = UINT32_MAX;
        for (const Row& row : m_Rows)
        {
            if (row.Entry == lastEntry)
                continue; // one entry once, all its lines
            lastEntry = row.Entry;
            const LogEntry& entry = m_Entries[row.Entry];
            if (selectedOnly && !std::binary_search(m_Selected.begin(), m_Selected.end(), entry.Sequence))
                continue;
            text += Format(entry);
            text += '\n';
        }
        if (!text.empty())
            SDL_SetClipboardText(text.c_str());
    }

    void OutputLogV2Panel::UpdateSuggestions()
    {
        if (!m_Suggestions || !m_Command)
            return;
        const std::string& typed = m_Command->Text;
        if (typed == m_SuggestionsFor)
            return;
        m_SuggestionsFor = typed;
        if (typed.empty() || typed.find(' ') != std::string::npos)
        {
            m_Suggestions->Visible = false;
            return;
        }
        const std::vector<std::string> names = Console::Get().Complete(typed);
        if (names.empty())
        {
            m_Suggestions->Visible = false;
            return;
        }
        // The names that fit, with a variable's value or a command's usage; Tab narrows.
        std::string text;
        const size_t shown = std::min<size_t>(names.size(), 6);
        for (size_t i = 0; i < shown; ++i)
        {
            const std::string& name = names[i];
            if (!text.empty())
                text += "     ";
            if (const ConsoleVariable* variable = Console::Get().FindVariable(name))
                text += name + " = " + variable->GetString();
            else if (const Console::CommandInfo* command = Console::Get().FindCommand(name))
                text += command->Usage.empty() ? name : command->Usage;
            else
                text += name;
        }
        if (names.size() > shown)
            text += "     ... " + std::to_string(names.size() - shown) + " more (Tab narrows)";
        m_Suggestions->Text = text;
        m_Suggestions->Visible = true;
    }

    void OutputLogV2Panel::RunCommand(const std::string& rawLine)
    {
        const size_t first = rawLine.find_first_not_of(" \t");
        const std::string line = first == std::string::npos ? std::string() : rawLine.substr(first);
        if (line.empty())
            return;
        if (m_History.empty() || m_History.back() != line)
            m_History.push_back(line);
        if (m_History.size() > 100)
            m_History.erase(m_History.begin());
        Console::Get().Execute(line); // its lines arrive in the log
        if (m_Lines)
            m_Lines->PinToBottom();
    }

    void OutputLogV2Panel::FocusCommandLine()
    {
        if (NoxUI::UIContext* ctx = NoxUI::UIContext::GetCurrent(); ctx && m_Command)
            ctx->SetFocus(m_Command.get());
    }
}
