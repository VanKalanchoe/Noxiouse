#include "EditorViewportV2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/EntityBounds.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "EditorSelection.h"

namespace Nox
{
    namespace
    {
        constexpr const char* kViewModeNames[] = { "Perspective", "Top", "Bottom", "Front", "Back", "Left", "Right" };
        constexpr const char* kFigureNames[] = { "No Figure", "Figure at Origin", "Figure at Cursor" };
        constexpr const char* kDisplayUnitNames[] = { "cm", "m", "auto" };
        constexpr const char* kPivotModeNames[] = { "Center (Bounds)", "Pivot (Origin)" };
        constexpr float kRotateRingRadiusFactor = 0.88f;

        void DrawLine(UI::UIDrawList& drawList, const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color)
        {
            drawList.AddBezier(a, glm::mix(a, b, 1.0f / 3.0f), glm::mix(a, b, 2.0f / 3.0f), b, thickness, color);
        }

        void DrawCircleFill(UI::UIDrawList& drawList, const glm::vec2& center, float radius, const glm::vec4& color)
        {
            drawList.AddRoundedRect(center - glm::vec2(radius), center + glm::vec2(radius), color, glm::vec4(radius));
        }

        void DrawCircleRing(UI::UIDrawList& drawList, const glm::vec2& center, float radius, float thickness, const glm::vec4& color)
        {
            drawList.AddRoundedRect(
                center - glm::vec2(radius),
                center + glm::vec2(radius),
                { 0.0f, 0.0f, 0.0f, 0.0f },
                glm::vec4(radius),
                thickness,
                color
            );
        }

        void DrawArcApprox(UI::UIDrawList& drawList, const glm::vec2& center, float radius,
                           float startRad, float endRad, int segments, float thickness, const glm::vec4& color)
        {
            glm::vec2 prev = center + glm::vec2(std::cos(startRad), std::sin(startRad)) * radius;
            for (int i = 1; i <= segments; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(segments);
                const float a = glm::mix(startRad, endRad, t);
                const glm::vec2 cur = center + glm::vec2(std::cos(a), std::sin(a)) * radius;
                DrawLine(drawList, prev, cur, thickness, color);
                prev = cur;
            }
        }

