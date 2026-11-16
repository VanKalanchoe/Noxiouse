#include "ViewportOverlayV2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "Panels/SceneHierarchyV2Panel.h"

namespace Nox
{
    namespace
    {
        constexpr const char* kViewModeNames[] = { "Perspective", "Top", "Bottom", "Front", "Back", "Left", "Right" };
        constexpr const char* kFigureNames[] = { "No Figure", "Figure at Origin", "Figure at Cursor" };
        constexpr const char* kDisplayUnitNames[] = { "cm", "m", "auto" };

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
        if (m_Ctx.Tools->AxisIndicatorHovered || m_Ctx.Tools->GizmoUsing || m_Ctx.Tools->GizmoHovered)
            return true;
        return false;
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

    float ViewportCanvas::ComputeGizmoWorldScale(const glm::vec3& pivotRender, const glm::mat4& viewProj) const
    {
        const UI::UIRect& b = GetBounds();
        const float minDim = std::max(std::min(b.Width(), b.Height()), 100.0f);
        if (m_Ctx.Camera && m_Ctx.Camera->IsOrthographic())
            return m_Ctx.Camera->GetOrthoHalfHeight() * 0.24f;

        const glm::vec4 clip = viewProj * glm::vec4(pivotRender, 1.0f);
        const float tanHalfFov = std::tan(glm::radians(m_Ctx.Camera ? m_Ctx.Camera->GetFOV() : 30.0f) * 0.5f);
        const float dist = std::max(clip.w, 0.05f);
        return dist * tanHalfFov * (160.0f / minDim);
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
            constexpr int kSegments = 48;
            const float radius = scale * 0.78f;
            float bestDistSq = 100.0f; // 10 px tolerance
            GizmoPart best = GizmoPart::None;

            for (int a = 0; a < 3; ++a)
            {
                const glm::vec3 u = axes[(a + 1) % 3];
                const glm::vec3 v = axes[(a + 2) % 3];
                UI::UIVec2 prev{};
                bool hasPrev = false;
                for (int s = 0; s <= kSegments; ++s)
                {
                    const float ang = glm::two_pi<float>() * static_cast<float>(s) / kSegments;
                    const glm::vec3 pWorld = origin + (u * std::cos(ang) + v * std::sin(ang)) * radius;
                    UI::UIVec2 cur{};
                    const bool ok = ProjectPoint(pWorld, viewProj, cur);
                    if (ok && hasPrev)
                    {
                        const float dSq = DistPointToSegmentSq(mousePos, prev, cur);
                        if (dSq < bestDistSq)
                        {
                            bestDistSq = dSq;
                            best = axisParts[a];
                        }
                    }
                    prev = cur;
                    hasPrev = ok;
                }
            }
            return best;
        }

        // Center dot/cube (FreeMove / UniformScale)
        if (glm::dot(mousePos - originScreen, mousePos - originScreen) <= 10.0f * 10.0f)
            return GizmoPart::FreeCenter;

