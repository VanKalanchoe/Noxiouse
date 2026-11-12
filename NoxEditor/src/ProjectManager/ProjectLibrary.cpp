#include "ProjectLibrary.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"

namespace Nox::ProjectLibrary
{
    namespace
    {
        std::filesystem::path SettingsFile(const char* name)
        {
            char* prefPath = SDL_GetPrefPath("Nox", "NoxEditor");
            if (!prefPath)
                return {};
            std::filesystem::path file = std::filesystem::path(prefPath) / name;
            SDL_free(prefPath);
            return file;
        }

        void SaveRecent(const std::vector<std::filesystem::path>& projects)
        {
            const std::filesystem::path file = SettingsFile("RecentProjects.txt");
            if (file.empty())
                return;
            std::ofstream out(file, std::ios::trunc);
            for (const std::filesystem::path& recent : projects)
                out << recent.generic_string() << "\n";
        }

        std::filesystem::path Normalized(const std::filesystem::path& path)
        {
            std::error_code error;
            return std::filesystem::absolute(path, error).lexically_normal();
        }

        // Project Manager settings: "key=value" lines.
        std::string ReadSetting(const std::string& key, const std::string& fallback)
        {
            std::ifstream file(SettingsFile("ProjectManager.txt"));
            std::string line;
            while (std::getline(file, line))
                if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=')
                    return line.substr(key.size() + 1);
            return fallback;
        }

        void WriteSetting(const std::string& key, const std::string& value)
        {
            const std::filesystem::path path = SettingsFile("ProjectManager.txt");
            if (path.empty())
                return;
            std::vector<std::string> lines;
            {
                std::ifstream file(path);
                std::string line;
                while (std::getline(file, line))
                    if (!(line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '='))
                        lines.push_back(line);
            }
            lines.push_back(key + "=" + value);
            std::ofstream out(path, std::ios::trunc);
            for (const std::string& line : lines)
                out << line << "\n";
        }

        bool WriteText(const std::filesystem::path& path, const std::string& text, std::string& error)
        {
            std::ofstream out(path, std::ios::trunc | std::ios::binary);
            out << text;
            if (!out)
            {
                error = "cannot write " + path.generic_string();
                return false;
            }
            return true;
        }

        // How the scripts project reaches the engine's script API: the Nox.ScriptCore.dll the editor's build puts next to it (as
        // Unity's generated projects reference its UnityEngine.dll), not copied into the game's output (the editor and the packaged
        // game load their own). From a repository build without that copy, the API's own project.
        std::string ScriptCoreReference()
        {
            const char* base = SDL_GetBasePath();
            const std::filesystem::path executable = base ? std::filesystem::path(base) : std::filesystem::path();
            const std::filesystem::path dll = executable / "Scripts/Core/Nox.ScriptCore.dll";
            std::error_code error;
            if (!executable.empty() && std::filesystem::exists(dll, error))
                return "    <Reference Include=\"Nox.ScriptCore\">\n"
                       "      <HintPath>" + Normalized(dll).generic_string() + "</HintPath>\n"
                       "      <Private>false</Private>\n"
                       "    </Reference>\n";
            for (std::filesystem::path folder = executable; !folder.empty(); folder = folder.parent_path())
            {
                const std::filesystem::path csproj = folder / "NoxScriptCore/Nox.ScriptCore.csproj";
                if (std::filesystem::exists(csproj, error))
                    return "    <ProjectReference Include=\"" + Normalized(csproj).generic_string() + "\" Private=\"false\" />\n";
                if (folder == folder.parent_path())
                    break;
            }
            NOX_CORE_WARN("New Project: Nox.ScriptCore.dll was not found next to the editor; set its path in the .csproj by hand");
            return "    <Reference Include=\"Nox.ScriptCore\">\n      <HintPath>Nox.ScriptCore.dll</HintPath>\n      <Private>false</Private>\n    </Reference>\n";
        }
    }

