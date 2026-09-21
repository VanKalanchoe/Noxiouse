#include "PhysicsEngine.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>

#include <spdlog/spdlog.h>

namespace Nox {

    bool PhysicsEngine::s_Initialized = false;

    void PhysicsEngine::Init()
    {
        if (s_Initialized)
            return;

        JPH::RegisterDefaultAllocator();

        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        s_Initialized = true;
        SPDLOG_INFO("PhysicsEngine (Jolt) initialized successfully.");
    }

    void PhysicsEngine::Shutdown()
    {
        if (!s_Initialized)
            return;

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;

        s_Initialized = false;
        SPDLOG_INFO("PhysicsEngine (Jolt) shut down successfully.");
    }

    bool PhysicsEngine::IsInitialized()
    {
        return s_Initialized;
    }

} // namespace Nox
