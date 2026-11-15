#pragma once

#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Layout/UIGroupBox.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Layout/UIWrapBox.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISeparator.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    // Zero-overhead fluent wrapper around Ref<T> for declarative UI construction.
    template <typename T>
    class UIWidget
    {
    public:
        explicit UIWidget(Ref<T> element) : m_Element(std::move(element)) {}

        T* operator->() const { return m_Element.get(); }
        T& operator*() const { return *m_Element; }
        const Ref<T>& Get() const { return m_Element; }

        template <typename U, typename = std::enable_if_t<std::is_base_of_v<U, T>>>
        operator Ref<U>() const { return Ref<U>(m_Element); }

        UIWidget& Padding(float all) { m_Element->Padding = UIThickness::All(all); return *this; }
        UIWidget& Padding(float h, float v) { m_Element->Padding = UIThickness::Symmetric(h, v); return *this; }
        UIWidget& Padding(const UIThickness& t) { m_Element->Padding = t; return *this; }

        UIWidget& Margin(float all) { m_Element->Margin = UIThickness::All(all); return *this; }
        UIWidget& Margin(float h, float v) { m_Element->Margin = UIThickness::Symmetric(h, v); return *this; }
        UIWidget& Margin(const UIThickness& t) { m_Element->Margin = t; return *this; }

        UIWidget& Fill(float weight = 1.0f)
        {
            m_Element->WidthMode = UISizeMode::Fill;
            m_Element->HeightMode = UISizeMode::Fill;
            m_Element->FlexWeight = weight;
            return *this;
        }

        UIWidget& FillWidth(float weight = 1.0f)
        {
            m_Element->WidthMode = UISizeMode::Fill;
            m_Element->FlexWeight = weight;
            return *this;
        }

        UIWidget& FillHeight(float weight = 1.0f)
        {
            m_Element->HeightMode = UISizeMode::Fill;
            m_Element->FlexWeight = weight;
            return *this;
        }

        UIWidget& FixedSize(float w, float h)
        {
            m_Element->WidthMode = UISizeMode::Fixed;
            m_Element->HeightMode = UISizeMode::Fixed;
            m_Element->FixedSize = { w, h };
            return *this;
        }

        UIWidget& MinSize(float w, float h) { m_Element->MinSize = { w, h }; return *this; }
        UIWidget& MaxSize(float w, float h) { m_Element->MaxSize = { w, h }; return *this; }

        UIWidget& Align(UIAlignment h, UIAlignment v) { m_Element->HAlign = h; m_Element->VAlign = v; return *this; }
        UIWidget& HAlign(UIAlignment h) { m_Element->HAlign = h; return *this; }
        UIWidget& VAlign(UIAlignment v) { m_Element->VAlign = v; return *this; }

        UIWidget& Style(std::string styleClass, std::string variant = {})
        {
            m_Element->StyleClass = std::move(styleClass);
            m_Element->StyleVariant = std::move(variant);
            return *this;
        }

        UIWidget& Primary() { m_Element->StyleVariant = "primary"; return *this; }
        UIWidget& Background(bool draw = true) { m_Element->DrawBackground = draw; return *this; }
        UIWidget& Tooltip(std::string tip) { m_Element->Tooltip = std::move(tip); return *this; }
        UIWidget& Visible(bool visible) { m_Element->Visible = visible; return *this; }
        UIWidget& Enabled(bool enabled) { m_Element->SetEnabled(enabled); return *this; }
        UIWidget& Name(std::string name) { m_Element->Name = std::move(name); return *this; }

        UIWidget& RefOut(Ref<T>& out) { out = m_Element; return *this; }

    private:
        Ref<T> m_Element;
    };

    namespace Detail
    {
        inline void AppendChildren(UIElement& parent, std::initializer_list<Ref<UIElement>> children)
        {
            for (const auto& child : children)
            {
                if (child)
                    parent.AddChild(child);
            }
        }
    }

    inline UIWidget<UIStackBox> VStack(float spacing, std::initializer_list<Ref<UIElement>> children = {})
    {
        auto box = MakeUI<UIStackBox>(UIOrientation::Vertical, spacing);
        Detail::AppendChildren(*box, children);
        return UIWidget<UIStackBox>(std::move(box));
    }

    inline UIWidget<UIStackBox> VStack(std::initializer_list<Ref<UIElement>> children)
    {
        return VStack(6.0f, children);
    }

    inline UIWidget<UIStackBox> HStack(float spacing, std::initializer_list<Ref<UIElement>> children = {})
    {
        auto box = MakeUI<UIStackBox>(UIOrientation::Horizontal, spacing);
        Detail::AppendChildren(*box, children);
        return UIWidget<UIStackBox>(std::move(box));
    }

    inline UIWidget<UIStackBox> HStack(std::initializer_list<Ref<UIElement>> children)
    {
        return HStack(6.0f, children);
    }

    inline UIWidget<UIWrapBox> Wrap(float hSpacing, float vSpacing, std::initializer_list<Ref<UIElement>> children = {})
    {
        auto box = MakeUI<UIWrapBox>(hSpacing, vSpacing);
        Detail::AppendChildren(*box, children);
        return UIWidget<UIWrapBox>(std::move(box));
    }

    inline UIWidget<UIWrapBox> Wrap(std::initializer_list<Ref<UIElement>> children)
    {
        return Wrap(6.0f, 6.0f, children);
    }

    inline UIWidget<UIGridBox> Grid(uint32_t columns, float colSpacing, float rowSpacing,
                                    std::initializer_list<Ref<UIElement>> children = {})
    {
        auto box = MakeUI<UIGridBox>(columns, colSpacing, rowSpacing);
        Detail::AppendChildren(*box, children);
        return UIWidget<UIGridBox>(std::move(box));
    }

    inline UIWidget<UIGridBox> Grid(uint32_t columns, std::initializer_list<Ref<UIElement>> children)
    {
        return Grid(columns, 6.0f, 6.0f, children);
    }

    inline UIWidget<UIScrollView> Scroll(std::initializer_list<Ref<UIElement>> children = {}, float spacing = 8.0f)
    {
        auto scroll = MakeUI<UIScrollView>();
        scroll->WidthMode = UISizeMode::Fill;
        scroll->HeightMode = UISizeMode::Fill;
        scroll->ChildSpacing = spacing;
        Detail::AppendChildren(*scroll, children);
        return UIWidget<UIScrollView>(std::move(scroll));
    }

    inline UIWidget<UIGroupBox> Card(std::string title, std::initializer_list<Ref<UIElement>> children = {})
    {
        auto card = MakeUI<UIGroupBox>(std::move(title));
        for (const auto& child : children)
        {
            if (child)
                card->AddContent(child);
        }
        return UIWidget<UIGroupBox>(std::move(card));
    }

    inline UIWidget<UIMenuBar> MenuBar(
        std::initializer_list<std::pair<std::string, std::vector<UIMenuItem>>> menus)
    {
        auto bar = MakeUI<UIMenuBar>();
        for (const auto& [title, items] : menus)
            bar->AddMenu(title, items);
        return UIWidget<UIMenuBar>(std::move(bar));
    }

    inline UIWidget<UILabel> Label(std::string text = {}, float fontSize = 0.0f)
    {
        return UIWidget<UILabel>(MakeUI<UILabel>(std::move(text), fontSize));
    }

    inline UIWidget<UIButton> Button(std::string text = {}, std::function<void()> onClick = {})
    {
        return UIWidget<UIButton>(MakeUI<UIButton>(std::move(text), std::move(onClick)));
    }

    inline UIWidget<UICheckBox> CheckBox(std::string label = {}, bool checked = false,
                                         std::function<void(bool)> onChanged = {})
    {
        return UIWidget<UICheckBox>(MakeUI<UICheckBox>(std::move(label), checked, std::move(onChanged)));
    }

    inline UIWidget<UISlider> Slider(float value = 0.0f, float min = 0.0f, float max = 1.0f,
                                     std::function<void(float)> onChanged = {})
    {
        return UIWidget<UISlider>(MakeUI<UISlider>(value, min, max, std::move(onChanged)));
    }

    inline UIWidget<UITextInput> TextInput(std::string text = {}, std::string placeholder = {})
    {
        return UIWidget<UITextInput>(MakeUI<UITextInput>(std::move(text), std::move(placeholder)));
    }

    inline UIWidget<UISeparator> Separator(UIOrientation orientation = UIOrientation::Horizontal, float thickness = 1.0f)
    {
        return UIWidget<UISeparator>(MakeUI<UISeparator>(orientation, thickness));
    }
}
