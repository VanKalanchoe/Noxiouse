#include "ScriptEngine.h"

#include "DotNet/DotNetBackend.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/ModelInstance.h"

#include <optional>
#include <system_error>

namespace Nox
{
    namespace
    {
        std::filesystem::path FindRepositoryRoot(const std::filesystem::path& executableDirectory)
        {
            std::filesystem::path candidate = executableDirectory;
            for (int depth = 0; depth < 8 && !candidate.empty(); ++depth)
            {
                if (std::filesystem::exists(candidate / "Facerun/Assets/Scripts/Facerun.csproj"))
                    return candidate;
                candidate = candidate.parent_path();
            }
            return {};
        }

        struct ScriptGeneration
        {
            std::filesystem::path CoreAssembly;
            std::filesystem::path GameAssembly;
        };

        bool CopyArtifact(const std::filesystem::path& source,
                          const std::filesystem::path& destination,
                          bool required)
        {
            if (!std::filesystem::exists(source))
            {
                if (required)
                    NOX_CORE_ERROR("Cannot reload scripts: built artifact '{}' does not exist. Build Facerun.sln first.",
                                   source.string());
                return !required;
            }

            std::error_code error;
            std::filesystem::copy_file(source, destination,
                std::filesystem::copy_options::overwrite_existing, error);
            if (error)
            {
                NOX_CORE_ERROR("Cannot stage script artifact '{}': {}", source.string(), error.message());
                return false;
            }
            return true;
        }

        std::optional<ScriptGeneration> StageBuiltScripts(const std::filesystem::path& executableDirectory)
        {
            const std::filesystem::path repositoryRoot = FindRepositoryRoot(executableDirectory);
            if (repositoryRoot.empty())
            {
                NOX_CORE_ERROR("Cannot reload scripts: Facerun.csproj was not found above '{}'",
                               executableDirectory.string());
                return std::nullopt;
            }

#ifdef NDEBUG
            constexpr const char* configuration = "Release";
#else
            constexpr const char* configuration = "Debug";
#endif
            const std::filesystem::path coreBuildDirectory =
                repositoryRoot / "NoxScriptCore/bin" / configuration / "net9.0";
            const std::filesystem::path gameBuildDirectory =
                repositoryRoot / "Facerun/Assets/Scripts/bin" / configuration;

            static uint64_t generation = 0;
            const std::filesystem::path candidateDirectory =
                repositoryRoot / "build/managed/HotReload" /
                ("Generation-" + std::to_string(++generation));

            std::error_code error;
            std::filesystem::remove_all(candidateDirectory, error);
            if (error)
            {
                NOX_CORE_ERROR("Cannot clear script candidate directory '{}': {}",
                               candidateDirectory.string(), error.message());
                return std::nullopt;
            }
            std::filesystem::create_directories(candidateDirectory, error);
            if (error)
            {
                NOX_CORE_ERROR("Cannot create script candidate directory '{}': {}",
                               candidateDirectory.string(), error.message());
                return std::nullopt;
            }

            const bool copied =
                CopyArtifact(coreBuildDirectory / "Nox.ScriptCore.dll",
                             candidateDirectory / "Nox.ScriptCore.dll", true) &&
                CopyArtifact(coreBuildDirectory / "Nox.ScriptCore.pdb",
                             candidateDirectory / "Nox.ScriptCore.pdb", false) &&
                CopyArtifact(coreBuildDirectory / "Nox.ScriptCore.deps.json",
                             candidateDirectory / "Nox.ScriptCore.deps.json", false) &&
                CopyArtifact(gameBuildDirectory / "Facerun.dll",
                             candidateDirectory / "Facerun.dll", true) &&
                CopyArtifact(gameBuildDirectory / "Facerun.pdb",
                             candidateDirectory / "Facerun.pdb", false) &&
                CopyArtifact(gameBuildDirectory / "Facerun.deps.json",
                             candidateDirectory / "Facerun.deps.json", false) &&
                CopyArtifact(gameBuildDirectory / "Facerun.runtimeconfig.json",
                             candidateDirectory / "Facerun.runtimeconfig.json", false);
            if (!copied) return std::nullopt;

            ScriptGeneration build{
                .CoreAssembly = candidateDirectory / "Nox.ScriptCore.dll",
                .GameAssembly = candidateDirectory / "Facerun.dll"
            };
            NOX_CORE_INFO("Staged manually built C# assemblies as hot-reload generation {}", generation);
            return build;
        }
    }

