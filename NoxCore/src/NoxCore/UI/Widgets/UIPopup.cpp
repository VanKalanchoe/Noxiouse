#include "UIPopup.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    namespace
    {
        class UIPopupBackdrop final : public UIAnchorBox
        {
        public:
            UIPopupBackdrop(UIContext& context, const UIRect& anchor, const Ref<UIElement>& content, bool alignRight)
                : m_Context(&context), m_Anchor(anchor), m_Content(content), m_AlignRight(alignRight), m_Open(std::make_shared<bool>(true))
            {
                HitTestMode = UIHitTestMode::Self; // everything outside the content is the backdrop's
                if (m_Content)
                    AddChild(m_Content);
            }

            ~UIPopupBackdrop() override { *m_Open = false; }

            std::shared_ptr<const bool> GetOpenFlag() const { return m_Open; }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                // A press on the content bubbles up to here as well: only one outside it closes the popup.
                if (AnyChildContains(e.Position))
                    return true;
                if (m_Context && m_Context->GetOverlay().get() == this)
                    m_Context->ClearOverlay();
                return true;
            }

        protected:
            void ArrangeChildren(const UIRect& contentRect) override
            {
                if (!m_Content || !m_Content->Visible)
                    return;

                constexpr float kMargin = 6.0f;
                constexpr float kGap = 4.0f;
                const UIVec2 desired = m_Content->Measure();
                const float w = std::min(desired.x, std::max(40.0f, contentRect.Width() - kMargin * 2.0f));
                // Narrower than it wants to be (a small window): its height follows the width it gets.
                const float wanted = w < desired.x - 0.5f ? m_Content->ComputeHeightForWidth(w) : desired.y;
                const float h = std::min(wanted, std::max(40.0f, contentRect.Height() - kMargin * 2.0f));

                // Under the anchor; above it when it does not fit under and there is more room above.
                const float spaceBelow = contentRect.Max.y - kMargin - (m_Anchor.Max.y + kGap);
                const float spaceAbove = (m_Anchor.Min.y - kGap) - (contentRect.Min.y + kMargin);
                float y = m_Anchor.Max.y + kGap;
                if (h > spaceBelow && spaceAbove > spaceBelow)
                    y = m_Anchor.Min.y - kGap - h;
                y = std::clamp(y, contentRect.Min.y + kMargin, std::max(contentRect.Min.y + kMargin, contentRect.Max.y - kMargin - h));

                float x = m_AlignRight ? (m_Anchor.Max.x - w) : m_Anchor.Min.x;
                x = std::clamp(x, contentRect.Min.x + kMargin, std::max(contentRect.Min.x + kMargin, contentRect.Max.x - kMargin - w));

                m_Content->Arrange({ { x, y }, { x + w, y + h } });
            }

        private:
            UIContext* m_Context = nullptr;
            UIRect m_Anchor{};
            Ref<UIElement> m_Content;
            bool m_AlignRight = false;
            std::shared_ptr<bool> m_Open;
        };
    }

    std::shared_ptr<const bool> UIPopup::Show(UIContext& context, const UIRect& anchorRect, const Ref<UIElement>& content, bool alignRight)
    {
        Ref<UIPopupBackdrop> backdrop(new UIPopupBackdrop(context, anchorRect, content, alignRight));
        std::shared_ptr<const bool> open = backdrop->GetOpenFlag();
        context.SetOverlay(backdrop);
        return open;
    }
}
