#pragma once

#include <functional>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UIElement.h"
#include <string>
#include <vector>

namespace Nox::UI
{
    class UIContext;

    // Convenience helpers for centered modal confirmation and text-prompt overlays.
    class UIModal
    {
    public:
        static void ShowConfirm(
            UIContext& context,
            std::string title,
            std::string message,
            std::function<void()> onConfirm,
            std::function<void()> onCancel = {},
            std::string confirmLabel = "Confirm",
            std::string cancelLabel = "Cancel");

        static void ShowPrompt(
            UIContext& context,
            std::string title,
            std::string message,
            std::string defaultValue,
            std::string placeholder,
            std::function<void(const std::string&)> onSubmit,
            std::function<void()> onCancel = {});

        // A dialog with your own content between its title and its buttons (an import's options). Buttons close the dialog
        // and then call their action; Escape and a press beside the dialog act as the last button (Cancel).
        struct Button
        {
            std::string Label;
            std::function<void()> OnClick;
            bool Primary = false;
        };
        static void ShowCustom(UIContext& context, std::string title, const Ref<UIElement>& content, std::vector<Button> buttons,
                               float width = 460.0f);
    };
}
