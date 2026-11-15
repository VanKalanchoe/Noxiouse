#pragma once

#include <functional>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIButton;

    // Page navigation bar (<, 1, 2, ..., N, >).
    class UIPagination : public UIStackBox
    {
    public:
        explicit UIPagination(
            int totalPages = 5,
            int currentPage = 1,
            std::function<void(int)> onPageChanged = {});

        void SetTotalPages(int totalPages);
        void SetCurrentPage(int page, bool notify = false);
        int GetCurrentPage() const { return m_CurrentPage; }

        std::function<void(int)> OnPageChanged;

    private:
        void Rebuild();
        void SyncButtons();

        int m_TotalPages = 5;
        int m_CurrentPage = 1;
        Ref<UIButton> m_PrevBtn;
        Ref<UIButton> m_NextBtn;
        std::vector<Ref<UIButton>> m_PageBtns;
    };
}
