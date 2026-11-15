#pragma once

#include <functional>
#include <string>

#include "NoxCore/Core/Ref.h"

namespace Nox::UI
{
    class UIContext;
    class UIElement;

    // Builds an interactive ImGui-Demo-style gallery showcasing all Nox::UI widgets,
    // layouts, virtualized lists, overlays/modals, and .nstyle theming.
    Ref<UIElement> CreateUIDemoGallery(UIContext& context, const std::function<void(const std::string&)>& onStatus);
}
