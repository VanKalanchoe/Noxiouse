#pragma once

#include "Project.h"
#include "NoxCore/Core/core.h"

namespace Nox
{
    class ProjectSerializer
    {
    public:
        ProjectSerializer(Ref<Project> project);

        bool Serialize(const std::filesystem::path& filePath);
        bool Deserialize(const std::filesystem::path& filePath);
    private:
        Ref<Project> m_Project;
    };
}
