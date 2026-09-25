#include "EditorAssetManager.h"

#include <entt/entt.hpp>

#include "AssetManager.h"

#include <algorithm>
#include "AssetImporter.h"
#include "MeshImporter.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Core/Application.h"
#include "Material.h"
#include "MaterialSerializer.h"

#include <fstream>
#include <cstring>
#include <cctype>
#include <yaml-cpp/yaml.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"

namespace Nox
{
    static std::string_view ImageFormatToString(NRI::ImageFormat format)
    {
        switch (format)
        {
        case NRI::ImageFormat::RGBA8:              return "RGBA8";
        case NRI::ImageFormat::SRGBA8:             return "SRGBA8";
        case NRI::ImageFormat::RGB8:               return "RGB8";
        case NRI::ImageFormat::SRGB8:              return "SRGB8";
        case NRI::ImageFormat::R16G16_SFLOAT:      return "R16G16_SFLOAT";
        case NRI::ImageFormat::R16G16B16A16_SFLOAT:return "R16G16B16A16_SFLOAT";
        case NRI::ImageFormat::BC1_UNorm:          return "BC1_UNorm";
        case NRI::ImageFormat::BC1_UNorm_SRGB:     return "BC1_UNorm_SRGB";
        case NRI::ImageFormat::BC2_UNorm:          return "BC2_UNorm";
        case NRI::ImageFormat::BC2_UNorm_SRGB:     return "BC2_UNorm_SRGB";
        case NRI::ImageFormat::BC3_UNorm:          return "BC3_UNorm";
        case NRI::ImageFormat::BC3_UNorm_SRGB:     return "BC3_UNorm_SRGB";
        case NRI::ImageFormat::BC4_UNorm:          return "BC4_UNorm";
        case NRI::ImageFormat::BC4_SNorm:          return "BC4_SNorm";
        case NRI::ImageFormat::BC5_UNorm:          return "BC5_UNorm";
        case NRI::ImageFormat::BC5_SNorm:          return "BC5_SNorm";
        case NRI::ImageFormat::BC6H_UF16:          return "BC6H_UF16";
        case NRI::ImageFormat::BC6H_SF16:          return "BC6H_SF16";
        case NRI::ImageFormat::BC7_UNorm:          return "BC7_UNorm";
        case NRI::ImageFormat::BC7_UNorm_SRGB:     return "BC7_UNorm_SRGB";
        default:                                   return "RGBA8";
        }
    }

    static NRI::ImageFormat ImageFormatFromString(std::string_view str)
    {
        if (str == "SRGBA8")             return NRI::ImageFormat::SRGBA8;
        if (str == "RGBA8")              return NRI::ImageFormat::RGBA8;
        if (str == "SRGB8")              return NRI::ImageFormat::SRGB8;
        if (str == "RGB8")               return NRI::ImageFormat::RGB8;
        if (str == "R16G16_SFLOAT")      return NRI::ImageFormat::R16G16_SFLOAT;
        if (str == "R16G16B16A16_SFLOAT")return NRI::ImageFormat::R16G16B16A16_SFLOAT;
        if (str == "BC1_UNorm")          return NRI::ImageFormat::BC1_UNorm;
        if (str == "BC1_UNorm_SRGB")     return NRI::ImageFormat::BC1_UNorm_SRGB;
        if (str == "BC2_UNorm")          return NRI::ImageFormat::BC2_UNorm;
        if (str == "BC2_UNorm_SRGB")     return NRI::ImageFormat::BC2_UNorm_SRGB;
        if (str == "BC3_UNorm")          return NRI::ImageFormat::BC3_UNorm;
        if (str == "BC3_UNorm_SRGB")     return NRI::ImageFormat::BC3_UNorm_SRGB;
        if (str == "BC4_UNorm")          return NRI::ImageFormat::BC4_UNorm;
        if (str == "BC4_SNorm")          return NRI::ImageFormat::BC4_SNorm;
        if (str == "BC5_UNorm")          return NRI::ImageFormat::BC5_UNorm;
        if (str == "BC5_SNorm")          return NRI::ImageFormat::BC5_SNorm;
        if (str == "BC6H_UF16")          return NRI::ImageFormat::BC6H_UF16;
        if (str == "BC6H_SF16")          return NRI::ImageFormat::BC6H_SF16;
        if (str == "BC7_UNorm")          return NRI::ImageFormat::BC7_UNorm;
        if (str == "BC7_UNorm_SRGB")     return NRI::ImageFormat::BC7_UNorm_SRGB;
        return NRI::ImageFormat::RGBA8;
    }