        void DrawVectorIcon(UI::UIDrawList& drawList, EditorIconKind kind, const glm::vec2& c, const glm::vec4& col, float scale = 1.0f)
        {
            switch (kind)
            {
            case EditorIconKind::SelectArrow:
            {
                const glm::vec2 tip = c + glm::vec2(-4.0f, -6.5f);
                const glm::vec2 btm = c + glm::vec2(-4.0f, 5.0f);
                const glm::vec2 notch = c + glm::vec2(-0.8f, 2.2f);
                const glm::vec2 tailEnd = c + glm::vec2(2.2f, 6.2f);
                const glm::vec2 wing = c + glm::vec2(4.8f, 0.8f);
                DrawLine(drawList, tip, btm, 1.7f, col);
                DrawLine(drawList, tip, wing, 1.7f, col);
                DrawLine(drawList, wing, notch, 1.5f, col);
                DrawLine(drawList, notch, btm, 1.5f, col);
                DrawLine(drawList, notch + glm::vec2(0.5f, 0.0f), tailEnd, 1.8f, col);
                break;
            }
            case EditorIconKind::MoveArrows:
            {
                DrawLine(drawList, c - glm::vec2(6.5f, 0.0f), c + glm::vec2(6.5f, 0.0f), 1.6f, col);
                DrawLine(drawList, c - glm::vec2(0.0f, 6.5f), c + glm::vec2(0.0f, 6.5f), 1.6f, col);
                DrawLine(drawList, c + glm::vec2(0.0f, -7.0f), c + glm::vec2(-2.4f, -4.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(0.0f, -7.0f), c + glm::vec2(2.4f, -4.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(0.0f, 7.0f), c + glm::vec2(-2.4f, 4.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(0.0f, 7.0f), c + glm::vec2(2.4f, 4.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(-7.0f, 0.0f), c + glm::vec2(-4.4f, -2.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(-7.0f, 0.0f), c + glm::vec2(-4.4f, 2.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(7.0f, 0.0f), c + glm::vec2(4.4f, -2.4f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(7.0f, 0.0f), c + glm::vec2(4.4f, 2.4f), 1.5f, col);
                break;
            }
            case EditorIconKind::RotateRing:
            {
                DrawArcApprox(drawList, c, 5.5f, -0.4f, 4.35f, 16, 1.7f, col);
                const glm::vec2 head = c + glm::vec2(std::cos(-0.4f), std::sin(-0.4f)) * 5.5f;
                DrawLine(drawList, head, head + glm::vec2(-3.2f, 0.2f), 1.5f, col);
                DrawLine(drawList, head, head + glm::vec2(0.2f, -3.2f), 1.5f, col);
                DrawCircleFill(drawList, c, 1.4f, col);
                break;
            }
            case EditorIconKind::ScaleBox:
            {
                drawList.AddRoundedRect(c - glm::vec2(5.5f), c + glm::vec2(5.5f), { 0, 0, 0, 0 }, glm::vec4(1.5f), 1.4f, col);
                DrawLine(drawList, c - glm::vec2(2.5f, -2.5f), c + glm::vec2(3.0f, -3.0f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(3.2f, -3.2f), c + glm::vec2(0.2f, -3.2f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(3.2f, -3.2f), c + glm::vec2(3.2f, -0.2f), 1.5f, col);
                break;
            }
            case EditorIconKind::SnapMagnet:
            {
                DrawArcApprox(drawList, c + glm::vec2(0.0f, -1.0f), 4.5f, 0.0f, glm::pi<float>(), 12, 1.8f, col);
                DrawLine(drawList, c + glm::vec2(-4.5f, -1.0f), c + glm::vec2(-4.5f, -5.8f), 1.8f, col);
                DrawLine(drawList, c + glm::vec2(4.5f, -1.0f), c + glm::vec2(4.5f, -5.8f), 1.8f, col);
                DrawLine(drawList, c + glm::vec2(-5.4f, -3.5f), c + glm::vec2(-3.6f, -3.5f), 1.4f, col);
                DrawLine(drawList, c + glm::vec2(3.6f, -3.5f), c + glm::vec2(5.4f, -3.5f), 1.4f, col);
                break;
            }
            case EditorIconKind::ToFloor:
            {
                DrawLine(drawList, c + glm::vec2(0.0f, -6.2f), c + glm::vec2(0.0f, 2.5f), 1.7f, col);
                DrawLine(drawList, c + glm::vec2(0.0f, 2.5f), c + glm::vec2(-3.0f, -0.8f), 1.6f, col);
                DrawLine(drawList, c + glm::vec2(0.0f, 2.5f), c + glm::vec2(3.0f, -0.8f), 1.6f, col);
                DrawLine(drawList, c + glm::vec2(-6.0f, 5.5f), c + glm::vec2(6.0f, 5.5f), 1.8f, col);
                break;
            }
            case EditorIconKind::ToGrid:
            {
                DrawLine(drawList, c + glm::vec2(-5.5f, -2.5f), c + glm::vec2(5.5f, -2.5f), 1.1f, { col.r, col.g, col.b, col.a * 0.55f });
                DrawLine(drawList, c + glm::vec2(-5.5f, 2.5f), c + glm::vec2(5.5f, 2.5f), 1.1f, { col.r, col.g, col.b, col.a * 0.55f });
                DrawLine(drawList, c + glm::vec2(-2.5f, -5.5f), c + glm::vec2(-2.5f, 5.5f), 1.1f, { col.r, col.g, col.b, col.a * 0.55f });
                DrawLine(drawList, c + glm::vec2(2.5f, -5.5f), c + glm::vec2(2.5f, 5.5f), 1.1f, { col.r, col.g, col.b, col.a * 0.55f });
                drawList.AddRoundedRect(c - glm::vec2(2.2f), c + glm::vec2(2.2f), col, glm::vec4(1.0f));
                break;
            }
            case EditorIconKind::MeasureRuler:
            {
                const glm::vec2 a = c + glm::vec2(-5.5f, 5.0f);
                const glm::vec2 b = c + glm::vec2(5.5f, -5.0f);
                DrawLine(drawList, a, b, 1.8f, col);
                DrawLine(drawList, a + glm::vec2(-1.5f, -2.0f), a + glm::vec2(2.0f, 1.5f), 1.5f, col);
                DrawLine(drawList, b + glm::vec2(-2.0f, -1.5f), b + glm::vec2(1.5f, 2.0f), 1.5f, col);
                DrawLine(drawList, c + glm::vec2(-1.4f, -1.4f), c + glm::vec2(1.4f, 1.4f), 1.4f, col);
                break;
            }
            case EditorIconKind::GridMesh:
            {
                DrawLine(drawList, c + glm::vec2(-6.0f, -2.0f), c + glm::vec2(6.0f, -2.0f), 1.2f, col);
                DrawLine(drawList, c + glm::vec2(-6.0f, 2.0f), c + glm::vec2(6.0f, 2.0f), 1.2f, col);
                DrawLine(drawList, c + glm::vec2(-2.0f, -6.0f), c + glm::vec2(-2.0f, 6.0f), 1.2f, col);
                DrawLine(drawList, c + glm::vec2(2.0f, -6.0f), c + glm::vec2(2.0f, 6.0f), 1.2f, col);
                drawList.AddRoundedRect(c - glm::vec2(6.0f), c + glm::vec2(6.0f), { 0, 0, 0, 0 }, glm::vec4(1.5f), 1.2f, col);
                break;
            }
            case EditorIconKind::WireframeMesh:
            {
                const glm::vec2 top = c + glm::vec2(0.0f, -6.2f);
                const glm::vec2 bl = c + glm::vec2(-6.0f, 4.8f);
                const glm::vec2 br = c + glm::vec2(6.0f, 4.8f);
                const glm::vec2 mid = c + glm::vec2(0.0f, 0.2f);
                DrawLine(drawList, top, bl, 1.4f, col);
                DrawLine(drawList, bl, br, 1.4f, col);
                DrawLine(drawList, br, top, 1.4f, col);
                DrawLine(drawList, top, mid, 1.2f, col);
                DrawLine(drawList, bl, mid, 1.2f, col);
                DrawLine(drawList, br, mid, 1.2f, col);
                break;
            }
            case EditorIconKind::Cogwheel:
            {
                DrawCircleRing(drawList, c, 3.8f * scale, 1.7f * scale, col);
                for (int i = 0; i < 8; ++i)
                {
                    const float a = glm::two_pi<float>() * static_cast<float>(i) / 8.0f;
                    const glm::vec2 dir(std::cos(a), std::sin(a));
                    DrawLine(drawList, c + dir * (4.2f * scale), c + dir * (6.4f * scale), 2.1f * scale, col);
                }
                break;
            }
            }
        }

        float DistPointToSegmentSq(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, float* outT = nullptr)
        {
            const glm::vec2 ab = b - a;
            const float lenSq = glm::dot(ab, ab);
            if (lenSq <= 1e-6f)
            {
                if (outT)
                    *outT = 0.0f;
                return glm::dot(p - a, p - a);
            }
            const float t = std::clamp(glm::dot(p - a, ab) / lenSq, 0.0f, 1.0f);
            if (outT)
                *outT = t;
            const glm::vec2 proj = a + ab * t;
            return glm::dot(p - proj, p - proj);
        }

        float ClosestRayAxisParameter(const glm::vec3& rayOrig, const glm::vec3& rayDir,
                                      const glm::vec3& axisOrig, const glm::vec3& axisDir)
        {
            const glm::vec3 w0 = axisOrig - rayOrig;
            const float a = glm::dot(axisDir, axisDir);
            const float b = glm::dot(axisDir, rayDir);
            const float c = glm::dot(rayDir, rayDir);
            const float d = glm::dot(axisDir, w0);
            const float e = glm::dot(rayDir, w0);
            const float denom = a * c - b * b;
            if (std::abs(denom) < 1e-6f)
                return 0.0f;
            return (b * e - c * d) / denom;
        }

        bool IntersectRayPlane(const glm::vec3& rayOrig, const glm::vec3& rayDir,
                               const glm::vec3& planePoint, const glm::vec3& planeNormal,
                               glm::vec3& outHit)
        {
            const float denom = glm::dot(rayDir, planeNormal);
            if (std::abs(denom) < 1e-5f)
                return false;
            const float t = glm::dot(planePoint - rayOrig, planeNormal) / denom;
            if (t < 0.0f)
                return false;
            outHit = rayOrig + rayDir * t;
            return true;
        }

        float SnapValue(float value, float step)
        {
            if (step <= 0.0f)
                return value;
            return std::round(value / step) * step;
        }
    }

    UIVectorIconButton::UIVectorIconButton(EditorIconKind icon, std::string tooltip, std::function<void()> onClick)
        : UI::UIIconButton({}, std::move(tooltip), std::move(onClick)), Icon(icon)
    {
        DrawIcon = [this](UI::UIDrawList& drawList, const UI::UIRect& b, const UI::UIColor& color)
        {
            DrawVectorIcon(drawList, Icon, (b.Min + b.Max) * 0.5f, color, IconScale);
        };
    }

    float ViewportToolState::LocationSnapWorldUnits() const
    {
        return WorldUnits::FromCentimeters(kLocationSnapsCm[std::clamp(LocationSnapIndex, 0, static_cast<int>(std::size(kLocationSnapsCm)) - 1)]);
    }

    glm::dvec3 ViewportToolState::SnapLocation(const glm::dvec3& point) const
    {
        if (!SnapEnabled)
            return point;
        const double step = LocationSnapWorldUnits();
        return glm::round(point / step) * step;
    }

    bool ViewportCanvas::GizmoTakesMouse() const
    {
        if (!m_Ctx.Tools)
            return false;
        return m_Ctx.Tools->AxisIndicatorHovered || m_Ctx.Tools->GizmoUsing || m_Ctx.Tools->GizmoHovered;
    }

    bool ViewportCanvas::RayFromScreen(const UI::UIVec2& screenPos, glm::vec3& outOrigin, glm::vec3& outDir) const
    {
        if (!m_Ctx.Camera || !m_Ctx.Render)
            return false;
        const UI::UIRect& b = GetBounds();
        const float w = b.Width();
        const float h = b.Height();
        if (w <= 1.0f || h <= 1.0f)
            return false;

        const glm::vec2 ndc(
            ((screenPos.x - b.Min.x) / w) * 2.0f - 1.0f,
            1.0f - ((screenPos.y - b.Min.y) / h) * 2.0f
        );
        const glm::mat4 invVP = glm::inverse(
            m_Ctx.Camera->GetGizmoProjection() * m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin()));
        glm::vec4 nearP = invVP * glm::vec4(ndc, -1.0f, 1.0f);
        glm::vec4 farP = invVP * glm::vec4(ndc, 1.0f, 1.0f);
        if (std::abs(nearP.w) < 1e-6f || std::abs(farP.w) < 1e-6f)
            return false;
        nearP /= nearP.w;
        farP /= farP.w;
        outOrigin = glm::vec3(nearP);
        const glm::vec3 diff = glm::vec3(farP - nearP);
        const float len = glm::length(diff);
        if (len < 1e-6f)
            return false;
        outDir = diff / len;
        return true;
    }

    bool ViewportCanvas::ProjectPoint(const glm::vec3& renderPoint, const glm::mat4& viewProj,
                                      UI::UIVec2& outScreen, float* outClipW) const
    {
        const UI::UIRect& b = GetBounds();
        const float w = b.Width();
        const float h = b.Height();
        if (w <= 1.0f || h <= 1.0f)
            return false;

        const glm::vec4 clip = viewProj * glm::vec4(renderPoint, 1.0f);
        if (clip.w <= 0.01f)
            return false;
        if (outClipW)
            *outClipW = clip.w;
        const glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
        outScreen = {
            b.Min.x + (ndc.x * 0.5f + 0.5f) * w,
            b.Min.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * h
        };
        return true;
    }

    bool ViewportCanvas::ComputeGizmoTransform(Entity selected, glm::mat4& outWorldRender) const
    {
        if (!selected || !selected.HasComponent<WorldTransformComponent>() || !m_Ctx.Render)
            return false;

        outWorldRender = m_Ctx.Render->ToRenderSpace(selected.GetComponent<WorldTransformComponent>().WorldMatrix);
        if (!m_Ctx.Tools || m_Ctx.Tools->PivotMode != GizmoPivotMode::Center || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene)
            return true;

        if (m_Ctx.Selection && m_Ctx.Selection->GetEntities().size() > 1)
        {
            glm::dvec3 lowest(std::numeric_limits<double>::max());
            glm::dvec3 highest(std::numeric_limits<double>::lowest());
            bool any = false;
            for (Entity entity : m_Ctx.Selection->GetEntities())
            {
                if (!entity || !entity.HasComponent<WorldTransformComponent>())
                    continue;
                const WorldBounds bounds = ComputeWorldBounds(**m_Ctx.ActiveScene, entity, true);
                if (bounds.Valid)
                {
                    lowest = glm::min(lowest, bounds.Min);
                    highest = glm::max(highest, bounds.Max);
                }
                else
                {
                    const glm::dvec3 pos = entity.GetComponent<WorldTransformComponent>().GetPosition();
                    lowest = glm::min(lowest, pos);
                    highest = glm::max(highest, pos);
                }
                any = true;
            }
            if (any)
                outWorldRender[3] = glm::vec4(m_Ctx.Render->ToRenderSpace((lowest + highest) * 0.5), 1.0f);
            return true;
        }

        const WorldBounds bounds = ComputeWorldBounds(**m_Ctx.ActiveScene, selected, true);
        if (bounds.Valid)
            outWorldRender[3] = glm::vec4(m_Ctx.Render->ToRenderSpace(bounds.Center()), 1.0f);
        return true;
    }

    bool ViewportCanvas::ClosestPointOnRotateRing(const UI::UIVec2& mousePos, const glm::vec3& origin,
                                                  const glm::vec3& u, const glm::vec3& v, float radius,
                                                  const glm::mat4& viewProj, glm::vec3& outWorldPoint, float& inOutBestDistSq) const
    {
        constexpr int kSegments = 64;
        UI::UIVec2 prevScr{};
        glm::vec3 prevWorld{};
        bool hasPrev = false;
        bool improved = false;
        for (int s = 0; s <= kSegments; ++s)
        {
            const float ang = glm::two_pi<float>() * static_cast<float>(s) / kSegments;
            const glm::vec3 pWorld = origin + (u * std::cos(ang) + v * std::sin(ang)) * radius;
            UI::UIVec2 curScr{};
            const bool ok = ProjectPoint(pWorld, viewProj, curScr);
            if (ok && hasPrev)
            {
                float segT = 0.0f;
                const float dSq = DistPointToSegmentSq(mousePos, prevScr, curScr, &segT);
                if (dSq < inOutBestDistSq)
                {
                    inOutBestDistSq = dSq;
                    outWorldPoint = glm::mix(prevWorld, pWorld, segT);
                    improved = true;
                }
            }
            prevScr = curScr;
            prevWorld = pWorld;
            hasPrev = ok;
        }
        return improved;
    }

    float ViewportCanvas::ComputeGizmoWorldScale(const glm::vec3& pivotRender, const glm::mat4& viewProj) const
    {
        const UI::UIRect& b = GetBounds();
        const float vpH = std::max(b.Height(), 100.0f);
        // Constant screen-space size (~13% of viewport height with a comfortable floor so close-up and smaller viewports stay crisp).
        const float targetRadiusPx = std::clamp(vpH * 0.13f, 96.0f, 150.0f);
        if (m_Ctx.Camera && m_Ctx.Camera->IsOrthographic())
            return m_Ctx.Camera->GetOrthoHalfHeight() * (2.0f * targetRadiusPx / vpH);

        const glm::vec4 clip = viewProj * glm::vec4(pivotRender, 1.0f);
        const float tanHalfFov = std::tan(glm::radians(m_Ctx.Camera ? m_Ctx.Camera->GetFOV() : 30.0f) * 0.5f);
        const float minDist = m_Ctx.Camera ? std::max(m_Ctx.Camera->GetNearClip() * 2.0f, 0.05f) : 0.05f;
        const float dist = std::max(clip.w, minDist);
        return dist * tanHalfFov * (2.0f * targetRadiusPx / vpH);
    }

    int ViewportCanvas::HitTestAxisIndicator(const UI::UIVec2& mousePos) const
    {
        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (!m_Ctx.Camera || isPlay || m_Ctx.Camera->IsOrthographic())
            return -1;
        const UI::UIRect& b = GetBounds();
        if (b.Width() < 90.0f || b.Height() < 90.0f || !b.Contains(mousePos))
            return -1;

        const glm::mat3 view(m_Ctx.Camera->GetGizmoView(m_Ctx.Camera->GetPosition()));
        const glm::vec2 center(b.Min.x + 40.0f, b.Max.y - 40.0f);
        constexpr float kLength = 24.0f;
        constexpr float kLabelRadius = 8.5f;

        const glm::vec3 worldAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        int bestIdx = -1;
        float nearest = kLabelRadius + 3.0f;
        float bestZ = -1e9f;
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 inView = view * worldAxes[i];
            const glm::vec2 end(center.x + inView.x * kLength, center.y - inView.y * kLength);
            const float d = std::hypot(mousePos.x - end.x, mousePos.y - end.y);
            if (d < nearest && inView.z >= bestZ - 0.01f)
            {
                nearest = d;
                bestZ = inView.z;
                bestIdx = i;
            }
        }
        return bestIdx;
    }

    ViewportCanvas::GizmoPart ViewportCanvas::HitTestGizmo(const UI::UIVec2& mousePos, const glm::mat4& worldTransform) const
    {
        if (!m_Ctx.Tools || m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Select || !m_Ctx.Camera || !m_Ctx.Render)
            return GizmoPart::None;

        const glm::mat4 viewProj = m_Ctx.Camera->GetGizmoProjection() * m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin());
        const glm::vec3 origin(worldTransform[3]);
        UI::UIVec2 originScreen{};
        if (!ProjectPoint(origin, viewProj, originScreen))
            return GizmoPart::None;

        const float scale = ComputeGizmoWorldScale(origin, viewProj);
        glm::vec3 axes[3];
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 col(worldTransform[i]);
            const float len = glm::length(col);
            axes[i] = len > 1e-6f ? (col / len) : glm::vec3(i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f);
        }

        const GizmoPart axisParts[3] = { GizmoPart::AxisX, GizmoPart::AxisY, GizmoPart::AxisZ };

        if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Rotate)
        {
            const float radius = scale * kRotateRingRadiusFactor;
            float bestDistSq = 121.0f;
            GizmoPart best = GizmoPart::None;

            for (int a = 0; a < 3; ++a)
            {
                const glm::vec3 u = axes[(a + 1) % 3];
                const glm::vec3 v = axes[(a + 2) % 3];
                glm::vec3 hitWorld{};
                if (ClosestPointOnRotateRing(mousePos, origin, u, v, radius, viewProj, hitWorld, bestDistSq))
                    best = axisParts[a];
            }
            return best;
        }

        if (glm::dot(mousePos - originScreen, mousePos - originScreen) <= 11.0f * 11.0f)
            return GizmoPart::FreeCenter;

        if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
        {
            struct PlaneSpec { int A; int B; GizmoPart Part; };
            const PlaneSpec planes[3] = {
                { 1, 2, GizmoPart::PlaneYZ },
                { 0, 2, GizmoPart::PlaneXZ },
                { 0, 1, GizmoPart::PlaneXY }
            };
            const float p0 = scale * 0.24f;
            const float p1 = scale * 0.44f;
            for (const PlaneSpec& ps : planes)
            {
                UI::UIVec2 c{};
                if (ProjectPoint(origin + (axes[ps.A] + axes[ps.B]) * ((p0 + p1) * 0.5f), viewProj, c))
                {
                    if (glm::dot(mousePos - c, mousePos - c) <= 12.0f * 12.0f)
                        return ps.Part;
                }
            }
        }

        float bestDistSq = 100.0f;
        GizmoPart best = GizmoPart::None;
        for (int a = 0; a < 3; ++a)
        {
            UI::UIVec2 endScreen{};
            if (!ProjectPoint(origin + axes[a] * scale, viewProj, endScreen))
                continue;
            const float dSq = DistPointToSegmentSq(mousePos, originScreen, endScreen);
            if (dSq < bestDistSq)
            {
                bestDistSq = dSq;
                best = axisParts[a];
            }
        }
        return best;
    }

    bool ViewportCanvas::PickWorldPoint(glm::dvec3& out) const
    {
        if (!m_Ctx.Render || !m_Ctx.Camera)
            return false;
        const NRI::Extent2D outputSize = m_Ctx.Render->getViewPortSize();
        const glm::vec2 viewportSize(static_cast<float>(outputSize.width), static_cast<float>(outputSize.height));
        if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
            return false;

        int32_t pickedX = 0, pickedY = 0;
        float depth = 0.0f;
        bool hasDepth = m_Ctx.Render->getPickedDepth(pickedX, pickedY, depth, 4);

        const UI::UIContext* ctx = UI::UIContext::GetCurrent();
        const glm::vec2 mouse = ctx ? ctx->GetLastMousePos() : Input::GetMousePosition();
        const glm::vec2 mousePixel(mouse.x - GetBounds().Min.x, mouse.y - GetBounds().Min.y);
        // The depth read back belongs to the last pick request. When the pointer has moved away since (pick requests pause while
        // an asset is dragged over the viewport), it is not the surface under the pointer: the ground plane is used instead.
        if (hasDepth && glm::length(glm::vec2(static_cast<float>(pickedX), static_cast<float>(pickedY)) - mousePixel) > 6.0f)
            hasDepth = false;

        glm::vec2 pixel = hasDepth ? glm::vec2(static_cast<float>(pickedX) + 0.5f, static_cast<float>(pickedY) + 0.5f) : mousePixel;

        const glm::vec2 ndc((pixel.x / viewportSize.x) * 2.0f - 1.0f, 1.0f - (pixel.y / viewportSize.y) * 2.0f);
        const glm::mat4 invVP = glm::inverse(
            m_Ctx.Camera->GetGizmoProjection() * m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin()));
        glm::vec4 nearPoint = invVP * glm::vec4(ndc, -1.0f, 1.0f);
        glm::vec4 farPoint = invVP * glm::vec4(ndc, 1.0f, 1.0f);
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;

        const glm::vec3 origin = glm::vec3(nearPoint);
        const glm::vec3 direction = glm::normalize(glm::vec3(farPoint - nearPoint));

        if (hasDepth && depth > 0.0f)
        {
            const glm::dmat4 inverse = glm::inverse(
                glm::dmat4(m_Ctx.Camera->GetProjection()) * glm::dmat4(m_Ctx.Camera->GetViewMatrix(m_Ctx.Render->GetRenderOrigin())));
            const glm::dvec4 world = inverse * glm::dvec4(ndc.x, ndc.y, depth, 1.0);
            if (std::abs(world.w) > 1e-12)
            {
                out = m_Ctx.Render->GetRenderOrigin() + glm::dvec3(world / world.w);
                return true;
            }
        }

        const int axis = m_Ctx.GridPlaneAxisFn ? m_Ctx.GridPlaneAxisFn() : 1;
        if (std::abs(direction[axis]) > 1e-4f)
        {
            const float plane = static_cast<float>(-m_Ctx.Render->GetRenderOrigin()[axis]);
            const float distance = (plane - origin[axis]) / direction[axis];
            if (distance > 0.0f)
            {
                out = m_Ctx.Render->FromRenderSpace(origin + direction * distance);
                return true;
            }
        }
        return false;
    }

    void ViewportCanvas::MeasureClick()
    {
        if (!m_Ctx.Tools)
            return;
        glm::dvec3 point{ 0.0 };
        if (!PickWorldPoint(point))
            return;
        if (m_Ctx.Tools->SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL))
        {
            const double step = m_Ctx.Tools->LocationSnapWorldUnits();
            point = glm::round(point / step) * step;
        }
        if (m_Ctx.Tools->MeasureStage == 1)
        {
            m_Ctx.Tools->MeasureB = point;
            m_Ctx.Tools->MeasureStage = 2;
        }
        else
        {
            m_Ctx.Tools->MeasureA = point;
            m_Ctx.Tools->MeasureStage = 1;
        }
    }

    void ViewportCanvas::CommitClickSelection(bool shift, bool ctrl)
    {
        if (!m_Ctx.Selection || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene)
            return;

        Entity picked = m_Ctx.HoveredEntity ? *m_Ctx.HoveredEntity : Entity{};
        while (picked && !ctrl && picked.HasComponent<PrefabNodeComponent>())
        {
            Entity instance = (*m_Ctx.ActiveScene)->GetEntityByUUID(picked.GetComponent<PrefabNodeComponent>().Instance);
            if (!instance)
                break;
            picked = instance;
        }

        // Unreal's viewport: click selects, Shift+click adds, Ctrl+click toggles, a click on nothing clears. (It was a range by
        // the hierarchy's row order, which means nothing in the viewport.)
        if (picked)
        {
            if (shift)
                m_Ctx.Selection->Add(picked);
            else if (ctrl)
                m_Ctx.Selection->Toggle(picked);
            else
                m_Ctx.Selection->Select(picked);
        }
        else if (!shift && !ctrl)
        {
            m_Ctx.Selection->Clear();
        }
    }

    void ViewportCanvas::CommitBoxSelection(bool shift, bool ctrl)
    {
        if (!m_Ctx.Render || !m_Ctx.Selection || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene)
            return;

        std::vector<Entity> boxed;
        for (int32_t id : m_Ctx.Render->getPickedEntityIDs())
        {
            if (id < 0)
                continue;
            Entity entity{ static_cast<entt::entity>(id), (*m_Ctx.ActiveScene).get() };
            if (entity)
                boxed.push_back(entity);
        }
        // A box replaces the selection; Shift adds to it; Ctrl toggles what it covers.
        if (!shift && !ctrl)
        {
            m_Ctx.Selection->Set(boxed);
            return;
        }
        for (Entity entity : boxed)
        {
            if (ctrl)
                m_Ctx.Selection->Toggle(entity);
            else
                m_Ctx.Selection->Add(entity);
        }
    }

    void ViewportCanvas::ApplyEntityDelta(const glm::mat4& deltaRender, const glm::mat4& primaryWorldRender)
    {
        if (!m_Ctx.Selection || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene || !m_Ctx.Render)
            return;
        const auto& selected = m_Ctx.Selection->GetEntities();
        Entity primary = m_Ctx.Selection->GetPrimary();
        if (!primary)
            return;

        if (selected.size() > 1)
        {
            for (Entity entity : selected)
            {
                if (!entity)
                    continue;
                if (entity.HasComponent<RelationshipComponent>())
                {
                    const UUID parentUUID = entity.GetComponent<RelationshipComponent>().Parent;
                    if (parentUUID != 0)
                    {
                        Entity parent = (*m_Ctx.ActiveScene)->GetEntityByUUID(parentUUID);
                        if (parent && m_Ctx.Selection->Contains(parent))
                            continue;
                    }
                }
                const glm::mat4 currentWorld = m_Ctx.Render->ToRenderSpace(entity.GetComponent<WorldTransformComponent>().WorldMatrix);
                entity.SetWorldTransform(m_Ctx.Render->FromRenderSpace(deltaRender * currentWorld));
            }
        }
        else
        {
            primary.SetWorldTransform(m_Ctx.Render->FromRenderSpace(primaryWorldRender));
        }
    }

    void ViewportCanvas::UpdateGizmoDrag(const UI::UIVec2& mousePos)
    {
        if (!m_Ctx.Tools || !m_Ctx.Camera || !m_Ctx.Render || !m_Ctx.Selection)
            return;
        Entity selected = m_Ctx.Selection->GetPrimary();
        if (!selected)
            return;

        glm::vec3 rayOrig{}, rayDir{};
        if (!RayFromScreen(mousePos, rayOrig, rayDir))
            return;

        const bool snap = m_Ctx.Tools->SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL);
        glm::mat4 newWorld = m_DragInitialEntityWorldRender;

        auto axisIndex = [](GizmoPart part) -> int
        {
            if (part == GizmoPart::AxisX) return 0;
            if (part == GizmoPart::AxisY) return 1;
            if (part == GizmoPart::AxisZ) return 2;
            return -1;
        };

        if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
        {
            if (const int idx = axisIndex(m_ActiveGizmo); idx >= 0)
            {
                const glm::vec3 axis = m_DragAxisDirs[idx];
                const float s = ClosestRayAxisParameter(rayOrig, rayDir, m_DragOriginRender, axis);
                float delta = s - m_DragStartScalar;
                if (snap)
                    delta = SnapValue(delta, m_Ctx.Tools->LocationSnapWorldUnits());
                newWorld = glm::translate(glm::mat4(1.0f), axis * delta) * m_DragInitialEntityWorldRender;
            }
            else if (m_ActiveGizmo == GizmoPart::FreeCenter)
            {
                glm::vec3 hit{};
                if (IntersectRayPlane(rayOrig, rayDir, m_DragOriginRender, m_DragPlaneNormal, hit))
                {
                    glm::vec3 delta = hit - m_DragStartHit;
                    if (snap)
                    {
                        const float step = m_Ctx.Tools->LocationSnapWorldUnits();
                        delta = glm::round(delta / step) * step;
                    }
                    newWorld = glm::translate(glm::mat4(1.0f), delta) * m_DragInitialEntityWorldRender;
                }
            }
            else
            {
                int a0 = 1, a1 = 2;
                if (m_ActiveGizmo == GizmoPart::PlaneXZ) { a0 = 0; a1 = 2; }
                else if (m_ActiveGizmo == GizmoPart::PlaneXY) { a0 = 0; a1 = 1; }
                glm::vec3 hit{};
                if (IntersectRayPlane(rayOrig, rayDir, m_DragOriginRender, m_DragPlaneNormal, hit))
                {
                    const glm::vec3 dVec = hit - m_DragStartHit;
                    float c0 = glm::dot(dVec, m_DragAxisDirs[a0]);
                    float c1 = glm::dot(dVec, m_DragAxisDirs[a1]);
                    if (snap)
                    {
                        const float step = m_Ctx.Tools->LocationSnapWorldUnits();
                        c0 = SnapValue(c0, step);
                        c1 = SnapValue(c1, step);
                    }
                    newWorld = glm::translate(glm::mat4(1.0f), m_DragAxisDirs[a0] * c0 + m_DragAxisDirs[a1] * c1) * m_DragInitialEntityWorldRender;
                }
            }
        }
        else if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Rotate)
        {
            if (const int idx = axisIndex(m_ActiveGizmo); idx >= 0)
            {
                const glm::vec3 axis = m_DragAxisDirs[idx];
                const glm::vec2 mouseDelta = mousePos - m_DragPrevMousePos;
                float stepRad = 0.0f;
                if (m_DragRotateCircularMode)
                {
                    const glm::mat4 viewProj = m_Ctx.Camera->GetGizmoProjection() * m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin());
                    UI::UIVec2 oScr{};
                    if (ProjectPoint(m_DragOriginRender, viewProj, oScr))
                    {
                        const glm::vec2 d0 = m_DragPrevMousePos - oScr;
                        const glm::vec2 d1 = mousePos - oScr;
                        if (glm::length(d0) > 8.0f && glm::length(d1) > 8.0f)
                        {
                            const float cross2d = d0.x * d1.y - d0.y * d1.x;
                            const float dot2d = glm::dot(d0, d1);
                            stepRad = std::atan2(cross2d, dot2d) * m_DragRotateScreenSign;
                        }
                        else
                        {
                            stepRad = glm::dot(mouseDelta, m_DragRotateScreenTangent) / m_DragRotatePixelRadius;
                        }
                    }
                    else
                    {
                        stepRad = glm::dot(mouseDelta, m_DragRotateScreenTangent) / m_DragRotatePixelRadius;
                    }
                }
                else
                {
                    stepRad = glm::dot(mouseDelta, m_DragRotateScreenTangent) / m_DragRotatePixelRadius;
                }
                m_DragRotateAccumRad += stepRad;
                m_DragPrevMousePos = mousePos;

                float angleDeg = glm::degrees(m_DragRotateAccumRad);
                if (snap)
                {
                    const float step = ViewportToolState::kRotationSnaps[std::clamp(m_Ctx.Tools->RotationSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kRotationSnaps)) - 1)];
                    angleDeg = SnapValue(angleDeg, step);
                }
                m_DragRotateAppliedDeg = angleDeg;
                const glm::mat4 rot = glm::translate(glm::mat4(1.0f), m_DragOriginRender) *
                                      glm::rotate(glm::mat4(1.0f), glm::radians(angleDeg), axis) *
                                      glm::translate(glm::mat4(1.0f), -m_DragOriginRender);
                newWorld = rot * m_DragInitialEntityWorldRender;
            }
        }
        else if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Scale)
        {
            const float step = ViewportToolState::kScaleSnaps[std::clamp(m_Ctx.Tools->ScaleSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kScaleSnaps)) - 1)];
            const glm::vec3 offsetFromPivot = glm::vec3(m_DragInitialEntityWorldRender[3]) - m_DragOriginRender;
            if (const int idx = axisIndex(m_ActiveGizmo); idx >= 0)
            {
                const glm::vec3 axis = m_DragAxisDirs[idx];
                const float s = ClosestRayAxisParameter(rayOrig, rayDir, m_DragOriginRender, axis);
                const float denom = std::max(std::abs(m_DragStartScalar), 0.1f);
                float factor = 1.0f + (s - m_DragStartScalar) / denom;
                if (snap)
                    factor = SnapValue(factor, step);
                factor = std::max(factor, 0.01f);
                newWorld[idx] = m_DragInitialEntityWorldRender[idx] * factor;
                newWorld[3] = m_DragInitialEntityWorldRender[3] + glm::vec4(axis * (glm::dot(offsetFromPivot, axis) * (factor - 1.0f)), 0.0f);
            }
            else if (m_ActiveGizmo == GizmoPart::FreeCenter)
            {
                float factor = 1.0f + (mousePos.x - m_DragStartMouseX) * 0.01f;
                if (snap)
                    factor = SnapValue(factor, step);
                factor = std::max(factor, 0.01f);
                newWorld[0] = m_DragInitialEntityWorldRender[0] * factor;
                newWorld[1] = m_DragInitialEntityWorldRender[1] * factor;
                newWorld[2] = m_DragInitialEntityWorldRender[2] * factor;
                newWorld[3] = glm::vec4(m_DragOriginRender + offsetFromPivot * factor, 1.0f);
            }
        }

        const glm::mat4 deltaMatrix = newWorld * glm::inverse(m_DragPrevWorldRender);
        m_DragPrevWorldRender = newWorld;
        ApplyEntityDelta(deltaMatrix, newWorld);
    }

    void ViewportCanvas::OnUpdate(float dt)
    {
        if (!m_Ctx.Tools || !m_Ctx.Camera)
            return;

        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (!isPlay && !m_Ctx.Camera->IsOrthographic() && m_Ctx.Tools->ViewTurn.Active)
        {
            auto& vt = m_Ctx.Tools->ViewTurn;
            vt.Time = std::min(vt.Time + dt / 0.2f, 1.0f);
            const float t = vt.Time * vt.Time * (3.0f - 2.0f * vt.Time);
            m_Ctx.Camera->SetOrientation(glm::mix(vt.FromPitch, vt.ToPitch, t), glm::mix(vt.FromYaw, vt.ToYaw, t));
            vt.Active = vt.Time < 1.0f;
        }
    }

    bool ViewportCanvas::OnPointerMove(const UI::UIPointerEvent& e)
    {
        if (m_Ctx.Camera)
            m_Ctx.Camera->SetCursorPosition({ e.Position.x - GetBounds().Min.x, e.Position.y - GetBounds().Min.y });

        if (!m_Ctx.Tools)
            return false;

        if (m_Ctx.Tools->GizmoUsing)
        {
            UpdateGizmoDrag(e.Position);
            return true;
        }

        if (m_LeftTracking)
        {
            if (!m_Ctx.Tools->IsBoxSelecting)
            {
                const UI::UIVec2 diff = e.Position - m_LeftDownPos;
                if (diff.x * diff.x + diff.y * diff.y > 16.0f)
                {
                    m_Ctx.Tools->IsBoxSelecting = true;
                    m_Ctx.Tools->BoxSelectStart = m_LeftDownPos;
                }
            }
            if (m_Ctx.Tools->IsBoxSelecting)
                m_Ctx.Tools->BoxSelectEnd = e.Position;
            return true;
        }

        m_HoveredAxisIdx = HitTestAxisIndicator(e.Position);
        m_Ctx.Tools->AxisIndicatorHovered = (m_HoveredAxisIdx >= 0);

        m_HoveredGizmo = GizmoPart::None;
        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (!m_Ctx.Tools->AxisIndicatorHovered && m_Ctx.Selection && m_Ctx.Render && !isPlay)
        {
            if (Entity sel = m_Ctx.Selection->GetPrimary())
            {
                glm::mat4 world{};
                if (ComputeGizmoTransform(sel, world))
                    m_HoveredGizmo = HitTestGizmo(e.Position, world);
            }
        }
        m_Ctx.Tools->GizmoHovered = (m_HoveredGizmo != GizmoPart::None);

        if (m_HoveredAxisIdx >= 0 && m_Ctx.Camera)
        {
            static const glm::vec3 kWorldAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            static const char* kNames[3] = { "X", "Y", "Z" };
            const glm::mat4 view = m_Ctx.Camera->GetGizmoView(m_Ctx.Camera->GetPosition());
            const glm::vec3 forward = glm::normalize(-glm::vec3(glm::inverse(view)[2]));
            const bool fromOpposite = glm::dot(forward, -kWorldAxes[m_HoveredAxisIdx]) > 0.999f;
            Tooltip = std::string("View from ") + (fromOpposite ? "-" : "+") + kNames[m_HoveredAxisIdx] +
                      " (click again: from " + (fromOpposite ? "+" : "-") + kNames[m_HoveredAxisIdx] + ")";
        }
        else
        {
            Tooltip.clear();
        }
        return false;
    }

    bool ViewportCanvas::OnPointerLeave(const UI::UIPointerEvent&)
    {
        m_HoveredAxisIdx = -1;
        m_HoveredGizmo = GizmoPart::None;
        Tooltip.clear();
        if (m_Ctx.Tools)
        {
            m_Ctx.Tools->AxisIndicatorHovered = false;
            m_Ctx.Tools->GizmoHovered = false;
        }
        return false;
    }

    bool ViewportCanvas::OnPointerDown(const UI::UIPointerEvent& e)
    {
        if (!m_Ctx.Tools)
            return false;

        if (e.Button != 1)
        {
            if (m_Ctx.OnCaptureChanged)
                m_Ctx.OnCaptureChanged(true, e.Button);
            return true;
        }

        const bool alt = Input::IsKeyPressed(SDL_SCANCODE_LALT) || Input::IsKeyPressed(SDL_SCANCODE_RALT);
        if (alt)
        {
            if (m_Ctx.OnCaptureChanged)
                m_Ctx.OnCaptureChanged(true, e.Button);
            return true;
        }

        if (const int axisIdx = HitTestAxisIndicator(e.Position); axisIdx >= 0 && m_Ctx.Camera)
        {
            static const glm::vec3 kWorldAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            const glm::mat4 view = m_Ctx.Camera->GetGizmoView(m_Ctx.Camera->GetPosition());
            const glm::vec3 forward = glm::normalize(-glm::vec3(glm::inverse(view)[2]));
            const bool fromOpposite = glm::dot(forward, -kWorldAxes[axisIdx]) > 0.999f;
            const glm::vec3 target = fromOpposite ? kWorldAxes[axisIdx] : -kWorldAxes[axisIdx];
            const float a = std::asin(std::clamp(target.y, -1.0f, 1.0f));
            const float b = std::abs(target.y) > 0.999f ? -m_Ctx.Camera->GetYaw() : std::atan2(-target.x, -target.z);
            const float fromYaw = m_Ctx.Camera->GetYaw();
            float toYaw = -b;
            toYaw = fromYaw + std::remainder(toYaw - fromYaw, glm::two_pi<float>());
            m_Ctx.Tools->ViewTurn = { true, m_Ctx.Camera->GetPitch(), fromYaw, -a, toYaw, 0.0f };
            return true;
        }

        if (m_Ctx.Tools->MeasureTool)
        {
            MeasureClick();
            return true;
        }

        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (m_Ctx.Tools->GizmoMode != ViewportGizmoMode::Select && m_Ctx.Selection && m_Ctx.Render && !isPlay)
        {
            if (Entity sel = m_Ctx.Selection->GetPrimary(); sel && sel.HasComponent<WorldTransformComponent>())
            {
                glm::mat4 world{};
                if (ComputeGizmoTransform(sel, world))
                {
                    const GizmoPart hit = HitTestGizmo(e.Position, world);
                    if (hit != GizmoPart::None)
                    {
                        glm::vec3 rayOrig{}, rayDir{};
                        if (RayFromScreen(e.Position, rayOrig, rayDir))
                        {
                            const glm::mat4 entityWorld = m_Ctx.Render->ToRenderSpace(sel.GetComponent<WorldTransformComponent>().WorldMatrix);
                            m_ActiveGizmo = hit;
                            m_DragInitialWorldRender = world;
                            m_DragInitialEntityWorldRender = entityWorld;
                            m_DragPrevWorldRender = entityWorld;
                            m_DragOriginRender = glm::vec3(world[3]);
                            m_DragPrevMousePos = e.Position;
                            for (int i = 0; i < 3; ++i)
                            {
                                const glm::vec3 col(world[i]);
                                const float len = glm::length(col);
                                m_DragAxisDirs[i] = len > 1e-6f ? (col / len) : glm::vec3(i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f);
                            }
                            m_DragStartMouseX = e.Position.x;

                            int aIdx = hit == GizmoPart::AxisX ? 0 : hit == GizmoPart::AxisY ? 1 : hit == GizmoPart::AxisZ ? 2 : -1;
                            if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Rotate && aIdx >= 0 && m_Ctx.Camera)
                            {
                                const glm::vec3 axis = m_DragAxisDirs[aIdx];
                                const glm::vec3 u = m_DragAxisDirs[(aIdx + 1) % 3];
                                const glm::vec3 v = m_DragAxisDirs[(aIdx + 2) % 3];
                                const glm::mat4 viewProj = m_Ctx.Camera->GetGizmoProjection() * m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin());
                                const float scale = ComputeGizmoWorldScale(m_DragOriginRender, viewProj);
                                const float radius = scale * kRotateRingRadiusFactor;
                                glm::vec3 closestWorld{};
                                float bestDistSq = std::numeric_limits<float>::max();
                                if (!ClosestPointOnRotateRing(e.Position, m_DragOriginRender, u, v, radius, viewProj, closestWorld, bestDistSq))
                                    closestWorld = m_DragOriginRender + u * radius;

                                glm::vec3 r0 = closestWorld - m_DragOriginRender;
                                r0 -= axis * glm::dot(r0, axis);
                                r0 = glm::length(r0) > 1e-5f ? glm::normalize(r0) : u;
                                const glm::vec3 tangent3D = glm::normalize(glm::cross(axis, r0));

                                UI::UIVec2 oScr{}, pScr{}, pTanScr{}, tRingScr{};
                                ProjectPoint(m_DragOriginRender, viewProj, oScr);
                                const bool hasP = ProjectPoint(m_DragOriginRender + r0 * radius, viewProj, pScr);
                                const bool hasPTan = ProjectPoint(m_DragOriginRender + r0 * radius + tangent3D * (radius * 0.25f), viewProj, pTanScr);
                                if (hasP && hasPTan && glm::length(pTanScr - pScr) > 1e-3f)
                                    m_DragRotateScreenTangent = glm::normalize(pTanScr - pScr);
                                else
                                    m_DragRotateScreenTangent = { 1.0f, 0.0f };
                                m_DragRotatePixelRadius = hasP ? std::max(glm::length(pScr - oScr), 48.0f) : 96.0f;

                                const glm::vec3 toCam = m_Ctx.Camera->IsOrthographic()
                                    ? -m_Ctx.Camera->GetForwardDirection()
                                    : glm::normalize(glm::vec3(m_Ctx.Camera->GetPosition() - m_Ctx.Render->GetRenderOrigin()) - m_DragOriginRender);
                                m_DragRotateCircularMode = (std::abs(glm::dot(toCam, axis)) > 0.25f);
                                if (m_DragRotateCircularMode && hasP && ProjectPoint(m_DragOriginRender + (r0 + tangent3D) * radius, viewProj, tRingScr))
                                {
                                    const glm::vec2 a = pScr - oScr;
                                    const glm::vec2 b = tRingScr - oScr;
                                    const float cross2d = a.x * b.y - a.y * b.x;
                                    m_DragRotateScreenSign = (cross2d >= 0.0f) ? 1.0f : -1.0f;
                                }
                                else
                                {
                                    m_DragRotateScreenSign = 1.0f;
                                }
                                m_DragRotateStartRadial = r0;
                                m_DragRotateAccumRad = 0.0f;
                                m_DragRotateAppliedDeg = 0.0f;
                            }
                            else if (aIdx >= 0)
                            {
                                m_DragStartScalar = ClosestRayAxisParameter(rayOrig, rayDir, m_DragOriginRender, m_DragAxisDirs[aIdx]);
                                IntersectRayPlane(rayOrig, rayDir, m_DragOriginRender, m_DragAxisDirs[aIdx], m_DragStartHit);
                            }
                            else if (hit == GizmoPart::FreeCenter)
                            {
                                m_DragPlaneNormal = m_Ctx.Camera ? -m_Ctx.Camera->GetForwardDirection() : glm::vec3(0, 0, 1);
                                IntersectRayPlane(rayOrig, rayDir, m_DragOriginRender, m_DragPlaneNormal, m_DragStartHit);
                            }
                            else
                            {
                                const int nIdx = hit == GizmoPart::PlaneYZ ? 0 : hit == GizmoPart::PlaneXZ ? 1 : 2;
                                m_DragPlaneNormal = m_DragAxisDirs[nIdx];
                                IntersectRayPlane(rayOrig, rayDir, m_DragOriginRender, m_DragPlaneNormal, m_DragStartHit);
                            }

                            m_Ctx.Tools->GizmoUsing = true;
                            m_Ctx.Tools->GizmoWasUsing = true;
                            m_Ctx.Tools->GizmoDragStart = world;
                            return true;
                        }
                    }
                }
            }
        }

        const bool shift = Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);
        const bool ctrl = Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);
        CommitClickSelection(shift, ctrl);

        m_LeftTracking = true;
        m_LeftDownPos = e.Position;
        m_Ctx.Tools->BoxSelectStart = e.Position;
        m_Ctx.Tools->BoxSelectEnd = e.Position;
        return true;
    }

    bool ViewportCanvas::OnPointerUp(const UI::UIPointerEvent& e)
    {
        if (m_Ctx.OnCaptureChanged)
            m_Ctx.OnCaptureChanged(false, e.Button);

        if (!m_Ctx.Tools)
            return false;

        if (e.Button == 1 && m_Ctx.Tools->GizmoUsing)
        {
            m_Ctx.Tools->GizmoUsing = false;
            m_ActiveGizmo = GizmoPart::None;
            if (m_Ctx.OnSceneMutated)
                m_Ctx.OnSceneMutated();
            return true;
        }

        if (e.Button == 1 && m_LeftTracking)
        {
            m_LeftTracking = false;
            if (m_Ctx.Tools->IsBoxSelecting)
            {
                m_Ctx.Tools->IsBoxSelecting = false;
                const bool shift = Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) || Input::IsKeyPressed(SDL_SCANCODE_RSHIFT);
                const bool ctrl = Input::IsKeyPressed(SDL_SCANCODE_LCTRL) || Input::IsKeyPressed(SDL_SCANCODE_RCTRL);
                CommitBoxSelection(shift, ctrl);
            }
            return true;
        }
        return false;
    }

    void ViewportCanvas::DrawMeasureLine(UI::UIDrawList& drawList, const glm::mat4& view, const glm::mat4& projection,
                                         const glm::vec3& startRender, const glm::vec3& endRender, float minimumLength) const
    {
        const float distance = glm::length(endRender - startRender);
        if (distance < minimumLength)
            return;

        const glm::mat4 vp = projection * view;
        UI::UIVec2 a{}, b{};
        if (!ProjectPoint(startRender, vp, a) || !ProjectPoint(endRender, vp, b))
            return;

        glm::vec2 dir = b - a;
        const float len = glm::length(dir);
        if (len < 1.0f)
            return;
        dir /= len;
        const glm::vec2 across(-dir.y * 9.0f, dir.x * 9.0f);
        const glm::vec4 yellow{ 1.0f, 0.88f, 0.27f, 1.0f };
        const glm::vec4 casing{ 0.0f, 0.0f, 0.0f, 0.63f };

        DrawLine(drawList, a, b, 4.0f, casing);
        DrawLine(drawList, a, b, 2.0f, yellow);
        DrawLine(drawList, a - across, a + across, 2.0f, yellow);
        DrawLine(drawList, b - across, b + across, 2.0f, yellow);

        const std::string text = WorldUnits::Format(distance);
        constexpr float fontSize = 14.0f;
        const UI::UIVec2 textSize = UI::UILabel::MeasureText(text, nullptr, fontSize);
        const glm::vec2 middle = (a + b) * 0.5f;
        const glm::vec2 boxMin(middle.x - textSize.x * 0.5f - 5.0f, middle.y - textSize.y * 0.5f - 3.0f);
        const glm::vec2 boxMax(middle.x + textSize.x * 0.5f + 5.0f, middle.y + textSize.y * 0.5f + 3.0f);
        drawList.AddRoundedRect(boxMin, boxMax, { 0.0f, 0.0f, 0.0f, 0.78f }, glm::vec4(3.0f));
        drawList.AddText(text, nullptr, { boxMin.x + 5.0f, boxMin.y + 3.0f }, fontSize, yellow);
    }

    void ViewportCanvas::DrawMeasureOverlay(UI::UIDrawList& drawList) const
    {
        if (!m_Ctx.Tools || (!m_Ctx.Tools->MeasureTool && m_Ctx.Tools->MeasureStage == 0) || !m_Ctx.Camera || !m_Ctx.Render)
            return;

        const glm::mat4 view = m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin());
        const glm::mat4 proj = m_Ctx.Camera->GetGizmoProjection();
        const glm::mat4 vp = proj * view;

        glm::dvec3 worldCursor{ 0.0 };
        bool hasCursor = false;
        const bool hovered = UI::HasState(GetState(), UI::UIState::Hovered);
        if (m_Ctx.Tools->MeasureTool && hovered)
        {
            hasCursor = PickWorldPoint(worldCursor);
            if (hasCursor && m_Ctx.Tools->SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL))
            {
                const double step = m_Ctx.Tools->LocationSnapWorldUnits();
                worldCursor = glm::round(worldCursor / step) * step;
            }
        }

        const glm::vec3 cursor = m_Ctx.Render->ToRenderSpace(worldCursor);
        const glm::vec3 measureA = m_Ctx.Render->ToRenderSpace(m_Ctx.Tools->MeasureA);
        const glm::vec3 measureB = m_Ctx.Render->ToRenderSpace(m_Ctx.Tools->MeasureB);

        if (m_Ctx.Tools->MeasureStage == 1 && hasCursor)
            DrawMeasureLine(drawList, view, proj, measureA, cursor, 0.0f);
        else if (m_Ctx.Tools->MeasureStage == 2)
            DrawMeasureLine(drawList, view, proj, measureA, measureB, 0.0f);

        if (hasCursor)
        {
            UI::UIVec2 screen{};
            if (ProjectPoint(cursor, vp, screen))
            {
                DrawCircleRing(drawList, screen, 6.0f, 3.0f, { 0.0f, 0.0f, 0.0f, 0.78f });
                DrawCircleRing(drawList, screen, 6.0f, 1.5f, { 1.0f, 0.88f, 0.27f, 1.0f });
            }
        }
    }

    void ViewportCanvas::DrawTransformGizmo(UI::UIDrawList& drawList) const
    {
        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (!m_Ctx.Tools || m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Select || !m_Ctx.Camera || !m_Ctx.Render || !m_Ctx.Selection || isPlay)
            return;
        Entity selected = m_Ctx.Selection->GetPrimary();
        if (!selected || !selected.HasComponent<WorldTransformComponent>())
            return;

        const glm::mat4 view = m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin());
        const glm::mat4 proj = m_Ctx.Camera->GetGizmoProjection();
        const glm::mat4 vp = proj * view;
        glm::mat4 world{};
        if (m_Ctx.Tools->GizmoUsing)
        {
            world = m_DragPrevWorldRender;
            const glm::vec3 translateOffset = (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
                ? glm::vec3(m_DragPrevWorldRender[3] - m_DragInitialEntityWorldRender[3])
                : glm::vec3(0.0f);
            world[3] = glm::vec4(m_DragOriginRender + translateOffset, 1.0f);
        }
        else if (!ComputeGizmoTransform(selected, world))
        {
            return;
        }

        const glm::vec3 origin(world[3]);
        UI::UIVec2 oScr{};
        if (!ProjectPoint(origin, vp, oScr))
            return;

        const float scale = ComputeGizmoWorldScale(origin, vp);
        glm::vec3 axes[3];
        for (int i = 0; i < 3; ++i)
        {
            const glm::vec3 col(world[i]);
            const float len = glm::length(col);
            axes[i] = len > 1e-6f ? (col / len) : glm::vec3(i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f);
        }

        const GizmoPart activeHighlight = m_Ctx.Tools->GizmoUsing ? m_ActiveGizmo : m_HoveredGizmo;
        const glm::vec4 baseColors[3] = {
            { 0.92f, 0.28f, 0.28f, 1.0f },
            { 0.43f, 0.78f, 0.28f, 1.0f },
            { 0.28f, 0.54f, 0.94f, 1.0f }
        };
        const glm::vec4 highlightCol{ 1.0f, 0.86f, 0.25f, 1.0f };
        const GizmoPart axisParts[3] = { GizmoPart::AxisX, GizmoPart::AxisY, GizmoPart::AxisZ };

        if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Rotate)
        {
            constexpr int kSegs = 64;
            const float radius = scale * kRotateRingRadiusFactor;
            const glm::vec3 toCam = m_Ctx.Camera->IsOrthographic()
                ? -m_Ctx.Camera->GetForwardDirection()
                : glm::normalize(glm::vec3(m_Ctx.Camera->GetPosition() - m_Ctx.Render->GetRenderOrigin()) - origin);
            for (int a = 0; a < 3; ++a)
            {
                const bool isActive = (activeHighlight == axisParts[a]);
                const bool dimmedOther = m_Ctx.Tools->GizmoUsing && !isActive;
                glm::vec4 col = isActive ? highlightCol : baseColors[a];
                if (dimmedOther)
                    col.a *= 0.25f;
                const float thick = isActive ? 3.2f : 2.1f;
                const glm::vec3 u = axes[(a + 1) % 3];
                const glm::vec3 v = axes[(a + 2) % 3];
                UI::UIVec2 prev{};
                glm::vec3 prevWorld{};
                bool hasPrev = false;
                for (int s = 0; s <= kSegs; ++s)
                {
                    const float ang = glm::two_pi<float>() * static_cast<float>(s) / kSegs;
                    const glm::vec3 pW = origin + (u * std::cos(ang) + v * std::sin(ang)) * radius;
                    UI::UIVec2 cur{};
                    const bool ok = ProjectPoint(pW, vp, cur);
                    if (ok && hasPrev)
                    {
                        const glm::vec3 midOffset = (prevWorld + pW) * 0.5f - origin;
                        const bool frontFacing = isActive || glm::dot(midOffset, toCam) >= -radius * 0.05f;
                        glm::vec4 segCol = col;
                        if (!frontFacing && !dimmedOther)
                            segCol.a *= 0.34f;
                        DrawLine(drawList, prev, cur, thick, segCol);
                    }
                    prev = cur;
                    prevWorld = pW;
                    hasPrev = ok;
                }
            }

            if (m_Ctx.Tools->GizmoUsing)
            {
                const int activeIdx = m_ActiveGizmo == GizmoPart::AxisX ? 0 : m_ActiveGizmo == GizmoPart::AxisY ? 1 : m_ActiveGizmo == GizmoPart::AxisZ ? 2 : -1;
                if (activeIdx >= 0)
                {
                    const glm::vec3 axis = m_DragAxisDirs[activeIdx];
                    const glm::vec3 u = m_DragRotateStartRadial;
                    const glm::vec3 v = glm::normalize(glm::cross(axis, u));
                    const float clampedRad = glm::clamp(glm::radians(m_DragRotateAppliedDeg), -glm::two_pi<float>(), glm::two_pi<float>());
                    constexpr int kArcSegs = 28;
                    UI::UIVec2 startScr{}, prevScr{};
                    bool hasStart = ProjectPoint(origin + u * radius, vp, startScr);
                    if (hasStart)
                        DrawLine(drawList, oScr, startScr, 1.8f, { 1.0f, 0.86f, 0.25f, 0.75f });

                    bool hasPrev = false;
                    UI::UIVec2 endScr{};
                    for (int s = 0; s <= kArcSegs; ++s)
                    {
                        const float ang = clampedRad * (static_cast<float>(s) / kArcSegs);
                        const glm::vec3 pW = origin + (u * std::cos(ang) + v * std::sin(ang)) * radius;
                        UI::UIVec2 curScr{};
                        const bool ok = ProjectPoint(pW, vp, curScr);
                        if (ok && hasPrev)
                            DrawLine(drawList, prevScr, curScr, 4.0f, { 1.0f, 0.86f, 0.25f, 0.90f });
                        if (ok)
                            endScr = curScr;
                        prevScr = curScr;
                        hasPrev = ok;
                    }
                    if (hasPrev)
                        DrawLine(drawList, oScr, endScr, 1.8f, { 1.0f, 0.86f, 0.25f, 0.95f });

                    char degBuf[32];
                    std::snprintf(degBuf, sizeof(degBuf), "%+.1f deg", m_DragRotateAppliedDeg);
                    const UI::UIVec2 labelPos{ oScr.x + 14.0f, oScr.y - 24.0f };
                    const UI::UIVec2 textSize = UI::UILabel::MeasureText(degBuf, nullptr, 13.0f);
                    drawList.AddRoundedRect(labelPos - glm::vec2(5.0f, 3.0f), labelPos + textSize + glm::vec2(5.0f, 3.0f),
                                            { 0.08f, 0.09f, 0.11f, 0.85f }, glm::vec4(4.0f));
                    drawList.AddText(degBuf, nullptr, labelPos, 13.0f, { 1.0f, 0.88f, 0.30f, 1.0f });
                }
            }
            return;
        }

        if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
        {
            struct PlaneDraw { int A; int B; int ColorIdx; GizmoPart Part; };
            const PlaneDraw planes[3] = {
                { 1, 2, 0, GizmoPart::PlaneYZ },
                { 0, 2, 1, GizmoPart::PlaneXZ },
                { 0, 1, 2, GizmoPart::PlaneXY }
            };
            const float p0 = scale * 0.24f;
            const float p1 = scale * 0.44f;
            for (const PlaneDraw& pd : planes)
            {
                const bool lit = (activeHighlight == pd.Part);
                glm::vec4 c = lit ? highlightCol : baseColors[pd.ColorIdx];
                UI::UIVec2 center{};
                if (ProjectPoint(origin + (axes[pd.A] + axes[pd.B]) * ((p0 + p1) * 0.5f), vp, center))
                {
                    drawList.AddRoundedRect(
                        center - glm::vec2(6.0f),
                        center + glm::vec2(6.0f),
                        { c.r, c.g, c.b, lit ? 0.55f : 0.26f },
                        glm::vec4(2.0f),
                        1.4f,
                        c
                    );
                }
            }
        }

        for (int a = 0; a < 3; ++a)
        {
            const bool lit = (activeHighlight == axisParts[a]);
            const glm::vec4 col = lit ? highlightCol : baseColors[a];
            const float thick = lit ? 3.2f : 2.3f;
            UI::UIVec2 endScr{};
            if (!ProjectPoint(origin + axes[a] * scale, vp, endScr))
                continue;

            DrawLine(drawList, oScr + glm::vec2(1.0f), endScr + glm::vec2(1.0f), thick + 0.8f, { 0.0f, 0.0f, 0.0f, 0.4f });
            DrawLine(drawList, oScr, endScr, thick, col);

            if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
            {
                const glm::vec2 dir = glm::length(endScr - oScr) > 1.0f ? glm::normalize(endScr - oScr) : glm::vec2(1, 0);
                const glm::vec2 perp(-dir.y, dir.x);
                const glm::vec2 tip = endScr + dir * 7.0f;
                const glm::vec2 base = endScr - dir * 2.0f;
                DrawLine(drawList, tip, base + perp * 4.5f, 2.2f, col);
                DrawLine(drawList, tip, base - perp * 4.5f, 2.2f, col);
                DrawLine(drawList, base + perp * 4.5f, base - perp * 4.5f, 2.0f, col);
            }
            else
            {
                drawList.AddRoundedRect(endScr - glm::vec2(4.5f), endScr + glm::vec2(4.5f), col, glm::vec4(1.5f));
            }
        }

        const bool centerLit = (activeHighlight == GizmoPart::FreeCenter);
        const glm::vec4 centerCol = centerLit ? highlightCol : glm::vec4{ 0.95f, 0.95f, 0.95f, 0.92f };
        if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
            DrawCircleFill(drawList, oScr, centerLit ? 5.5f : 4.2f, centerCol);
        else
            drawList.AddRoundedRect(oScr - glm::vec2(4.5f), oScr + glm::vec2(4.5f), centerCol, glm::vec4(1.5f));

        if (m_Ctx.Tools->GizmoUsing && m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Translate)
        {
            const bool snap = m_Ctx.Tools->SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL);
            if (snap)
            {
                DrawMeasureLine(drawList, view, proj, glm::vec3(m_Ctx.Tools->GizmoDragStart[3]),
                                origin, m_Ctx.Tools->LocationSnapWorldUnits() * 0.05f);
            }
        }
    }

    void ViewportCanvas::DrawAxisIndicator(UI::UIDrawList& drawList) const
    {
        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (!m_Ctx.Camera || isPlay)
            return;
        const UI::UIRect& b = GetBounds();
        if (b.Width() < 90.0f || b.Height() < 90.0f)
            return;

        const glm::mat3 view(m_Ctx.Camera->GetGizmoView(m_Ctx.Camera->GetPosition()));
        const glm::vec2 center(b.Min.x + 40.0f, b.Max.y - 40.0f);
        constexpr float kLength = 24.0f;
        constexpr float kLabelRadius = 9.0f;

        struct AxisDraw
        {
            int Index;
            glm::vec3 InView;
            glm::vec4 Color;
            const char* Name;
        };
        AxisDraw axes[3] = {
            { 0, view * glm::vec3(1, 0, 0), { 0.886f, 0.290f, 0.290f, 1.0f }, "X" },
            { 1, view * glm::vec3(0, 1, 0), { 0.431f, 0.769f, 0.282f, 1.0f }, "Y" },
            { 2, view * glm::vec3(0, 0, 1), { 0.290f, 0.525f, 0.910f, 1.0f }, "Z" }
        };
        std::sort(std::begin(axes), std::end(axes), [](const AxisDraw& a, const AxisDraw& c) { return a.InView.z < c.InView.z; });

        for (const AxisDraw& ax : axes)
        {
            const glm::vec2 end(center.x + ax.InView.x * kLength, center.y - ax.InView.y * kLength);
            const bool isHovered = (ax.Index == m_HoveredAxisIdx);
            DrawLine(drawList, center + glm::vec2(1.0f), end + glm::vec2(1.0f), 2.0f, { 0.0f, 0.0f, 0.0f, 0.43f });
            DrawLine(drawList, center, end, 2.0f, ax.Color);

            if (isHovered)
                DrawCircleFill(drawList, end, kLabelRadius, ax.Color);
            else
                DrawCircleFill(drawList, end, kLabelRadius - 1.5f, { 0.078f, 0.078f, 0.086f, 0.67f });

            constexpr float fontSize = 12.0f;
            const UI::UIVec2 tSize = UI::UILabel::MeasureText(ax.Name, nullptr, fontSize);
            drawList.AddText(ax.Name, nullptr, { end.x - tSize.x * 0.5f, end.y - tSize.y * 0.5f }, fontSize,
                             isHovered ? glm::vec4(1.0f) : ax.Color);
        }

        if (m_Ctx.ShowCameraPositionFn && m_Ctx.ShowCameraPositionFn())
        {
            const glm::dvec3 pos = m_Ctx.Camera->GetPosition();
            const double meters = static_cast<double>(WorldUnits::ToMeters(1.0f));
            char text[160];
            std::snprintf(text, sizeof(text), "X %.0f  Y %.0f  Z %.0f  (%.1f, %.1f, %.1f m)",
                          pos.x, pos.y, pos.z, pos.x * meters, pos.y * meters, pos.z * meters);
            constexpr float fontSize = 13.0f;
            const UI::UIVec2 size = UI::UILabel::MeasureText(text, nullptr, fontSize);
            const glm::vec2 textPos(center.x + kLength + 22.0f, b.Max.y - 24.0f);
            drawList.AddRoundedRect({ textPos.x - 5.0f, textPos.y - 3.0f }, { textPos.x + size.x + 5.0f, textPos.y + size.y + 3.0f },
                                    { 0.078f, 0.078f, 0.086f, 0.67f }, glm::vec4(3.0f));
            drawList.AddText(text, nullptr, textPos, fontSize, { 0.88f, 0.88f, 0.88f, 1.0f });
        }
    }

    void ViewportCanvas::OnPaint(UI::UIDrawList& drawList, const UI::UITheme&)
    {
        const UI::UIRect& b = GetBounds();
        if (b.Width() <= 1.0f || b.Height() <= 1.0f)
            return;

        drawList.PushClipRect(b.Min, b.Max);
        DrawTransformGizmo(drawList);
        if (m_Ctx.Tools && m_Ctx.Tools->IsBoxSelecting)
        {
            const glm::vec2 pMin(
                std::min(m_Ctx.Tools->BoxSelectStart.x, m_Ctx.Tools->BoxSelectEnd.x),
                std::min(m_Ctx.Tools->BoxSelectStart.y, m_Ctx.Tools->BoxSelectEnd.y)
            );
            const glm::vec2 pMax(
                std::max(m_Ctx.Tools->BoxSelectStart.x, m_Ctx.Tools->BoxSelectEnd.x),
                std::max(m_Ctx.Tools->BoxSelectStart.y, m_Ctx.Tools->BoxSelectEnd.y)
            );
            drawList.AddRoundedRect(pMin, pMax, { 0.90f, 0.55f, 0.12f, 0.16f }, glm::vec4(0.0f),
                                    1.5f, { 1.0f, 0.63f, 0.16f, 0.86f });
        }
        DrawMeasureOverlay(drawList);
        DrawAxisIndicator(drawList);

        if (m_Ctx.ShowActiveFrameFn && m_Ctx.ShowActiveFrameFn())
        {
            drawList.AddRoundedRect(
                b.Min + glm::vec2(1.0f),
                b.Max - glm::vec2(1.0f),
                { 0.0f, 0.0f, 0.0f, 0.0f },
                glm::vec4(0.0f),
                2.0f,
                { 1.0f, 0.667f, 0.157f, 1.0f }
            );
        }
        drawList.PopClipRect();
    }

    bool ExtraViewportCanvas::OnPointerDown(const UI::UIPointerEvent&)
    {
        if (m_Ctx.OnActivate)
            m_Ctx.OnActivate(m_Ctx.Index);
        return true;
    }

    bool ExtraViewportCanvas::OnMouseWheel(const UI::UIPointerEvent&)
    {
        if (m_Ctx.OnActivate)
            m_Ctx.OnActivate(m_Ctx.Index);
        return false;
    }

    void ExtraViewportCanvas::OnPaint(UI::UIDrawList& drawList, const UI::UITheme&)
    {
        const UI::UIRect& b = GetBounds();
        if (b.Width() <= 1.0f || b.Height() <= 1.0f || !m_Ctx.Camera || !m_Ctx.Render)
            return;

        drawList.PushClipRect(b.Min, b.Max);
        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();

        // Non-clickable axis indicator in lower-left
        if (!isPlay && b.Width() >= 90.0f && b.Height() >= 90.0f)
        {
            const glm::mat3 view(m_Ctx.Camera->GetGizmoView(m_Ctx.Camera->GetPosition()));
            const glm::vec2 center(b.Min.x + 40.0f, b.Max.y - 40.0f);
            constexpr float kLength = 24.0f;
            constexpr float kLabelRadius = 8.0f;

            struct AxisDraw { glm::vec3 InView; glm::vec4 Color; const char* Name; };
            AxisDraw axes[3] = {
                { view * glm::vec3(1, 0, 0), { 0.886f, 0.290f, 0.290f, 1.0f }, "X" },
                { view * glm::vec3(0, 1, 0), { 0.431f, 0.769f, 0.282f, 1.0f }, "Y" },
                { view * glm::vec3(0, 0, 1), { 0.290f, 0.525f, 0.910f, 1.0f }, "Z" }
            };
            std::sort(std::begin(axes), std::end(axes), [](const AxisDraw& a, const AxisDraw& c) { return a.InView.z < c.InView.z; });
            for (const AxisDraw& ax : axes)
            {
                const glm::vec2 end(center.x + ax.InView.x * kLength, center.y - ax.InView.y * kLength);
                DrawLine(drawList, center + glm::vec2(1.0f), end + glm::vec2(1.0f), 2.0f, { 0.0f, 0.0f, 0.0f, 0.43f });
                DrawLine(drawList, center, end, 2.0f, ax.Color);
                DrawCircleFill(drawList, end, kLabelRadius - 1.5f, { 0.078f, 0.078f, 0.086f, 0.67f });
                const UI::UIVec2 tSize = UI::UILabel::MeasureText(ax.Name, nullptr, 11.5f);
                drawList.AddText(ax.Name, nullptr, { end.x - tSize.x * 0.5f, end.y - tSize.y * 0.5f }, 11.5f, ax.Color);
            }

            if (m_Ctx.ShowCameraPosition && *m_Ctx.ShowCameraPosition)
            {
                const glm::dvec3 pos = m_Ctx.Camera->GetPosition();
                const double meters = static_cast<double>(WorldUnits::ToMeters(1.0f));
                char text[160];
                std::snprintf(text, sizeof(text), "X %.0f  Y %.0f  Z %.0f  (%.1f, %.1f, %.1f m)",
                              pos.x, pos.y, pos.z, pos.x * meters, pos.y * meters, pos.z * meters);
                const UI::UIVec2 size = UI::UILabel::MeasureText(text, nullptr, 13.0f);
                const glm::vec2 textPos(center.x + kLength + 22.0f, b.Max.y - 24.0f);
                drawList.AddRoundedRect({ textPos.x - 5.0f, textPos.y - 3.0f }, { textPos.x + size.x + 5.0f, textPos.y + size.y + 3.0f },
                                        { 0.078f, 0.078f, 0.086f, 0.67f }, glm::vec4(3.0f));
                drawList.AddText(text, nullptr, textPos, 13.0f, { 0.88f, 0.88f, 0.88f, 1.0f });
            }
        }

        // Mini selected entity gizmo axes preview (only once the extra view image has been drawn)
        const bool hasImage = !m_Ctx.HasImageFn || m_Ctx.HasImageFn();
        if (hasImage && m_Ctx.Selection && m_Ctx.GizmoMode && *m_Ctx.GizmoMode != ViewportGizmoMode::Select && !isPlay)
        {
            Entity selected = m_Ctx.Selection->GetPrimary();
            if (selected && selected.HasComponent<WorldTransformComponent>())
            {
                const glm::mat4 world = m_Ctx.Render->ToRenderSpace(selected.GetComponent<WorldTransformComponent>().WorldMatrix);
                const glm::mat4 vp = m_Ctx.Camera->GetProjection() * m_Ctx.Camera->GetViewMatrix(m_Ctx.Render->GetRenderOrigin());
                auto toScreen = [&](const glm::vec3& pt, glm::vec2& out) -> bool
                {
                    const glm::vec4 clip = vp * glm::vec4(pt, 1.0f);
                    if (clip.w <= 1e-6f)
                        return false;
                    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                    out = { b.Min.x + (ndc.x * 0.5f + 0.5f) * b.Width(), b.Min.y + (0.5f - ndc.y * 0.5f) * b.Height() };
                    return true;
                };
                const glm::vec3 centre = glm::vec3(world[3]);
                glm::vec2 centreScr{};
                if (toScreen(centre, centreScr))
                {
                    const glm::vec4 cols[3] = {
                        { 0.90f, 0.24f, 0.24f, 0.86f },
                        { 0.31f, 0.82f, 0.31f, 0.86f },
                        { 0.27f, 0.47f, 0.94f, 0.86f }
                    };
                    const float length = std::min(b.Width(), b.Height()) * 0.09f;
                    const float probe = WorldUnits::FromMeters(0.01f);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const glm::vec3 dir = glm::length(glm::vec3(world[axis])) > 0.0f ? glm::normalize(glm::vec3(world[axis])) : glm::vec3(0.0f);
                        glm::vec2 probeScr{};
                        if (!toScreen(centre + dir * probe, probeScr))
                            continue;
                        const glm::vec2 d = probeScr - centreScr;
                        const float dLen = glm::length(d);
                        if (dLen < 1e-4f)
                            continue;
                        const glm::vec2 end = centreScr + (d / dLen) * length;
                        DrawLine(drawList, centreScr, end, 2.0f, cols[axis]);
                        DrawCircleFill(drawList, end, 3.2f, cols[axis]);
                    }
                    DrawCircleFill(drawList, centreScr, 3.0f, { 1.0f, 1.0f, 1.0f, 0.8f });
                }
            }
        }

        // Corner status badge
        std::string viewLabel = kViewModeNames[std::clamp(static_cast<int>(m_Ctx.Camera->GetViewMode()), 0, 6)];
        const uint32_t flags = m_Ctx.ShowFlags ? *m_Ctx.ShowFlags : 0u;
        const int res = m_Ctx.ResolutionIndex ? *m_Ctx.ResolutionIndex : 0;
        const bool realtime = m_Ctx.Realtime ? *m_Ctx.Realtime : true;
        if (flags & Renderer::ExtraViewUnlit)
            viewLabel += " | Unlit";
        if (flags & Renderer::ExtraViewWireframe)
            viewLabel += " | Wireframe";
        if (res > 0)
            viewLabel += res == 1 ? " | 1/2 res" : " | 1/4 res";
        if (!realtime)
            viewLabel += " | Realtime off";

        const UI::UIVec2 textSize = UI::UILabel::MeasureText(viewLabel, nullptr, 13.0f);
        const glm::vec2 lMin(b.Min.x + 8.0f, b.Min.y + 8.0f);
        drawList.AddRoundedRect(lMin, { lMin.x + textSize.x + 12.0f, lMin.y + textSize.y + 6.0f },
                                { 0.0f, 0.0f, 0.0f, 0.63f }, glm::vec4(3.0f));
        drawList.AddText(viewLabel, nullptr, { lMin.x + 6.0f, lMin.y + 3.0f }, 13.0f, { 0.90f, 0.90f, 0.90f, 1.0f });

        drawList.PopClipRect();
    }

    Ref<UI::UIElement> ViewportOverlayToolbar::Build(Renderer* renderer, EditorCamera* camera,
                                                     ViewportToolState* tools, Callbacks callbacks)
    {
        m_Renderer = renderer;
        m_Camera = camera;
        m_Tools = tools;
        m_Callbacks = std::move(callbacks);

        auto sep = []() -> Ref<UI::UIElement>
        {
            auto s = UI::MakeUI<UI::UIElement>();
            s->WidthMode = UI::UISizeMode::Fixed;
            s->HeightMode = UI::UISizeMode::Fixed;
            s->FixedSize = { 1.0f, 18.0f };
            s->Margin = UI::UIThickness::Symmetric(2.0f, 0.0f);
            s->VAlign = UI::UIAlignment::Center;
            s->DrawBackground = true;
            s->StyleClass = "Border";
            return s;
        };

        m_GizmoButtons[0] = UI::MakeUI<UIVectorIconButton>(EditorIconKind::SelectArrow, "Select Mode", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Select; Sync(); });
        m_GizmoButtons[1] = UI::MakeUI<UIVectorIconButton>(EditorIconKind::MoveArrows, "Move Gizmo", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Translate; Sync(); });
        m_GizmoButtons[2] = UI::MakeUI<UIVectorIconButton>(EditorIconKind::RotateRing, "Rotate Gizmo", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Rotate; Sync(); });
        m_GizmoButtons[3] = UI::MakeUI<UIVectorIconButton>(EditorIconKind::ScaleBox, "Scale Gizmo", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Scale; Sync(); });

        m_SnapBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::SnapMagnet,
            "Toggle Snapping (hold Ctrl to invert temporarily)",
            [this]() { m_Tools->SnapEnabled = !m_Tools->SnapEnabled; Sync(); });

        auto toFloorBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::ToFloor,
            "Snap Selection to Floor (End)",
            [this]() { if (m_Callbacks.SnapToFloor) m_Callbacks.SnapToFloor(); });

        auto toGridBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::ToGrid,
            "Snap Selection to Grid",
            [this]() { if (m_Callbacks.SnapToGrid) m_Callbacks.SnapToGrid(); });

        m_MeasureBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::MeasureRuler,
            "Measure Tool (M): click two points; Esc clears",
            [this]()
            {
                m_Tools->MeasureTool = !m_Tools->MeasureTool;
                if (!m_Tools->MeasureTool)
                    m_Tools->MeasureStage = 0;
                Sync();
            });

        m_GridBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::GridMesh,
            "Toggle World Grid",
            [this]() { m_Tools->ShowGrid = !m_Tools->ShowGrid; Sync(); });

        m_WireframeBtn = UI::MakeUI<UIVectorIconButton>(
            EditorIconKind::WireframeMesh,
            "Toggle Wireframe Overlay",
            [this]()
            {
                if (m_Renderer)
                    m_Renderer->setWireframeOverlay(!m_Renderer->getWireframeOverlay());
                Sync();
            });

        m_ViewModeBtn = UI::MakeUI<UI::UIButton>("Perspective v");
        m_ViewModeBtn->Tooltip = "Camera View Mode (Numpad 7 / 1 / 3, 5 = Perspective)";
        m_ViewModeBtn->Padding = UI::UIThickness::Symmetric(8.0f, 3.0f);
        m_ViewModeBtn->WidthMode = UI::UISizeMode::Auto;
        m_ViewModeBtn->HeightMode = UI::UISizeMode::Auto;
        m_ViewModeBtn->VAlign = UI::UIAlignment::Center;
        UI::UIButton* vmPtr = m_ViewModeBtn.get();
        m_ViewModeBtn->OnClick = [this, vmPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx || !m_Camera)
                return;
            std::vector<UI::UIMenuItem> items;
            for (int i = 0; i < 7; ++i)
            {
                items.push_back({ kViewModeNames[i], "", [this, i]()
                {
                    if (m_Callbacks.SetViewMode)
                        m_Callbacks.SetViewMode(static_cast<EditorViewMode>(i));
                    Sync();
                }, false, true, static_cast<int>(m_Camera->GetViewMode()) == i });
            }
            const UI::UIRect& b = vmPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 3.0f }, std::move(items));
        };

        m_SettingsCogBtn = UI::MakeUI<UIVectorIconButton>(EditorIconKind::Cogwheel, "Viewport & Snap Settings");
        UIVectorIconButton* cogPtr = m_SettingsCogBtn.get();
        m_SettingsCogBtn->OnClick = [this, cogPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx || !m_Tools)
                return;

            std::vector<UI::UIMenuItem> locItems;
            for (int i = 0; i < static_cast<int>(std::size(ViewportToolState::kLocationSnapsCm)); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g cm", ViewportToolState::kLocationSnapsCm[i]);
                locItems.push_back({ buf, "", [this, i]() { m_Tools->LocationSnapIndex = i; Sync(); }, false, true, m_Tools->LocationSnapIndex == i });
            }

            std::vector<UI::UIMenuItem> rotItems;
            for (int i = 0; i < static_cast<int>(std::size(ViewportToolState::kRotationSnaps)); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g deg", ViewportToolState::kRotationSnaps[i]);
                rotItems.push_back({ buf, "", [this, i]() { m_Tools->RotationSnapIndex = i; Sync(); }, false, true, m_Tools->RotationSnapIndex == i });
            }

            std::vector<UI::UIMenuItem> sclItems;
            for (int i = 0; i < static_cast<int>(std::size(ViewportToolState::kScaleSnaps)); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", ViewportToolState::kScaleSnaps[i]);
                sclItems.push_back({ buf, "", [this, i]() { m_Tools->ScaleSnapIndex = i; Sync(); }, false, true, m_Tools->ScaleSnapIndex == i });
            }

            const int curFig = !m_Tools->ShowReferenceFigure ? 0 : m_Tools->ReferenceFigureAtCursor ? 2 : 1;
            std::vector<UI::UIMenuItem> figItems;
            for (int i = 0; i < 3; ++i)
            {
                figItems.push_back({ kFigureNames[i], "", [this, i]()
                {
                    m_Tools->ShowReferenceFigure = (i != 0);
                    m_Tools->ReferenceFigureAtCursor = (i == 2);
                    Sync();
                }, false, true, curFig == i });
            }

            const int curUnit = static_cast<int>(WorldUnits::GetDisplayUnit());
            std::vector<UI::UIMenuItem> unitItems;
            for (int i = 0; i < 3; ++i)
            {
                unitItems.push_back({ kDisplayUnitNames[i], "", [this, i]()
                {
                    WorldUnits::SetDisplayUnit(static_cast<WorldUnits::DisplayUnit>(i));
                    Sync();
                }, false, true, curUnit == i });
            }

            const int curPivot = static_cast<int>(m_Tools->PivotMode);
            std::vector<UI::UIMenuItem> pivotItems;
            for (int i = 0; i < 2; ++i)
            {
                pivotItems.push_back({ kPivotModeNames[i], "", [this, i]()
                {
                    m_Tools->PivotMode = static_cast<GizmoPivotMode>(i);
                    Sync();
                }, false, true, curPivot == i });
            }

            const bool showCamPos = m_Callbacks.GetShowCameraPosition ? m_Callbacks.GetShowCameraPosition() : false;
            std::vector<UI::UIMenuItem> items = {
                {"Location Step", "", {}, false, true, false, false, "", std::move(locItems)},
                {"Rotation Step", "", {}, false, true, false, false, "", std::move(rotItems)},
                {"Scale Step", "", {}, false, true, false, false, "", std::move(sclItems)},
                {"Gizmo Pivot", "", {}, false, true, false, false, "", std::move(pivotItems)},
                {"", "", {}, true},
                {"Reference Figure", "", {}, false, true, false, false, "", std::move(figItems)},
                {"Display Units", "", {}, false, true, false, false, "", std::move(unitItems)},
                {"", "", {}, true},
                {"Show Physics Colliders", "", [this]() { m_Tools->ShowPhysicsColliders = !m_Tools->ShowPhysicsColliders; }, false, true, m_Tools->ShowPhysicsColliders},
                {"Show Camera Position", "", [this, showCamPos]()
                {
                    if (m_Callbacks.SetShowCameraPosition)
                        m_Callbacks.SetShowCameraPosition(!showCamPos);
                }, false, true, showCamPos}
            };

            const UI::UIRect& b = cogPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 3.0f }, std::move(items));
        };

        m_UnfreezeBtn = UI::MakeUI<UI::UIButton>("Unfreeze Culling", [this]()
        {
            if (m_Renderer)
                m_Renderer->setFrozen(false);
            Sync();
        });
        m_UnfreezeBtn->Tooltip = "Culling view is frozen; click to unfreeze";
        m_UnfreezeBtn->StyleVariant = "primary";
        m_UnfreezeBtn->Padding = UI::UIThickness::Symmetric(8.0f, 3.0f);
        m_UnfreezeBtn->WidthMode = UI::UISizeMode::Auto;
        m_UnfreezeBtn->HeightMode = UI::UISizeMode::Auto;
        m_UnfreezeBtn->VAlign = UI::UIAlignment::Center;
        m_UnfreezeBtn->Visible = false;

        auto bar = UI::HStack(3.0f, {
            m_GizmoButtons[0], m_GizmoButtons[1], m_GizmoButtons[2], m_GizmoButtons[3],
            sep(),
            m_SnapBtn, toFloorBtn, toGridBtn, m_MeasureBtn,
            sep(),
            m_GridBtn, m_WireframeBtn,
            sep(),
            m_ViewModeBtn,
            m_SettingsCogBtn,
            m_UnfreezeBtn
        }).Padding(6.0f, 3.0f).Style("Surface").Background(true).Get();

        bar->HitTestMode = UI::UIHitTestMode::Self;
        bar->WidthMode = UI::UISizeMode::Auto;
        bar->HeightMode = UI::UISizeMode::Auto;
        bar->AnchorMin = { 0.0f, 0.0f };
        bar->AnchorMax = { 0.0f, 0.0f };
        bar->AnchorOffsets = { 8.0f, 8.0f, 0.0f, 0.0f };

        Sync();
        return bar;
    }

    void ViewportOverlayToolbar::Sync()
    {
        if (!m_Tools)
            return;

        const int activeIdx = static_cast<int>(m_Tools->GizmoMode) + 1;
        for (int i = 0; i < 4; ++i)
        {
            if (m_GizmoButtons[i])
                m_GizmoButtons[i]->Active = (i == activeIdx);
        }
        if (m_SnapBtn)
            m_SnapBtn->Active = m_Tools->SnapEnabled;
        if (m_MeasureBtn)
            m_MeasureBtn->Active = m_Tools->MeasureTool;
        if (m_GridBtn)
            m_GridBtn->Active = m_Tools->ShowGrid;
        if (m_WireframeBtn && m_Renderer)
            m_WireframeBtn->Active = m_Renderer->getWireframeOverlay();
        if (m_UnfreezeBtn)
            m_UnfreezeBtn->Visible = (m_Renderer && m_Renderer->getFrozen());
        if (m_ViewModeBtn && m_Camera)
        {
            const int vm = std::clamp(static_cast<int>(m_Camera->GetViewMode()), 0, 6);
            m_ViewModeBtn->SetText(std::string(kViewModeNames[vm]) + " v");
        }
    }
}
