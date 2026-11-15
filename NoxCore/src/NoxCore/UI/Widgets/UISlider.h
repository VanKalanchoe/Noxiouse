#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UISlider : public UIElement
    {
    public:
        explicit UISlider(float value = 0.0f, float min = 0.0f, float max = 1.0f,
                          std::function<void(float)> onChanged = {});

        float Value = 0.0f;
        float Min = 0.0f;
        float Max = 1.0f;
        float Step = 0.0f;
        bool ShowValue = true;
        std::string Format = "{:.2f}";
        std::function<void(float)> OnChanged;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnKeyDown(const UIKeyEvent& e) override;
        bool OnTextInput(std::string_view text) override;
        void OnUpdate(float deltaTime) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void UpdateFromPointerX(float x);
        void BeginEdit();
        void CommitEdit(bool apply);

        bool m_Dragging = false;
        bool m_Editing = false;
        bool m_SelectAll = false;
        std::string m_EditText;
        size_t m_Cursor = 0;
        float m_BlinkTimer = 0.0f;
        uint64_t m_LastClickTicks = 0;
    };
}
