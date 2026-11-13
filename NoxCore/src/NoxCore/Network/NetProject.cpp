#include "NetProject.h"

#include <filesystem>

#include "NoxCore/Core/Hash.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scripting/ScriptEngine.h"

namespace Nox::Net
{
    NetCompatibility MakeProjectCompatibility()
    {
        NetCompatibility compatibility;
        compatibility.Protocol = ProtocolVersion;
        if (!Project::GetActive())
            return compatibility;
        compatibility.ProjectName = Project::GetActive()->GetConfig().Name;

        uint64_t checksum = Hash::compute(compatibility.ProjectName);
        std::filesystem::path coreAssembly, gameAssembly;
        if (ScriptEngine::FindAssemblies(coreAssembly, gameAssembly))
        {
            checksum = Hash::computeFile(coreAssembly.string(), checksum);
            checksum = Hash::computeFile(gameAssembly.string(), checksum);
        }
        else
            NOX_CORE_WARN("Net: the project's script assemblies were not found: the content checksum covers the project name only");
        compatibility.ContentChecksum = checksum;
        return compatibility;
    }
}
