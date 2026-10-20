#include "GamePackager.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <system_error>
#include <unordered_set>

#include <SDL3/SDL_filesystem.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Project/ProjectSerializer.h"

namespace Nox
{
    namespace
    {
        struct Copier
        {
            PackageReport& Report;

            bool File(const std::filesystem::path& source, const std::filesystem::path& destination)
            {
                std::error_code error;
                std::filesystem::create_directories(destination.parent_path(), error);
                std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
                if (error)
                {
                    Report.Warnings.push_back("Could not copy " + source.generic_string() + ": " + error.message());
                    return false;
                }
                ++Report.Files;
                Report.Bytes += std::filesystem::file_size(destination, error);
                return true;
            }

            // A directory tree, skipping what `skip` refuses (given the path relative to `source`).
            template <typename Skip>
            void Tree(const std::filesystem::path& source, const std::filesystem::path& destination, Skip&& skip)
            {
                std::error_code error;
                for (auto it = std::filesystem::recursive_directory_iterator(source, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
                {
                    const std::filesystem::path relative = it->path().lexically_relative(source);
                    if (skip(relative, it->is_directory(error)))
                    {
                        if (it->is_directory(error))
                            it.disable_recursion_pending();
                        continue;
                    }
                    if (it->is_regular_file(error))
                        File(it->path(), destination / relative);
                }
            }
        };

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // `path` is `directory` or somewhere below it.
        bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& directory)
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::weakly_canonical(path, error).lexically_relative(std::filesystem::weakly_canonical(directory, error));
            return !relative.empty() && *relative.begin() != "..";
        }

