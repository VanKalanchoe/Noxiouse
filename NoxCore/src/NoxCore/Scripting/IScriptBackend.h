#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "ScriptTypes.h"

namespace Nox
{
    class Scene;

    class IScriptBackend
    {
    public:
        virtual ~IScriptBackend() = default;
        virtual ScriptLanguage GetLanguage() const = 0;
        virtual bool Initialize(const std::filesystem::path& runtimeDirectory) = 0;
        virtual void Shutdown() = 0;
        // Loads the assemblies in place of the module loaded before, which goes first with its instances (a .NET backend cannot
        // keep two: unloading one clears the runtime's type cache for both). A failed load brings the module before back and
        // returns false. Every load, the restored one too, is a new generation: instance handles of the one before stop
        // working, so the caller makes the instances again whenever it called this.
        virtual bool LoadModule(const std::filesystem::path& coreAssembly,
                                const std::filesystem::path& gameAssembly) = 0;
        // Waits for the scripts' background jobs (Nox.Jobs) before their code is unloaded; false if some still run after
        // timeoutMilliseconds.
        virtual bool WaitForJobs(uint32_t timeoutMilliseconds) = 0;
        // Blocks until a debugger for the scripts attaches (at most timeoutMilliseconds); true once one is.
        virtual bool WaitForDebugger(uint32_t timeoutMilliseconds) = 0;
        virtual bool HasType(std::string_view fullName) const = 0;
        virtual std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName) const = 0;
        virtual ScriptInstanceHandle CreateInstance(std::string_view fullName, UUID entity) = 0;
        virtual void SetEntityField(ScriptInstanceHandle instance, std::string_view fieldName, UUID entity) = 0;
        virtual void SetFieldValue(ScriptInstanceHandle instance, std::string_view fieldName, const ScriptValue& value) = 0;
        // The current value of an exposed field (monostate for an unknown instance or field); Entity fields as their UUID.
        virtual ScriptValue GetFieldValue(ScriptInstanceHandle instance, std::string_view fieldName, ScriptFieldType type) const = 0;
        // The class an instance was created from (empty for an unknown or stale handle).
        virtual std::string GetInstanceClass(ScriptInstanceHandle instance) const = 0;
        virtual void DestroyInstance(ScriptInstanceHandle instance) = 0;
        virtual void Invoke(ScriptInstanceHandle instance, ScriptCallback callback, float argument = 0.0f) = 0;
        // Whether the instance's class implements an optional callback (FixedUpdate, LateUpdate); the others always count.
        virtual bool Implements(ScriptInstanceHandle instance, ScriptCallback callback) const = 0;
        // A method of the script taking one string (e.g. OnAnimationEvent(string name)).
        virtual void InvokeWithString(ScriptInstanceHandle instance, std::string_view method, std::string_view argument) = 0;
        virtual void SetSceneContext(Scene* scene) = 0;
    };
}
