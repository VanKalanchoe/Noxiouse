#pragma once

#include "Scene.h"
#include "NoxCore/Core/core.h"

namespace Nox
{
    class SceneSerializer
    {
    public:
        SceneSerializer(const Ref<Scene>& scene);

        void Serialize(const std::filesystem::path& filepath);
        void SerializeRuntime(const std::filesystem::path& filepath);
        
        bool Deserialize(const std::filesystem::path& filepath);
        bool DeserializeRuntime(const std::filesystem::path& filepath);
    private:
        Ref<Scene> m_Scene;
    };
 
}
