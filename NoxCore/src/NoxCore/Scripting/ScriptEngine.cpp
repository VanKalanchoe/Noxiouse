#include "ScriptEngine.h"

#include "DotNet/DotNetBackend.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Project/Project.h"

#include <optional>
#include <system_error>

namespace Nox
{
    namespace
    {
#ifdef NDEBUG
        constexpr const char* kConfiguration = "Release";
#else
        constexpr const char* kConfiguration = "Debug";
#endif

        // The engine's source checkout above the executable (a development build): where NoxScriptCore is built.
        std::filesystem::path FindRepositoryRoot(const std::filesystem::path& executableDirectory)
        {
            std::filesystem::path candidate = executableDirectory;
            for (int depth = 0; depth < 8 && !candidate.empty(); ++depth)
            {
                if (std::filesystem::exists(candidate / "NoxScriptCore/Nox.ScriptCore.csproj"))
                    return candidate;
                if (candidate == candidate.parent_path())
                    break;
                candidate = candidate.parent_path();
            }
            return {};
        }

        // The newest of the files that exist (empty if none does).
        std::filesystem::path Newest(std::initializer_list<std::filesystem::path> candidates)
        {
            std::filesystem::path newest;
            std::filesystem::file_time_type newestTime{};
            for (const std::filesystem::path& candidate : candidates)
            {
                std::error_code error;
                if (candidate.empty() || !std::filesystem::is_regular_file(candidate, error))
                    continue;
                const auto time = std::filesystem::last_write_time(candidate, error);
                if (!error && (newest.empty() || time > newestTime))
                {
                    newest = candidate;
                    newestTime = time;
                }
            }
            return newest;
        }

        struct ScriptGeneration
        {
            std::filesystem::path CoreAssembly;
            std::filesystem::path GameAssembly;
        };

        // An assembly and the files beside it that load with it (.pdb for the debugger, .deps.json, .runtimeconfig.json).
        bool CopyAssembly(const std::filesystem::path& assembly, const std::filesystem::path& directory)
        {
            std::error_code error;
            for (const char* extension : { ".dll", ".pdb", ".deps.json", ".runtimeconfig.json" })
            {
                std::filesystem::path source = assembly;
                source.replace_extension(extension);
                if (!std::filesystem::exists(source, error))
                {
                    if (std::string_view(extension) == ".dll")
                        return false;
                    continue;
                }
                std::filesystem::copy_file(source, directory / source.filename(), std::filesystem::copy_options::overwrite_existing, error);
                if (error)
                {
                    NOX_CORE_ERROR("Cannot stage script file '{}': {}", source.string(), error.message());
                    return false;
                }
            }
            return true;
        }

