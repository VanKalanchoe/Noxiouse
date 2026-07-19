#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include "string"

#include "SceneCamera.h"

#include "NoxCore/Core/UUID.h"

namespace Nox
{
    struct IDComponent
    {
        UUID ID;
        IDComponent() = default;
        IDComponent(const IDComponent&) = default;
        IDComponent(const UUID& uuid) : ID(uuid) {}
    };

    struct TagComponent
    {
        std::string Tag;
        TagComponent() = default;
        TagComponent(const TagComponent&) = default;
        TagComponent(const std::string& tag) : Tag(tag) {}
    };
    
    struct TransformComponent
    {
        glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

        TransformComponent() = default;
        TransformComponent(const TransformComponent&) = default;
        TransformComponent(const glm::vec3& translation) : Translation(translation) {}

        glm::mat4 GetTransform() const
        {
            glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));

            return glm::translate(glm::mat4(1.0f), Translation)
                * rotation
                * glm::scale(glm::mat4(1.0f), Scale);
        }
    };
    
    struct CameraComponent
    {
        SceneCamera Camera;
        bool Primary = true; // todo think about moving to scene
        bool FixedAspectRatio = false;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
    };
}
