#pragma once

#include <functional>

#include <glm/glm.hpp>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIDragScalar;

    // Compact multi-axis vector editor (Vec2 / Vec3 / Vec4) with color-coded X/Y/Z/W drag scalars.
    class UIVectorEdit : public UIStackBox
    {
    public:
        explicit UIVectorEdit(
            int components = 3,
            const glm::vec4& initialValue = glm::vec4(0.0f),
            float speed = 0.05f,
            std::function<void(const glm::vec4&)> onChanged = {});

        void SetValue(const glm::vec4& value, bool notify = false);
        void SetSpeed(float speed);
        void SetFormat(const std::string& format);
        glm::vec4 GetValue() const;
        bool IsEditing() const;

        std::function<void(const glm::vec4&)> OnChanged;

    private:
        int m_Components = 3;
        Ref<UIDragScalar> m_Axes[4];
    };
}
