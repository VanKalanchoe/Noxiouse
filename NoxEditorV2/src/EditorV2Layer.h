#pragma once

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/UI/Core/UIContext.h"

namespace Nox::UI
{
    class UILabel;
}

namespace Nox
{
    class EditorV2Layer : public Layer
    {
    public:
        EditorV2Layer();
        ~EditorV2Layer() override;

        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;

    private:
        void BuildUI();

        UI::UIContext m_UI;
        IntrusiveRef<UI::UILabel> m_StatusLabel;
        float m_LastDeltaSeconds = 0.016f;
        int m_ClickCount = 0;
    };
}