        // Copies of the assemblies in a directory of their own: the loaded ones stay locked (Windows maps them), so the next
        // build can still write its output.
        std::optional<ScriptGeneration> StageAssemblies(const ScriptGeneration& built, const std::filesystem::path& stagingRoot)
        {
            static uint64_t generation = 0;
            if (generation == 0)
            {
                // Generations of an earlier session (the ones still locked stay; nothing needs them).
                std::error_code ignored;
                std::filesystem::remove_all(stagingRoot, ignored);
            }
            const std::filesystem::path directory = stagingRoot / ("Generation-" + std::to_string(++generation));
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error)
            {
                NOX_CORE_ERROR("Cannot create the script staging directory '{}': {}", directory.string(), error.message());
                return std::nullopt;
            }
            if (!CopyAssembly(built.CoreAssembly, directory) || !CopyAssembly(built.GameAssembly, directory))
                return std::nullopt;
            return ScriptGeneration{ directory / built.CoreAssembly.filename(), directory / built.GameAssembly.filename() };
        }
    }

    std::unique_ptr<IScriptBackend> ScriptEngine::s_Backend;
    std::unordered_map<UUID, std::vector<ScriptInstanceHandle>> ScriptEngine::s_Instances;
    std::filesystem::path ScriptEngine::s_ExecutableDirectory;
    bool ScriptEngine::s_StageAssemblies = false;
    bool ScriptEngine::s_WaitForDebugger = false;
    Scene* ScriptEngine::s_Scene = nullptr;

    bool ScriptEngine::Initialize(const std::filesystem::path& executableDirectory, bool stageAssemblies)
    {
        if (s_Backend) return true;
        s_ExecutableDirectory = executableDirectory;
        s_StageAssemblies = stageAssemblies;
        s_Backend = std::make_unique<DotNetBackend>();
        if (!s_Backend->Initialize(executableDirectory))
        {
            s_Backend.reset();
            return false;
        }
        return LoadProjectAssembly();
    }

    std::string ScriptEngine::GetProjectAssemblyName()
    {
        Ref<Project> project = Project::GetActive();
        if (!project)
            return {};
        const std::filesystem::path& module = project->GetConfig().ScriptModulePath;
        return module.empty() ? project->GetConfig().Name : module.stem().string();
    }

    std::filesystem::path ScriptEngine::GetProjectScriptsProject()
    {
        if (!Project::GetActive())
            return {};
        const std::filesystem::path scripts = Project::GetActiveAssetDirectory() / "Scripts";
        const std::string name = GetProjectAssemblyName();
        std::error_code error;
        if (!std::filesystem::is_directory(scripts, error))
            return {};
        std::filesystem::path first;
        for (auto it = std::filesystem::recursive_directory_iterator(scripts, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
        {
            const std::filesystem::path& path = it->path();
            const std::string folder = path.filename().string();
            if (it->is_directory() && (folder == "bin" || folder == "obj"))
            {
                it.disable_recursion_pending();
                continue;
            }
            if (path.extension() != ".csproj")
                continue;
            if (path.stem() == name)
                return path;
            if (first.empty())
                first = path;
        }
        return first;
    }

    bool ScriptEngine::FindAssemblies(std::filesystem::path& coreAssembly, std::filesystem::path& gameAssembly)
    {
        const std::filesystem::path repository = FindRepositoryRoot(s_ExecutableDirectory);
        coreAssembly = Newest({ s_ExecutableDirectory / "Scripts/Core/Nox.ScriptCore.dll",
                                repository.empty() ? std::filesystem::path() : repository / "NoxScriptCore/bin" / kConfiguration / "net9.0/Nox.ScriptCore.dll" });

        const std::string name = GetProjectAssemblyName();
        if (name.empty())
            return false;
        const std::string file = name + ".dll";
        std::filesystem::path fromConfig;
        if (Project::GetActive() && !Project::GetActive()->GetConfig().ScriptModulePath.empty())
            fromConfig = Project::GetActiveAssetDirectory() / Project::GetActive()->GetConfig().ScriptModulePath;
        const std::filesystem::path csproj = GetProjectScriptsProject();
        gameAssembly = Newest({ s_ExecutableDirectory / "Scripts" / name / file, fromConfig,
                                csproj.empty() ? std::filesystem::path() : csproj.parent_path() / "bin" / kConfiguration / file });

        if (coreAssembly.empty())
            NOX_CORE_ERROR("Scripts: Nox.ScriptCore.dll was not found (next to the executable in Scripts/Core, or built in NoxScriptCore)");
        if (gameAssembly.empty())
            NOX_CORE_WARN("Scripts: the project's script assembly {} was not found{}", file,
                          csproj.empty() ? std::string(" (no .csproj in the project's Assets/Scripts)") : " -- build " + csproj.generic_string() + " first");
        return !coreAssembly.empty() && !gameAssembly.empty();
    }

    bool ScriptEngine::LoadProjectAssembly()
    {
        if (!s_Backend)
            return false;
        std::filesystem::path core, game;
        if (!FindAssemblies(core, game))
            return false;
        // The editor loads copies (the build overwrites its output while the editor runs); a packaged game loads them where they are.
        if (s_StageAssemblies && Project::GetActive())
        {
            const auto staged = StageAssemblies({ core, game }, Project::GetActiveProjectDirectory() / "Intermediate/Scripts");
            if (!staged)
                return false;
            core = staged->CoreAssembly;
            game = staged->GameAssembly;
        }
        NOX_CORE_INFO("Scripts: loading {}", game.generic_string());
        return s_Backend->LoadModule(core, game);
    }

    void ScriptEngine::Shutdown()
    {
        OnRuntimeStop();
        if (s_Backend) s_Backend->Shutdown();
        s_Backend.reset();
    }

    bool ScriptEngine::ReloadAssembly()
    {
        if (!s_Backend || !Project::GetActive()) return false;
        std::filesystem::path core, game;
        if (!FindAssemblies(core, game)) return false;
        const auto build = StageAssemblies({ core, game }, Project::GetActiveProjectDirectory() / "Intermediate/Scripts");
        if (!build) return false;

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
        if (s_Backend && s_WaitForDebugger && !s_Backend->WaitForDebugger(60000))
            NOX_CORE_WARN("Scripts: no debugger attached within 60 s, playing on");
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
