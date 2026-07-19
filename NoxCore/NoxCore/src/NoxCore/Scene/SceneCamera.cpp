#include "SceneCamera.h"

#include <glm/ext/matrix_clip_space.hpp>

namespace Nox
{
    SceneCamera::SceneCamera()
    {
        RecalculateProjection();
    }

    void SceneCamera::SetOrthographic(float size, float nearClip, float farClip)
    {
        m_ProjectionType = ProjectionType::Orthographic;
        m_OrthographicSize = size;
        m_OrthographicNear = nearClip;
        m_OrthographicFar = farClip;

        RecalculateProjection();
    }

    void SceneCamera::SetPerspective(float verticalFOV, float nearClip, float farClip)
    {
        m_ProjectionType = ProjectionType::Perspective;
        m_PerspectiveFOV = verticalFOV;
        m_PerspectiveNear = nearClip;
        m_PerspectiveFar = farClip;
        RecalculateProjection();
    }

    void SceneCamera::SetViewportSize(uint32_t width, uint32_t height)
    {
        m_AspectRatio = (float)width / (float)height;
        
        RecalculateProjection();
    }
    
    static glm::mat4 perspectiveProjection(float fovY, float aspectWbyH, float zNear)
    {
        float f = 1.0f / tanf(fovY / 2.0f);
        return glm::mat4(
            f / aspectWbyH, 0.0f, 0.0f, 0.0f,
            0.0f, f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, zNear, 0.0f);
    }
    
    void SceneCamera::RecalculateProjection()
    {
        if (m_ProjectionType == ProjectionType::Perspective)
        {
            m_Projection = perspectiveProjection(m_PerspectiveFOV, m_AspectRatio, m_PerspectiveNear);
        } else 
        {
            float orthoLeft = -m_OrthographicSize * m_AspectRatio * 0.5f;
            float orthoRight = m_OrthographicSize * m_AspectRatio * 0.5f;
            float orthoBottom = -m_OrthographicSize  * 0.5f;
            float orthoTop = m_OrthographicSize  * 0.5f;
        
            m_Projection = glm::ortho(orthoLeft, orthoRight, orthoBottom, orthoTop, 1.0f, 0.0f);
        }
    }
}
