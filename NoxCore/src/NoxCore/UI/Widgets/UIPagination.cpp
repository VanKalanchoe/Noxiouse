#include "UIPagination.h"

#include <algorithm>
#include <string>

#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    UIPagination::UIPagination(
        int totalPages,
        int currentPage,
        std::function<void(int)> onPageChanged)
        : UIStackBox(UIOrientation::Horizontal, 4.0f)
        , OnPageChanged(std::move(onPageChanged))
        , m_TotalPages(std::max(1, totalPages))
        , m_CurrentPage(std::clamp(currentPage, 1, m_TotalPages))
    {
        WidthMode = UISizeMode::Fill;
        ClipChildren = true;
        Rebuild();
    }

    void UIPagination::SetTotalPages(int totalPages)
    {
        m_TotalPages = std::max(1, totalPages);
        m_CurrentPage = std::clamp(m_CurrentPage, 1, m_TotalPages);
        Rebuild();
    }

    void UIPagination::SetCurrentPage(int page, bool notify)
    {
        const int clamped = std::clamp(page, 1, m_TotalPages);
        if (m_CurrentPage == clamped)
            return;
        m_CurrentPage = clamped;
        SyncButtons();
        if (notify && OnPageChanged)
            OnPageChanged(m_CurrentPage);
    }

    void UIPagination::Rebuild()
    {
        ClearChildren();
        m_PageBtns.clear();

        m_PrevBtn = Ref<UIButton>(new UIButton("<", [this]() { SetCurrentPage(m_CurrentPage - 1, true); }));
        AddChild(m_PrevBtn);

        const int visibleCount = std::min(5, m_TotalPages);
        m_PageBtns.reserve(static_cast<size_t>(visibleCount));
        for (int i = 0; i < visibleCount; ++i)
        {
            auto pageBtn = Ref<UIButton>(new UIButton("1"));
            m_PageBtns.push_back(pageBtn);
            AddChild(pageBtn);
        }

        m_NextBtn = Ref<UIButton>(new UIButton(">", [this]() { SetCurrentPage(m_CurrentPage + 1, true); }));
        AddChild(m_NextBtn);

        SyncButtons();
    }

    void UIPagination::SyncButtons()
    {
        const int visibleCount = static_cast<int>(m_PageBtns.size());
        if (visibleCount == 0)
            return;

        if (m_PrevBtn)
            m_PrevBtn->SetEnabled(m_CurrentPage > 1);
        if (m_NextBtn)
            m_NextBtn->SetEnabled(m_CurrentPage < m_TotalPages);

        const int startPage = std::max(1, std::min(m_CurrentPage - 2, m_TotalPages - visibleCount + 1));
        for (int i = 0; i < visibleCount; ++i)
        {
            const int p = startPage + i;
            auto& btn = m_PageBtns[static_cast<size_t>(i)];
            btn->SetText(std::to_string(p));
            btn->StyleVariant = (p == m_CurrentPage) ? "primary" : "";
            btn->OnClick = [this, p]() { SetCurrentPage(p, true); };
        }
    }
}
