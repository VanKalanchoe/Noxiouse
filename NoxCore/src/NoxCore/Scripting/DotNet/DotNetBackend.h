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
        bool ReloadModule() override;
        bool HasType(std::string_view fullName) const override;
        std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName) const override;
        ScriptInstanceHandle CreateInstance(std::string_view fullName, UUID entity) override;
        void SetEntityField(ScriptInstanceHandle instance, std::string_view fieldName, UUID entity) override;
        void SetFieldValue(ScriptInstanceHandle instance, std::string_view fieldName, const ScriptValue& value) override;
        void DestroyInstance(ScriptInstanceHandle instance) override;
        void Invoke(ScriptInstanceHandle instance, ScriptCallback callback, float argument = 0.0f) override;
        void SetSceneContext(Scene* scene) override;

    private:
        struct Instance { uint32_t Generation; Coral::ManagedObject Object; };
        void ClearModule();
        void RegisterInternalCalls(Coral::ManagedAssembly& coreAssembly);

        Coral::HostInstance m_Host;
        std::unique_ptr<Coral::AssemblyLoadContext> m_LoadContext;
        Coral::ManagedAssembly* m_CoreAssembly = nullptr;
        Coral::ManagedAssembly* m_GameAssembly = nullptr;
        std::unordered_map<std::string, const Coral::Type*> m_Types;
        std::unordered_map<std::string, std::vector<ScriptFieldInfo>> m_ExposedFields;
        std::unordered_map<uint64_t, std::unique_ptr<Instance>> m_Instances;
        std::filesystem::path m_CorePath, m_GamePath;
        Scene* m_Scene = nullptr;
        uint64_t m_NextInstanceID = 1;
        uint32_t m_Generation = 1;
    };
}
