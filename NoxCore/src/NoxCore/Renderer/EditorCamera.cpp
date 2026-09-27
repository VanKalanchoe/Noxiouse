#include "NoxCore//Core/core.h"
#include "EditorCamera.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "NoxCore/Core/Input.h"
#include "NoxCore/Core/WorldUnits.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace Nox 
{
	static  glm::mat4 perspectiveProjection(float fovY, float aspectWbyH, float zNear)
	{
		float f = 1.0f / tanf(fovY / 2.0f);
		return glm::mat4(
			f / aspectWbyH, 0.0f, 0.0f, 0.0f,
			0.0f, f, 0.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f,
			0.0f, 0.0f, zNear, 0.0f);
	}

	EditorCamera::EditorCamera(float fov, float aspectRatio, float nearClip, float farClip)
		: m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip), Camera(perspectiveProjection(glm::radians(fov), aspectRatio, nearClip))
	{
		UpdateView();
	}

	void EditorCamera::ApplyWorldUnitDefaults()
	{
		m_NearClip = WorldUnits::FromMeters(0.01f);
		m_FarClip = WorldUnits::FromMeters(1000.0f);
		SetDistance(WorldUnits::FromMeters(10.0f));
		m_FlySpeed = WorldUnits::FromMeters(5.0f);
		UpdateProjection();
	}

	void EditorCamera::UpdateProjection()
	{
		m_AspectRatio = m_ViewportWidth / m_ViewportHeight;
		if (IsOrthographic())
		{
			// Reverse-Z like the perspective projection (near -> 1, far -> 0), no perspective divide.
			const float halfHeight = m_OrthoHalfHeight;
			const float halfWidth = halfHeight * m_AspectRatio;
			m_Projection = glm::mat4(
				1.0f / halfWidth, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f / halfHeight, 0.0f, 0.0f,
				0.0f, 0.0f, -1.0f / (kOrthoFar - kOrthoNear), 0.0f,
				0.0f, 0.0f, kOrthoFar / (kOrthoFar - kOrthoNear), 1.0f);
			gizmoProjection = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, kOrthoNear, kOrthoFar);
			return;
		}
		m_Projection = perspectiveProjection(glm::radians(m_FOV), m_AspectRatio, m_NearClip);
		gizmoProjection = glm::perspective(glm::radians(m_FOV),
											 m_AspectRatio,
											 m_NearClip, m_FarClip);
	}

	void EditorCamera::UpdateView()
	{
		// m_Yaw = m_Pitch = 0.0f; // Lock the camera's rotation
		m_Position = CalculatePosition();
		glm::quat orientation = GetOrientation();

		// Build the view matrix
		glm::mat4 view = glm::mat4_cast(orientation);
		view[3] = glm::vec4(m_Position, 1.0f);
		view = glm::inverse(view);

		// Flip Z to account for negative viewport
		m_ViewMatrix = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * view;
	}
	
	glm::mat4 EditorCamera::GetGizmoView() const
	{
		glm::mat4 view = glm::mat4_cast(GetOrientation());
		view[3] = glm::vec4(GetPosition(), 1.0f);
		return glm::inverse(view); // no reverse-Z scale here
	}

	std::pair<float, float> EditorCamera::PanSpeed() const
	{
		float x = std::min(m_ViewportWidth / 1000.0f, 2.4f); // max = 2.4f
		float xFactor = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		float y = std::min(m_ViewportHeight / 1000.0f, 2.4f); // max = 2.4f
		float yFactor = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		return { xFactor, yFactor };
	}

	float EditorCamera::RotationSpeed() const
	{
		return 0.8f;
	}

	float EditorCamera::ZoomSpeed() const
	{
		float distance = m_Distance * 0.2f;
		distance = std::max(distance, 0.0f);
		float speed = distance * distance;
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}

	void EditorCamera::OnUpdate(Timestep ts)
	{
		if (!m_TargetsValid)
		{
			m_TargetFocalPoint = m_FocalPoint;
			m_TargetDistance = m_Distance;
			m_TargetPitch = m_Pitch;
			m_TargetYaw = m_Yaw;
			m_TargetsValid = true;
		}

		const bool altHeld = Input::IsKeyPressed(SDL_SCANCODE_LALT);
		// Godot / UE5: plain right mouse button (no Alt) flies with WASD instead of Blender-style Alt+drag orbiting.
		const bool flying = !altHeld && !IsOrthographic() && Input::IsMouseButtonPressed(SDL_BUTTON_RIGHT);

		// Where the click happened: restored on release, so the cursor reappears exactly where it went missing instead of
		// wherever it ended up. The cursor's ICON is still hidden through ImGui's own cursor state (EditorLayer's
		// OnImGuiRender, re-asserted every frame after ImGui's own cursor update runs), but actually confining the OS
		// cursor to this window -- so it can't wander onto, and click, another panel while invisible, which the ImGui
		// hide alone never did -- needs real SDL relative mouse mode, entered and left here.
		if (flying && !m_WasFlying)
		{
			m_FlyStartMousePosition = { Input::GetMouseX(), Input::GetMouseY() };
			m_FlyHasMoved = false;
			Input::SetRelativeMouseMode(true);
			Input::GetRelativeMouseDelta(); // drain whatever motion accumulated before this fly session started
		}
		else if (!flying && m_WasFlying)
		{
			Input::SetRelativeMouseMode(false);
			Input::WarpMouseInWindow(m_FlyStartMousePosition.x, m_FlyStartMousePosition.y);
			m_InitialMousePosition = m_FlyStartMousePosition; // the absolute-position tracking below resumes from the same spot
		}
		m_WasFlying = flying;

		// Flying reads relative motion (SDL keeps the OS cursor from moving at all in relative mode, so an absolute
		// position diff would read zero). Everything else -- orbit, pan, zoom -- still tracks the real cursor position:
		// m_InitialMousePosition used to sit stale from whenever the camera was last used, so the very first frame of a
		// new click computed its delta against wherever the mouse happened to be back then -- often way off, which
		// snapped the camera hard the instant you clicked. Now the delta any interaction below sees is only ever one
		// frame's worth of movement.
		glm::vec2 delta;
		if (flying)
		{
			delta = Input::GetRelativeMouseDelta() * 0.003f;
		}
		else
		{
			const glm::vec2 mouse{ Input::GetMouseX(), Input::GetMouseY() };
			delta = (mouse - m_InitialMousePosition) * 0.003f;
			m_InitialMousePosition = mouse;
		}

		if (flying)
		{
			// UE5: the click alone doesn't hide the cursor, only actually moving the mouse while held does.
			if (glm::length(delta) > 0.001f)
				m_FlyHasMoved = true;
			MouseFly(delta, static_cast<float>(ts));
		}
		else
		{
			if (altHeld)
			{
				if (Input::IsMouseButtonPressed(SDL_BUTTON_MIDDLE))
					MousePan(delta);
				else if (Input::IsMouseButtonPressed(SDL_BUTTON_LEFT))
				{
					if (!IsOrthographic()) // an ortho view keeps its axis
						MouseRotate(delta);
				}
				else if (Input::IsMouseButtonPressed(SDL_BUTTON_RIGHT))
					MouseZoom(delta.y);
			}
		}


		// Ease towards the input-driven targets, by time, so the motion is even at any frame rate (time constant 40 ms).
		const float blend = 1.0f - std::exp(-25.0f * static_cast<float>(ts));
		m_FocalPoint += (m_TargetFocalPoint - m_FocalPoint) * blend;
		m_Distance += (m_TargetDistance - m_Distance) * blend;
		m_Pitch += (m_TargetPitch - m_Pitch) * blend;
		m_Yaw += (m_TargetYaw - m_Yaw) * blend;
		if (IsOrthographic())
		{
			m_OrthoHalfHeight += (m_TargetOrthoHalfHeight - m_OrthoHalfHeight) * blend;
			UpdateProjection();
		}
		UpdateView();
	}

	void EditorCamera::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<MouseScrolledEvent>(Nox_BIND_EVENT_FN(EditorCamera::OnMouseScroll));
	}

	bool EditorCamera::OnMouseScroll(MouseScrolledEvent& e)
	{
		float delta = e.GetYOffset() * 0.1f;
		// While flying, the wheel changes fly speed instead of dollying (Godot / UE5).
		if (!IsOrthographic() && !Input::IsKeyPressed(SDL_SCANCODE_LALT) && Input::IsMouseButtonPressed(SDL_BUTTON_RIGHT))
		{
			m_FlySpeed = std::clamp(m_FlySpeed * std::exp(delta * 2.0f), WorldUnits::FromCentimeters(10.0f), WorldUnits::FromMeters(1000.0f));
			return false;
		}
		MouseZoom(delta);
		UpdateView();
		return false;
	}

	void EditorCamera::MouseFly(const glm::vec2& mouseDelta, float ts)
	{
		// Position/FocalPoint/Distance/Yaw/Pitch is an ORBIT camera's representation: Position is derived FROM FocalPoint and
		// Distance (CalculatePosition() = FocalPoint - Forward * Distance), so simply changing Yaw/Pitch while FocalPoint and
		// Distance stay put swings Position around FocalPoint on an arc -- that IS an orbit, and it's what a first attempt at
		// this looked like ("rotating moves the scene"). UE5's RMB look rotates the camera in place: the camera's own position
		// does not move just from looking around. So here the actual current position is captured BEFORE the look, and
		// FocalPoint is put back afterwards (with the NEW forward direction) to land Position exactly where it started --
		// only the WASD move below is then allowed to actually shift it.
		const glm::vec3 positionBeforeLook = CalculatePosition();

		// Look: set immediately (not eased) for a responsive FPS-style look; the move below is immediate for the same reason.
		const float yawSign = GetUpDirection().y < 0.0f ? -1.0f : 1.0f;
		m_TargetYaw += yawSign * mouseDelta.x * RotationSpeed();
		m_TargetPitch += mouseDelta.y * RotationSpeed();
		m_Yaw = m_TargetYaw;
		m_Pitch = m_TargetPitch;

		// Move: WASD forward/strafe, E/Q up/down (the gizmo's own Q/W/E/R now live on a toolbar, not these keys), Shift to go faster.
		// Forward/Right/Up already reflect the new look direction (Yaw/Pitch were just updated above).
		glm::vec3 move(0.0f);
		if (Input::IsKeyPressed(SDL_SCANCODE_W)) move += GetForwardDirection();
		if (Input::IsKeyPressed(SDL_SCANCODE_S)) move -= GetForwardDirection();
		if (Input::IsKeyPressed(SDL_SCANCODE_D)) move += GetRightDirection();
		if (Input::IsKeyPressed(SDL_SCANCODE_A)) move -= GetRightDirection();
		if (Input::IsKeyPressed(SDL_SCANCODE_E)) move += GetUpDirection();
		if (Input::IsKeyPressed(SDL_SCANCODE_Q)) move -= GetUpDirection();

		glm::vec3 offset(0.0f);
		const float lengthSq = glm::dot(move, move);
		if (lengthSq > 1e-6f)
		{
			move *= 1.0f / std::sqrt(lengthSq);
			const float speed = m_FlySpeed * (Input::IsKeyPressed(SDL_SCANCODE_LSHIFT) ? 3.0f : 1.0f);
			offset = move * speed * ts;
		}

		// Re-anchor: FocalPoint so that Position (with the new look direction) is the pre-look position plus this frame's move.
		m_FocalPoint = m_TargetFocalPoint = positionBeforeLook + offset + GetForwardDirection() * m_Distance;
	}

	void EditorCamera::MousePan(const glm::vec2& delta)
	{
		if (IsOrthographic())
		{
			// One pixel of mouse movement moves the view by one pixel (delta is in pixels x 0.003).
			const float worldPerPixel = 2.0f * m_OrthoHalfHeight / std::max(m_ViewportHeight, 1.0f);
			const glm::vec2 pixels = delta / 0.003f;
			m_TargetFocalPoint += -GetRightDirection() * pixels.x * worldPerPixel;
			m_TargetFocalPoint += GetUpDirection() * pixels.y * worldPerPixel;
			return;
		}

		auto [xSpeed, ySpeed] = PanSpeed();
		m_TargetFocalPoint += -GetRightDirection() * delta.x * xSpeed * m_Distance;
		m_TargetFocalPoint += GetUpDirection() * delta.y * ySpeed * m_Distance;
	}

	void EditorCamera::MouseRotate(const glm::vec2& delta)
	{
		float yawSign = GetUpDirection().y < 0 ? -1.0f : 1.0f;
		m_TargetYaw += yawSign * delta.x * RotationSpeed();
		m_TargetPitch += delta.y * RotationSpeed();
	}

	void EditorCamera::MouseZoom(float delta)
	{
		if (IsOrthographic())
		{
			// Each scroll notch (0.1) scales the visible size by about 11 %.
			m_TargetOrthoHalfHeight = std::clamp(m_TargetOrthoHalfHeight * std::exp(-delta * 1.2f), 0.001f, 1.0e6f);
			return;
		}

		m_TargetDistance -= delta * ZoomSpeed();
		constexpr float minimumDistance = 1.0f;
		if (m_TargetDistance < minimumDistance)
		{
			m_TargetFocalPoint += GetForwardDirection() * (minimumDistance - m_TargetDistance);
			m_TargetDistance = minimumDistance;
		}
	}

	glm::vec3 EditorCamera::GetUpDirection() const
	{
		return glm::rotate(GetOrientation(), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	glm::vec3 EditorCamera::GetRightDirection() const
	{
		return glm::rotate(GetOrientation(), glm::vec3(1.0f, 0.0f, 0.0f));
	}

	glm::vec3 EditorCamera::GetForwardDirection() const
	{
		return glm::rotate(GetOrientation(), glm::vec3(0.0f, 0.0f, -1.0f));
	}

	glm::vec3 EditorCamera::CalculatePosition() const
	{
		return m_FocalPoint - GetForwardDirection() * (IsOrthographic() ? kOrthoCameraDistance : m_Distance);
	}

	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::quat(glm::vec3(-m_Pitch, -m_Yaw, 0.0f));
	}

	void EditorCamera::Focus(const glm::vec3& centre, float radius)
	{
		radius = std::max(radius, WorldUnits::FromCentimeters(10.0f)); // a point-sized entity still frames at a sensible distance
		SetFocalPoint(centre);
		if (IsOrthographic())
		{
			// 1.3x padding so the entity isn't touching the frame edge, matching the perspective case below.
			m_OrthoHalfHeight = m_TargetOrthoHalfHeight = radius * 1.3f;
			UpdateProjection();
		}
		else
		{
			const float halfFov = glm::radians(m_FOV) * 0.5f;
			SetDistance(std::max(radius / std::sin(halfFov) * 1.3f, WorldUnits::FromCentimeters(10.0f)));
		}
	}

	void EditorCamera::SetViewMode(EditorViewMode mode)
	{
		if (mode == m_ViewMode)
			return;

		const bool wasOrthographic = IsOrthographic();
		m_ViewMode = mode;
		if (mode == EditorViewMode::Perspective)
		{
			UpdateProjection();
			UpdateView();
			return;
		}

		// Coming from the perspective view the ortho view frames the same amount of the scene (the height seen at the focal point).
		if (!wasOrthographic)
			m_OrthoHalfHeight = m_TargetOrthoHalfHeight = std::max(m_TargetDistance * std::tan(glm::radians(m_FOV) * 0.5f), 0.01f);
		m_TargetFocalPoint = m_FocalPoint;

		float pitch = 0.0f, yaw = 0.0f;
		switch (mode)
		{
		case EditorViewMode::Top: pitch = glm::half_pi<float>(); break;       // looks down
		case EditorViewMode::Bottom: pitch = -glm::half_pi<float>(); break;   // looks up
		case EditorViewMode::Front: break;                                    // looks along -Z
		case EditorViewMode::Back: yaw = glm::pi<float>(); break;             // looks along +Z
		case EditorViewMode::Left: yaw = glm::half_pi<float>(); break;        // looks along +X (the camera is on the left)
		case EditorViewMode::Right: yaw = -glm::half_pi<float>(); break;      // looks along -X
		default: break;
		}
		m_Pitch = m_TargetPitch = pitch;
		m_Yaw = m_TargetYaw = yaw;
		m_TargetsValid = true;
		UpdateProjection();
		UpdateView();
	}

}
