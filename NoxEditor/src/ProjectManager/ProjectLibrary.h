#pragma once

#include <filesystem>
#include <string>
#include <vector>

// What the Project Manager and the editor share about projects on this computer: the recent list and the manager's settings (kept in
// the user's settings folder, not in any project), reading a project's name, and making a new project's files. Like Unreal's Project
// Browser / Godot's Project Manager: a project is a folder with its <Name>.nproj, its Assets/ and its own C# scripts project, so any
// number of games live side by side and each opens on its own.
namespace Nox::ProjectLibrary
{
    // Most recent first (at most 10), absolute paths of .nproj files.
    std::vector<std::filesystem::path> LoadRecentProjects();
    void RememberRecentProject(const std::filesystem::path& projectFile);
    void ForgetRecentProject(const std::filesystem::path& projectFile);

    // Unreal's "Always load last project on startup": the editor skips the Project Manager and opens the most recent project.
    bool GetOpenLastProjectAtStartup();
    void SetOpenLastProjectAtStartup(bool open);
    // Where New Project puts projects (last used, else Documents/Nox Projects).
    std::filesystem::path GetDefaultProjectLocation();
    void SetDefaultProjectLocation(const std::filesystem::path& location);

    // The Name in a .nproj (its file name when it cannot be read).
    std::string ReadProjectName(const std::filesystem::path& projectFile);

    // The project's picture in the Project Manager (Unreal's Saved/AutoScreenshot.png): the editor's view, taken when a scene is saved
    // and once after the project opens if it has none.
    std::filesystem::path ThumbnailPath(const std::filesystem::path& projectDirectory);

    // The sample project of the repository the editor was built in (Facerun), if the editor runs from its build folder there.
    std::filesystem::path FindSampleProject();

    // What a new project starts with (Unreal's templates): Basic opens on a level with a sky, a sun, a floor and a camera (Unreal's
    // default "Basic" level); Blank on nothing at all (black until something is added).
    enum class ProjectTemplate { Basic, Blank };

    struct NewProjectSettings
    {
        ProjectTemplate Template = ProjectTemplate::Basic;
        std::string Name;                 // also the C# namespace and assembly: letters, digits, '_', not starting with a digit
        std::filesystem::path Location;   // the project's folder is Location/Name
        float WorldUnitsPerMeter = 100.0f;// 100 = 1 unit is 1 cm (Unreal), 1 = 1 unit is 1 m (Godot, Unity)
        std::string RenderQuality = "Auto";
        bool CreateScripts = true;        // a C# scripts project with one example behaviour
    };
    // Why a project cannot be made with these settings (empty: it can).
    std::string ValidateNewProject(const NewProjectSettings& settings);
    // Makes Location/Name/ with <Name>.nproj, Assets/ (Scenes, Scripts, an empty asset registry), the C# project and a .gitignore.
    // Returns the .nproj, or empty with `error` set.
    std::filesystem::path CreateProject(const NewProjectSettings& settings, std::string& error);

    // A C# scripts project for an existing project that has none (Assets/Scripts/<Name>.csproj + an example behaviour).
    bool CreateScriptsProject(const std::filesystem::path& projectDirectory, const std::filesystem::path& assetDirectory, const std::string& name,
                              std::string& error);
}
