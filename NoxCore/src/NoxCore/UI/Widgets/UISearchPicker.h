#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UITypes.h"

namespace Nox::UI
{
    class UIContext;

    // A popup list with a search field above it, anchored under (or above, when there is more room) a button: an asset picker,
    // a "pick a bone / an entity / a type" list. Typing filters the rows (label or detail contains the text, any case); a click,
    // or Enter for the first row shown, picks one and closes it; a click outside or Escape closes it without picking. The current
    // choice is highlighted. At most MaxShown rows are built (a note says how many more match: type to narrow).
    struct UISearchPickerOption
    {
        std::string Label;
        std::string Detail; // shown after the label and as its tooltip; searched too
        uint64_t Id = 0;
    };

    struct UISearchPickerSettings
    {
        std::string Placeholder = "Filter...";
        std::string NoneLabel;            // non-empty: a first row that picks Id 0 ("None"), shown while nothing is typed
        std::string NoMatches = "No matches";
        size_t MaxShown = 60;
        bool AlignRight = false;          // right edges of popup and anchor in line (a button at a panel's right side)
        float MinWidth = 270.0f, MaxWidth = 360.0f;
    };

    class UISearchPicker
    {
    public:
        static void Show(UIContext& context, const UIRect& anchor, std::vector<UISearchPickerOption> options, uint64_t current,
                         std::function<void(uint64_t id)> onPick, UISearchPickerSettings settings = {});
    };
}
