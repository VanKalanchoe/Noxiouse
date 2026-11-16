#pragma once

#include <memory>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIContext;

    // A floating panel over the UI, opened by an element (a colour swatch's picker, a field's options).
    class UIPopup
    {
    public:
        // Shows `content` as the context's overlay beside anchorRect: under it, above it when there is no room under, and
        // always inside the viewport, wherever the anchor is (an inspector at the window's right edge included).
        // A press outside the content closes it, Escape closes it. Returns a flag that reads true while the popup is open
        // (it turns false when the overlay is replaced or cleared), for the opener to know its popup is up.
        static std::shared_ptr<const bool> Show(UIContext& context, const UIRect& anchorRect, const Ref<UIElement>& content,
                                                bool alignRight = false);
    };
}
