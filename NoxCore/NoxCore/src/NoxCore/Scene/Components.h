#pragma once
#include "string"
#include "glm/glm.hpp"

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
        glm::mat4 Transform{ 1.0f };
        
        TransformComponent() = default;
        TransformComponent(const TransformComponent&) = default;
    };
}
