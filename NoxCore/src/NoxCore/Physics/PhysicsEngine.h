#pragma once

namespace Nox {

    class PhysicsEngine
    {
    public:
        static void Init();
        static void Shutdown();
        static bool IsInitialized();

    private:
        static bool s_Initialized;
    };

} // namespace Nox
