#pragma once

#include <functional>
#include <string>

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
    };
}
