#pragma once

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Flow layout that arranges children left-to-right and wraps onto new rows when width is exceeded.
    class UIWrapBox : public UIElement
    {
    public:
        explicit UIWrapBox(float hSpacing = 6.0f, float vSpacing = 6.0f);

        float HSpacing = 6.0f;
        float VSpacing = 6.0f;

        float ComputeHeightForWidth(float width) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;

    private:
        UIVec2 ComputeWrappedSize(float wrapWidth, bool measureChildren);
    };
}
