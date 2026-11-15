#pragma once

#include <string>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIBadge;
    class UILabel;

    // Dashboard metric / KPI card with title, large value readout, and optional status badge.
    class UIStatCard : public UIStackBox
    {
    public:
        UIStatCard(
            std::string title,
            std::string value,
            std::string badgeText = {},
            const UIColor& badgeColor = { 0.24f, 0.76f, 0.48f, 1.0f });

        void SetValue(const std::string& value);
        void SetBadge(const std::string& text);

    private:
        Ref<UILabel> m_TitleLabel;
        Ref<UILabel> m_ValueLabel;
        Ref<UIBadge> m_Badge;
    };
}
