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
        virtual bool LoadModule(const std::filesystem::path& coreAssembly,
                                const std::filesystem::path& gameAssembly) = 0;
        virtual bool ReloadModule() = 0;
        virtual bool HasType(std::string_view fullName) const = 0;
        virtual std::vector<ScriptFieldInfo> GetExposedFields(std::string_view fullName) const = 0;
        virtual ScriptInstanceHandle CreateInstance(std::string_view fullName, UUID entity) = 0;
        virtual void SetEntityField(ScriptInstanceHandle instance, std::string_view fieldName, UUID entity) = 0;
        virtual void SetFieldValue(ScriptInstanceHandle instance, std::string_view fieldName, const ScriptValue& value) = 0;
        virtual void DestroyInstance(ScriptInstanceHandle instance) = 0;
        virtual void Invoke(ScriptInstanceHandle instance, ScriptCallback callback, float argument = 0.0f) = 0;
        virtual void SetSceneContext(Scene* scene) = 0;
    };
}
