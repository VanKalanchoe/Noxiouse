#include "UILogView.h"

#include <algorithm>
#include <cctype>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty())
                return true;
            auto it = std::search(
                haystack.begin(), haystack.end(),
                needle.begin(), needle.end(),
                [](char a, char b)
                {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }
            );
            return it != haystack.end();
        }

        class UILogListSurface : public UIElement
        {
        public:
            UILogListSurface()
            {
                StyleClass = "TextInput";
                DrawBackground = true;
                ClipChildren = true;
                HitTestMode = UIHitTestMode::Self;
                WidthMode = UISizeMode::Fill;
                HeightMode = UISizeMode::Fill;
                MinSize = { 200.0f, 120.0f };
            }

            void SetFiltered(std::vector<UILogEntry> entries, bool autoScroll)
            {
                m_Filtered = std::move(entries);
                if (autoScroll)
                    m_PinToBottom = true;
            }

            bool OnMouseWheel(const UIPointerEvent& e) override
            {
                const float viewH = std::max(0.0f, GetBounds().Height());
                const float totalH = static_cast<float>(m_Filtered.size()) * kRowHeight;
                const float maxScroll = std::max(0.0f, totalH - viewH);
                m_ScrollOffset = std::clamp(m_ScrollOffset - e.ScrollDelta.y * 36.0f, 0.0f, maxScroll);
                m_PinToBottom = (m_ScrollOffset >= maxScroll - 2.0f);
                return true;
            }

        protected:
            UIVec2 ComputeDesiredSize() override { return MinSize; }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                UIElement::OnPaint(drawList, theme);

                const UIRect& b = GetBounds();
                const float viewH = std::max(0.0f, b.Height());
                const float totalH = static_cast<float>(m_Filtered.size()) * kRowHeight;
                const float maxScroll = std::max(0.0f, totalH - viewH);
                if (m_PinToBottom)
                    m_ScrollOffset = maxScroll;
                else
                    m_ScrollOffset = std::clamp(m_ScrollOffset, 0.0f, maxScroll);

                drawList.PushClipRect(b.Min, b.Max);
                const Ref<Font> font = Font::GetDefault();

                const int first = std::max(0, static_cast<int>(m_ScrollOffset / kRowHeight));
                const int last = std::min(
                    static_cast<int>(m_Filtered.size()) - 1,
                    static_cast<int>((m_ScrollOffset + viewH) / kRowHeight) + 1
                );

                for (int i = first; i <= last; ++i)
                {
                    const float y = b.Min.y + static_cast<float>(i) * kRowHeight - m_ScrollOffset;
                    const auto& entry = m_Filtered[i];

                    UIColor color{ 0.82f, 0.86f, 0.92f, 1.0f };
                    const char* tag = "[INFO]";
                    if (entry.Level == UILogLevel::Warning)
                    {
                        color = { 0.96f, 0.78f, 0.28f, 1.0f };
                        tag = "[WARN]";
                    }
                    else if (entry.Level == UILogLevel::Error)
                    {
                        color = { 0.96f, 0.36f, 0.36f, 1.0f };
                        tag = "[ERR ]";
                    }

                    drawList.AddText(tag, font, { b.Min.x + 8.0f, y + 3.0f }, 12.5f, color);
                    drawList.AddText(entry.Message, font, { b.Min.x + 58.0f, y + 3.0f }, 12.5f, { 0.88f, 0.91f, 0.96f, 1.0f });
                }

                if (maxScroll > 0.0f && viewH > 12.0f)
                {
                    const float thumbH = std::max(20.0f, (viewH / totalH) * viewH);
                    const float thumbY = b.Min.y + (m_ScrollOffset / maxScroll) * (viewH - thumbH);
                    drawList.AddRoundedRect(
                        { b.Max.x - 6.0f, thumbY + 2.0f },
                        { b.Max.x - 2.0f, thumbY + thumbH - 2.0f },
                        { 0.42f, 0.47f, 0.56f, 0.75f },
                        glm::vec4(2.0f)
                    );
                }

                drawList.PopClipRect();
            }

        private:
            static constexpr float kRowHeight = 20.0f;
            std::vector<UILogEntry> m_Filtered;
            float m_ScrollOffset = 0.0f;
            bool m_PinToBottom = true;
        };
    }

    UILogView::UILogView()
        : UIStackBox(UIOrientation::Vertical, 6.0f)
    {
        WidthMode = UISizeMode::Fill;
        MinSize = { 240.0f, 180.0f };

        auto toolbar = Ref<UIStackBox>(new UIStackBox(UIOrientation::Horizontal, 6.0f));
        auto filterInput = Ref<UITextInput>(new UITextInput("", "Filter logs..."));
        filterInput->WidthMode = UISizeMode::Fill;
        filterInput->OnChanged = [this](const std::string& text)
        {
            m_Filter = text;
            RebuildFiltered();
        };

        auto clearBtn = Ref<UIButton>(new UIButton("Clear", [this]() { Clear(); }));
        toolbar->AddChild(filterInput);
        toolbar->AddChild(clearBtn);
        AddChild(toolbar);

        m_ListSurface = Ref<UILogListSurface>(new UILogListSurface());
        AddChild(m_ListSurface);
    }

    void UILogView::AddLog(UILogLevel level, std::string message)
    {
        m_Entries.push_back({ level, std::move(message) });
        if (m_Entries.size() > MaxEntries && MaxEntries > 0)
            m_Entries.erase(m_Entries.begin(), m_Entries.begin() + (m_Entries.size() - MaxEntries));
        RebuildFiltered();
    }

    void UILogView::Clear()
    {
        m_Entries.clear();
        RebuildFiltered();
    }

    void UILogView::RebuildFiltered()
    {
        std::vector<UILogEntry> filtered;
        filtered.reserve(m_Entries.size());
        for (const auto& e : m_Entries)
        {
            if (ContainsCaseInsensitive(e.Message, m_Filter))
                filtered.push_back(e);
        }
        if (auto* surface = static_cast<UILogListSurface*>(m_ListSurface.get()))
            surface->SetFiltered(std::move(filtered), AutoScroll);
    }
}
