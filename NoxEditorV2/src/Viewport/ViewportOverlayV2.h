#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/UI/Core/UIElement.h"

namespace Nox
{
    class Renderer;
    class SceneHierarchyV2Panel;

    namespace UI
    {
        class UIButton;
        class UICheckBox;
        class UILabel;
        class UIStackBox;
    }

    enum class ViewportGizmoMode : int
    {
        Select = -1,
        Translate = 0,
        Rotate = 1,
        Scale = 2
    };

    struct ViewportToolState
    {
        static constexpr float kLocationSnapsCm[] = { 1.0f, 5.0f, 10.0f, 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f };
        static constexpr float kRotationSnaps[] = { 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 60.0f, 90.0f, 120.0f };
        static constexpr float kScaleSnaps[] = { 0.1f, 0.25f, 0.5f, 1.0f };

        ViewportGizmoMode GizmoMode = ViewportGizmoMode::Translate;
        bool SnapEnabled = false;
        int LocationSnapIndex = 2; // 10 cm
        int RotationSnapIndex = 2; // 15 degrees
        int ScaleSnapIndex = 1;    // 0.25

        bool ShowGrid = true;
        bool ShowPhysicsColliders = false;
        bool ShowReferenceFigure = false;
        bool ReferenceFigureAtCursor = false;
        glm::dvec3 ReferenceFigurePosition{ 0.0 };
        bool ShowCameraPosition = false;

        bool MeasureTool = false;
        int MeasureStage = 0; // 0 none, 1 placing second, 2 locked
        glm::dvec3 MeasureA{ 0.0 };
        glm::dvec3 MeasureB{ 0.0 };

        struct ViewTurnState
        {
            bool Active = false;
            float FromPitch = 0.0f;
            float FromYaw = 0.0f;
            float ToPitch = 0.0f;
            float ToYaw = 0.0f;
            float Time = 0.0f;
        } ViewTurn;

        bool AxisIndicatorHovered = false;
        bool GizmoHovered = false;
        bool GizmoUsing = false;
        bool GizmoWasUsing = false;
        glm::mat4 GizmoDragStart{ 1.0f };

        bool IsBoxSelecting = false;
        UI::UIVec2 BoxSelectStart{ 0.0f, 0.0f };
        UI::UIVec2 BoxSelectEnd{ 0.0f, 0.0f };

        float LocationSnapWorldUnits() const;
        glm::dvec3 SnapLocation(const glm::dvec3& point) const;
    };

    class ViewportCanvas : public UI::UIElement
    {
    public:
        struct Context
        {
            Renderer* Render = nullptr;
            EditorCamera* Camera = nullptr;
            Ref<Scene>* ActiveScene = nullptr;
            SceneHierarchyV2Panel* Hierarchy = nullptr;
            Entity* HoveredEntity = nullptr;
            ViewportToolState* Tools = nullptr;
            std::function<bool()> IsPlayModeFn;
            std::function<bool()> IsEditModeFn;

            std::function<int()> GridPlaneAxisFn;
            std::function<void()> OnSceneMutated;
            std::function<void(bool captured, int button)> OnCaptureChanged;
        };

        void Bind(const Context& ctx) { m_Ctx = ctx; }

        bool PickWorldPoint(glm::dvec3& out) const;
        void MeasureClick();
        bool GizmoTakesMouse() const;

        bool OnPointerMove(const UI::UIPointerEvent& e) override;
        bool OnPointerLeave(const UI::UIPointerEvent& e) override;
        bool OnPointerDown(const UI::UIPointerEvent& e) override;
        bool OnPointerUp(const UI::UIPointerEvent& e) override;

    protected:
        void OnUpdate(float dt) override;
        void OnPaint(UI::UIDrawList& drawList, const UI::UITheme& theme) override;

    private:
        enum class GizmoPart : uint8_t
        {
            None = 0,
            AxisX, AxisY, AxisZ,
            PlaneYZ, PlaneXZ, PlaneXY,
            FreeCenter
        };

