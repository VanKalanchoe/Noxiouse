#include "GamePackager.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <system_error>
#include <unordered_set>

#include <SDL3/SDL_filesystem.h>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/VirtualFileSystem.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Project/ProjectSerializer.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scripting/ScriptEngine.h"

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

        bool IsWithin(const std::filesystem::path& path, const std::filesystem::path& directory)
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::weakly_canonical(path, error).lexically_relative(std::filesystem::weakly_canonical(directory, error));
            return !relative.empty() && *relative.begin() != "..";
        }

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

        const std::string baseName = settings.Server ? config.Name + "Server" : config.Name;
#ifdef _WIN32
        const std::filesystem::path runtime = binaryDirectory / "NoxRuntime.exe";
        const std::string executableName = baseName + ".exe";
#else
        const std::filesystem::path runtime = binaryDirectory / "NoxRuntime";
        const std::string executableName = baseName;
#endif
        if (!settings.Server && Lower(config.Name).ends_with("server"))
            report.Warnings.push_back("The project's name ends with \"Server\": its game executable would start as a dedicated server "
                                      "(an executable named <Name>Server always does) -- rename the project");
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
        std::filesystem::remove_all(output / "Game", error);
        Copier copier{ report };

        const std::string assemblyName = ScriptEngine::GetProjectAssemblyName();
        copier.Tree(binaryDirectory, output, [&](const std::filesystem::path& relative, bool directory)
        {
            if (directory && relative.parent_path() == "Scripts" && relative.filename() != "Core" && relative.filename() != assemblyName)
                return true;
            if (directory)
                return IsWithin(binaryDirectory / relative, output) || (relative.parent_path().empty() && Lower(relative.filename().string()) == "assets") ||
                       (settings.Server && relative.parent_path().empty() &&
                        (Lower(relative.filename().string()) == "shaders" || Lower(relative.filename().string()) == "agilitysdk"));
            const std::string name = Lower(relative.filename().string());
            const std::string extension = Lower(relative.extension().string());
            if (settings.PrecompiledShadersOnly &&
                ((name.rfind("slang", 0) == 0 || name == "dxcompiler.dll" || name == "dxil.dll") &&
                 (extension == ".dll" || extension == ".so" || extension == ".dylib")))
                return true;
            // Never ship the D3D12 debug validation layer in packaged games; AgilitySDK/D3D12Core.dll is copied automatically.
            if (name == "d3d12sdklayers.dll")
                return true;
            return name.rfind("noxeditor", 0) == 0 || name.rfind("noxruntime", 0) == 0 || extension == ".pdb" || extension == ".ilk" ||
                   extension == ".exp" || extension == ".lib" || extension == ".log" || name == "imgui.ini";
        });
        report.Executable = output / executableName;
        copier.File(runtime, report.Executable);

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

        if (settings.SelfContainedDotNet)
        {
            std::vector<std::filesystem::path> roots;
            if (const char* dotnetRoot = std::getenv("DOTNET_ROOT"))
                roots.emplace_back(dotnetRoot);
#ifdef _WIN32
            if (const char* programFiles = std::getenv("ProgramFiles"))
                roots.push_back(std::filesystem::path(programFiles) / "dotnet");
#else
            roots.insert(roots.end(), { "/usr/share/dotnet", "/usr/lib/dotnet", "/usr/local/share/dotnet" });
#endif
            bool copied = false;
            for (const std::filesystem::path& root : roots)
            {
                auto newest9 = [&](const std::filesystem::path& folder)
                {
                    std::filesystem::path best;
                    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
                    {
                        const std::string name = entry.path().filename().string();
                        if (entry.is_directory(error) && name.rfind("9.", 0) == 0 && (best.empty() || name > best.filename().string()))
                            best = entry.path();
                    }
                    return best;
                };
                const std::filesystem::path fxr = newest9(root / "host/fxr");
                const std::filesystem::path framework = newest9(root / "shared/Microsoft.NETCore.App");
                if (fxr.empty() || framework.empty())
                    continue;
                auto all = [](const std::filesystem::path&, bool) { return false; };
                copier.Tree(fxr, output / "dotnet/host/fxr" / fxr.filename(), all);
                copier.Tree(framework, output / "dotnet/shared/Microsoft.NETCore.App" / framework.filename(), all);
                copied = true;
                break;
            }
            if (!copied)
                report.Warnings.push_back("Self-contained .NET: no .NET 9 runtime found (DOTNET_ROOT, Program Files/dotnet): the game needs one installed");
        }

        if (!settings.Server)
        {
            if (const Renderer* renderer = Application::Get().GetRenderer();
                renderer && renderer->GetCustomLitStatus().ShaderCount > 0 && !renderer->GetCustomLitStatus().Ready)
                report.Warnings.push_back("The project's Material Shaders do not compile (" + renderer->GetCustomLitStatus().Message +
                                          "): Custom Lit materials are lit as Default Lit in the game until they do and it is packaged again");
            if (const Renderer* renderer = Application::Get().GetRenderer(); renderer)
            {
                const Renderer::CustomLitStatus& status = renderer->GetCustomLitStatus();
                if (status.Building)
                    report.Warnings.push_back("The project's Material Shaders / material graphs are still compiling: package again once the "
                                              "editor says Built, or the game may lack them");
                if (status.GraphCount > 0 && !status.GraphsReady)
                    report.Warnings.push_back("The project's material graphs are not built (" + (status.Building ? std::string("still building") : status.Message) +
                                              "): their materials show their own values in the game until they are and it is packaged again");
                for (const auto& [path, err] : status.GraphErrors)
                    report.Warnings.push_back("Material graph " + path + " does not compile (" + err + "): its materials show their own values");
            }
        }

        for (const char* folder : { "shaders", "fonts", "textures" })
        {
            if (settings.Server)
                break;
            if (std::filesystem::exists(engineAssets / folder))
                copier.Tree(engineAssets / folder, output / "assets" / folder, [](const std::filesystem::path&, bool) { return false; });
            else
                report.Warnings.push_back(std::string("The engine's assets/") + folder + " folder was not found (is the editor running from NoxEditor/?)");
        }

        std::vector<AssetHandle> roots{ config.StartScene };
        if (const AssetHandle mixer = assetManager->FindHandleByPath(config.AudioMixer, AssetType::AudioMixer))
            roots.push_back(mixer);
        else
            report.Warnings.push_back("The project's audio mixer (" + config.AudioMixer.generic_string() + ") is not in the asset registry: the game uses the default buses");
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
        std::unordered_set<AssetHandle> assets = assetManager->CollectDependencies(roots, scripts);
        std::vector<std::filesystem::path> cellFiles;
        for (size_t found = 0;;)
        {
            cellFiles.clear();
            for (AssetHandle handle : assets)
            {
                const AssetMetadata& metadata = assetManager->GetMetadata(handle);
                if (metadata.Type != AssetType::Scene)
                    continue;
                std::filesystem::path cellDirectory;
                try
                {
                    const YAML::Node scene = YAML::LoadFile((assetDirectory / metadata.FilePath).string());
                    if (const YAML::Node world = scene["WorldPartition"]; world && world["CellDirectory"])
                        cellDirectory = world["CellDirectory"].as<std::string>();
                }
                catch (const YAML::Exception&)
                {
                    continue;
                }
                if (cellDirectory.empty())
                    continue;
                for (const auto& entry : std::filesystem::directory_iterator(assetDirectory / cellDirectory, error))
                {
                    if (entry.is_regular_file(error) && entry.path().extension() == ".noxcell")
                        cellFiles.push_back(entry.path());
                }
            }
            if (cellFiles.size() == found)
                break;
            found = cellFiles.size();
            std::vector<std::filesystem::path> tokenFiles = scripts;
            tokenFiles.insert(tokenFiles.end(), cellFiles.begin(), cellFiles.end());
            assets = assetManager->CollectDependencies(roots, tokenFiles);
        }
        if (settings.Server)
        {
            std::erase_if(assets, [&](AssetHandle handle)
            {
                const AssetType type = assetManager->GetMetadata(handle).Type;
                return type == AssetType::Texture2D || type == AssetType::Font || type == AssetType::AudioClip || type == AssetType::AcousticData;
            });
        }
        report.Assets = assets.size();

        const std::filesystem::path gameAssets = output / "Game" / config.AssetDirectory;
        auto copyAsset = [&](const std::filesystem::path& relative)
        {
            const std::filesystem::path source = assetDirectory / relative;
            if (std::filesystem::exists(source, error))
                copier.File(source, gameAssets / relative);
        };
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
                std::filesystem::path cooked = metadata.FilePath;
                if (cooked.extension() != ".ntex")
                    cooked.replace_extension(".ntex");
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

        for (const std::filesystem::path& cell : cellFiles)
            copyWithCompanions(cell.lexically_relative(assetDirectory));

        assetManager->SerializeAssetRegistry(gameAssets / config.AssetRegistryPath, assets);

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
                report.Files = report.Files - std::min(report.Files, files.size()) + 1;
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
        NOX_CORE_INFO("Packaged {}: {} assets, {} files, {:.1f} MB -> {}", baseName, report.Assets, report.Files,
                      static_cast<double>(report.Bytes) / (1024.0 * 1024.0), output.generic_string());
        return report;
    }
}
