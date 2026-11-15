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

        constexpr int kRingSegments = 28;
        constexpr float kTwoPi = 6.28318530718f;

        float DistToSegment(const UIVec2& p, const UIVec2& a, const UIVec2& b)
        {
            const UIVec2 ab = b - a;
            const float lenSq = glm::dot(ab, ab);
            const float t = lenSq > 1e-5f ? std::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f) : 0.0f;
            return glm::length(p - (a + ab * t));
        }

        void GetOrthonormalBasis(const glm::vec3& normal, glm::vec3& outU, glm::vec3& outV)
        {
            const glm::vec3 up = std::abs(normal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            outU = glm::normalize(glm::cross(up, normal));
            outV = glm::cross(normal, outU);
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
        m_InvViewProj = glm::inverse(viewProj);
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

    bool UIGizmo::ScreenToRay(const UIVec2& screenPos, glm::vec3& outOrigin, glm::vec3& outDir) const
    {
        const UIRect& b = GetBounds();
        if (b.Width() <= 1.0f || b.Height() <= 1.0f)
            return false;

        const float ndcX = ((screenPos.x - b.Min.x) / b.Width()) * 2.0f - 1.0f;
        const float ndcY = 1.0f - ((screenPos.y - b.Min.y) / b.Height()) * 2.0f;

        // Reverse-Z: z=1.0 is near plane, z=0.5 is mid-depth (works for finite & infinite far).
        const glm::vec4 nearClip = m_InvViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
        const glm::vec4 midClip = m_InvViewProj * glm::vec4(ndcX, ndcY, 0.5f, 1.0f);
        if (std::abs(nearClip.w) < 1e-6f || std::abs(midClip.w) < 1e-6f)
            return false;

        const glm::vec3 pNear = glm::vec3(nearClip) / nearClip.w;
        const glm::vec3 pMid = glm::vec3(midClip) / midClip.w;
        outOrigin = pNear;
        outDir = glm::normalize(pMid - pNear);
        return true;
    }

    bool UIGizmo::IntersectAxisPlane(const UIVec2& screenPos, int axis, const glm::vec3& origin, float& outT) const
    {
        glm::vec3 rayO{}, rayD{};
        if (!ScreenToRay(screenPos, rayO, rayD))
            return false;

        const glm::vec3& axisDir = kAxes[axis];
        const glm::vec3 toCam = glm::normalize(m_CameraPos - origin);
        glm::vec3 tangent = glm::cross(axisDir, toCam);
        if (glm::dot(tangent, tangent) < 1e-4f)
            tangent = glm::cross(axisDir, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::dot(tangent, tangent) < 1e-4f)
            tangent = glm::cross(axisDir, glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::vec3 planeNormal = glm::normalize(glm::cross(axisDir, tangent));

        const float denom = glm::dot(rayD, planeNormal);
        if (std::abs(denom) < 1e-5f)
            return false;

        const float tRay = glm::dot(origin - rayO, planeNormal) / denom;
        if (tRay < 0.0f)
            return false;

        const glm::vec3 hit = rayO + rayD * tRay;
        outT = glm::dot(hit - origin, axisDir);
        return true;
    }

    bool UIGizmo::IntersectRotationPlane(const UIVec2& screenPos, int axis, const glm::vec3& origin, float& outAngle) const
    {
        glm::vec3 rayO{}, rayD{};
        if (!ScreenToRay(screenPos, rayO, rayD))
            return false;

        const glm::vec3& normal = kAxes[axis];
        const float denom = glm::dot(rayD, normal);
        if (std::abs(denom) < 1e-5f)
            return false;

        const float tRay = glm::dot(origin - rayO, normal) / denom;
        if (tRay < 0.0f)
            return false;

        const glm::vec3 hit = rayO + rayD * tRay - origin;
        glm::vec3 u{}, v{};
        GetOrthonormalBasis(normal, u, v);
        outAngle = std::atan2(glm::dot(hit, v), glm::dot(hit, u));
        return true;
    }

    int UIGizmo::HitTestGizmo(const UIVec2& screenPos) const
    {
        if (!HasTarget)
            return -1;

        const float worldLen = AxisScreenScale();
        int bestAxis = -1;
        float bestDist = 11.0f;

        if (Mode == UIGizmoMode::Rotate)
        {
            for (int i = 0; i < 3; ++i)
            {
                glm::vec3 u{}, v{};
                GetOrthonormalBasis(kAxes[i], u, v);
                UIVec2 prev{};
                bool hasPrev = false;
                for (int s = 0; s <= kRingSegments; ++s)
                {
                    const float a = (static_cast<float>(s) / static_cast<float>(kRingSegments)) * kTwoPi;
                    const glm::vec3 pt = TargetPosition + (u * std::cos(a) + v * std::sin(a)) * worldLen;
                    UIVec2 cur{};
                    if (ProjectPoint(pt, cur))
                    {
                        if (hasPrev)
                        {
                            const float d = DistToSegment(screenPos, prev, cur);
                            if (d < bestDist)
                            {
                                bestDist = d;
                                bestAxis = i;
                            }
                        }
                        prev = cur;
                        hasPrev = true;
                    }
                    else
                        hasPrev = false;
                }
            }
            return bestAxis;
        }

        UIVec2 origin2D{};
        if (!ProjectPoint(TargetPosition, origin2D))
            return -1;

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
        if (m_ActiveAxis >= 0 && HasTarget)
        {
            if (Mode == UIGizmoMode::Rotate)
            {
                float angle = 0.0f;
                if (IntersectRotationPlane(e.Position, m_ActiveAxis, m_DragStartPos, angle))
                {
                    float delta = angle - m_DragStartParam;
                    if (delta > 3.14159265f) delta -= kTwoPi;
                    if (delta < -3.14159265f) delta += kTwoPi;
                    TargetRotation = m_DragStartRot;
                    TargetRotation[m_ActiveAxis] += delta;
                    if (OnTransformChanged)
                        OnTransformChanged(TargetPosition, TargetRotation, TargetScale);
                }
            }
            else
            {
                float axisT = 0.0f;
                if (IntersectAxisPlane(e.Position, m_ActiveAxis, m_DragStartPos, axisT))
                {
                    const float delta = axisT - m_DragStartParam;
                    if (Mode == UIGizmoMode::Translate)
                    {
                        TargetPosition = m_DragStartPos + kAxes[m_ActiveAxis] * delta;
                    }
                    else if (Mode == UIGizmoMode::Scale)
                    {
                        const float scaleFactor = 1.0f + delta / std::max(0.1f, AxisScreenScale());
                        TargetScale = m_DragStartScale;
                        TargetScale[m_ActiveAxis] = std::max(0.01f, m_DragStartScale[m_ActiveAxis] * scaleFactor);
                    }
                    if (OnTransformChanged)
                        OnTransformChanged(TargetPosition, TargetRotation, TargetScale);
                }
            }
            return true;
        }

        m_HoveredAxis = HitTestGizmo(e.Position);
        return false;
    }

    bool UIGizmo::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button == 1 && HasTarget)
        {
            const int axis = HitTestGizmo(e.Position);
            if (axis >= 0)
            {
                m_ActiveAxis = axis;
                m_DragStartPos = TargetPosition;
                m_DragStartRot = TargetRotation;
                m_DragStartScale = TargetScale;

                if (Mode == UIGizmoMode::Rotate)
                    IntersectRotationPlane(e.Position, axis, m_DragStartPos, m_DragStartParam);
                else
                    IntersectAxisPlane(e.Position, axis, m_DragStartPos, m_DragStartParam);
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

        if (Mode == UIGizmoMode::Rotate)
        {
            for (int i = 0; i < 3; ++i)
            {
                const bool hot = (m_ActiveAxis == i || m_HoveredAxis == i);
                const UIColor col = hot ? UIColor{ 1.0f, 0.92f, 0.30f, 1.0f } : kAxisColors[i];
                const float thickness = hot ? 3.5f : 2.2f;

                glm::vec3 u{}, v{};
                GetOrthonormalBasis(kAxes[i], u, v);
                UIVec2 prev{};
                bool hasPrev = false;
                for (int s = 0; s <= kRingSegments; ++s)
                {
                    const float a = (static_cast<float>(s) / static_cast<float>(kRingSegments)) * kTwoPi;
                    const glm::vec3 pt = TargetPosition + (u * std::cos(a) + v * std::sin(a)) * worldLen;
                    UIVec2 cur{};
                    if (ProjectPoint(pt, cur))
                    {
                        if (hasPrev)
                            drawList.AddBezier(prev, prev, cur, cur, thickness, col);
                        prev = cur;
                        hasPrev = true;
                    }
                    else
                        hasPrev = false;
                }
            }
        }
        else
        {
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
        }

        drawList.AddRoundedRect(origin2D - UIVec2(4.5f), origin2D + UIVec2(4.5f), { 0.96f, 0.96f, 0.98f, 1.0f }, glm::vec4(4.5f));
        drawList.PopClipRect();
    }
}
