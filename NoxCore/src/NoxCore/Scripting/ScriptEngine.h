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
        // stageAssemblies (the editor): load copies of the assemblies, so a build can overwrite its output while they are loaded.
        static bool Initialize(const std::filesystem::path& executableDirectory, bool stageAssemblies = false);
        static bool IsInitialized() { return s_Backend != nullptr; }
        static void Shutdown();
        // The active project's script assembly (and Nox.ScriptCore): the newest of the one next to the executable
        // (Scripts/<Name>/<Name>.dll: the editor's CMake build, a packaged game), the project's ScriptModulePath, and the
        // project's own build (Assets/Scripts/bin/<config>/<Name>.dll).
        static bool LoadProjectAssembly();
        // The project's script assembly name: the file name of ProjectConfig::ScriptModulePath, else the project's name.
        static std::string GetProjectAssemblyName();
        // The project's C# project: the .csproj under Assets/Scripts (the one named like the assembly, else the first).
        static std::filesystem::path GetProjectScriptsProject();
        static bool ReloadAssembly();
        static void OnRuntimeStart(Scene* scene);
        static void OnRuntimeStop();
        static void OnCreateEntity(Entity entity);
        static void OnDestroyEntity(Entity entity);
        static void OnUpdateEntity(Entity entity, float deltaTime);
        // The optional callbacks; false when none of the entity's scripts implements it (nothing was called).
        static bool OnFixedUpdateEntity(Entity entity, float fixedDeltaTime);
        static bool OnLateUpdateEntity(Entity entity, float deltaTime);
        // An animation notify (AnimationSequence::Notifies) the entity's animator crossed: OnAnimationEvent(name) on its scripts.
        static void OnAnimationEvent(Entity entity, const std::string& name);
        static bool EntityClassExists(std::string_view fullName);
        static std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName);
    private:
        // Exposed field values of an entity's script instances: class -> field -> value (kept across a hot reload).
        using FieldSnapshot = std::unordered_map<std::string, std::unordered_map<std::string, ScriptValue>>;
        static void CreateInstances(Entity entity, const FieldSnapshot* restore);
        static bool FindAssemblies(std::filesystem::path& coreAssembly, std::filesystem::path& gameAssembly);

        static std::unique_ptr<IScriptBackend> s_Backend;
        static std::unordered_map<UUID, std::vector<ScriptInstanceHandle>> s_Instances;
        static std::filesystem::path s_ExecutableDirectory;
        static bool s_StageAssemblies;
        static Scene* s_Scene;
    };
}
