#pragma once

#include <filesystem>
#include <memory>
#include <string>
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
        // An animation notify (AnimationSequence::Notifies) the entity's animator crossed: OnAnimationEvent(name) on its scripts.
        static void OnAnimationEvent(Entity entity, const std::string& name);
        static bool EntityClassExists(std::string_view fullName);
        static std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName);
    private:
        // Exposed field values of an entity's script instances: class -> field -> value (kept across a hot reload).
        using FieldSnapshot = std::unordered_map<std::string, std::unordered_map<std::string, ScriptValue>>;
        static void CreateInstances(Entity entity, const FieldSnapshot* restore);

        static std::unique_ptr<IScriptBackend> s_Backend;
        static std::unordered_map<UUID, std::vector<ScriptInstanceHandle>> s_Instances;
        static std::filesystem::path s_ExecutableDirectory;
        static Scene* s_Scene;
    };
}