    std::unique_ptr<IScriptBackend> ScriptEngine::s_Backend;
    std::unordered_map<UUID, std::vector<ScriptInstanceHandle>> ScriptEngine::s_Instances;
    std::filesystem::path ScriptEngine::s_ExecutableDirectory;
    Scene* ScriptEngine::s_Scene = nullptr;

    bool ScriptEngine::Initialize(const std::filesystem::path& executableDirectory)
    {
        if (s_Backend) return true;
        s_ExecutableDirectory = executableDirectory;
        s_Backend = std::make_unique<DotNetBackend>();
        if (!s_Backend->Initialize(executableDirectory))
        {
            s_Backend.reset();
            return false;
        }
        return LoadProjectAssembly();
    }

    bool ScriptEngine::LoadProjectAssembly()
    {
        return s_Backend && s_Backend->LoadModule(
            s_ExecutableDirectory / "Scripts/Core/Nox.ScriptCore.dll",
            s_ExecutableDirectory / "Scripts/Facerun/Facerun.dll");
    }

    void ScriptEngine::Shutdown()
    {
        OnRuntimeStop();
        if (s_Backend) s_Backend->Shutdown();
        s_Backend.reset();
    }

    bool ScriptEngine::ReloadAssembly()
    {
        const auto build = StageBuiltScripts(s_ExecutableDirectory);
        if (!build || !s_Backend) return false;

        // The jobs the scripts started run code of the assemblies about to go: they finish first, or the reload waits.
        if (!s_Backend->WaitForJobs(2000))
        {
            NOX_CORE_ERROR("Script reload skipped: script jobs are still running (try again when they have finished)");
            return false;
        }

        // What the running instances' exposed fields hold now (tweaked in Play, or changed by the scripts themselves): the
        // new instances start from it instead of the scene's values.
        std::unordered_map<UUID, FieldSnapshot> snapshots;
        for (const auto& [entityID, instances] : s_Instances)
        {
            FieldSnapshot& snapshot = snapshots[entityID];
            for (ScriptInstanceHandle instance : instances)
            {
                const std::string className = s_Backend->GetInstanceClass(instance);
                if (className.empty())
                    continue;
                for (const ScriptFieldInfo& field : s_Backend->GetExposedFields(className))
                    snapshot[className][field.Name] = s_Backend->GetFieldValue(instance, field.Name, field.Type);
            }
        }

        // A build that does not load leaves the scripts that were running untouched.
        if (!s_Backend->LoadModule(build->CoreAssembly, build->GameAssembly)) return false;
        s_Instances.clear();
        if (s_Scene)
        {
            auto scripts = s_Scene->GetAllEntitiesWith<ScriptComponent>();
            for (auto entity : scripts)
            {
                Entity scriptEntity(entity, s_Scene);
                auto snapshot = snapshots.find(scriptEntity.GetUUID());
                CreateInstances(scriptEntity, snapshot != snapshots.end() ? &snapshot->second : nullptr);
            }
            for (const auto& [_, instances] : s_Instances)
                for (ScriptInstanceHandle instance : instances)
                    s_Backend->Invoke(instance, ScriptCallback::AfterReload);
        }
        return true;
    }

    void ScriptEngine::OnRuntimeStart(Scene* scene)
    {
        s_Scene = scene;
        if (s_Backend) s_Backend->SetSceneContext(scene);
    }

    void ScriptEngine::OnRuntimeStop()
    {
        if (!s_Backend) return;
        for (auto& [_, instances] : s_Instances)
            for (auto instance : instances)
                s_Backend->DestroyInstance(instance);
        s_Instances.clear();
        s_Backend->SetSceneContext(nullptr);
        s_Scene = nullptr;
    }

    void ScriptEngine::OnCreateEntity(Entity entity)
    {
        CreateInstances(entity, nullptr);
    }