    std::vector<std::filesystem::path> LoadRecentProjects()
    {
        std::vector<std::filesystem::path> projects;
        std::ifstream file(SettingsFile("RecentProjects.txt"));
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                projects.emplace_back(line);
        }
        return projects;
    }

    void RememberRecentProject(const std::filesystem::path& projectFile)
    {
        const std::filesystem::path absolute = Normalized(projectFile);
        std::vector<std::filesystem::path> projects = LoadRecentProjects();
        std::erase_if(projects, [&](const std::filesystem::path& recent) { return recent == absolute; });
        projects.insert(projects.begin(), absolute);
        if (projects.size() > 10)
            projects.resize(10);
        SaveRecent(projects);
    }

    void ForgetRecentProject(const std::filesystem::path& projectFile)
    {
        const std::filesystem::path absolute = Normalized(projectFile);
        std::vector<std::filesystem::path> projects = LoadRecentProjects();
        std::erase_if(projects, [&](const std::filesystem::path& recent) { return recent == absolute; });
        SaveRecent(projects);
    }

    bool GetOpenLastProjectAtStartup() { return ReadSetting("OpenLastProject", "0") == "1"; }
    void SetOpenLastProjectAtStartup(bool open) { WriteSetting("OpenLastProject", open ? "1" : "0"); }

    std::filesystem::path GetDefaultProjectLocation()
    {
        const std::string saved = ReadSetting("ProjectLocation", "");
        if (!saved.empty())
            return saved;
        const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        if (documents)
            return std::filesystem::path(documents) / "Nox Projects";
        const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
        return home ? std::filesystem::path(home) / "Nox Projects" : std::filesystem::path("Nox Projects");
    }

    void SetDefaultProjectLocation(const std::filesystem::path& location) { WriteSetting("ProjectLocation", location.generic_string()); }

    std::string ReadProjectName(const std::filesystem::path& projectFile)
    {
        try
        {
            const YAML::Node data = YAML::LoadFile(projectFile.string());
            if (const YAML::Node name = data["Project"]["Name"]; name && name.IsScalar())
                return name.as<std::string>();
        }
        catch (const std::exception&)
        {
        }
        return projectFile.stem().string();
    }

    std::filesystem::path ThumbnailPath(const std::filesystem::path& projectDirectory)
    {
        return projectDirectory / "Saved" / "Thumbnail.png";
    }

    std::filesystem::path FindSampleProject()
    {
        const char* base = SDL_GetBasePath();
        std::filesystem::path candidate = base ? std::filesystem::path(base) : std::filesystem::path();
        for (int depth = 0; depth < 8 && !candidate.empty(); ++depth)
        {
            std::error_code error;
            if (std::filesystem::exists(candidate / "Facerun/Facerun.nproj", error))
                return Normalized(candidate / "Facerun/Facerun.nproj");
            if (candidate == candidate.parent_path())
                break;
            candidate = candidate.parent_path();
        }
        return {};
    }

    std::string ValidateNewProject(const NewProjectSettings& settings)
    {
        const std::string& name = settings.Name;
        if (name.empty())
            return "Type a name for the project.";
        if (name.size() > 64)
            return "The name is too long (64 characters at most).";
        if (std::isdigit(static_cast<unsigned char>(name.front())))
            return "The name cannot start with a digit (it is also the C# namespace of the scripts).";
        if (!std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
            return "Use only letters, digits and _ (the name is also the C# namespace and the scripts' assembly).";
        if (settings.Location.empty())
            return "Choose a location.";
        const std::filesystem::path folder = settings.Location / name;
        std::error_code error;
        if (std::filesystem::exists(folder, error) && !std::filesystem::is_empty(folder, error))
            return "The folder " + folder.generic_string() + " already exists and is not empty.";
        return {};
    }

    bool CreateScriptsProject(const std::filesystem::path& projectDirectory, const std::filesystem::path& assetDirectory, const std::string& name,
                              std::string& error)
    {
        const std::filesystem::path scripts = assetDirectory / "Scripts";
        std::error_code fileError;
        std::filesystem::create_directories(scripts / "Source", fileError);
        if (fileError)
        {
            error = "cannot create " + (scripts / "Source").generic_string() + ": " + fileError.message();
            return false;
        }
        // The same settings as the sample project's: a library the engine loads at run time, its output in bin/<Configuration>/
        // (where the editor's script build and loader look).
        const std::string csproj =
            "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
            "  <PropertyGroup>\n"
            "    <OutputType>Library</OutputType>\n"
            "    <AssemblyName>" + name + "</AssemblyName>\n"
            "    <RootNamespace>" + name + "</RootNamespace>\n"
            "    <TargetFramework>net9.0</TargetFramework>\n"
            "    <ImplicitUsings>enable</ImplicitUsings>\n"
            "    <Nullable>enable</Nullable>\n"
            "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
            "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n"
            "    <CopyLocalLockFileAssemblies>true</CopyLocalLockFileAssemblies>\n"
            "    <EnableDynamicLoading>true</EnableDynamicLoading>\n"
            "  </PropertyGroup>\n"
            "\n"
            "  <ItemGroup>\n" + ScriptCoreReference() +
            "  </ItemGroup>\n"
            "</Project>\n";
        if (!WriteText(scripts / (name + ".csproj"), csproj, error))
            return false;

        const std::filesystem::path example = scripts / "Source/Rotator.cs";
        if (std::filesystem::exists(example, fileError))
            return true;
        const std::string rotator =
            "using Nox;\n"
            "\n"
            "namespace " + name + ";\n"
            "\n"
            "// An example behaviour: put it on an entity (Inspector > Add Component > Script, Class \"" + name + ".Rotator\")\n"
            "// and press Play -- the entity turns around its up axis.\n"
            "public sealed class Rotator : EntityBehaviour\n"
            "{\n"
            "    [Expose] public float DegreesPerSecond = 90.0f;\n"
            "\n"
            "    protected override void OnUpdate(float deltaTime)\n"
            "    {\n"
            "        TransformComponent transform = GetComponent<TransformComponent>();\n"
            "        Vector3 rotation = transform.LocalRotation; // radians\n"
            "        rotation.Y += DegreesPerSecond * (System.MathF.PI / 180.0f) * deltaTime;\n"
            "        transform.LocalRotation = rotation;\n"
            "    }\n"
            "}\n";
        (void)projectDirectory;
        return WriteText(example, rotator, error);
    }

    std::filesystem::path CreateProject(const NewProjectSettings& settings, std::string& error)
    {
        error = ValidateNewProject(settings);
        if (!error.empty())
            return {};
        const std::filesystem::path directory = Normalized(settings.Location / settings.Name);
        const std::filesystem::path assets = directory / "Assets";
        std::error_code fileError;
        std::filesystem::create_directories(assets / "Scenes", fileError);
        if (!fileError)
            std::filesystem::create_directories(assets / "Scripts", fileError);
        if (fileError)
        {
            error = "cannot create the folders in " + directory.generic_string() + ": " + fileError.message();
            return {};
        }
        if (!WriteText(assets / "AssetRegistry.nxr", "AssetRegistry: []\n", error))
            return {};

        const std::string& name = settings.Name;
        char units[32];
        SDL_snprintf(units, sizeof(units), "%g", settings.WorldUnitsPerMeter);
        // The Start Scene is set once a scene is saved (the editor opens an empty scene until then).
        const std::string project =
            "Project:\n"
            "  Name: " + name + "\n"
            "  StartScene: 0\n"
            "  AssetDirectory: \"Assets\"\n"
            "  AssetRegistryPath: \"AssetRegistry.nxr\"\n"
            "  ScriptModulePath: \"Scripts/Binaries/" + name + ".dll\"\n"
            "  WorldUnitsPerMeter: " + units + "\n"
            "  RenderQuality: " + (settings.RenderQuality.empty() ? std::string("Auto") : settings.RenderQuality) + "\n";
        const std::filesystem::path projectFile = directory / (name + ".nproj");
        if (!WriteText(projectFile, project, error))
            return {};

        // What version control should not keep: the editor's per-project caches and the scripts' build output.
        WriteText(directory / ".gitignore", "Intermediate/\nSaved/\nBuild/\nAssets/Scripts/bin/\nAssets/Scripts/obj/\nAssets/Scripts/Binaries/\n.vs/\n", error);
        error.clear();

        if (settings.CreateScripts && !CreateScriptsProject(directory, assets, name, error))
            return {};

        SetDefaultProjectLocation(settings.Location);
        return projectFile;
    }
}
