#include "UIVectorEdit.h"

#include <algorithm>

#include "NoxCore/UI/Widgets/UIDragScalar.h"

namespace Nox::UI
{
    UIVectorEdit::UIVectorEdit(
        int components,
        const glm::vec4& initialValue,
        float speed,
        std::function<void(const glm::vec4&)> onChanged)
        : UIStackBox(UIOrientation::Horizontal, 6.0f)
        , OnChanged(std::move(onChanged))
        , m_Components(std::clamp(components, 2, 4))
    {
        WidthMode = UISizeMode::Fill;

        static const char* kPrefixes[4] = { "X", "Y", "Z", "W" };
        static constexpr UIColor kColors[4] = {
            { 0.85f, 0.28f, 0.28f, 1.0f },
            { 0.26f, 0.72f, 0.36f, 1.0f },
            { 0.24f, 0.54f, 0.90f, 1.0f },
            { 0.68f, 0.42f, 0.88f, 1.0f }
        };

        for (int i = 0; i < m_Components; ++i)
        {
            auto scalar = Ref<UIDragScalar>(new UIDragScalar(initialValue[i], speed));
            scalar->Prefix = kPrefixes[i];
            scalar->PrefixColor = kColors[i];
            scalar->WidthMode = UISizeMode::Fill;
            scalar->OnChanged = [this](float)
            {
                if (OnChanged)
                    OnChanged(GetValue());
            };
            m_Axes[i] = scalar;
            AddChild(scalar);
        }
    }

    void UIVectorEdit::SetValue(const glm::vec4& value, bool notify)
    {
        for (int i = 0; i < m_Components; ++i)
        {
            if (m_Axes[i])
                m_Axes[i]->Value = value[i];
        }
        if (notify && OnChanged)
            OnChanged(GetValue());
    }

    void UIVectorEdit::SetSpeed(float speed)
    {
        for (int i = 0; i < m_Components; ++i)
        {
            if (m_Axes[i])
                m_Axes[i]->Speed = speed;
        }
    }

    void UIVectorEdit::SetFormat(const std::string& format)
    {
        for (int i = 0; i < m_Components; ++i)
        {
            if (m_Axes[i])
                m_Axes[i]->Format = format;
        }
    }

    glm::vec4 UIVectorEdit::GetValue() const
    {
        glm::vec4 v(0.0f);
        for (int i = 0; i < m_Components; ++i)
        {
            if (m_Axes[i])
                v[i] = m_Axes[i]->Value;
        }
        return v;
    }

    bool UIVectorEdit::IsEditing() const
    {
        for (int i = 0; i < m_Components; ++i)
        {
            if (m_Axes[i] && m_Axes[i]->IsInteracting())
                return true;
        }
        return false;
    }
}
