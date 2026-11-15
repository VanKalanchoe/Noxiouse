#include "UIBreadcrumb.h"

#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIBreadcrumb::UIBreadcrumb(
        std::vector<std::string> segments,
        std::function<void(int)> onNavigate)
        : UIStackBox(UIOrientation::Horizontal, 4.0f)
        , OnNavigate(std::move(onNavigate))
        , m_Segments(std::move(segments))
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        Padding = UIThickness::Symmetric(8.0f, 4.0f);
        Rebuild();
    }

    void UIBreadcrumb::SetSegments(std::vector<std::string> segments)
    {
        m_Segments = std::move(segments);
        Rebuild();
    }

    void UIBreadcrumb::Rebuild()
    {
        ClearChildren();
        for (int i = 0; i < static_cast<int>(m_Segments.size()); ++i)
        {
            if (i > 0)
            {
                auto sep = Ref<UILabel>(new UILabel(Separator, 13.0f));
                sep->VAlign = UIAlignment::Center;
                sep->UseColorOverride = true;
                sep->ColorOverride = { 0.50f, 0.55f, 0.65f, 1.0f };
                AddChild(sep);
            }

            const bool isLast = (i == static_cast<int>(m_Segments.size()) - 1);
            auto btn = Ref<UIButton>(new UIButton(m_Segments[i], [this, i]()
            {
                if (OnNavigate)
                    OnNavigate(i);
            }));
            btn->StyleClass = "Button";
            if (isLast)
                btn->StyleVariant = "primary";
            btn->VAlign = UIAlignment::Center;
            AddChild(btn);
        }
    }
}