        // The files a .gltf names (buffers, images), relative to it; data: URIs are inside the file.
        std::vector<std::string> GltfUris(const std::filesystem::path& gltf)
        {
            std::vector<std::string> uris;
            std::ifstream stream(gltf);
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            static const std::regex uriPattern("\"uri\"\\s*:\\s*\"([^\"]+)\"");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), uriPattern); it != std::sregex_iterator(); ++it)
            {
                const std::string uri = (*it)[1].str();
                if (uri.rfind("data:", 0) != 0)
                    uris.push_back(uri);
            }
            return uris;
        }
    }

    PackageReport PackageGame(const PackageSettings& settings)
    {
        PackageReport report;
        Ref<Project> project = Project::GetActive();
        if (!project)
        {
            report.Error = "No project is open.";
            return report;
        }
        const ProjectConfig& config = project->GetConfig();
        auto assetManager = project->GetEditorAssetManager();
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        const std::filesystem::path binaryDirectory = SDL_GetBasePath() ? std::filesystem::path(SDL_GetBasePath()) : std::filesystem::current_path();
        const std::filesystem::path engineAssets = std::filesystem::current_path() / "assets";
        const std::filesystem::path output = settings.OutputDirectory;

#ifdef _WIN32
        const std::filesystem::path runtime = binaryDirectory / "NoxRuntime.exe";
        const std::string executableName = config.Name + ".exe";
#else
        const std::filesystem::path runtime = binaryDirectory / "NoxRuntime";
        const std::string executableName = config.Name;
#endif
        if (!std::filesystem::exists(runtime))
        {
            report.Error = "NoxRuntime is not built next to the editor (" + runtime.generic_string() +
                           "). Configure CMake with -DNOX_BUILD_RUNTIME=ON and build it first.";
            return report;
        }
        if (output.empty())
        {
            report.Error = "Choose an output folder.";
            return report;
        }
        if (IsWithin(assetDirectory, output) || IsWithin(output, assetDirectory))
        {
            report.Error = "The output folder cannot be inside the project's assets (or contain them).";
            return report;
        }
        if (config.StartScene == 0)
        {
            report.Error = "The project has no start scene (set one by saving a scene as the start scene).";
            return report;
        }

        std::error_code error;
        std::filesystem::create_directories(output, error);
        // The game's content is written fresh (a stale asset must not ship); the rest is overwritten file by file.
        std::filesystem::remove_all(output / "Game", error);
        Copier copier{ report };

        // 1. The runtime: everything next to the editor's executable but the editor, debug files and editor state (and the output
        //    folder itself, when it was chosen in there).
        const std::string assemblyName = ScriptEngine::GetProjectAssemblyName();
        copier.Tree(binaryDirectory, output, [&](const std::filesystem::path& relative, bool directory)
        {
            // Scripts/: the engine's (Core) and this project's assembly only, not another game's built next to the editor.
            if (directory && relative.parent_path() == "Scripts" && relative.filename() != "Core" && relative.filename() != assemblyName)
                return true;
            if (directory)
                return IsWithin(binaryDirectory / relative, output) || (relative.parent_path().empty() && Lower(relative.filename().string()) == "assets");
            const std::string name = Lower(relative.filename().string());
            const std::string extension = Lower(relative.extension().string());
            if (settings.PrecompiledShadersOnly && name.rfind("slang", 0) == 0 && (extension == ".dll" || extension == ".so" || extension == ".dylib"))
                return true;
            return name.rfind("noxeditor", 0) == 0 || name.rfind("noxruntime", 0) == 0 || extension == ".pdb" || extension == ".ilk" ||
                   extension == ".exp" || extension == ".lib" || extension == ".log" || name == "imgui.ini";
        });
        report.Executable = output / executableName;
        copier.File(runtime, report.Executable);

        // The project's scripts as last built (the project's own build, or the one next to the editor): Scripts/<Name>/.
        {
            std::filesystem::path core, game;
            if (ScriptEngine::FindAssemblies(core, game))
            {
                for (const char* extension : { ".dll", ".deps.json", ".runtimeconfig.json" })
                {
                    std::filesystem::path source = game;
                    source.replace_extension(extension);
                    if (std::filesystem::exists(source, error))
                        copier.File(source, output / "Scripts" / assemblyName / source.filename());
                }
            }
            else
                report.Warnings.push_back("The project's script assembly was not found (build its scripts first): scripts will not run");
        }

        // 2. The engine's assets the runtime reads (shaders are compiled at start, fonts for text, textures for defaults).
        for (const char* folder : { "shaders", "fonts", "textures" })
        {
            if (std::filesystem::exists(engineAssets / folder))
                copier.Tree(engineAssets / folder, output / "assets" / folder, [](const std::filesystem::path&, bool) { return false; });
            else
                report.Warnings.push_back(std::string("The engine's assets/") + folder + " folder was not found (is the editor running from NoxEditor/?)");
        }

        // 3. The assets the scenes need.
        std::vector<AssetHandle> roots{ config.StartScene };
        if (settings.AllScenes)
        {
            for (const auto& [handle, metadata] : assetManager->GetAssetRegistry())
            {
                if (metadata.Type == AssetType::Scene)
                    roots.push_back(handle);
            }
        }
        std::vector<std::filesystem::path> scripts;
        for (auto it = std::filesystem::recursive_directory_iterator(assetDirectory, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
        {
            if (it->is_regular_file(error) && it->path().extension() == ".cs")
                scripts.push_back(it->path());
        }
        const std::unordered_set<AssetHandle> assets = assetManager->CollectDependencies(roots, scripts);
        report.Assets = assets.size();

        const std::filesystem::path gameAssets = output / "Game" / config.AssetDirectory;
        auto copyAsset = [&](const std::filesystem::path& relative)
        {
            const std::filesystem::path source = assetDirectory / relative;
            if (std::filesystem::exists(source, error))
                copier.File(source, gameAssets / relative);
        };
        // A file and its companions next to it ("x.nsmesh.hash", "x.nanim.notifies").
        auto copyWithCompanions = [&](const std::filesystem::path& relative)
        {
            copyAsset(relative);
            const std::filesystem::path folder = (assetDirectory / relative).parent_path();
            const std::string prefix = relative.filename().string() + ".";
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
            {
                const std::string name = entry.path().filename().string();
                if (entry.is_regular_file(error) && name.size() > prefix.size() && name.rfind(prefix, 0) == 0)
                    copyAsset(relative.parent_path() / name);
            }
        };

        for (AssetHandle handle : assets)
        {
            const AssetMetadata& metadata = assetManager->GetMetadata(handle);
            switch (metadata.Type)
            {
            case AssetType::Texture2D:
            {
                // The cooked .ntex beside the source (the source stays home); a texture never cooked ships its source to cook there.
                std::filesystem::path cooked = metadata.FilePath;
                if (cooked.extension() != ".ntex")
                    cooked.replace_extension(".ntex");
                // Not cooked yet: cooked now (the game then ships the GPU-ready, block-compressed file, not the PNG/JPG).
                if (!std::filesystem::exists(assetDirectory / cooked, error) && TextureImporter::CookTexture(assetDirectory, metadata))
                    ++report.Cooked;
                if (std::filesystem::exists(assetDirectory / cooked, error))
                    copyWithCompanions(cooked);
                else
                {
                    copyAsset(metadata.SourceFilePath.empty() ? metadata.FilePath : metadata.SourceFilePath);
                    report.Warnings.push_back("Not cooked yet, shipped as source: " + metadata.FilePath.generic_string());
                }
                break;
            }
            case AssetType::MeshSource:
            {
                // A model placed as itself: the file and what it names (buffers, images).
                copyWithCompanions(metadata.FilePath);
                const std::filesystem::path folder = metadata.FilePath.parent_path();
                for (const std::string& uri : GltfUris(assetDirectory / metadata.FilePath))
                    copyAsset(folder / uri);
                break;
            }
            default:
                copyWithCompanions(metadata.FilePath);
                break;
            }
        }

        // 4. Its registry and the project file.
        assetManager->SerializeAssetRegistry(gameAssets / config.AssetRegistryPath, assets);

        // 5. Into one pack: the game opens it and reads each asset's bytes from its offset when it loads (only the table of
        //    contents stays in memory); the loose copies go.
        if (settings.PackFiles)
        {
            std::vector<std::filesystem::path> files;
            for (auto it = std::filesystem::recursive_directory_iterator(gameAssets, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            {
                if (it->is_regular_file(error))
                    files.push_back(it->path().lexically_relative(gameAssets));
            }
            std::sort(files.begin(), files.end());
            std::string packError;
            uint64_t packBytes = 0;
            if (VFS::WritePack(output / "Game" / "Content.npak", gameAssets, files, packError, &packBytes))
            {
                std::filesystem::remove_all(gameAssets, error);
                NOX_CORE_INFO("Packed {} files into Content.npak ({:.1f} MB)", files.size(), static_cast<double>(packBytes) / (1024.0 * 1024.0));
            }
            else
                report.Warnings.push_back("Could not write the pack (" + packError + "): the assets stay loose");
        }

        ProjectSerializer serializer(project);
        if (!serializer.Serialize(output / "Game" / (config.Name + ".nproj")))
            report.Warnings.push_back("Could not write the project file");

        report.Success = true;
        if (report.Cooked > 0)
            NOX_CORE_INFO("Package Game: cooked {} textures that were not cooked yet", report.Cooked);
        NOX_CORE_INFO("Packaged {}: {} assets, {} files, {:.1f} MB -> {}", config.Name, report.Assets, report.Files,
                      static_cast<double>(report.Bytes) / (1024.0 * 1024.0), output.generic_string());
        return report;
    }
}
