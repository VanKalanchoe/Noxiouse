#pragma once

#include <functional>

#include <glm/glm.hpp>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    enum class UIGizmoMode : uint8_t
    {
        Translate = 0,
        Rotate,
        Scale
    };

    // Screen-space 3D Transform Gizmo rendered via UIDrawList SDF primitives.
    class UIGizmo : public UIElement
    {
    public:
        UIGizmo();

        bool HasTarget = false;
        UIGizmoMode Mode = UIGizmoMode::Translate;
        glm::vec3 TargetPosition{ 0.0f };
        glm::vec3 TargetRotation{ 0.0f }; // radians
        glm::vec3 TargetScale{ 1.0f };

        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& viewProj, const glm::vec3& camPos);

        std::function<void(const glm::vec3& pos, const glm::vec3& rot, const glm::vec3& scale)> OnTransformChanged;
        std::function<void(const UIPointerEvent&)> OnViewportOrbitDrag;
        std::function<void(const UIPointerEvent&)> OnViewportZoom;

        bool IsUsing() const { return m_ActiveAxis >= 0; }

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;

    protected:
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        bool ProjectPoint(const glm::vec3& world, UIVec2& outScreen) const;
        int HitTestAxis(const UIVec2& screenPos) const;
        float AxisScreenScale() const;

        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_ViewProj{ 1.0f };
        glm::vec3 m_CameraPos{ 0.0f, 0.0f, 10.0f };

        int m_HoveredAxis = -1;
        int m_ActiveAxis = -1;
        bool m_Orbiting = false;
    };
}
