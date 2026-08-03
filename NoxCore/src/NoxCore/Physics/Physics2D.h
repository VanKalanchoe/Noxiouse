#pragma once

#include "box2d/box2d.h"
#include "NoxCore/Core/core.h"
#include "NoxCore/Scene/Components.h"

namespace Nox
{
    namespace Utils
    {
        inline b2BodyType Rigidbody2DTypeToBox2DBody(RigidBody2DComponent::BodyType bodyType)
        {
            switch (bodyType)
            {
            case RigidBody2DComponent::BodyType::Static: return b2_staticBody;
            case RigidBody2DComponent::BodyType::Dynamic: return b2_dynamicBody;
            case RigidBody2DComponent::BodyType::Kinematic: return b2_kinematicBody;
            }
    
            NOX_CORE_ASSERT(false, "Unknown RigidBody2DComponent::BodyType!");
            return b2_staticBody;
        }

        inline RigidBody2DComponent::BodyType Rigidbody2DTypeFromBox2DBody(b2BodyType bodyType)
        {
            switch (bodyType)
            {
            case b2_staticBody: return RigidBody2DComponent::BodyType::Static;
            case b2_dynamicBody: return RigidBody2DComponent::BodyType::Dynamic;
            case b2_kinematicBody: return RigidBody2DComponent::BodyType::Kinematic;
            }
    
            NOX_CORE_ASSERT(false, "Unknown RigidBody2DComponent::BodyType!");
            return RigidBody2DComponent::BodyType::Static;
        }
    }
}
