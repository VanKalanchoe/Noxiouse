#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/Core/Ref.h"

namespace Nox::UI
{
    class UIContext;
    class UIElement;

    struct UIDemoPanel
    {
        std::string Title;
        Ref<UIElement> Content;
    };

    // Builds the individual demo sections as separate dockable panels:
    // "Controls", "Inputs & Data", "Layout & Graphs", and "Overlays & Theme".
    std::vector<UIDemoPanel> CreateUIDemoPanels(UIContext& context, const std::function<void(const std::string&)>& onStatus = {});

    // Builds an interactive ImGui-Demo-style gallery showcasing all Nox::UI widgets,
    // layouts, virtualized lists, overlays/modals, and .nstyle theming.
    Ref<UIElement> CreateUIDemoGallery(UIContext& context, const std::function<void(const std::string&)>& onStatus = {});
}