    void ScriptEngine::CreateInstances(Entity entity, const FieldSnapshot* restore)
    {
        const auto& component = entity.GetComponent<ScriptComponent>();
        if (!s_Backend) return;
        auto& instances = s_Instances[entity.GetUUID()];
        for (const auto& className : component.ClassNames)
        {
            if (!s_Backend->HasType(className)) continue;
            auto instance = s_Backend->CreateInstance(className, entity.GetUUID());
            if (instance)
            {
                if (auto classFields = component.FieldOverrides.find(className);
                    classFields != component.FieldOverrides.end())
                    for (const auto& [fieldName, value] : classFields->second)
                        s_Backend->SetFieldValue(instance, fieldName, value);

                if (auto classReferences = component.EntityReferences.find(className);
                    classReferences != component.EntityReferences.end())
                {
                    for (const auto& [fieldName, reference] : classReferences->second)
                    {
                        UUID target = reference.Entity;
                        if (reference.IsModelNode())
                            target = ModelInstance::NodeUUID(reference.ModelInstance, reference.ModelNodeIndex);
                        s_Backend->SetEntityField(instance, fieldName, target);
                    }
                }

                // After a hot reload: the values the instance before it had, for the fields that kept their name and type.
                if (auto classSnapshot = restore ? restore->find(className) : FieldSnapshot::const_iterator{};
                    restore && classSnapshot != restore->end())
                {
                    for (const ScriptFieldInfo& field : s_Backend->GetExposedFields(className))
                    {
                        auto saved = classSnapshot->second.find(field.Name);
                        if (saved == classSnapshot->second.end() || saved->second.index() != field.DefaultValue.index())
                            continue;
                        if (const UUID* target = std::get_if<UUID>(&saved->second))
                            s_Backend->SetEntityField(instance, field.Name, *target);
                        else
                            s_Backend->SetFieldValue(instance, field.Name, saved->second);
                    }
                }
                instances.push_back(instance);
                s_Backend->Invoke(instance, ScriptCallback::Create);
            }
        }
        if (instances.empty()) s_Instances.erase(entity.GetUUID());
    }

    void ScriptEngine::OnDestroyEntity(Entity entity)
    {
        auto it = s_Instances.find(entity.GetUUID());
        if (!s_Backend || it == s_Instances.end())
            return;
        for (auto instance : it->second)
            s_Backend->DestroyInstance(instance);
        s_Instances.erase(it);
    }

    void ScriptEngine::OnUpdateEntity(Entity entity, float deltaTime)
    {
        auto it = s_Instances.find(entity.GetUUID());
        if (s_Backend && it != s_Instances.end())
            for (auto instance : it->second)
                s_Backend->Invoke(instance, ScriptCallback::Update, deltaTime);
    }

    bool ScriptEngine::OnFixedUpdateEntity(Entity entity, float fixedDeltaTime)
    {
        auto it = s_Instances.find(entity.GetUUID());
        bool called = false;
        if (s_Backend && it != s_Instances.end())
        {
            for (auto instance : it->second)
            {
                if (!s_Backend->Implements(instance, ScriptCallback::FixedUpdate))
                    continue;
                s_Backend->Invoke(instance, ScriptCallback::FixedUpdate, fixedDeltaTime);
                called = true;
            }
        }
        return called;
    }

    bool ScriptEngine::OnLateUpdateEntity(Entity entity, float deltaTime)
    {
        auto it = s_Instances.find(entity.GetUUID());
        bool called = false;
        if (s_Backend && it != s_Instances.end())
        {
            for (auto instance : it->second)
            {
                if (!s_Backend->Implements(instance, ScriptCallback::LateUpdate))
                    continue;
                s_Backend->Invoke(instance, ScriptCallback::LateUpdate, deltaTime);
                called = true;
            }
        }
        return called;
    }

    void ScriptEngine::OnAnimationEvent(Entity entity, const std::string& name)
    {
        auto it = s_Instances.find(entity.GetUUID());
        if (s_Backend && it != s_Instances.end())
            for (auto instance : it->second)
                s_Backend->InvokeWithString(instance, "OnAnimationEvent", name);
    }

    bool ScriptEngine::EntityClassExists(std::string_view fullName)
    {
        return s_Backend && s_Backend->HasType(fullName);
    }

    std::vector<ScriptFieldInfo> ScriptEngine::GetExposedFields(std::string_view fullName)
    {
        return s_Backend ? s_Backend->GetExposedFields(fullName) : std::vector<ScriptFieldInfo>{};
    }
}