        // Plane quads (Translate only)
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
                    if (glm::dot(mousePos - c, mousePos - c) <= 10.0f * 10.0f)
                        return ps.Part;
                }
            }
        }

        // X/Y/Z axis stems
        float bestDistSq = 100.0f; // 10 px tolerance
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
        const bool hasDepth = m_Ctx.Render->getPickedDepth(pickedX, pickedY, depth, 4);

        glm::vec2 pixel;
        if (hasDepth)
        {
            pixel = glm::vec2(static_cast<float>(pickedX) + 0.5f, static_cast<float>(pickedY) + 0.5f);
        }
        else
        {
            const UI::UIContext* ctx = UI::UIContext::GetCurrent();
            const glm::vec2 mouse = ctx ? ctx->GetLastMousePos() : Input::GetMousePosition();
            pixel = glm::vec2(mouse.x - GetBounds().Min.x, mouse.y - GetBounds().Min.y);
        }

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
        if (!m_Ctx.Hierarchy || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene)
            return;

        Entity picked = m_Ctx.HoveredEntity ? *m_Ctx.HoveredEntity : Entity{};
        while (picked && !ctrl && picked.HasComponent<PrefabNodeComponent>())
        {
            Entity instance = (*m_Ctx.ActiveScene)->GetEntityByUUID(picked.GetComponent<PrefabNodeComponent>().Instance);
            if (!instance)
                break;
            picked = instance;
        }

        if (picked)
        {
            if (shift)
                m_Ctx.Hierarchy->SelectRangeTo(picked);
            else if (ctrl)
                m_Ctx.Hierarchy->ToggleSelectedEntity(picked);
            else
                m_Ctx.Hierarchy->SetSelectedEntity(picked);
        }
        else if (!shift && !ctrl)
        {
            m_Ctx.Hierarchy->ClearSelection();
        }
    }

    void ViewportCanvas::CommitBoxSelection(bool shift, bool ctrl)
    {
        if (!m_Ctx.Render || !m_Ctx.Hierarchy || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene)
            return;

        if (!shift && !ctrl)
            m_Ctx.Hierarchy->ClearSelection();

        const std::vector<int32_t> pickedIDs = m_Ctx.Render->getPickedEntityIDs();
        for (int32_t id : pickedIDs)
        {
            if (id < 0)
                continue;
            Entity entity{ static_cast<entt::entity>(id), (*m_Ctx.ActiveScene).get() };
            if (!entity)
                continue;
            if (ctrl || !m_Ctx.Hierarchy->IsSelected(entity))
                m_Ctx.Hierarchy->ToggleSelectedEntity(entity);
        }
    }

    void ViewportCanvas::ApplyEntityDelta(const glm::mat4& deltaRender, const glm::mat4& primaryWorldRender)
    {
        if (!m_Ctx.Hierarchy || !m_Ctx.ActiveScene || !*m_Ctx.ActiveScene || !m_Ctx.Render)
            return;
        const auto& selected = m_Ctx.Hierarchy->GetSelectedEntities();
        Entity primary = m_Ctx.Hierarchy->GetSelectedEntity();
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
                        if (parent && m_Ctx.Hierarchy->IsSelected(parent))
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
        if (!m_Ctx.Tools || !m_Ctx.Camera || !m_Ctx.Render || !m_Ctx.Hierarchy)
            return;
        Entity selected = m_Ctx.Hierarchy->GetSelectedEntity();
        if (!selected)
            return;

        glm::vec3 rayOrig{}, rayDir{};
        if (!RayFromScreen(mousePos, rayOrig, rayDir))
            return;

        const bool snap = m_Ctx.Tools->SnapEnabled != Input::IsKeyPressed(SDL_SCANCODE_LCTRL);
        glm::mat4 newWorld = m_DragInitialWorldRender;

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
                newWorld = glm::translate(glm::mat4(1.0f), axis * delta) * m_DragInitialWorldRender;
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
                    newWorld = glm::translate(glm::mat4(1.0f), delta) * m_DragInitialWorldRender;
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
                    newWorld = glm::translate(glm::mat4(1.0f), m_DragAxisDirs[a0] * c0 + m_DragAxisDirs[a1] * c1) * m_DragInitialWorldRender;
                }
            }
        }
        else if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Rotate)
        {
            if (const int idx = axisIndex(m_ActiveGizmo); idx >= 0)
            {
                const glm::vec3 axis = m_DragAxisDirs[idx];
                glm::vec3 hit{};
                if (IntersectRayPlane(rayOrig, rayDir, m_DragOriginRender, axis, hit))
                {
                    glm::vec3 v0 = m_DragStartHit - m_DragOriginRender;
                    glm::vec3 v1 = hit - m_DragOriginRender;
                    if (glm::length(v0) > 1e-4f && glm::length(v1) > 1e-4f)
                    {
                        v0 = glm::normalize(v0);
                        v1 = glm::normalize(v1);
                        const float angleRad = std::atan2(glm::dot(axis, glm::cross(v0, v1)), glm::clamp(glm::dot(v0, v1), -1.0f, 1.0f));
                        float angleDeg = glm::degrees(angleRad);
                        if (snap)
                        {
                            const float step = ViewportToolState::kRotationSnaps[std::clamp(m_Ctx.Tools->RotationSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kRotationSnaps)) - 1)];
                            angleDeg = SnapValue(angleDeg, step);
                        }
                        const glm::mat4 rot = glm::translate(glm::mat4(1.0f), m_DragOriginRender) *
                                              glm::rotate(glm::mat4(1.0f), glm::radians(angleDeg), axis) *
                                              glm::translate(glm::mat4(1.0f), -m_DragOriginRender);
                        newWorld = rot * m_DragInitialWorldRender;
                    }
                }
            }
        }
        else if (m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Scale)
        {
            const float step = ViewportToolState::kScaleSnaps[std::clamp(m_Ctx.Tools->ScaleSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kScaleSnaps)) - 1)];
            if (const int idx = axisIndex(m_ActiveGizmo); idx >= 0)
            {
                const glm::vec3 axis = m_DragAxisDirs[idx];
                const float s = ClosestRayAxisParameter(rayOrig, rayDir, m_DragOriginRender, axis);
                const float denom = std::max(std::abs(m_DragStartScalar), 0.1f);
                float factor = 1.0f + (s - m_DragStartScalar) / denom;
                if (snap)
                    factor = SnapValue(factor, step);
                factor = std::max(factor, 0.01f);
                newWorld[idx] = m_DragInitialWorldRender[idx] * factor;
            }
            else if (m_ActiveGizmo == GizmoPart::FreeCenter)
            {
                float factor = 1.0f + (mousePos.x - m_DragStartMouseX) * 0.01f;
                if (snap)
                    factor = SnapValue(factor, step);
                factor = std::max(factor, 0.01f);
                newWorld[0] = m_DragInitialWorldRender[0] * factor;
                newWorld[1] = m_DragInitialWorldRender[1] * factor;
                newWorld[2] = m_DragInitialWorldRender[2] * factor;
            }
        }

        const glm::mat4 deltaMatrix = newWorld * glm::inverse(m_DragPrevWorldRender);
        m_DragPrevWorldRender = newWorld;
        ApplyEntityDelta(deltaMatrix, newWorld);
    }

    void ViewportCanvas::OnUpdate(float dt)
    {
        m_LastDt = dt;
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
        if (!m_Ctx.Tools->AxisIndicatorHovered && m_Ctx.Hierarchy && m_Ctx.Render && !isPlay)
        {
            if (Entity sel = m_Ctx.Hierarchy->GetSelectedEntity(); sel && sel.HasComponent<WorldTransformComponent>())
            {
                const glm::mat4 world = m_Ctx.Render->ToRenderSpace(sel.GetComponent<WorldTransformComponent>().WorldMatrix);
                m_HoveredGizmo = HitTestGizmo(e.Position, world);
            }
        }
        m_Ctx.Tools->GizmoHovered = (m_HoveredGizmo != GizmoPart::None);

        if (m_HoveredAxisIdx >= 0 && m_Ctx.Camera && m_Ctx.Render)
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

        // 1. Click on Axis Indicator handle
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

        // 2. Measure tool click
        if (m_Ctx.Tools->MeasureTool)
        {
            MeasureClick();
            return true;
        }

        // 3. Transform gizmo hit
        const bool isPlay = m_Ctx.IsPlayModeFn && m_Ctx.IsPlayModeFn();
        if (m_Ctx.Tools->GizmoMode != ViewportGizmoMode::Select && m_Ctx.Hierarchy && m_Ctx.Render && !isPlay)
        {
            if (Entity sel = m_Ctx.Hierarchy->GetSelectedEntity(); sel && sel.HasComponent<WorldTransformComponent>())
            {
                const glm::mat4 world = m_Ctx.Render->ToRenderSpace(sel.GetComponent<WorldTransformComponent>().WorldMatrix);
                const GizmoPart hit = HitTestGizmo(e.Position, world);
                if (hit != GizmoPart::None)
                {
                    glm::vec3 rayOrig{}, rayDir{};
                    if (RayFromScreen(e.Position, rayOrig, rayDir))
                    {
                        m_ActiveGizmo = hit;
                        m_DragInitialWorldRender = world;
                        m_DragPrevWorldRender = world;
                        m_DragOriginRender = glm::vec3(world[3]);
                        for (int i = 0; i < 3; ++i)
                        {
                            const glm::vec3 col(world[i]);
                            const float len = glm::length(col);
                            m_DragAxisDirs[i] = len > 1e-6f ? (col / len) : glm::vec3(i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f);
                            m_DragStartScale[i] = len;
                        }
                        m_DragStartMouseX = e.Position.x;

                        int aIdx = hit == GizmoPart::AxisX ? 0 : hit == GizmoPart::AxisY ? 1 : hit == GizmoPart::AxisZ ? 2 : -1;
                        if (aIdx >= 0)
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

        // 4. Entity Click Selection + Start Box Marquee tracking
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
        if (!m_Ctx.Tools || m_Ctx.Tools->GizmoMode == ViewportGizmoMode::Select || !m_Ctx.Camera || !m_Ctx.Render || !m_Ctx.Hierarchy || isPlay)
            return;
        Entity selected = m_Ctx.Hierarchy->GetSelectedEntity();
        if (!selected || !selected.HasComponent<WorldTransformComponent>())
            return;

        const glm::mat4 view = m_Ctx.Camera->GetGizmoView(m_Ctx.Render->GetRenderOrigin());
        const glm::mat4 proj = m_Ctx.Camera->GetGizmoProjection();
        const glm::mat4 vp = proj * view;
        const glm::mat4 world = m_Ctx.Render->ToRenderSpace(selected.GetComponent<WorldTransformComponent>().WorldMatrix);

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
            constexpr int kSegs = 48;
            const float radius = scale * 0.78f;
            for (int a = 0; a < 3; ++a)
            {
                const bool lit = (activeHighlight == axisParts[a]);
                const glm::vec4 col = lit ? highlightCol : baseColors[a];
                const float thick = lit ? 3.0f : 2.0f;
                const glm::vec3 u = axes[(a + 1) % 3];
                const glm::vec3 v = axes[(a + 2) % 3];
                UI::UIVec2 prev{};
                bool hasPrev = false;
                for (int s = 0; s <= kSegs; ++s)
                {
                    const float ang = glm::two_pi<float>() * static_cast<float>(s) / kSegs;
                    const glm::vec3 pW = origin + (u * std::cos(ang) + v * std::sin(ang)) * radius;
                    UI::UIVec2 cur{};
                    const bool ok = ProjectPoint(pW, vp, cur);
                    if (ok && hasPrev)
                        DrawLine(drawList, prev, cur, thick, col);
                    prev = cur;
                    hasPrev = ok;
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

        // Snap measuring feedback line while translating with snapping active
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

        if (m_Ctx.Tools && m_Ctx.Tools->ShowCameraPosition)
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
        drawList.PopClipRect();
    }

    Ref<UI::UIElement> ViewportOverlayToolbar::Build(Renderer* renderer, EditorCamera* camera,
                                                     ViewportToolState* tools, Callbacks callbacks)
    {
        m_Renderer = renderer;
        m_Camera = camera;
        m_Tools = tools;
        m_Callbacks = std::move(callbacks);

        auto makeSmallBtn = [](const std::string& label, const std::string& tip, std::function<void()> onClick) -> Ref<UI::UIButton>
        {
            auto btn = UI::MakeUI<UI::UIButton>(label, std::move(onClick));
            btn->Tooltip = tip;
            btn->Padding = UI::UIThickness::Symmetric(7.0f, 3.0f);
            btn->HeightMode = UI::UISizeMode::Auto;
            btn->WidthMode = UI::UISizeMode::Auto;
            btn->VAlign = UI::UIAlignment::Center;
            return btn;
        };

        m_GizmoButtons[0] = makeSmallBtn("Sel", "Select only, no gizmo", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Select; Sync(); });
        m_GizmoButtons[1] = makeSmallBtn("Mov", "Move", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Translate; Sync(); });
        m_GizmoButtons[2] = makeSmallBtn("Rot", "Rotate", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Rotate; Sync(); });
        m_GizmoButtons[3] = makeSmallBtn("Scl", "Scale", [this]() { m_Tools->GizmoMode = ViewportGizmoMode::Scale; Sync(); });

        auto row1 = UI::HStack(4.0f, { m_GizmoButtons[0], m_GizmoButtons[1], m_GizmoButtons[2], m_GizmoButtons[3] });
        row1->HeightMode = UI::UISizeMode::Auto;

        auto frozenLabel = UI::Label("Culling view frozen").VAlign(UI::UIAlignment::Center);
        frozenLabel->UseColorOverride = true;
        frozenLabel->ColorOverride = { 1.0f, 0.6f, 0.2f, 1.0f };
        auto unfreezeBtn = makeSmallBtn("Unfreeze", "Only what was in view when it froze is drawn", [this]()
        {
            if (m_Renderer)
                m_Renderer->setFrozen(false);
            Sync();
        });
        m_FrozenRow = UI::HStack(6.0f, { frozenLabel, unfreezeBtn }).Get();
        m_FrozenRow->HeightMode = UI::UISizeMode::Auto;
        m_FrozenRow->Visible = false;

        m_SnapCheck = UI::MakeUI<UI::UICheckBox>("Snap", m_Tools->SnapEnabled, [this](bool v) { m_Tools->SnapEnabled = v; });
        m_SnapCheck->Tooltip = "Snap moving, rotating, scaling and placing to the steps on the right. Hold Ctrl to switch it for one move.";
        m_SnapCheck->VAlign = UI::UIAlignment::Center;

        m_LocStepBtn = makeSmallBtn("10 cm", "Location step", nullptr);
        UI::UIButton* locPtr = m_LocStepBtn.get();
        m_LocStepBtn->OnClick = [this, locPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx || !m_Tools)
                return;
            std::vector<UI::UIMenuItem> items;
            for (int i = 0; i < static_cast<int>(std::size(ViewportToolState::kLocationSnapsCm)); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g cm", ViewportToolState::kLocationSnapsCm[i]);
                items.push_back({ buf, "", [this, i]() { m_Tools->LocationSnapIndex = i; Sync(); }, false, true, m_Tools->LocationSnapIndex == i });
            }
            const UI::UIRect& b = locPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 2.0f }, std::move(items));
        };

        m_RotStepBtn = makeSmallBtn("15 deg", "Rotation step", nullptr);
        UI::UIButton* rotPtr = m_RotStepBtn.get();
        m_RotStepBtn->OnClick = [this, rotPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx || !m_Tools)
                return;
            std::vector<UI::UIMenuItem> items;
            for (int i = 0; i < static_cast<int>(std::size(ViewportToolState::kRotationSnaps)); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g deg", ViewportToolState::kRotationSnaps[i]);
                items.push_back({ buf, "", [this, i]() { m_Tools->RotationSnapIndex = i; Sync(); }, false, true, m_Tools->RotationSnapIndex == i });
            }
            const UI::UIRect& b = rotPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 2.0f }, std::move(items));
        };

        m_ScaleStepBtn = makeSmallBtn("0.25", "Scale step", nullptr);
        UI::UIButton* sclPtr = m_ScaleStepBtn.get();
        m_ScaleStepBtn->OnClick = [this, sclPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx || !m_Tools)
                return;
            std::vector<UI::UIMenuItem> items;
            for (int i = 0; i < static_cast<int>(std::size(ViewportToolState::kScaleSnaps)); ++i)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", ViewportToolState::kScaleSnaps[i]);
                items.push_back({ buf, "", [this, i]() { m_Tools->ScaleSnapIndex = i; Sync(); }, false, true, m_Tools->ScaleSnapIndex == i });
            }
            const UI::UIRect& b = sclPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 2.0f }, std::move(items));
        };

        m_ViewModeBtn = makeSmallBtn("Perspective", "View: Perspective, or orthographic along an axis (Numpad 7 / 1 / 3, 5 = Perspective)", nullptr);
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
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 2.0f }, std::move(items));
        };

        m_FigureBtn = makeSmallBtn("No Figure", "A 180 cm figure to judge sizes: at the world origin, or standing on the surface under the cursor", nullptr);
        UI::UIButton* figPtr = m_FigureBtn.get();
        m_FigureBtn->OnClick = [this, figPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx || !m_Tools)
                return;
            const int cur = !m_Tools->ShowReferenceFigure ? 0 : m_Tools->ReferenceFigureAtCursor ? 2 : 1;
            std::vector<UI::UIMenuItem> items;
            for (int i = 0; i < 3; ++i)
            {
                items.push_back({ kFigureNames[i], "", [this, i]()
                {
                    m_Tools->ShowReferenceFigure = (i != 0);
                    m_Tools->ReferenceFigureAtCursor = (i == 2);
                    Sync();
                }, false, true, cur == i });
            }
            const UI::UIRect& b = figPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 2.0f }, std::move(items));
        };

        m_GridCheck = UI::MakeUI<UI::UICheckBox>("Grid", m_Tools->ShowGrid, [this](bool v) { m_Tools->ShowGrid = v; });
        m_GridCheck->Tooltip = "The world grid, with the cells of the location step";
        m_GridCheck->VAlign = UI::UIAlignment::Center;

        m_WireframeCheck = UI::MakeUI<UI::UICheckBox>("Wireframe", m_Renderer ? m_Renderer->getWireframeOverlay() : false, [this](bool v)
        {
            if (m_Renderer)
                m_Renderer->setWireframeOverlay(v);
        });
        m_WireframeCheck->Tooltip = "Every mesh's triangle edges over the image";
        m_WireframeCheck->VAlign = UI::UIAlignment::Center;

        auto toFloorBtn = makeSmallBtn("To Floor", "Drop the selection onto the surface below it (End)", [this]()
        {
            if (m_Callbacks.SnapToFloor)
                m_Callbacks.SnapToFloor();
        });
        auto toGridBtn = makeSmallBtn("To Grid", "Move the selection to the nearest grid point of the location step", [this]()
        {
            if (m_Callbacks.SnapToGrid)
                m_Callbacks.SnapToGrid();
        });

        m_MeasureCheck = UI::MakeUI<UI::UICheckBox>("Measure", m_Tools->MeasureTool, [this](bool v)
        {
            m_Tools->MeasureTool = v;
            if (!v)
                m_Tools->MeasureStage = 0;
        });
        m_MeasureCheck->Tooltip = "Measure (M): click two points, the distance is written on the line. Esc leaves it.";
        m_MeasureCheck->VAlign = UI::UIAlignment::Center;

        m_UnitBtn = makeSmallBtn("auto", "Unit lengths are shown in", nullptr);
        UI::UIButton* unitPtr = m_UnitBtn.get();
        m_UnitBtn->OnClick = [this, unitPtr]()
        {
            UI::UIContext* ctx = UI::UIContext::GetCurrent();
            if (!ctx)
                return;
            const int cur = static_cast<int>(WorldUnits::GetDisplayUnit());
            std::vector<UI::UIMenuItem> items;
            for (int i = 0; i < 3; ++i)
            {
                items.push_back({ kDisplayUnitNames[i], "", [this, i]()
                {
                    WorldUnits::SetDisplayUnit(static_cast<WorldUnits::DisplayUnit>(i));
                    Sync();
                }, false, true, cur == i });
            }
            const UI::UIRect& b = unitPtr->GetBounds();
            UI::UIMenuPopup::ShowContextMenu(*ctx, { b.Min.x, b.Max.y + 2.0f }, std::move(items));
        };

        auto row2 = UI::Wrap(5.0f, 4.0f, {
            m_SnapCheck, m_LocStepBtn, m_RotStepBtn, m_ScaleStepBtn,
            m_ViewModeBtn, m_FigureBtn, m_GridCheck, m_WireframeCheck,
            toFloorBtn, toGridBtn, m_MeasureCheck, m_UnitBtn
        });
        row2->HeightMode = UI::UISizeMode::Auto;
        row2->MaxSize = { 760.0f, 200.0f };

        auto container = UI::VStack(4.0f, { row1, m_FrozenRow, row2 })
            .Padding(6.0f, 5.0f)
            .Style("Surface")
            .Background(true)
            .Get();
        container->HitTestMode = UI::UIHitTestMode::Self;
        container->WidthMode = UI::UISizeMode::Auto;
        container->HeightMode = UI::UISizeMode::Auto;
        container->AnchorMin = { 0.0f, 0.0f };
        container->AnchorMax = { 0.0f, 0.0f };
        container->AnchorOffsets = { 10.0f, 10.0f, 0.0f, 0.0f };

        Sync();
        return container;
    }

    void ViewportOverlayToolbar::Sync()
    {
        if (!m_Tools)
            return;

        const int activeIdx = static_cast<int>(m_Tools->GizmoMode) + 1; // Select=-1 -> 0
        for (int i = 0; i < 4; ++i)
        {
            if (m_GizmoButtons[i])
                m_GizmoButtons[i]->StyleVariant = (i == activeIdx) ? "primary" : "";
        }
        if (m_FrozenRow)
            m_FrozenRow->Visible = (m_Renderer && m_Renderer->getFrozen());

        if (m_SnapCheck)
            m_SnapCheck->Checked = m_Tools->SnapEnabled;
        if (m_LocStepBtn)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g cm", ViewportToolState::kLocationSnapsCm[std::clamp(m_Tools->LocationSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kLocationSnapsCm)) - 1)]);
            m_LocStepBtn->SetText(buf);
        }
        if (m_RotStepBtn)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g deg", ViewportToolState::kRotationSnaps[std::clamp(m_Tools->RotationSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kRotationSnaps)) - 1)]);
            m_RotStepBtn->SetText(buf);
        }
        if (m_ScaleStepBtn)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", ViewportToolState::kScaleSnaps[std::clamp(m_Tools->ScaleSnapIndex, 0, static_cast<int>(std::size(ViewportToolState::kScaleSnaps)) - 1)]);
            m_ScaleStepBtn->SetText(buf);
        }
        if (m_ViewModeBtn && m_Camera)
        {
            const int vm = std::clamp(static_cast<int>(m_Camera->GetViewMode()), 0, 6);
            m_ViewModeBtn->SetText(kViewModeNames[vm]);
        }
        if (m_FigureBtn)
        {
            const int fig = !m_Tools->ShowReferenceFigure ? 0 : m_Tools->ReferenceFigureAtCursor ? 2 : 1;
            m_FigureBtn->SetText(kFigureNames[fig]);
        }
        if (m_GridCheck)
            m_GridCheck->Checked = m_Tools->ShowGrid;
        if (m_WireframeCheck && m_Renderer)
            m_WireframeCheck->Checked = m_Renderer->getWireframeOverlay();
        if (m_MeasureCheck)
            m_MeasureCheck->Checked = m_Tools->MeasureTool;
        if (m_UnitBtn)
            m_UnitBtn->SetText(kDisplayUnitNames[std::clamp(static_cast<int>(WorldUnits::GetDisplayUnit()), 0, 2)]);
    }
}
