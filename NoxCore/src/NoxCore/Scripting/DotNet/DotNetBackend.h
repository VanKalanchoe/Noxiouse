#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <Coral/HostInstance.hpp>

#include "NoxCore/Scripting/IScriptBackend.h"

namespace Nox
{
    class DotNetBackend final : public IScriptBackend
    {
    public:
        ScriptLanguage GetLanguage() const override { return ScriptLanguage::DotNet; }
        bool Initialize(const std::filesystem::path& runtimeDirectory) override;
        void Shutdown() override;
        bool LoadModule(const std::filesystem::path& coreAssembly,
                        const std::filesystem::path& gameAssembly) override;
        bool WaitForJobs(uint32_t timeoutMilliseconds) override;
        bool WaitForDebugger(uint32_t timeoutMilliseconds) override;
        // Nox.ScriptApi.Version (ScriptApi.cs) this engine's internal calls match.
        static constexpr int32_t kScriptApiVersion = 13;
        bool HasType(std::string_view fullName) const override;
        std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName) const override;
        ScriptInstanceHandle CreateInstance(std::string_view fullName, UUID entity) override;
        void SetEntityField(ScriptInstanceHandle instance, std::string_view fieldName, UUID entity) override;
        void SetFieldValue(ScriptInstanceHandle instance, std::string_view fieldName, const ScriptValue& value) override;
        ScriptValue GetFieldValue(ScriptInstanceHandle instance, std::string_view fieldName, ScriptFieldType type) const override;
        std::string GetInstanceClass(ScriptInstanceHandle instance) const override;
        void DestroyInstance(ScriptInstanceHandle instance) override;
        void Invoke(ScriptInstanceHandle instance, ScriptCallback callback, float argument = 0.0f) override;
        bool Implements(ScriptInstanceHandle instance, ScriptCallback callback) const override;
        void InvokeWithString(ScriptInstanceHandle instance, std::string_view method, std::string_view argument) override;
        void SetSceneContext(Scene* scene) override;

    private:
        struct Instance { uint32_t Generation; Coral::ManagedObject Object; std::string ClassName; int32_t CallbackMask = 0; };
        void ClearModule();
        // Loads the assemblies into a new load context (no module may be loaded); false (nothing loaded) when they do not load.
        bool LoadModuleFrom(const std::filesystem::path& coreAssembly, const std::filesystem::path& gameAssembly);
        void RegisterInternalCalls(Coral::ManagedAssembly& coreAssembly);

        Coral::HostInstance m_Host;
        std::unique_ptr<Coral::AssemblyLoadContext> m_LoadContext;
        Coral::ManagedAssembly* m_CoreAssembly = nullptr;
        Coral::ManagedAssembly* m_GameAssembly = nullptr;
        std::unordered_map<std::string, const Coral::Type*> m_Types;
        std::unordered_map<std::string, std::vector<ScriptFieldInfo>> m_ExposedFields;
        std::unordered_map<std::string, int32_t> m_CallbackMasks; // per class (EntityBehaviour.GetCallbackMask)
        std::unordered_map<uint64_t, std::unique_ptr<Instance>> m_Instances;
        std::filesystem::path m_CorePath, m_GamePath;
        uint32_t m_LoadContextCount = 0; // names each load context apart: the old one is still loaded while the new one loads
        Scene* m_Scene = nullptr;
        uint64_t m_NextInstanceID = 1;
        uint32_t m_Generation = 1;
    };
}
