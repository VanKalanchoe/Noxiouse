#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UIWrapBox.h"

namespace Nox::UI
{
    // Clickable path/hierarchy navigation bar (e.g., Root > Folder > Asset).
    class UIBreadcrumb : public UIWrapBox
    {
    public:
        explicit UIBreadcrumb(
            std::vector<std::string> segments = {},
            std::function<void(int)> onNavigate = {});

        void SetSegments(std::vector<std::string> segments);
        const std::vector<std::string>& GetSegments() const { return m_Segments; }

        std::string Separator = ">";
        std::function<void(int segmentIndex)> OnNavigate;

    private:
        void Rebuild();

        std::vector<std::string> m_Segments;
    };
}
