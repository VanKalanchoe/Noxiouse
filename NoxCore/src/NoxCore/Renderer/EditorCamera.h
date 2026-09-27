#pragma once

#include "Camera.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/Core/Timestep.h"
#include "NoxCore/Events/Event.h"
#include "NoxCore/Events/InputEvents.h"

#include <cstdint>
#include <utility>

#include <glm/glm.hpp>

namespace Nox 
{

    // Perspective, or an orthographic view along a world axis (Top looks down, Front looks along -Z from +Z, Right looks along -X from +X).
    enum class EditorViewMode : uint8_t { Perspective, Top, Bottom, Front, Back, Left, Right };

    class EditorCamera : public Camera
    {
    public:
        EditorCamera() = default;
        EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

        // The near/far clip planes and the default orbit distance are real 3D lengths, but this camera is a long-lived object
        // constructed before a project (and so its WorldUnits::PerMeter()) is known -- default member initializers would bake in
        // whatever unit was live at app start. Call this once a project has loaded instead (docs/Units_And_World_Tools_Plan_2026.md, U6).
        void ApplyWorldUnitDefaults();

        void OnUpdate(Timestep ts);
        void OnEvent(Event& e);

        inline float GetDistance() const { return m_Distance; }
        inline void SetDistance(float distance) { m_Distance = m_TargetDistance = distance; }
        inline void SetFocalPoint(const glm::vec3& point) { m_FocalPoint = m_TargetFocalPoint = point; }
        // UE5's "Frame Selected" (F, or double-clicking an entity in the Outliner): re-centres on `centre` at a distance that
        // fits a sphere of `radius` in view (padded so it isn't touching the frame edge), instead of a fixed offset.
        void Focus(const glm::vec3& centre, float radius);

        inline void SetViewportSize(float width, float height) { m_ViewportWidth = width; m_ViewportHeight = height; UpdateProjection(); }

        const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
        glm::mat4 GetGizmoView() const;
        glm::mat4 GetViewProjection() const { return m_Projection * m_ViewMatrix; }

        glm::vec3 GetUpDirection() const;
        glm::vec3 GetRightDirection() const;
        glm::vec3 GetForwardDirection() const;
        const glm::vec3& GetPosition() const { return m_Position; }
        glm::quat GetOrientation() const;

        float GetPitch() const { return m_Pitch; }
        float GetYaw() const { return m_Yaw; }
        
        // UE5: right-click alone does not hide the cursor, only moving the mouse while it's held does -- a still cursor right
        // after the click reads as about to open a context menu, not as "now flying". True once that has happened this
        // right-click; EditorLayer uses it to keep the cursor hidden every frame, from after ImGui's own per-frame cursor
        // update runs (see the comment in OnUpdate).
        bool ShouldHideCursor() const { return m_WasFlying && m_FlyHasMoved; }

        EditorViewMode GetViewMode() const { return m_ViewMode; }
        bool IsOrthographic() const { return m_ViewMode != EditorViewMode::Perspective; }
        void SetViewMode(EditorViewMode mode);
        // Half the visible height in world units (ortho views only) and the point the camera looks at.
        float GetOrthoHalfHeight() const { return m_OrthoHalfHeight; }
        const glm::vec3& GetFocalPoint() const { return m_FocalPoint; }

        // Vertical FOV: what the projection matrix, DLSS and ImGuizmo all use -- kept internal so none of them need touching.
        float GetFOV() const { return m_FOV; }
        void SetFOV(float fov) { m_FOV = fov; UpdateProjection(); }
        float GetAspectRatio() const { return m_AspectRatio; }
        float GetNearClip() const { return m_NearClip; }
    private:
        void UpdateProjection();
        void UpdateView();

        bool OnMouseScroll(MouseScrolledEvent& e);

        void MousePan(const glm::vec2& delta);
        void MouseRotate(const glm::vec2& delta);
        void MouseZoom(float delta);
        // Godot / UE5's viewport navigation: hold the right mouse button (no Alt) to look around and fly with WASD (E/Q up/down,
        // Shift to go faster); the wheel changes fly speed while held, instead of dollying.
        void MouseFly(const glm::vec2& mouseDelta, float ts);

        glm::vec3 CalculatePosition() const;

        std::pair<float, float> PanSpeed() const;
        float RotationSpeed() const;
        float ZoomSpeed() const;
    private:
        float m_FOV = 90.0f, m_AspectRatio = 1.778f, m_NearClip = 0.01f, m_FarClip = 1000.0f;

        glm::mat4 m_ViewMatrix;
        glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_FocalPoint = { 0.0f, 0.0f, 0.0f };

        glm::vec2 m_InitialMousePosition = { 0.0f, 0.0f };

        float m_Distance = 1000.0f;
        float m_FlySpeed = 100.0f; // world units/second; ApplyWorldUnitDefaults sets the real starting value once a project is loaded
        bool m_WasFlying = false; // edge-detects entering/leaving fly mode
        bool m_FlyHasMoved = false; // the mouse has actually moved since this right-click started (see ShouldHideCursor)
        glm::vec2 m_FlyStartMousePosition = { 0.0f, 0.0f }; // where the cursor was hidden from; restored here on release
        // Mouse input moves these; OnUpdate eases the real values towards them over a few frames. The mouse reports at
        // 125-1000 Hz, far below the frame rate of a light scene, so most frames saw no movement and a few saw all of it --
        // fast camera motion stuttered in bursts. Time-based easing turns that into steady motion.
        glm::vec3 m_TargetFocalPoint = { 0.0f, 0.0f, 0.0f };
        float m_TargetDistance = 10.0f;
        float m_TargetPitch = 0.0f, m_TargetYaw = 0.0f;
        bool m_TargetsValid = false;
        float m_Pitch = 0.0f, m_Yaw = 0.0f;

        float m_ViewportWidth = 1280, m_ViewportHeight = 720;

        // Ortho views: the camera sits kOrthoCameraDistance from the focal point along the view axis (so the shaders' camera position gives
        // a nearly parallel view vector) and sees kOrthoNear .. kOrthoFar in front of it; the zoom is the visible half height.
        static constexpr float kOrthoCameraDistance = 5000.0f;
        static constexpr float kOrthoNear = 0.1f;
        static constexpr float kOrthoFar = 10000.0f;
        EditorViewMode m_ViewMode = EditorViewMode::Perspective;
        float m_OrthoHalfHeight = 10.0f;
        float m_TargetOrthoHalfHeight = 10.0f;
    };

}