    static std::filesystem::path GeneratedAssetPath(
        const std::filesystem::path& sourcePath,
        const char* folder,
        const char* extension)
    {
        auto parent = sourcePath.parent_path();
        std::string parentFolder = parent.filename().string();
        std::string expectedFolder = folder;
        std::transform(parentFolder.begin(), parentFolder.end(), parentFolder.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        std::transform(expectedFolder.begin(), expectedFolder.end(), expectedFolder.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        if (parentFolder == expectedFolder)
            return parent / (sourcePath.stem().string() + extension);
        return parent / folder / (sourcePath.stem().string() + extension);
    }

    static std::map<std::filesystem::path, AssetType> s_AssetExtensionMap = 
    {
        { ".nox", AssetType::Scene },
        { ".png", AssetType::Texture2D },
        { ".jpg", AssetType::Texture2D },
        { ".jpeg", AssetType::Texture2D },
        { ".ktx2", AssetType::Texture2D },
        { ".ntex", AssetType::Texture2D },
        { ".gltf", AssetType::MeshSource },
        { ".glb", AssetType::MeshSource },
        { ".nmesh", AssetType::Mesh },
        { ".nsmesh", AssetType::StaticMesh },
        { ".nmat", AssetType::Material },
        
        { ".nskel",   AssetType::Skeleton },
        { ".nanim",   AssetType::AnimationSequence },
        { ".nskmesh", AssetType::SkeletalMesh },
        { ".nanimgraph", AssetType::AnimationGraph }
    };

    AssetType EditorAssetManager::GetAssetTypeFromExtension(const std::filesystem::path& extension)
    {
        if (s_AssetExtensionMap.find(extension) == s_AssetExtensionMap.end())
        {
            NOX_CORE_WARN("Could not find AssetType for {}", extension.string());
            return AssetType::None;
        }

        return s_AssetExtensionMap.at(extension);
    }
    
    /*YAML::Emitter& operator<<(YAML::Emitter& out, const std::string_view& v)
    {
        out << std::string(v.data(), v.size());
        return out;
    }*/
    
    bool EditorAssetManager::IsAssetHandleValid(AssetHandle handle) const
    {
        return handle != 0 && m_AssetRegistry.find(handle) != m_AssetRegistry.end();
    }

    Asset* EditorAssetManager::FindLoadedAsset(AssetHandle handle) const
    {
        auto found = m_LoadedAssets.find(handle);
        return found != m_LoadedAssets.end() ? found->second.get() : nullptr;
    }

    bool EditorAssetManager::IsAssetLoaded(AssetHandle handle) const
    {
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    AssetType EditorAssetManager::GetAssetType(AssetHandle handle) const
    {
        if (!IsAssetHandleValid(handle))
            return AssetType::None;

        return m_AssetRegistry.at(handle).Type;
    }

    void EditorAssetManager::Init()
    {
        m_AssetWatcher.watch(Project::GetActiveAssetDirectory(), [this](const std::filesystem::path& path) 
        {
            OnAssetModifiedOnDisk(path);
        });
    }

    void EditorAssetManager::Update()
    {
        std::vector<std::filesystem::path> modifiedPaths;
        {
            std::lock_guard<std::mutex> lock(m_ReimportMutex);
            modifiedPaths.swap(m_PendingModifiedPaths);
        }

        // Resolved here and not on the watcher thread: the registry is only ever touched by the main thread.
        const std::filesystem::path assetDir = Project::GetActiveAssetDirectory();
        std::set<AssetHandle> toReimport;
        for (const std::filesystem::path& absolutePath : modifiedPaths)
        {
            const std::filesystem::path relativePath = absolutePath.lexically_relative(assetDir).lexically_normal();
            for (const auto& [handle, metadata] : m_AssetRegistry)
            {
                if (metadata.SourceFilePath.lexically_normal() == relativePath)
                {
                    toReimport.insert(handle);
                    break;
                }
            }
        }

        // Now we are on the MAIN THREAD, we can safely invoke the importer and Vulkan code
        for (AssetHandle handle : toReimport)
        {
            ReimportAsset(handle);
        }

        PublishLoadedAssets();
        PublishSplitImports();

        if (m_RegistryDirty && std::chrono::steady_clock::now() - m_RegistryWritten >= std::chrono::seconds(1))
        {
            SerializeAssetRegistry();
            m_RegistryDirty = false;
            m_RegistryWritten = std::chrono::steady_clock::now();
        }
    }

    void EditorAssetManager::PublishLoadedAssets()
    {
        NOX_PROFILE_SCOPE("Publish Loaded Assets");
        std::vector<LoadedAsset> finished;
        uint64_t uploadBudget = AssetLoader::UploadBytesPerFrame;
        m_Loader.Update(uploadBudget, finished);

        bool texturesLoaded = false;
        for (LoadedAsset& loaded : finished)
        {
            const AssetMetadata& metadata = m_AssetRegistry.at(loaded.Handle);
            // However it ends, materials that asked for the texture while it loaded drew without it: re-pack them.
            texturesLoaded |= metadata.Type == AssetType::Texture2D;
            if (loaded.NeedsImport)
            {
                // No current cooked data: cooked and loaded here, synchronously (cooking moves to workers in 5c).
                GetAsset(loaded.Handle);
                continue;
            }
            if (!loaded.Loaded)
            {
                NOX_CORE_ERROR("EditorAssetManager: loading {} failed", metadata.FilePath.generic_string());
                m_FailedAssets.insert(loaded.Handle);
                continue;
            }
            if (IsAssetLoaded(loaded.Handle))
            {
                // Loaded synchronously meanwhile (GetAsset): that copy stays, this one goes once frames are done with it.
                Renderer::DeferAssetRelease(std::move(loaded.Loaded));
                continue;
            }

            loaded.Loaded->Handle = loaded.Handle;
            m_LoadedAssets[loaded.Handle] = loaded.Loaded;
            if (loaded.SourceHash)
                m_LastKnownSourceHash[loaded.Handle] = *loaded.SourceHash;
            if (loaded.Streamed)
                m_Streamer.Register(loaded.Handle, *loaded.Streamed, loaded.StreamedFirstMip,
                                    static_cast<Texture2D*>(loaded.Loaded.get())->GetDescriptorIndexSlot());

            if (metadata.Type == AssetType::Mesh || metadata.Type == AssetType::StaticMesh || metadata.Type == AssetType::MeshSource)
            {
                NOX_PROFILE_SCOPE("Register Model Assets");
                ImportMeshTextures(loaded.Loaded);
                ImportMeshMaterials(loaded.Loaded, metadata);
                // New files appear next to a model only when it is cooked (skeleton, clips, extracted textures).
                if (loaded.Cooked)
                    ScanAndRegisterNewAssets(metadata.FilePath.parent_path().parent_path());
                LinkImportedAssets(loaded.Loaded, metadata);
            }

            NOX_CORE_INFO("[AssetLoad] {} ({}) streamed in {:.1f} ms", metadata.FilePath.generic_string(), AssetTypeToString(metadata.Type),
                          loaded.Milliseconds);
        }

        if (texturesLoaded)
            Renderer::MarkTexturesLoaded();

        // Textures that moved to a new image (more or fewer mips): swapped in like a reload -- materials resolve to the
        // new image's slot, the old image leaves once frames in flight are done with it.
        std::vector<std::pair<AssetHandle, Ref<Texture2D>>> streamed;
        m_Streamer.Update(*Application::Get().GetRenderer(), uploadBudget, streamed);
        std::vector<uint32_t> replacedSlots;
        for (auto& [handle, texture] : streamed)
        {
            auto loaded = m_LoadedAssets.find(handle);
            if (loaded == m_LoadedAssets.end())
            {
                Renderer::DeferAssetRelease(Ref<Asset>(texture));
                continue;
            }
            replacedSlots.push_back(static_cast<Texture2D*>(loaded->second.get())->GetDescriptorIndexSlot());
            Renderer::DeferAssetRelease(std::move(loaded->second));
            texture->Handle = handle;
            loaded->second = Ref<Asset>(texture);
        }
        Renderer::InvalidateTextureDescriptorSlots(replacedSlots);
    }
    
    void EditorAssetManager::ReimportAsset(AssetHandle handle, bool force)
    {
        if (!IsAssetHandleValid(handle)) return;

        const AssetMetadata& metadata = GetMetadata(handle);
        auto sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
        if (!std::filesystem::exists(sourcePath))
        {
            NOX_CORE_WARN("Skipping auto-reimport: source file no longer exists: {}", sourcePath.string());
            return;
        }
        // A reimport refreshes what is loaded. An asset not loaded yet (or still loading) reads its source when it loads,
        // and its cook is checked against the source then: a watcher event for it changes nothing (the files of a model
        // copied in and imported at once, the .nmat files its cook writes), and reimporting it here cooked it a second
        // time on the main thread next to its background load. One that failed to load is tried again on its next request.
        m_FailedAssets.erase(handle);
        
        if (!m_LoadedAssets.contains(handle))
            return;

        m_Streamer.Unregister(handle);

        // The file watcher can fire on this source file even though its content never actually
        // changed -- e.g. our own cooker rewrites extracted embedded textures unconditionally on
        // every recook, and that write is picked up by the same recursive watch. Verify against the
        // last known content hash before doing anything destructive; a spurious event is a no-op.
        XXH128_hash_t currentHash = Utility::calcul_hash_streaming(sourcePath.string());
        auto knownHashIt = m_LastKnownSourceHash.find(handle);
        if (!force && knownHashIt != m_LastKnownSourceHash.end() && XXH128_isEqual(currentHash, knownHashIt->second))
        {
            NOX_CORE_INFO("Skipping auto-reimport, source content unchanged: {}", metadata.SourceFilePath.string());
            return;
        }
        m_LastKnownSourceHash[handle] = currentHash;

        NOX_CORE_INFO("Auto-Reimporting asset from source: {}", metadata.SourceFilePath.string());

        // 1. Delete the old cooked cache (.nsmesh/.nmesh) so the Importer is forced to re-cook the GLTF
        if (metadata.Type == AssetType::Mesh || metadata.Type == AssetType::StaticMesh ||
            metadata.Type == AssetType::MeshSource || metadata.Type == AssetType::SkeletalMesh)
        {
            auto cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
            auto ext = cookedPath.extension();
            if ((ext == ".nsmesh" || ext == ".nmesh") && std::filesystem::exists(cookedPath))
            {
                std::filesystem::remove(cookedPath);
                if (std::filesystem::exists(cookedPath.string() + ".hash"))
                    std::filesystem::remove(cookedPath.string() + ".hash");
            }
        }
        else if (metadata.Type == AssetType::Texture2D)
        {
            auto cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
            if (cookedPath.extension() == ".ntex" && std::filesystem::exists(cookedPath))
            {
                std::filesystem::remove(cookedPath);
                if (std::filesystem::exists(cookedPath.string() + ".hash"))
                    std::filesystem::remove(cookedPath.string() + ".hash");
            }
        }

        // 2. Re-run the importer on the GLTF
        Ref<Asset> reimportedAsset = AssetImporter::ImportAsset(handle, metadata);
        
        // 3. Overwrite the loaded asset. (If your ECS holds a Ref<Asset> to this, 
        // it will automatically update in the viewport!)
        if (reimportedAsset)
        {
            reimportedAsset->Handle = handle;
            m_LoadedAssets[handle] = reimportedAsset;

            if (metadata.Type == AssetType::Mesh ||
                metadata.Type == AssetType::StaticMesh ||
                metadata.Type == AssetType::MeshSource)
            {
                ImportMeshTextures(reimportedAsset);
                ImportMeshMaterials(reimportedAsset, metadata);
            }
        }

        // 4. Scan and register any new .nanim / .nskel files generated during cooking (they land
        // next to the model: <Model>/Meshes, <Model>/Materials, <Model>/Textures).
        ScanAndRegisterNewAssets(metadata.FilePath.parent_path().parent_path());
    }

    void EditorAssetManager::OnAssetModifiedOnDisk(const std::filesystem::path& absolutePath)
    {
        // Runs on the watcher thread: only queue the path, Update() matches it against the registry.
        if (absolutePath.extension() == ".nsmesh" || absolutePath.extension() == ".nmesh")
            return;

        std::lock_guard<std::mutex> lock(m_ReimportMutex);
        m_PendingModifiedPaths.push_back(absolutePath);
    }

    void EditorAssetManager::ImportAsset(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, AssetType targetType, const MeshImportSettings& meshSettings)
    {
        AssetMetadata metadata;
        metadata.FilePath = destPath.empty() ? sourcePath : destPath;
        metadata.SourceFilePath = sourcePath;
        metadata.MeshSettings = meshSettings;

        // If a target type was provided (e.g. from a UI menu), use it.
        // Otherwise, fall back to whatever the file extension is.
        metadata.Type = (targetType != AssetType::None) ? targetType : GetAssetTypeFromExtension(sourcePath.extension());
        NOX_CORE_ASSERT(metadata.Type != AssetType::None, "could not determine asset type from extension");

        if (destPath.empty() && metadata.Type == AssetType::MeshSource)
            metadata.FilePath = GeneratedAssetPath(sourcePath, "Meshes", ".nmesh");

        if (metadata.Type == AssetType::Texture2D && destPath.empty() &&
            sourcePath.extension() != ".ntex" && sourcePath.extension() != ".ktx2")
        {
            metadata.FilePath = GeneratedAssetPath(sourcePath, "Textures", ".ntex");
        }

        // Registered now; cooked and loaded in the background (a model's textures, materials, skeleton and clips are
        // registered when it is published).
        const AssetHandle handle = RegisterAsset(metadata);
        SerializeAssetRegistry();
        if (AssetLoader::IsStreamable(metadata.Type))
            RequestAsset(handle);
    }

    EditorAssetManager::ImportProgress EditorAssetManager::GetImportProgress() const
    {
        ImportProgress progress;
        for (const PendingSplitImport& pending : m_PendingSplitImports)
        {
            ++progress.Imports;
            progress.Total += pending.Total->load();
            progress.Done += pending.Done->load();
        }
        return progress;
    }

    void EditorAssetManager::PublishSplitImports()
    {
        bool registryChanged = false;
        for (auto it = m_PendingSplitImports.begin(); it != m_PendingSplitImports.end();)
        {
            if (!it->Job.IsReady())
            {
                ++it;
                continue;
            }

            const std::vector<MeshImporter::SplitMesh> meshes = it->Job.Get();
            std::unordered_map<int32_t, AssetHandle> handleOfMesh;
            for (const MeshImporter::SplitMesh& mesh : meshes)
            {
                AssetMetadata metadata;
                metadata.Type = it->Skinned ? AssetType::Mesh : AssetType::StaticMesh;
                metadata.FilePath = mesh.FilePath;
                metadata.SourceFilePath = it->MaterialBase.SourceFilePath;
                metadata.MeshSettings.ImportStaticMeshes = !it->Skinned;
                metadata.MeshSettings.ImportSkeletalMeshes = it->Skinned;
                metadata.MeshSettings.ImportAnimations = false;
                metadata.MeshSettings.SourceMeshIndex = mesh.MeshIndex;
                metadata.MeshSettings.MaterialBasePath = it->MaterialBase.FilePath;
                metadata.MeshSettings.ImportScale = it->MaterialBase.MeshSettings.ImportScale;
                handleOfMesh[mesh.MeshIndex] = RegisterAsset(metadata);
                registryChanged = true;
            }
            // The job also wrote the shared .nmat files and the clips (they are not registered by loading a mesh that was
            // cooked here).
            ScanAndRegisterNewAssets(it->MaterialBase.FilePath.parent_path().parent_path());
            if (it->Group)
            {
                LevelGroup& group = *it->Group;
                ModelInstance::LevelDescription& level = group.Level;
                if (!group.HasStructure && it->Level)
                {
                    group.HasStructure = true;
                    level.Name = it->MaterialBase.SourceFilePath.stem().string();
                    level.Scale = it->MaterialBase.MeshSettings.ImportScale;
                    level.Nodes = it->Level->Nodes;
                    level.Lights = it->Level->Lights;
                    level.Cameras = it->Level->Cameras;
                }
                if (it->SkeletalAsset != 0)
                    level.SkeletalAsset = it->SkeletalAsset;
                // Static and skinned meshes are entries of one glTF mesh list, so one map holds both kinds.
                for (const MeshImporter::SplitMesh& mesh : meshes)
                {
                    auto handle = handleOfMesh.find(mesh.MeshIndex);
                    if (handle != handleOfMesh.end())
                        level.MeshAssets[mesh.MeshIndex] = handle->second;
                }
                if (it->Level)
                {
                    for (const MeshImporter::SplitLevel::Clip& clip : it->Level->Clips)
                    {
                        const AssetHandle handle = FindHandleByPath(clip.FilePath, AssetType::AnimationSequence);
                        if (handle != 0)
                            level.Clips.push_back({ handle, clip.Nodes });
                    }
                }
                if (--group.Remaining == 0)
                    m_FinishedLevelImports.push_back(std::move(level));
            }

            NOX_CORE_INFO("[EditorAssetManager] Registered {} {} mesh assets cooked from {}", meshes.size(), it->Skinned ? "skeletal" : "static", it->MaterialBase.SourceFilePath.generic_string());
            it = m_PendingSplitImports.erase(it);
        }
        if (registryChanged)
            m_RegistryDirty = true; // written by Update(), not per load
    }

    bool EditorAssetManager::ImportModel(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, const MeshImportSettings& settings)
    {
        const MeshImporter::GltfContent content = MeshImporter::InspectGltf(Project::GetActiveAssetDirectory() / sourcePath);
        // Geometry Only: skinned meshes count as static ones (and there is no skeleton).
        const bool skinnedAsStatic = !settings.ImportSkinWeights && settings.ImportSkeletalMeshes && content.HasSkinnedMeshes;
        const bool skeletal = settings.ImportSkeletalMeshes && settings.ImportSkinWeights && content.HasSkinnedMeshes;
        // Import Into Level places every mesh node, so it needs the static meshes as per-mesh assets whatever the toggles say.
        const bool intoLevel = settings.ImportIntoLevel;
        const bool statics = (settings.ImportStaticMeshes && content.HasStaticMeshes) || skinnedAsStatic || (intoLevel && content.HasStaticMeshes);
        if (!skeletal && !statics)
        {
            NOX_CORE_WARN("[EditorAssetManager] Nothing to import from {} with these settings (skinned meshes: {}, static meshes: {})",
                          sourcePath.generic_string(), content.HasSkinnedMeshes, content.HasStaticMeshes);
            return false;
        }

        auto registerAsset = [&](AssetType type, const char* extension, const MeshImportSettings& assetSettings) -> AssetHandle
        {
            AssetMetadata metadata;
            metadata.Type = type;
            metadata.FilePath = destPath;
            metadata.FilePath.replace_extension(extension);
            metadata.SourceFilePath = sourcePath;
            metadata.MeshSettings = assetSettings;
            const AssetHandle handle = RegisterAsset(metadata);
            SerializeAssetRegistry();
            RequestAsset(handle);
            return handle;
        };

        // Do Not Combine (Unreal): every mesh of that kind is an asset of its own -- import the meshes, drag them in. No
        // whole-file asset. Cooked in one background job, registered when it is done. `materialBase` only names the shared
        // .nmat files (and, for skinned meshes, the skeleton and clips written next to them). With `intoLevel` the job also
        // hands back the file's node structure, and the finished import places it in the level (ConsumeLevelImports).
        auto startSplit = [&](AssetType type, const char* extension, const MeshImportSettings& assetSettings, bool skinnedMeshes,
                              const std::shared_ptr<LevelGroup>& levelGroup, AssetHandle skeletalAsset)
        {
            AssetMetadata materialBase;
            materialBase.Type = type;
            materialBase.FilePath = destPath;
            materialBase.FilePath.replace_extension(extension);
            materialBase.SourceFilePath = sourcePath;
            materialBase.MeshSettings = assetSettings;
            const std::filesystem::path directory = destPath.parent_path();
            auto total = std::make_shared<std::atomic<uint32_t>>(0);
            auto done = std::make_shared<std::atomic<uint32_t>>(0);
            auto level = std::make_shared<MeshImporter::SplitLevel>();
            const bool placeInLevel = levelGroup != nullptr;
            if (levelGroup)
                ++levelGroup->Remaining;
            PendingSplitImport pending;
            pending.Job = JobSystem::Get().Async("Cook Split Meshes",
                [assetDirectory = Project::GetActiveAssetDirectory(), materialBase, directory, skinnedMeshes, total, done, level, placeInLevel](const CancellationToken&)
                {
                    return MeshImporter::CookSplitMeshes(assetDirectory, materialBase, directory, skinnedMeshes, total.get(), done.get(),
                                                         placeInLevel ? level.get() : nullptr);
                });
            pending.MaterialBase = materialBase;
            pending.Total = total;
            pending.Done = done;
            pending.Skinned = skinnedMeshes;
            pending.Group = levelGroup;
            pending.Level = level;
            pending.SkeletalAsset = skeletalAsset;
            m_PendingSplitImports.push_back(std::move(pending));
        };

        // Clips are cooked by one of the two, so recooking never has both write the same files.
        const auto levelGroup = intoLevel ? std::make_shared<LevelGroup>() : std::shared_ptr<LevelGroup>();
        AssetHandle skeletalAsset = 0;
        if (skeletal)
        {
            MeshImportSettings skeletalSettings = settings;
            skeletalSettings.ImportStaticMeshes = false;
            // One skinned mesh is just the asset itself; several become one asset each, sharing the skeleton and clips.
            if (settings.SkeletalCombine == MeshCombineMode::DoNotCombine && content.SkinnedMeshCount >= 2)
                startSplit(AssetType::Mesh, ".nmesh", skeletalSettings, true, levelGroup, 0);
            else
                skeletalAsset = registerAsset(AssetType::Mesh, ".nmesh", skeletalSettings);
        }
        // A file with only skinned meshes still needs the level job (the file's nodes, lights and cameras) when it is placed
        // as a level and the whole-file skeletal asset is what the skinned nodes use.
        const bool levelOnly = intoLevel && !statics && skeletalAsset != 0;
        if (statics || levelOnly)
        {
            MeshImportSettings staticSettings = settings;
            staticSettings.ImportSkeletalMeshes = false;
            if (skinnedAsStatic)
                staticSettings.ImportStaticMeshes = true; // the skinned meshes are what it holds
            staticSettings.ImportAnimations = settings.ImportAnimations && !skeletal;

            if (settings.StaticCombine != MeshCombineMode::DoNotCombine && !intoLevel)
                registerAsset(AssetType::StaticMesh, ".nsmesh", staticSettings);
            else
                startSplit(AssetType::StaticMesh, ".nsmesh", staticSettings, false, levelGroup, skeletalAsset);
        }
        return true;
    }

    void EditorAssetManager::ImportMeshTextures(const Ref<Asset>& meshAsset)
    {
        const std::vector<MaterialData>* materials = nullptr;
        if (meshAsset->GetType() == AssetType::Mesh)
            materials = &static_cast<Mesh*>(meshAsset.get())->GetMaterials();
        else if (meshAsset->GetType() == AssetType::StaticMesh)
            materials = &static_cast<StaticMesh*>(meshAsset.get())->GetMaterials();

        if (!materials)
            return;

        // Bistro: 254 materials x 6 slots = 1524 paths, all usually already imported. Checking each
        // with std::filesystem::relative + a linear registry scan measured 2.1s per load. Index the
        // registry once, resolve paths lexically, and handle each distinct path only once.
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        // (Registered textures are found through m_HandleByPath, which every registration keeps current.)
        std::unordered_set<std::string> processedPaths;
        std::vector<AssetHandle> texturesNeedingRecook;
        bool registryChanged = false;

        auto importTexture = [&](const std::string& texturePath, bool sRGB)
        {
            if (texturePath.empty() || !processedPaths.insert(texturePath).second)
                return;

            std::filesystem::path sourcePath(texturePath);
            std::filesystem::path relativePath;
            if (sourcePath.is_absolute())
            {
                relativePath = sourcePath.lexically_normal().lexically_relative(assetDirectory.lexically_normal());
                if (relativePath.empty() || *relativePath.begin() == "..")
                {
                    std::error_code ec;
                    relativePath = std::filesystem::relative(sourcePath, assetDirectory, ec);
                    if (ec)
                        return;
                }
            }
            else
            {
                relativePath = sourcePath;
            }

            std::filesystem::path cookedPath = GeneratedAssetPath(relativePath, "Textures", ".ntex");

            AssetHandle registered = FindHandleByPath(cookedPath, AssetType::Texture2D);
            if (registered == 0)
                registered = FindHandleByPath(relativePath, AssetType::Texture2D);
            if (registered != 0)
            {
                AssetMetadata& metadata = m_AssetRegistry.at(registered);
                const NRI::ImageFormat expectedFormat = sRGB ? NRI::ImageFormat::SRGBA8 : NRI::ImageFormat::RGBA8;
                if (metadata.TextureSpec.format != expectedFormat)
                {
                    metadata.TextureSpec.format = expectedFormat;
                    texturesNeedingRecook.push_back(registered);
                    registryChanged = true;
                }
                return;
            }

            std::filesystem::path fullSourcePath =
                Project::GetActiveAssetDirectory() / relativePath;
            if (!std::filesystem::exists(fullSourcePath))
            {
                // Donut/RTXPT searches for a same-name DDS whenever an uncompressed glTF image is absent.
                // Do this here too so already-cooked meshes/materials repair themselves when linked, without
                // requiring hand edits to the registry or every existing .nmat file.
                std::filesystem::path ddsRelativePath = relativePath;
                ddsRelativePath.replace_extension(".dds");
                const std::filesystem::path ddsSourcePath = Project::GetActiveAssetDirectory() / ddsRelativePath;
                if (!std::filesystem::exists(ddsSourcePath))
                    return;

                relativePath = std::move(ddsRelativePath);
                fullSourcePath = ddsSourcePath;
                cookedPath = GeneratedAssetPath(relativePath, "Textures", ".ntex");
            }

            // Registered only: it is cooked and loaded in the background when a material first requests it.
            TextureSpecification spec;
            spec.format = sRGB ? NRI::ImageFormat::SRGBA8 : NRI::ImageFormat::RGBA8;
            const AssetHandle newHandle = RegisterAsset(TextureMetadata(relativePath, spec, {}));
            registryChanged = true;

        };

        for (const MaterialData& material : *materials)
        {
            importTexture(material.BaseColorTexturePath, true);
            // In the legacy specular-glossiness workflow this slot contains sRGB specular color
            // in RGB and linear glossiness in alpha. An sRGB image view decodes RGB only, which is
            // exactly what KHR_materials_pbrSpecularGlossiness requires.
            importTexture(material.MetallicRoughnessTexturePath, material.Workflow == 1.0f);
            importTexture(material.NormalTexturePath, false);
            importTexture(material.OcclusionTexturePath, false);
            importTexture(material.EmissiveTexturePath, true);
            importTexture(material.TransmissionTexturePath, false);
        }

        if (registryChanged)
            m_RegistryDirty = true; // written by Update(), not per load

        // Import settings are part of the cooked texture. A content hash cannot detect a color-space
        // correction, so explicitly force a recook for already loaded textures whose role changed.
        for (AssetHandle handle : texturesNeedingRecook)
        {
            if (m_LoadedAssets.contains(handle))
            {
                ReimportAsset(handle, true);
                continue;
            }

            // An unloaded texture will be cooked on demand. Remove only its derived cache so the
            // corrected registry format is used when that happens; the source asset is untouched.
            const AssetMetadata& metadata = m_AssetRegistry.at(handle);
            const std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
            std::error_code ec;
            std::filesystem::remove(cookedPath, ec);
            std::filesystem::remove(cookedPath.string() + ".hash", ec);
        }
    }

    void EditorAssetManager::ImportMeshMaterials(const Ref<Asset>& meshAsset, const AssetMetadata& meshMetadata)
    {
        const std::vector<MaterialData>* materials = nullptr;
        std::vector<AssetHandle> materialAssets;

        if (meshAsset->GetType() == AssetType::Mesh)
        {
            auto* mesh = static_cast<Mesh*>(meshAsset.get());
            materials = &mesh->GetMaterials();
        }
        else if (meshAsset->GetType() == AssetType::StaticMesh)
        {
            auto* mesh = static_cast<StaticMesh*>(meshAsset.get());
            materials = &mesh->GetMaterials();
        }

        if (!materials)
            return;

        materialAssets.reserve(materials->size());

        // Materials come per primitive (Bistro: 2909 entries, 254 unique), so the same .nmat repeats many
        // times; a material is found through m_HandleByPath (kept current by every registration).

        bool registryChanged = false;
        // Per primitive, but each .nmat is checked on disk once (a check per entry cost Bistro ~2909 file system calls).
        std::unordered_set<std::string> checkedPaths;
        for (size_t index = 0; index < materials->size(); ++index)
        {
            const MaterialData& material = (*materials)[index];
            const std::filesystem::path materialPath = MeshImporter::MaterialAssetPath(meshMetadata.MeshSettings.MaterialBasePath.empty() ? meshMetadata.FilePath : meshMetadata.MeshSettings.MaterialBasePath, material, index);

            const std::string materialKey = materialPath.lexically_normal().generic_string();
            AssetHandle materialHandle = FindHandleByPath(materialPath, AssetType::Material);

            const auto fullMaterialPath = Project::GetActiveAssetDirectory() / materialPath;
            if (materialHandle == 0 || (checkedPaths.insert(materialKey).second && !std::filesystem::exists(fullMaterialPath)))
            {
                // The cook writes the .nmat files (MeshImporter::CookMesh); one deleted since is written again here.
                if (!std::filesystem::exists(fullMaterialPath) && !MaterialSerializer::Serialize(fullMaterialPath, material))
                    continue;

                if (materialHandle != 0)
                {
                    ReimportAsset(materialHandle);
                }
                else
                {
                    // Registered only: entities request (and load) the materials they draw with.
                    AssetMetadata metadata;
                    metadata.FilePath = materialPath;
                    metadata.SourceFilePath = materialPath;
                    metadata.Type = AssetType::Material;
                    materialHandle = RegisterAsset(metadata);
                    registryChanged = true;
                }
            }

            if (materialHandle != 0)
                materialAssets.push_back(materialHandle);
        }

        if (registryChanged)
            m_RegistryDirty = true; // written by Update(), not per load

        if (meshAsset->GetType() == AssetType::Mesh)
            static_cast<Mesh*>(meshAsset.get())->SetMaterialAssets(std::move(materialAssets));
        else
            static_cast<StaticMesh*>(meshAsset.get())->SetMaterialAssets(std::move(materialAssets));
    }
    
    void EditorAssetManager::ImportAsset(const std::filesystem::path& sourcePath, const TextureSpecification& spec, const std::filesystem::path& destPath)
    {
        // Registered only: the texture is cooked and loaded in the background when something first requests it.
        RegisterAsset(TextureMetadata(sourcePath, spec, destPath));
        SerializeAssetRegistry();
    }

    AssetMetadata EditorAssetManager::TextureMetadata(const std::filesystem::path& sourcePath, const TextureSpecification& spec, const std::filesystem::path& destPath)
    {
        AssetMetadata metadata;
        if (!destPath.empty())
            metadata.FilePath = destPath;
        else if (sourcePath.extension() == ".ntex" || sourcePath.extension() == ".ktx2")
            metadata.FilePath = sourcePath;
        else
            metadata.FilePath = GeneratedAssetPath(sourcePath, "Textures", ".ntex");
        metadata.SourceFilePath = sourcePath;
        metadata.Type = AssetType::Texture2D;
        metadata.TextureSpec = spec;
        return metadata;
    }

    void EditorAssetManager::IndexPath(AssetHandle handle, const AssetMetadata& metadata)
    {
        if (!metadata.FilePath.empty())
            m_HandleByPath[metadata.FilePath.lexically_normal().generic_string()] = handle;
        if (!metadata.SourceFilePath.empty())
            m_HandleByPath[metadata.SourceFilePath.lexically_normal().generic_string()] = handle;
    }

    AssetHandle EditorAssetManager::FindHandleByPath(const std::filesystem::path& path, AssetType type) const
    {
        const auto found = m_HandleByPath.find(path.lexically_normal().generic_string());
        if (found == m_HandleByPath.end())
            return 0;
        const auto entry = m_AssetRegistry.find(found->second);
        return entry != m_AssetRegistry.end() && entry->second.Type == type ? found->second : AssetHandle(0);
    }

    AssetHandle EditorAssetManager::RegisterAsset(const AssetMetadata& metadata)
    {
        AssetHandle handle;
        m_AssetRegistry[handle] = metadata;
        IndexPath(handle, metadata);
        return handle;
    }

    void EditorAssetManager::LinkImportedAssets(const Ref<Asset>& meshAsset, const AssetMetadata& meshMetadata)
    {
        // Cooked next to the model: <Model>/Meshes/<model>.nskel and <model>_<clip>.nanim (sorted by path, so the default
        // clip does not depend on the registry's order).
        // Per-mesh assets (Do Not Combine) share the whole file's skeleton and clips, named after MaterialBasePath.
        const std::filesystem::path& base = meshMetadata.MeshSettings.MaterialBasePath.empty() ? meshMetadata.FilePath : meshMetadata.MeshSettings.MaterialBasePath;
        std::filesystem::path skeletonPath = base;
        skeletonPath.replace_extension(".nskel");
        const std::filesystem::path animationDirectory = base.parent_path();
        const std::string animationPrefix = base.stem().string() + "_";

        AssetHandle skeleton = 0;
        std::vector<std::pair<std::string, AssetHandle>> animations;
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            if (metadata.Type == AssetType::Skeleton && (metadata.FilePath == skeletonPath || metadata.SourceFilePath == skeletonPath))
                skeleton = handle;
            else if (metadata.Type == AssetType::AnimationSequence && metadata.FilePath.parent_path() == animationDirectory &&
                     metadata.FilePath.stem().string().starts_with(animationPrefix))
                animations.emplace_back(metadata.FilePath.generic_string(), handle);
        }
        std::sort(animations.begin(), animations.end());

        std::vector<AssetHandle> animationHandles;
        for (const auto& [path, handle] : animations)
            animationHandles.push_back(handle);

        if (meshAsset->GetType() == AssetType::Mesh)
            static_cast<Mesh*>(meshAsset.get())->SetImportedAssets(skeleton, std::move(animationHandles));
        else if (meshAsset->GetType() == AssetType::StaticMesh)
            static_cast<StaticMesh*>(meshAsset.get())->SetImportedAssets(skeleton, std::move(animationHandles));
    }

    const AssetMetadata EditorAssetManager::GetMetadata(AssetHandle handle) const
    {
        static AssetMetadata s_NullMetadata;
        auto it = m_AssetRegistry.find(handle);
        if (it == m_AssetRegistry.end())
            return s_NullMetadata;

        return it->second;
    }

    const std::filesystem::path EditorAssetManager::GetFilePath(AssetHandle handle) const
    {
        return GetMetadata(handle).FilePath;
    }

    Ref<Asset> EditorAssetManager::GetAsset(AssetHandle handle)
    {
        // 1. check if handle is valid
        if (!IsAssetHandleValid(handle))
            return {};
        
        // 2. check if asset needs load (and if so, load)
        Ref<Asset> asset;
        if (IsAssetLoaded(handle))
        {
            asset = m_LoadedAssets.at(handle);
        }
        else
        {
            // load asset
            NOX_PROFILE_SCOPE("Asset Load");
            using Clock = std::chrono::steady_clock;
            auto elapsedMs = [](Clock::time_point from, Clock::time_point to)
            {
                return std::chrono::duration<double, std::milli>(to - from).count();
            };
            const Clock::time_point loadStart = Clock::now();

            const AssetMetadata& metadata = GetMetadata(handle);
            {
                NOX_PROFILE_SCOPE("Asset Import");
                asset = AssetImporter::ImportAsset(handle, metadata);
            }
            if (!asset)
            {
                // import failed
                NOX_CORE_ASSERT("EditorAssetManager::GetAsset - asset import failed")
            }
            m_LoadedAssets[handle] = asset;
            const Clock::time_point importDone = Clock::now();

            if (asset && !metadata.SourceFilePath.empty())
            {
                NOX_PROFILE_SCOPE("Asset Source Hash");
                auto sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
                if (std::filesystem::exists(sourcePath))
                    m_LastKnownSourceHash[handle] = Utility::calcul_hash_streaming(sourcePath.string());
            }
            const Clock::time_point hashDone = Clock::now();

            Clock::time_point texturesDone = hashDone, materialsDone = hashDone, scanDone = hashDone;
            if (asset && (metadata.Type == AssetType::Mesh ||
                          metadata.Type == AssetType::StaticMesh ||
                          metadata.Type == AssetType::MeshSource))
            {
                {
                    NOX_PROFILE_SCOPE("Mesh Textures");
                    ImportMeshTextures(asset);
                }
                texturesDone = Clock::now();
                {
                    NOX_PROFILE_SCOPE("Mesh Materials");
                    ImportMeshMaterials(asset, metadata);
                }
                materialsDone = Clock::now();
                {
                    NOX_PROFILE_SCOPE("Asset Folder Scan");
                    ScanAndRegisterNewAssets(metadata.FilePath.parent_path().parent_path());
                }
                LinkImportedAssets(asset, metadata);
                scanDone = Clock::now();
            }

            // Main-thread asset loads stall the editor; log the slow ones with a phase breakdown.
            const double totalMs = elapsedMs(loadStart, scanDone);
            if (totalMs >= 5.0)
            {
                NOX_CORE_INFO("[AssetLoad] {} ({}) took {:.1f} ms: import {:.1f}, source hash {:.1f}, mesh textures {:.1f}, materials {:.1f}, scan {:.1f}",
                              metadata.FilePath.generic_string(), AssetTypeToString(metadata.Type), totalMs,
                              elapsedMs(loadStart, importDone), elapsedMs(importDone, hashDone),
                              elapsedMs(hashDone, texturesDone), elapsedMs(texturesDone, materialsDone),
                              elapsedMs(materialsDone, scanDone));
            }
        }
        // 3. return asset
        return asset;
    }

    AssetState EditorAssetManager::RequestAsset(AssetHandle handle)
    {
        if (!IsAssetHandleValid(handle))
            return AssetState::Failed;
        if (IsAssetLoaded(handle))
            return AssetState::Ready;
        if (m_Loader.IsLoading(handle))
            return AssetState::Loading;
        if (m_FailedAssets.contains(handle))
            return AssetState::Failed;

        const AssetMetadata& metadata = m_AssetRegistry.at(handle);
        if (AssetLoader::IsStreamable(metadata.Type))
        {
            m_Loader.Begin(handle, metadata);
            return AssetState::Loading;
        }

        // The other types import right here.
        return GetAsset(handle) ? AssetState::Ready : AssetState::Failed;
    }

    void EditorAssetManager::Shutdown()
    {
        if (m_RegistryDirty)
        {
            SerializeAssetRegistry();
            m_RegistryDirty = false;
        }
        m_Streamer.Shutdown();
        m_Loader.Shutdown();
        m_LoadedAssets.clear();
    }

    bool EditorAssetManager::ConsumeLoadsSettledAfterSweep()
    {
        if (!m_SweepDuringLoads || !m_Loader.IsIdle() || !m_Streamer.IsIdle())
            return false;
        m_SweepDuringLoads = false;
        return true;
    }

    size_t EditorAssetManager::UnloadUnusedAssets(const std::unordered_set<AssetHandle>& referencedAssets)
    {
        // Loads in flight publish after this sweep; they are swept once they have all finished.
        // (A streamed texture being moved to a new image is held by the move and skipped below.)
        if (!m_Loader.IsIdle() || !m_Streamer.IsIdle())
            m_SweepDuringLoads = true;

        // Materials reference textures by path, not handle. Cached hits skip the filesystem entirely;
        // the registry index is only built if some path isn't cached yet.
        std::unordered_map<std::string, AssetHandle> textureByPath;
        bool textureIndexBuilt = false;

        if (m_TexturePathMissesRegistrySize != m_AssetRegistry.size())
        {
            m_TexturePathMisses.clear();
            m_TexturePathMissesRegistrySize = m_AssetRegistry.size();
        }

        auto resolveTexture = [&](const std::string& texturePath) -> AssetHandle
        {
            if (texturePath.empty())
                return 0;

            if (auto cached = m_TexturePathCache.find(texturePath); cached != m_TexturePathCache.end())
                return cached->second;
            if (m_TexturePathMisses.contains(texturePath))
                return 0;

            if (!textureIndexBuilt)
            {
                for (const auto& [handle, metadata] : m_AssetRegistry)
                {
                    if (metadata.Type != AssetType::Texture2D)
                        continue;
                    if (!metadata.SourceFilePath.empty())
                        textureByPath[metadata.SourceFilePath.generic_string()] = handle;
                    textureByPath[metadata.FilePath.generic_string()] = handle;
                }
                textureIndexBuilt = true;
            }

            std::filesystem::path relativePath(texturePath);
            if (relativePath.is_absolute())
            {
                // Material paths are built from the project's asset directory string, so a lexical
                // relative path almost always works; std::filesystem::relative (disk access) is the fallback.
                std::filesystem::path lexical = relativePath.lexically_normal().lexically_relative(
                    Project::GetActiveAssetDirectory().lexically_normal());
                if (!lexical.empty() && *lexical.begin() != "..")
                {
                    relativePath = lexical;
                }
                else
                {
                    std::error_code ec;
                    relativePath = std::filesystem::relative(relativePath, Project::GetActiveAssetDirectory(), ec);
                    if (ec)
                    {
                        m_TexturePathMisses.insert(texturePath);
                        return 0;
                    }
                }
            }

            auto it = textureByPath.find(relativePath.generic_string());
            if (it == textureByPath.end())
                it = textureByPath.find(GeneratedAssetPath(relativePath, "Textures", ".ntex").generic_string());
            if (it == textureByPath.end())
            {
                m_TexturePathMisses.insert(texturePath);
                return 0;
            }

            m_TexturePathCache.emplace(texturePath, it->second);
            return it->second;
        };

        // --- Mark ---
        std::unordered_set<AssetHandle> marked;
        std::vector<AssetHandle> pending(referencedAssets.begin(), referencedAssets.end());

        auto markMaterialTextures = [&](const MaterialData& material)
        {
            for (const std::string* path : { &material.BaseColorTexturePath, &material.MetallicRoughnessTexturePath,
                                             &material.NormalTexturePath, &material.OcclusionTexturePath,
                                             &material.EmissiveTexturePath, &material.TransmissionTexturePath })
            {
                if (AssetHandle textureHandle = resolveTexture(*path))
                    pending.push_back(textureHandle);
            }
        };

        while (!pending.empty())
        {
            AssetHandle handle = pending.back();
            pending.pop_back();
            if (handle == 0 || !marked.insert(handle).second)
                continue;

            auto loaded = m_LoadedAssets.find(handle);
            if (loaded == m_LoadedAssets.end() || !loaded->second)
                continue;

            const Ref<Asset>& asset = loaded->second;
            switch (asset->GetType())
            {
                case AssetType::Mesh:
                {
                    const auto* mesh = static_cast<Mesh*>(asset.get());
                    pending.insert(pending.end(), mesh->GetMaterialAssets().begin(), mesh->GetMaterialAssets().end());
                    for (const MaterialData& material : mesh->GetMaterials())
                        markMaterialTextures(material);
                    break;
                }
                case AssetType::StaticMesh:
                {
                    const auto* mesh = static_cast<StaticMesh*>(asset.get());
                    pending.insert(pending.end(), mesh->GetMaterialAssets().begin(), mesh->GetMaterialAssets().end());
                    for (const MaterialData& material : mesh->GetMaterials())
                        markMaterialTextures(material);
                    break;
                }
                case AssetType::Material:
                    markMaterialTextures(static_cast<Material*>(asset.get())->GetData());
                    break;
                default:
                    break;
            }
        }

        // --- Sweep ---
        auto isUnloadable = [](AssetType type)
        {
            switch (type)
            {
                case AssetType::Mesh:
                case AssetType::StaticMesh:
                case AssetType::MeshSource:
                case AssetType::Material:
                case AssetType::Texture2D:
                case AssetType::Skeleton:
                case AssetType::AnimationSequence:
                    return true;
                default:
                    return false;
            }
        };

        size_t unloadedCount = 0;
        std::vector<uint32_t> unloadedTextureSlots;
        for (auto it = m_LoadedAssets.begin(); it != m_LoadedAssets.end();)
        {
            const Ref<Asset>& asset = it->second;

            // RefCount > 1: something outside the registry still holds this asset (renderer,
            // thumbnail cache, an open editor panel) - it's in use even if no scene references it.
            if (marked.contains(it->first) || !asset || !isUnloadable(asset->GetType()) || asset->GetRefCount() > 1)
            {
                ++it;
                continue;
            }

            if (asset->GetType() == AssetType::Texture2D)
            {
                if (auto* texture = dynamic_cast<Texture2D*>(asset.get()))
                    unloadedTextureSlots.push_back(texture->GetDescriptorIndexSlot());
            }
            m_Streamer.Unregister(it->first);
            Renderer::DeferAssetRelease(asset);
            it = m_LoadedAssets.erase(it);
            ++unloadedCount;
        }

        // Only evict cache entries for the unloaded textures' slots: clearing the whole cache made
        // every remaining textured draw (all of Bistro) re-resolve its paths on the next frame.
        Renderer::InvalidateTextureDescriptorSlots(unloadedTextureSlots);

        if (unloadedCount > 0)
            NOX_CORE_INFO("EditorAssetManager: unloaded {} unused asset(s)", unloadedCount);

        return unloadedCount;
    }

    void EditorAssetManager::SerializeAssetRegistry()
    {
        auto path = Project::GetActiveAssetRegistryPath();

        YAML::Emitter out;
        {
            out << YAML::BeginMap; // Root
            out << YAML::Key << "AssetRegistry" << YAML::Value;

            out << YAML::BeginSeq;
            for (const auto&[handle, metadata] : m_AssetRegistry)
            {
                out << YAML::BeginMap;
                out << YAML::Key << "Handle" << YAML::Value << handle;
                std::string filepathStr = metadata.FilePath.generic_string();
                out << YAML::Key << "FilePath" << YAML::Value << filepathStr;
                if (!metadata.SourceFilePath.empty())
                    out << YAML::Key << "SourceFilePath" << YAML::Value << metadata.SourceFilePath.generic_string();
                out << YAML::Key << "Type" << YAML::Value << AssetTypeToString(metadata.Type);
                
                // --- ADD THIS: Save TextureSpecification for Texture2D assets ---
                if (metadata.Type == AssetType::Texture2D)
                {
                    out << YAML::Key << "TextureSpec" << YAML::Value;
                    out << YAML::BeginMap;
                    out << YAML::Key << "Format" << YAML::Value << std::string(ImageFormatToString(metadata.TextureSpec.format));
                    out << YAML::Key << "GenerateMips" << YAML::Value << metadata.TextureSpec.generateMips;
                    out << YAML::Key << "Flip" << YAML::Value << metadata.TextureSpec.flip;
                    out << YAML::EndMap;
                }

                if (metadata.Type == AssetType::Mesh || metadata.Type == AssetType::StaticMesh)
                {
                    out << YAML::Key << "MeshSettings" << YAML::Value;
                    out << YAML::BeginMap;
                    out << YAML::Key << "ImportStaticMeshes" << YAML::Value << metadata.MeshSettings.ImportStaticMeshes;
                    out << YAML::Key << "ImportSkeletalMeshes" << YAML::Value << metadata.MeshSettings.ImportSkeletalMeshes;
                    out << YAML::Key << "ImportAnimations" << YAML::Value << metadata.MeshSettings.ImportAnimations;
                    out << YAML::Key << "ImportSkinWeights" << YAML::Value << metadata.MeshSettings.ImportSkinWeights;
                    out << YAML::Key << "StaticCombine" << YAML::Value << static_cast<int>(metadata.MeshSettings.StaticCombine);
                    out << YAML::Key << "SkeletalCombine" << YAML::Value << static_cast<int>(metadata.MeshSettings.SkeletalCombine);
                    out << YAML::Key << "SourceMeshIndex" << YAML::Value << metadata.MeshSettings.SourceMeshIndex;
                    out << YAML::Key << "ImportScale" << YAML::Value << metadata.MeshSettings.ImportScale;
                    if (!metadata.MeshSettings.MaterialBasePath.empty())
                        out << YAML::Key << "MaterialBasePath" << YAML::Value << metadata.MeshSettings.MaterialBasePath.generic_string();
                    out << YAML::EndMap;
                }
                
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndMap; // Root
        }
        
        std::ofstream fout(path);
        fout << out.c_str();
    }

    void EditorAssetManager::ScanAndRegisterNewAssets(const std::filesystem::path& relativeDirectory)
    {
        // This runs on the main thread whenever a mesh is loaded or imported. The old version walked
        // the entire asset tree, called std::filesystem::relative (disk access) per cooked file and
        // compared each against the whole registry - measured ~150ms+ per call on this project,
        // sometimes several calls per drag-in. Now: only the imported model's folder, a lexical
        // relative path (entries are already prefixed by the scan root), and a hash set lookup.
        const std::filesystem::path assetDir = Project::GetActiveAssetDirectory();
        const std::filesystem::path scanRoot = relativeDirectory.empty() ? assetDir : assetDir / relativeDirectory;

        std::error_code ec;
        if (!std::filesystem::is_directory(scanRoot, ec)) return;

        std::unordered_set<std::string> registeredPaths;
        registeredPaths.reserve(m_AssetRegistry.size() * 2);
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            registeredPaths.insert(metadata.FilePath.lexically_normal().generic_string());
            if (!metadata.SourceFilePath.empty())
                registeredPaths.insert(metadata.SourceFilePath.lexically_normal().generic_string());
        }

        bool registryChanged = false;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(scanRoot, ec))
        {
            if (!entry.is_regular_file(ec)) continue;

            std::filesystem::path ext = entry.path().extension();
            if (ext == ".nox" || ext == ".nanim" || ext == ".nskel" || ext == ".nmat" ||
                ext == ".ntex" || ext == ".nanimgraph")
            {
                std::filesystem::path relativePath = entry.path().lexically_relative(assetDir).lexically_normal();

                // insert() returning false means it was already registered.
                if (registeredPaths.insert(relativePath.generic_string()).second)
                {
                    AssetHandle newHandle; // generates new random handle
                    AssetMetadata metadata;
                    metadata.FilePath = relativePath;
                    metadata.SourceFilePath = relativePath;
                    metadata.Type = GetAssetTypeFromExtension(ext);

                    m_AssetRegistry[newHandle] = metadata;
                    IndexPath(newHandle, metadata);
                    registryChanged = true;
                    NOX_CORE_INFO("[EditorAssetManager] Auto-registered newly discovered asset: {}", relativePath.string());
                }
            }
        }

        if (registryChanged)
        {
            m_RegistryDirty = true; // written by Update(), not per load
        }
    }

    bool EditorAssetManager::DeserializeAssetRegistry()
    {
        auto path = Project::GetActiveAssetRegistryPath();

        if (!std::filesystem::exists(path))
        {
            NOX_CORE_ERROR("Asset Registry file does not exist: {0}", path.string());
            return false;
        }
        
        YAML::Node data;
        try
        {
            data = YAML::LoadFile(path.string());
        }
        catch (YAML::ParserException e)
        {
            NOX_CORE_ERROR("Failed to load project file '{0}'\n    {1}", path.string(), e.what());
        }

        auto rootNode = data["AssetRegistry"];
        if (!rootNode)
            return false;

        for (const auto& node : rootNode)
        {
            AssetHandle handle = node["Handle"].as<uint64_t>();
            auto& metadata = m_AssetRegistry[handle];
            metadata.FilePath = node["FilePath"].as<std::string>();
            if (node["SourceFilePath"])
                metadata.SourceFilePath = node["SourceFilePath"].as<std::string>();
            metadata.Type = AssetTypeFromString(node["Type"].as<std::string>());
            
            // --- ADD THIS: Load TextureSpecification if present ---
            if (node["TextureSpec"])
            {
                auto specNode = node["TextureSpec"];
                if (specNode["Format"])
                    metadata.TextureSpec.format = ImageFormatFromString(specNode["Format"].as<std::string>());
                if (specNode["GenerateMips"])
                    metadata.TextureSpec.generateMips = specNode["GenerateMips"].as<bool>();
                if (specNode["Flip"])
                    metadata.TextureSpec.flip = specNode["Flip"].as<bool>();
            }

            if (auto settingsNode = node["MeshSettings"])
            {
                if (settingsNode["ImportStaticMeshes"])
                    metadata.MeshSettings.ImportStaticMeshes = settingsNode["ImportStaticMeshes"].as<bool>();
                if (settingsNode["ImportSkeletalMeshes"])
                    metadata.MeshSettings.ImportSkeletalMeshes = settingsNode["ImportSkeletalMeshes"].as<bool>();
                if (settingsNode["ImportAnimations"])
                    metadata.MeshSettings.ImportAnimations = settingsNode["ImportAnimations"].as<bool>();
                if (settingsNode["ImportSkinWeights"])
                    metadata.MeshSettings.ImportSkinWeights = settingsNode["ImportSkinWeights"].as<bool>();
                if (settingsNode["StaticCombine"])
                    metadata.MeshSettings.StaticCombine = static_cast<MeshCombineMode>(settingsNode["StaticCombine"].as<int>());
                if (settingsNode["SkeletalCombine"])
                    metadata.MeshSettings.SkeletalCombine = static_cast<MeshCombineMode>(settingsNode["SkeletalCombine"].as<int>());
                if (settingsNode["SourceMeshIndex"])
                    metadata.MeshSettings.SourceMeshIndex = settingsNode["SourceMeshIndex"].as<int32_t>();
                if (settingsNode["ImportScale"])
                    metadata.MeshSettings.ImportScale = settingsNode["ImportScale"].as<float>();
                if (settingsNode["MaterialBasePath"])
                    metadata.MeshSettings.MaterialBasePath = settingsNode["MaterialBasePath"].as<std::string>();
            }
        }

        m_HandleByPath.clear();
        for (const auto& [indexedHandle, indexedMetadata] : m_AssetRegistry)
            IndexPath(indexedHandle, indexedMetadata);

        return true;
    }
}