        bool RayFromScreen(const UI::UIVec2& screenPos, glm::vec3& outOrigin, glm::vec3& outDir) const;
        bool ProjectPoint(const glm::vec3& renderPoint, const glm::mat4& viewProj, UI::UIVec2& outScreen, float* outClipW = nullptr) const;
        float ComputeGizmoWorldScale(const glm::vec3& pivotRender, const glm::mat4& viewProj) const;
        GizmoPart HitTestGizmo(const UI::UIVec2& mousePos, const glm::mat4& worldTransform) const;
        int HitTestAxisIndicator(const UI::UIVec2& mousePos) const;
        void UpdateGizmoDrag(const UI::UIVec2& mousePos);
        void ApplyEntityDelta(const glm::mat4& deltaRender, const glm::mat4& primaryWorldRender);
        void DrawTransformGizmo(UI::UIDrawList& drawList) const;
        void DrawAxisIndicator(UI::UIDrawList& drawList) const;
        void DrawMeasureOverlay(UI::UIDrawList& drawList) const;
        void DrawMeasureLine(UI::UIDrawList& drawList, const glm::mat4& view, const glm::mat4& projection,
                             const glm::vec3& startRender, const glm::vec3& endRender, float minimumLength) const;
        void CommitClickSelection(bool shift, bool ctrl);
        void CommitBoxSelection(bool shift, bool ctrl);

        Context m_Ctx{};
        float m_LastDt = 0.016f;
        int m_HoveredAxisIdx = -1; // -1 none, 0..2 X/Y/Z
        GizmoPart m_HoveredGizmo = GizmoPart::None;
        GizmoPart m_ActiveGizmo = GizmoPart::None;

        bool m_LeftTracking = false;
        UI::UIVec2 m_LeftDownPos{ 0.0f, 0.0f };

        // Gizmo drag baseline
        glm::mat4 m_DragInitialWorldRender{ 1.0f };
        glm::mat4 m_DragPrevWorldRender{ 1.0f };
        glm::vec3 m_DragOriginRender{ 0.0f };
        glm::vec3 m_DragAxisDirs[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        glm::vec3 m_DragPlaneNormal{ 0.0f, 1.0f, 0.0f };
        glm::vec3 m_DragStartHit{ 0.0f };
        glm::vec3 m_DragStartScale{ 1.0f };
        float m_DragStartScalar = 0.0f;
        float m_DragStartMouseX = 0.0f;
    };

    class ViewportOverlayToolbar
    {
    public:
        struct Callbacks
        {
            std::function<void(EditorViewMode)> SetViewMode;
            std::function<void()> SnapToFloor;
            std::function<void()> SnapToGrid;
        };

        Ref<UI::UIElement> Build(Renderer* renderer, EditorCamera* camera, ViewportToolState* tools, Callbacks callbacks);
        void Sync();

    private:
        Renderer* m_Renderer = nullptr;
        EditorCamera* m_Camera = nullptr;
        ViewportToolState* m_Tools = nullptr;
        Callbacks m_Callbacks;

        std::array<Ref<UI::UIButton>, 4> m_GizmoButtons;
        Ref<UI::UIStackBox> m_FrozenRow;
        Ref<UI::UICheckBox> m_SnapCheck;
        Ref<UI::UIButton> m_LocStepBtn;
        Ref<UI::UIButton> m_RotStepBtn;
        Ref<UI::UIButton> m_ScaleStepBtn;
        Ref<UI::UIButton> m_ViewModeBtn;
        Ref<UI::UIButton> m_FigureBtn;
        Ref<UI::UICheckBox> m_GridCheck;
        Ref<UI::UICheckBox> m_WireframeCheck;
        Ref<UI::UICheckBox> m_MeasureCheck;
        Ref<UI::UIButton> m_UnitBtn;
    };
}
