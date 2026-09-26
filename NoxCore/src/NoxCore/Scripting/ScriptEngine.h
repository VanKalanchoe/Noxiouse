#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>
#include "ScriptTypes.h"

namespace Nox
{
    class IScriptBackend;
    class Scene;
    class Entity;

    class ScriptEngine
    {
    public:
        static bool Initialize(const std::filesystem::path& executableDirectory);
        static void Shutdown();
        static bool LoadProjectAssembly();
        static bool ReloadAssembly();
        static void OnRuntimeStart(Scene* scene);
        static void OnRuntimeStop();
        static void OnCreateEntity(Entity entity);
        static void OnDestroyEntity(Entity entity);
        static void OnUpdateEntity(Entity entity, float deltaTime);
        static bool EntityClassExists(std::string_view fullName);
        static std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName);
    private:
        static std::unique_ptr<IScriptBackend> s_Backend;
        static std::unordered_map<UUID, std::vector<ScriptInstanceHandle>> s_Instances;
        static std::filesystem::path s_ExecutableDirectory;
        static Scene* s_Scene;
    };
}
