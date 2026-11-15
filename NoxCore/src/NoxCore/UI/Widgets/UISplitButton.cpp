#include "UISplitButton.h"

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    UISplitButton::UISplitButton(
        UIContext& context,
        std::string primaryLabel,
        std::function<void()> onPrimaryClick,
        std::vector<UIMenuItem> secondaryItems)
        : UIStackBox(UIOrientation::Horizontal, 2.0f)
    {
        auto mainBtn = Ref<UIButton>(new UIButton(std::move(primaryLabel), std::move(onPrimaryClick)));
        mainBtn->StyleVariant = "primary";

        auto dropBtn = Ref<UIButton>(new UIButton("v"));
        dropBtn->StyleVariant = "primary";
        dropBtn->WidthMode = UISizeMode::Fixed;
        dropBtn->FixedSize.x = 28.0f;
        UIButton* dropPtr = dropBtn.get();
        dropBtn->OnClick = [&context, dropPtr, items = std::move(secondaryItems)]()
        {
            const UIRect& b = dropPtr->GetBounds();
            UIMenuPopup::ShowContextMenu(context, { b.Min.x - 120.0f, b.Max.y + 3.0f }, items);
        };

        AddChild(mainBtn);
        AddChild(dropBtn);
    }
}
