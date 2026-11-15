#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIMenu.h"

namespace Nox::UI
{
    class UIContext;

    // Primary action button paired with an attached dropdown menu of secondary actions.
    class UISplitButton : public UIStackBox
    {
    public:
        UISplitButton(
            UIContext& context,
            std::string primaryLabel,
            std::function<void()> onPrimaryClick,
            std::vector<UIMenuItem> secondaryItems);
    };
}
