#include "UIGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    namespace
    {
        constexpr std::array<glm::vec3, 3> kAxes = {
            glm::vec3{ 1.0f, 0.0f, 0.0f },
            glm::vec3{ 0.0f, 1.0f, 0.0f },
            glm::vec3{ 0.0f, 0.0f, 1.0f }
        };

        constexpr std::array<UIColor, 3> kAxisColors = {
            UIColor{ 0.92f, 0.28f, 0.28f, 1.0f },
            UIColor{ 0.30f, 0.85f, 0.38f, 1.0f },
            UIColor{ 0.28f, 0.56f, 0.96f, 1.0f }
        };

        float DistToSegment(const UIVec2& p, const UIVec2& a, const UIVec2& b)
        {
            const UIVec2 ab = b - a;
            const float lenSq = glm::dot(ab, ab);
            const float t = lenSq > 1e-5f ? std::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + ab * t));
        }
    }

    UIGizmo::UIGizmo()
    {
        StyleClass = "Gizmo";
        DrawBackground = false;
        HitTestMode = UIHitTestMode::Self;
    }

    void UIGizmo::SetCameraMatrices(const glm::mat4& view, const glm::mat4& viewProj, const glm::vec3& camPos)
    {
        m_View = view;
        m_ViewProj = viewProj;
        m_CameraPos = camPos;
    }

    float UIGizmo::AxisScreenScale() const
    {
        const float dist = std::max(1.0f, glm::length(TargetPosition - m_CameraPos));
        return dist * 0.16f;
    }

    bool UIGizmo::ProjectPoint(const glm::vec3& world, UIVec2& outScreen) const
    {
        const glm::vec4 clip = m_ViewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 1e-4f)
            return false;

        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        const UIRect& b = GetBounds();
        outScreen.x = b.Min.x + (ndc.x * 0.5f + 0.5f) * b.Width();
        outScreen.y = b.Min.y + (0.5f - ndc.y * 0.5f) * b.Height();
        return true;
    }

    int UIGizmo::HitTestAxis(const UIVec2& screenPos) const
    {
        if (!HasTarget)
            return -1;

        UIVec2 origin2D{};
        if (!ProjectPoint(TargetPosition, origin2D))
            return -1;

        const float worldLen = AxisScreenScale();
        int bestAxis = -1;
        float bestDist = 11.0f;
        for (int i = 0; i < 3; ++i)
        {
            UIVec2 tip2D{};
            if (!ProjectPoint(TargetPosition + kAxes[i] * worldLen, tip2D))
                continue;
            const float d = DistToSegment(screenPos, origin2D, tip2D);
            if (d < bestDist)
            {
                bestDist = d;
                bestAxis = i;
            }
        }
        return bestAxis;
    }

    bool UIGizmo::OnPointerMove(const UIPointerEvent& e)
    {
        if (m_Orbiting)
        {
            if (OnViewportOrbitDrag)
                OnViewportOrbitDrag(e);
            return true;
        }

        if (m_ActiveAxis >= 0 && HasTarget)
        {
            UIVec2 origin2D{}, tip2D{};
            const float worldLen = AxisScreenScale();
            if (ProjectPoint(TargetPosition, origin2D) && ProjectPoint(TargetPosition + kAxes[m_ActiveAxis] * worldLen, tip2D))
            {
                const UIVec2 axis2D = tip2D - origin2D;
                const float axisLen2D = std::max(1.0f, glm::length(axis2D));
                const float pixelsMoved = glm::dot(e.Delta, axis2D / axisLen2D);
                const float worldDelta = pixelsMoved * (worldLen / axisLen2D);

                if (Mode == UIGizmoMode::Translate)
                    TargetPosition += kAxes[m_ActiveAxis] * worldDelta;
                else if (Mode == UIGizmoMode::Rotate)
                    TargetRotation[m_ActiveAxis] += e.Delta.x * 0.01f;
                else if (Mode == UIGizmoMode::Scale)
                    TargetScale[m_ActiveAxis] = std::max(0.01f, TargetScale[m_ActiveAxis] + worldDelta);

                if (OnTransformChanged)
                    OnTransformChanged(TargetPosition, TargetRotation, TargetScale);
            }
            return true;
        }

        m_HoveredAxis = HitTestAxis(e.Position);
        return false;
    }

    bool UIGizmo::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && HasTarget)
        {
            const int axis = HitTestAxis(e.Position);
            if (axis >= 0)
            {
                m_ActiveAxis = axis;
                return true;
            }
        }
        return false;
    }

    bool UIGizmo::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_ActiveAxis >= 0 && e.Button == 1)
        {
            m_ActiveAxis = -1;
            return true;
        }
        if (m_Orbiting && (e.Button == 2 || e.Button == 3))
        {
            m_Orbiting = false;
            return true;
        }
        return false;
    }

    bool UIGizmo::OnMouseWheel(const UIPointerEvent&)
    {
        return false;
    }

    void UIGizmo::OnPaint(UIDrawList& drawList, const UITheme&)
    {
        if (!HasTarget)
            return;

        UIVec2 origin2D{};
        if (!ProjectPoint(TargetPosition, origin2D))
            return;

        const UIRect& b = GetBounds();
        drawList.PushClipRect(b.Min, b.Max);

        const float worldLen = AxisScreenScale();
        const char* labels[3] = { "X", "Y", "Z" };

        for (int i = 0; i < 3; ++i)
        {
            UIVec2 tip2D{};
            if (!ProjectPoint(TargetPosition + kAxes[i] * worldLen, tip2D))
                continue;

            const bool hot = (m_ActiveAxis == i || m_HoveredAxis == i);
            const UIColor col = hot ? UIColor{ 1.0f, 0.92f, 0.30f, 1.0f } : kAxisColors[i];
            const float thickness = hot ? 4.0f : 2.6f;

            drawList.AddBezier(origin2D, origin2D, tip2D, tip2D, thickness, col);

            const float capR = Mode == UIGizmoMode::Scale ? 5.5f : 4.5f;
            const glm::vec4 capRadius = Mode == UIGizmoMode::Scale ? glm::vec4(1.5f) : glm::vec4(capR);
            drawList.AddRoundedRect(tip2D - UIVec2(capR), tip2D + UIVec2(capR), col, capRadius);
            drawList.AddText(labels[i], nullptr, tip2D + UIVec2(7.0f, -7.0f), 12.0f, col);
        }

        drawList.AddRoundedRect(origin2D - UIVec2(4.5f), origin2D + UIVec2(4.5f), { 0.96f, 0.96f, 0.98f, 1.0f }, glm::vec4(4.5f));
        drawList.PopClipRect();
    }
}
