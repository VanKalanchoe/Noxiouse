#include "EditorAssetManager.h"

#include <unordered_set>
#include "NoxCore/Core/VirtualFileSystem.h"

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
#include <charconv>
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
        { ".nanimgraph", AssetType::AnimationGraph },
        { ".nprefab", AssetType::Prefab },
        { ".ntimeline", AssetType::Timeline },
        { ".ttf", AssetType::Font },
        { ".otf", AssetType::Font }
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
        return handle != 0 && (m_AssetRegistry.find(handle) != m_AssetRegistry.end() || m_MemoryAssets.contains(handle));
    }

    Asset* EditorAssetManager::FindLoadedAsset(AssetHandle handle) const
    {
        auto found = m_LoadedAssets.find(handle);
        if (found != m_LoadedAssets.end())
            return found->second.get();
        auto memory = m_MemoryAssets.find(handle);
        return memory != m_MemoryAssets.end() ? memory->second.get() : nullptr;
    }

    bool EditorAssetManager::IsAssetLoaded(AssetHandle handle) const
    {
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end() || m_MemoryAssets.contains(handle);
    }

    AssetType EditorAssetManager::GetAssetType(AssetHandle handle) const
    {
        if (auto memory = m_MemoryAssets.find(handle); memory != m_MemoryAssets.end())
            return memory->second->GetType();
        if (!IsAssetHandleValid(handle))
            return AssetType::None;

        return m_AssetRegistry.at(handle).Type;
    }

    void EditorAssetManager::AddMemoryAsset(AssetHandle handle, Ref<Asset> asset)
    {
        if (handle == 0 || !asset)
            return;
        asset->Handle = handle;
        m_MemoryAssets[handle] = std::move(asset);
    }

    void EditorAssetManager::RemoveMemoryAsset(AssetHandle handle)
    {
        m_MemoryAssets.erase(handle);
    }

    void EditorAssetManager::Init()
    {
        if (m_ReadOnly)
            return;
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

        if (m_RegistryDirty && !m_ReadOnly && std::chrono::steady_clock::now() - m_RegistryWritten >= std::chrono::seconds(1))
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
                if (loaded.Cooked && !m_ReadOnly)
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
        if (m_ReadOnly || !IsAssetHandleValid(handle) || m_MemoryAssets.contains(handle)) return;

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
        if (metadata.Type == AssetType::None)
        {
            NOX_CORE_ERROR("EditorAssetManager::ImportAsset - could not determine the asset type of '{}' from its extension", sourcePath.string());
            return;
        }

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
                // (Read-only: the cooked texture is taken as it is; correcting it means cooking again.)
                if (metadata.TextureSpec.format != expectedFormat && !m_ReadOnly)
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

            // Where the material is now: a renamed or moved one is found by its former path (FormerPaths) and lives elsewhere.
            const auto materialEntry = materialHandle != 0 ? m_AssetRegistry.find(materialHandle) : m_AssetRegistry.end();
            const auto fullMaterialPath = Project::GetActiveAssetDirectory() /
                                          (materialEntry != m_AssetRegistry.end() ? materialEntry->second.FilePath : materialPath);
            if (materialHandle == 0 || (checkedPaths.insert(materialKey).second && !VFS::Exists(fullMaterialPath)))
            {
                // Read-only: a material file that is not there leaves the slot to the mesh's embedded material.
                if (m_ReadOnly)
                {
                    materialAssets.push_back(AssetHandle(0));
                    continue;
                }
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
        // Former paths only where nothing else lives now.
        for (const std::filesystem::path& former : metadata.FormerPaths)
            m_HandleByPath.try_emplace(former.lexically_normal().generic_string(), handle);
    }

    AssetHandle EditorAssetManager::FindHandleByPath(const std::filesystem::path& path, AssetType type) const
    {
        const auto found = m_HandleByPath.find(path.lexically_normal().generic_string());
        if (found == m_HandleByPath.end())
            return 0;
        const auto entry = m_AssetRegistry.find(found->second);
        return entry != m_AssetRegistry.end() && entry->second.Type == type ? found->second : AssetHandle(0);
    }

    AssetHandle EditorAssetManager::RegisterExistingFile(const std::filesystem::path& relativePath, AssetType type)
    {
        AssetMetadata metadata;
        metadata.Type = type;
        metadata.FilePath = relativePath;
        metadata.SourceFilePath = relativePath;
        const AssetHandle handle = RegisterAsset(metadata);
        SerializeAssetRegistry();
        return handle;
    }

    bool EditorAssetManager::CanRename(AssetHandle handle) const
    {
        const auto found = m_AssetRegistry.find(handle);
        if (found == m_AssetRegistry.end() || m_ReadOnly)
            return false;
        const AssetMetadata& metadata = found->second;
        // Not scenes (the editor keeps the open scene's path) and not what a model made beside its meshes (skeletons, clips:
        // found by the model's name). Textures, materials and meshes keep their old paths as aliases (FormerPaths): materials
        // name textures by path, meshes derive their materials' paths from their own name.
        switch (metadata.Type)
        {
        case AssetType::Prefab:
        case AssetType::AnimationGraph:
        case AssetType::Timeline:
        case AssetType::Material:
        case AssetType::Texture2D:
        case AssetType::StaticMesh:
        case AssetType::Mesh:
            return true;
        default:
            return false;
        }
    }

    namespace
    {
        // A file and the companions named after it ("x.ntex.hash", "x.nsmesh.hash"): what moves together.
        std::vector<std::filesystem::path> FileWithCompanions(const std::filesystem::path& assetDirectory, const std::filesystem::path& relative)
        {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            if (relative.empty() || !std::filesystem::exists(assetDirectory / relative, error))
                return files;
            files.push_back(relative);
            const std::string prefix = relative.filename().string() + ".";
            for (const auto& entry : std::filesystem::directory_iterator((assetDirectory / relative).parent_path(), error))
            {
                const std::string name = entry.path().filename().string();
                if (entry.is_regular_file(error) && name.size() > prefix.size() && name.rfind(prefix, 0) == 0)
                    files.push_back(relative.parent_path() / name);
            }
            return files;
        }
    }

    bool EditorAssetManager::RelocateAsset(AssetHandle handle, const std::filesystem::path& relativeFolder, const std::string& newStem)
    {
        if (!CanRename(handle) || newStem.empty() || newStem.find_first_of("\\/:*?\"<>|") != std::string::npos)
            return false;
        AssetMetadata& metadata = m_AssetRegistry.find(handle)->second;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        const std::string oldStem = metadata.FilePath.stem().string();

        // The files it is: its file; a texture's source and its cooked .ntex beside it (same name); never a model's source,
        // which other assets are made from too.
        std::vector<std::filesystem::path> files = FileWithCompanions(assetDirectory, metadata.FilePath);
        if (metadata.Type == AssetType::Texture2D)
        {
            std::filesystem::path cooked = metadata.FilePath;
            cooked.replace_extension(".ntex");
            for (const std::filesystem::path& extra : { metadata.SourceFilePath, cooked })
            {
                if (extra.empty() || extra == metadata.FilePath || extra.parent_path() != metadata.FilePath.parent_path() || extra.stem() != metadata.FilePath.stem())
                    continue;
                for (const std::filesystem::path& file : FileWithCompanions(assetDirectory, extra))
                {
                    if (std::find(files.begin(), files.end(), file) == files.end())
                        files.push_back(file);
                }
            }
        }
        if (files.empty())
            return false;

        // Each file: the same name after the stem ("x.ntex.hash" -> "y.ntex.hash"), in the new folder.
        auto target = [&](const std::filesystem::path& file)
        {
            const std::string name = file.filename().string();
            const std::string rest = name.rfind(oldStem, 0) == 0 ? name.substr(oldStem.size()) : file.extension().string();
            return relativeFolder / (newStem + rest);
        };
        std::error_code error;
        for (const std::filesystem::path& file : files)
        {
            if (target(file).lexically_normal() != file.lexically_normal() && std::filesystem::exists(assetDirectory / target(file), error))
                return false; // taken
        }
        std::filesystem::create_directories(assetDirectory / relativeFolder, error);
        for (const std::filesystem::path& file : files)
        {
            std::filesystem::rename(assetDirectory / file, assetDirectory / target(file), error);
            if (error)
            {
                NOX_CORE_ERROR("Could not move {}: {}", file.generic_string(), error.message());
                return false;
            }
        }

        // A mesh's materials (and skeleton, clips) are named after the path it was cooked under: that name stays their base.
        if ((metadata.Type == AssetType::StaticMesh || metadata.Type == AssetType::Mesh) && metadata.MeshSettings.MaterialBasePath.empty())
            metadata.MeshSettings.MaterialBasePath = metadata.FilePath;
        auto remember = [&](const std::filesystem::path& former)
        {
            if (!former.empty() && std::find(metadata.FormerPaths.begin(), metadata.FormerPaths.end(), former) == metadata.FormerPaths.end())
                metadata.FormerPaths.push_back(former);
        };
        m_HandleByPath.erase(metadata.FilePath.lexically_normal().generic_string());
        remember(metadata.FilePath);
        const bool sourceMoved = !metadata.SourceFilePath.empty() &&
                                 std::find(files.begin(), files.end(), metadata.SourceFilePath) != files.end();
        if (metadata.SourceFilePath == metadata.FilePath || sourceMoved)
        {
            if (metadata.SourceFilePath != metadata.FilePath)
            {
                m_HandleByPath.erase(metadata.SourceFilePath.lexically_normal().generic_string());
                remember(metadata.SourceFilePath);
            }
            metadata.SourceFilePath = target(metadata.SourceFilePath);
        }
        metadata.FilePath = target(metadata.FilePath);
        IndexPath(handle, metadata);
        SerializeAssetRegistry();
        return true;
    }

    bool EditorAssetManager::MoveAsset(AssetHandle handle, const std::filesystem::path& relativeFolder)
    {
        const auto found = m_AssetRegistry.find(handle);
        if (found == m_AssetRegistry.end())
            return false;
        if (relativeFolder.lexically_normal() == found->second.FilePath.parent_path().lexically_normal())
            return true;
        return RelocateAsset(handle, relativeFolder, found->second.FilePath.stem().string());
    }

    bool EditorAssetManager::DeleteAsset(AssetHandle handle)
    {
        const auto found = m_AssetRegistry.find(handle);
        if (found == m_AssetRegistry.end())
            return false;
        const AssetMetadata metadata = found->second;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::error_code error;
        if (!metadata.FilePath.empty())
            std::filesystem::remove(assetDirectory / metadata.FilePath, error);

        // The source too (a .png of a texture, a .gltf of a model) when it is in the project and no other asset is made from
        // it (a model's static mesh, skeletal mesh, skeleton and clips share one source).
        const std::string source = metadata.SourceFilePath.lexically_normal().generic_string();
        if (!source.empty() && metadata.SourceFilePath != metadata.FilePath)
        {
            bool shared = false;
            for (const auto& [otherHandle, other] : m_AssetRegistry)
            {
                if (otherHandle != handle && other.SourceFilePath.lexically_normal().generic_string() == source)
                {
                    shared = true;
                    break;
                }
            }
            if (!shared)
                std::filesystem::remove(assetDirectory / metadata.SourceFilePath, error);
        }

        m_LoadedAssets.erase(handle);
        m_FailedAssets.erase(handle);
        for (auto it = m_HandleByPath.begin(); it != m_HandleByPath.end();)
            it = it->second == handle ? m_HandleByPath.erase(it) : std::next(it);
        m_AssetRegistry.erase(handle);
        SerializeAssetRegistry();
        return true;
    }

    namespace
    {
        // A path as files mention it: forward slashes, lower case (Windows paths ignore case).
        std::string NormalizeReferencePath(std::string_view text)
        {
            std::string path(text);
            std::replace(path.begin(), path.end(), '\\', '/');
            path.erase(std::unique(path.begin(), path.end(), [](char a, char b) { return a == '/' && b == '/'; }), path.end());
            std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            while (path.rfind("./", 0) == 0)
                path.erase(0, 2);
            return path;
        }

        // What one file mentions: numbers that can be handles, and path-like strings with every tail that starts after a '/'
        // (a material may hold an absolute path; the registry's paths are relative to the asset directory).
        struct ReferenceTokens
        {
            std::unordered_set<AssetHandle> Handles;
            std::unordered_set<std::string> Paths;
        };

        void AddPathToken(std::string_view raw, ReferenceTokens& tokens)
        {
            while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t'))
                raw.remove_prefix(1);
            while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t'))
                raw.remove_suffix(1);
            if (raw.size() < 3 || raw.find('.') == std::string_view::npos)
                return;
            const std::string path = NormalizeReferencePath(raw);
            tokens.Paths.insert(path);
            for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
            {
                if (slash + 1 < path.size())
                    tokens.Paths.insert(path.substr(slash + 1));
            }
        }

        ReferenceTokens ReadReferenceTokens(const std::filesystem::path& file)
        {
            ReferenceTokens tokens;
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
                return tokens;
            const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

            // Handles: runs of 6+ digits (YAML writes them as decimal numbers).
            for (size_t i = 0; i < content.size();)
            {
                if (content[i] < '0' || content[i] > '9')
                {
                    ++i;
                    continue;
                }
                size_t end = i;
                while (end < content.size() && content[end] >= '0' && content[end] <= '9')
                    ++end;
                if (end - i >= 6)
                {
                    uint64_t value = 0;
                    const auto [unused, errorCode] = std::from_chars(content.data() + i, content.data() + end, value);
                    if (errorCode == std::errc())
                        tokens.Handles.insert(AssetHandle(value));
                }
                i = end;
            }

            // Paths: printable runs (binary files: materials and meshes write length-prefixed strings), cut at what a path does
            // not hold or what separates YAML values (": " after a key, "- " of a list item, quotes, brackets, commas).
            size_t start = 0;
            auto flush = [&](size_t end)
            {
                if (end > start)
                    AddPathToken(std::string_view(content).substr(start, end - start), tokens);
                start = end + 1;
            };
            for (size_t i = 0; i < content.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(content[i]);
                const bool separator = c < 0x20 || c > 0x7E || c == '"' || c == '\'' || c == ',' || c == '[' || c == ']' ||
                                       c == '{' || c == '}' || ((c == ':' || c == '-') && i + 1 < content.size() && content[i + 1] == ' ');
                if (separator)
                    flush(i);
            }
            flush(content.size());
            return tokens;
        }

        bool IsReferencingType(AssetType type)
        {
            return type == AssetType::Scene || type == AssetType::Prefab || type == AssetType::AnimationGraph || type == AssetType::Material ||
                   type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::SkeletalMesh;
        }

        // The model's mesh owns, by name, the materials, skeleton and clips cooked from the same file (MeshImporter::MaterialAssetPath,
        // EditorAssetManager::LinkImportedAssets).
        bool OwnsByName(const AssetMetadata& mesh, const AssetMetadata& asset)
        {
            if (mesh.Type != AssetType::Mesh && mesh.Type != AssetType::StaticMesh && mesh.Type != AssetType::SkeletalMesh)
                return false;
            const std::filesystem::path base = mesh.MeshSettings.MaterialBasePath.empty() ? mesh.FilePath : mesh.MeshSettings.MaterialBasePath;
            const std::string prefix = NormalizeReferencePath(base.stem().string() + "_");
            const std::string stem = NormalizeReferencePath(asset.FilePath.stem().string());
            const std::string folder = NormalizeReferencePath(asset.FilePath.parent_path().lexically_normal().generic_string());
            switch (asset.Type)
            {
            case AssetType::Material:
                return folder == NormalizeReferencePath((base.parent_path().parent_path() / "Materials").lexically_normal().generic_string()) &&
                       stem.rfind(prefix, 0) == 0;
            case AssetType::Skeleton:
                return NormalizeReferencePath(asset.FilePath.lexically_normal().generic_string()) ==
                       NormalizeReferencePath(std::filesystem::path(base).replace_extension(".nskel").lexically_normal().generic_string());
            case AssetType::AnimationSequence:
                return folder == NormalizeReferencePath(base.parent_path().lexically_normal().generic_string()) && stem.rfind(prefix, 0) == 0;
            default:
                return false;
            }
        }

        bool MentionsAsset(const ReferenceTokens& tokens, AssetHandle handle, const AssetMetadata& metadata)
        {
            if (tokens.Handles.contains(handle))
                return true;
            for (const std::filesystem::path& path : { metadata.FilePath, metadata.SourceFilePath })
            {
                if (!path.empty() && tokens.Paths.contains(NormalizeReferencePath(path.lexically_normal().generic_string())))
                    return true;
            }
            return false;
        }

        // Files that can name assets: the referencing asset types, the project's scripts and the project file.
        std::vector<std::pair<std::filesystem::path, AssetHandle>> ReferencingFiles(const AssetRegistry& registry)
        {
            std::vector<std::pair<std::filesystem::path, AssetHandle>> files;
            const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
            for (const auto& [handle, metadata] : registry)
            {
                if (IsReferencingType(metadata.Type) && !metadata.FilePath.empty())
                    files.emplace_back(metadata.FilePath, handle);
            }
            std::error_code error;
            for (auto it = std::filesystem::recursive_directory_iterator(assetDirectory, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            {
                if (it->is_regular_file(error) && it->path().extension() == ".cs")
                    files.emplace_back(it->path().lexically_relative(assetDirectory), AssetHandle(0));
            }
            for (const auto& entry : std::filesystem::directory_iterator(Project::GetProjectDirectory(), error))
            {
                if (entry.path().extension() == ".nproj")
                    files.emplace_back(entry.path().lexically_relative(assetDirectory), AssetHandle(0));
            }
            return files;
        }
    }

    std::vector<std::filesystem::path> EditorAssetManager::FindReferences(AssetHandle handle) const
    {
        std::vector<std::filesystem::path> referencing;
        const auto found = m_AssetRegistry.find(handle);
        if (found == m_AssetRegistry.end())
            return referencing;
        const AssetMetadata& target = found->second;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        for (const auto& [file, fileHandle] : ReferencingFiles(m_AssetRegistry))
        {
            if (fileHandle == handle)
                continue;
            const auto owner = m_AssetRegistry.find(fileHandle);
            if ((owner != m_AssetRegistry.end() && OwnsByName(owner->second, target)) ||
                MentionsAsset(ReadReferenceTokens(assetDirectory / file), handle, target))
                referencing.push_back(file);
        }
        // Assets made from this one (a model's meshes from its .gltf).
        for (const auto& [otherHandle, other] : m_AssetRegistry)
        {
            if (otherHandle != handle && !other.SourceFilePath.empty() && other.SourceFilePath != other.FilePath &&
                other.SourceFilePath.lexically_normal() == target.FilePath.lexically_normal())
                referencing.push_back(other.FilePath);
        }
        return referencing;
    }

    size_t EditorAssetManager::ReplaceReferences(AssetHandle from, AssetHandle to)
    {
        const auto fromEntry = m_AssetRegistry.find(from);
        const auto toEntry = m_AssetRegistry.find(to);
        if (m_ReadOnly || from == to || fromEntry == m_AssetRegistry.end() || toEntry == m_AssetRegistry.end() ||
            fromEntry->second.Type != toEntry->second.Type)
            return 0;

        // The text files that name assets by handle (YAML: a handle is a run of digits on its own).
        std::vector<std::filesystem::path> files;
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            if ((metadata.Type == AssetType::Scene || metadata.Type == AssetType::Prefab || metadata.Type == AssetType::AnimationGraph ||
                 metadata.Type == AssetType::Timeline) && !metadata.FilePath.empty())
                files.push_back(Project::GetActiveAssetDirectory() / metadata.FilePath);
        }
        const std::string fromText = std::to_string(static_cast<uint64_t>(from));
        const std::string toText = std::to_string(static_cast<uint64_t>(to));
        size_t changedFiles = 0;
        for (const std::filesystem::path& file : files)
        {
            std::string content;
            {
                std::ifstream in(file, std::ios::binary);
                if (!in)
                    continue;
                content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            bool changed = false;
            for (size_t at = content.find(fromText); at != std::string::npos; at = content.find(fromText, at))
            {
                const bool digitBefore = at > 0 && std::isdigit(static_cast<unsigned char>(content[at - 1]));
                const bool digitAfter = at + fromText.size() < content.size() && std::isdigit(static_cast<unsigned char>(content[at + fromText.size()]));
                if (digitBefore || digitAfter)
                {
                    at += fromText.size();
                    continue;
                }
                content.replace(at, fromText.size(), toText);
                at += toText.size();
                changed = true;
            }
            if (!changed)
                continue;
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << content;
            ++changedFiles;
        }
        NOX_CORE_INFO("Replace References: {} -> {} in {} files", fromEntry->second.FilePath.generic_string(), toEntry->second.FilePath.generic_string(), changedFiles);
        return changedFiles;
    }

    std::vector<AssetHandle> EditorAssetManager::FindUnusedAssets() const
    {
        std::unordered_set<AssetHandle> used;
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        for (const auto& [file, fileHandle] : ReferencingFiles(m_AssetRegistry))
        {
            const ReferenceTokens tokens = ReadReferenceTokens(assetDirectory / file);
            const auto owner = m_AssetRegistry.find(fileHandle);
            for (const auto& [handle, metadata] : m_AssetRegistry)
            {
                if (handle == fileHandle || used.contains(handle))
                    continue;
                if ((owner != m_AssetRegistry.end() && OwnsByName(owner->second, metadata)) || MentionsAsset(tokens, handle, metadata))
                    used.insert(handle);
            }
        }
        // Sources assets are made from.
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            if (!metadata.SourceFilePath.empty() && metadata.SourceFilePath != metadata.FilePath)
            {
                for (const auto& [sourceHandle, source] : m_AssetRegistry)
                {
                    if (source.FilePath.lexically_normal() == metadata.SourceFilePath.lexically_normal())
                        used.insert(sourceHandle);
                }
            }
        }

        std::vector<AssetHandle> unused;
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            if (metadata.Type != AssetType::Scene && !used.contains(handle))
                unused.push_back(handle);
        }
        return unused;
    }

    std::unordered_set<AssetHandle> EditorAssetManager::CollectDependencies(const std::vector<AssetHandle>& roots,
                                                                         const std::vector<std::filesystem::path>& extraTokenFiles) const
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();

        // Path index (both the file and the source a cooked asset names) and the assets each model source made.
        std::unordered_map<std::string, std::vector<AssetHandle>> byPath;
        std::unordered_map<std::string, std::vector<AssetHandle>> bySource;
        std::vector<std::pair<AssetHandle, const AssetMetadata*>> ownable; // materials, skeletons, clips (owned by name by meshes)
        for (const auto& [handle, metadata] : m_AssetRegistry)
        {
            byPath[NormalizeReferencePath(metadata.FilePath.lexically_normal().generic_string())].push_back(handle);
            if (!metadata.SourceFilePath.empty() && metadata.SourceFilePath != metadata.FilePath)
            {
                const std::string source = NormalizeReferencePath(metadata.SourceFilePath.lexically_normal().generic_string());
                bySource[source].push_back(handle);
                if (metadata.Type == AssetType::Texture2D)
                    byPath[source].push_back(handle);
            }
            if (metadata.Type == AssetType::Material || metadata.Type == AssetType::Skeleton || metadata.Type == AssetType::AnimationSequence)
                ownable.emplace_back(handle, &metadata);
        }

        std::unordered_set<AssetHandle> result;
        std::vector<AssetHandle> pending;
        auto add = [&](AssetHandle handle)
        {
            if (handle != 0 && m_AssetRegistry.contains(handle) && result.insert(handle).second)
                pending.push_back(handle);
        };
        auto addTokens = [&](const ReferenceTokens& tokens)
        {
            for (AssetHandle handle : tokens.Handles)
                add(handle);
            for (const std::string& path : tokens.Paths)
            {
                if (auto found = byPath.find(path); found != byPath.end())
                    for (AssetHandle handle : found->second)
                        add(handle);
            }
        };

        for (AssetHandle root : roots)
            add(root);
        for (const std::filesystem::path& file : extraTokenFiles)
            addTokens(ReadReferenceTokens(file));

        while (!pending.empty())
        {
            const AssetHandle handle = pending.back();
            pending.pop_back();
            const AssetMetadata& metadata = m_AssetRegistry.at(handle);

            // What its file names (scenes, prefabs, graphs, timelines by handle; materials and meshes name textures by path).
            if (IsReferencingType(metadata.Type) || metadata.Type == AssetType::Timeline)
                addTokens(ReadReferenceTokens(assetDirectory / metadata.FilePath));

            const bool meshLike = metadata.Type == AssetType::Mesh || metadata.Type == AssetType::StaticMesh ||
                                  metadata.Type == AssetType::SkeletalMesh || metadata.Type == AssetType::MeshSource;
            if (!meshLike)
                continue;
            // The materials, skeleton and clips cooked from its model, which it owns by name.
            for (const auto& [owned, ownedMetadata] : ownable)
            {
                if (!result.contains(owned) && OwnsByName(metadata, *ownedMetadata))
                    add(owned);
            }
            // The other assets cooked from the same model (a model instance spawns them), or made from this source.
            const std::filesystem::path& source = metadata.SourceFilePath.empty() || metadata.SourceFilePath == metadata.FilePath
                ? metadata.FilePath : metadata.SourceFilePath;
            if (auto found = bySource.find(NormalizeReferencePath(source.lexically_normal().generic_string())); found != bySource.end())
            {
                for (AssetHandle sibling : found->second)
                    add(sibling);
            }
        }
        return result;
    }

    bool EditorAssetManager::RenameAsset(AssetHandle handle, const std::string& newName)
    {
        const auto found = m_AssetRegistry.find(handle);
        if (found == m_AssetRegistry.end())
            return false;
        if (newName == found->second.FilePath.stem().string())
            return true;
        return RelocateAsset(handle, found->second.FilePath.parent_path(), newName);
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

    AssetMetadata EditorAssetManager::GetMetadata(AssetHandle handle) const
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
        if (auto memory = m_MemoryAssets.find(handle); memory != m_MemoryAssets.end())
            return memory->second;
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
                // A missing or broken file on disk, not a programming error: log it, the null asset is kept so it is not
                // imported again every frame.
                NOX_CORE_ERROR("EditorAssetManager::GetAsset - importing '{}' failed", metadata.FilePath.string());
            }
            m_LoadedAssets[handle] = asset;
            const Clock::time_point importDone = Clock::now();

            if (asset && !metadata.SourceFilePath.empty() && !m_ReadOnly)
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
                if (!m_ReadOnly)
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
        if (m_RegistryDirty && !m_ReadOnly)
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
        std::unordered_set<AssetHandle> all;
        for (const auto& [handle, metadata] : m_AssetRegistry)
            all.insert(handle);
        SerializeAssetRegistry(Project::GetActiveAssetRegistryPath(), all);
    }

    void EditorAssetManager::SerializeAssetRegistry(const std::filesystem::path& path, const std::unordered_set<AssetHandle>& handles) const
    {
        YAML::Emitter out;
        {
            out << YAML::BeginMap; // Root
            out << YAML::Key << "AssetRegistry" << YAML::Value;

            out << YAML::BeginSeq;
            for (const auto&[handle, metadata] : m_AssetRegistry)
            {
                if (!handles.contains(handle))
                    continue;
                out << YAML::BeginMap;
                out << YAML::Key << "Handle" << YAML::Value << handle;
                std::string filepathStr = metadata.FilePath.generic_string();
                out << YAML::Key << "FilePath" << YAML::Value << filepathStr;
                if (!metadata.SourceFilePath.empty())
                    out << YAML::Key << "SourceFilePath" << YAML::Value << metadata.SourceFilePath.generic_string();
                out << YAML::Key << "Type" << YAML::Value << AssetTypeToString(metadata.Type);
                if (!metadata.FormerPaths.empty())
                {
                    out << YAML::Key << "FormerPaths" << YAML::Value << YAML::BeginSeq;
                    for (const std::filesystem::path& former : metadata.FormerPaths)
                        out << former.generic_string();
                    out << YAML::EndSeq;
                }
                
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
                ext == ".ntex" || ext == ".nanimgraph" || ext == ".nprefab")
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

        if (!VFS::Exists(path))
        {
            NOX_CORE_ERROR("Asset Registry file does not exist: {0}", path.string());
            return false;
        }
        
        YAML::Node data;
        try
        {
            data = YAML::Load(VFS::ReadTextOrEmpty(path));
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
            if (const YAML::Node former = node["FormerPaths"]; former && former.IsSequence())
            {
                for (const YAML::Node& path : former)
                    metadata.FormerPaths.emplace_back(path.as<std::string>());
            }
            
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

        // Stale entries (docs/Future.md item 14), as Unity treats a deleted source: what came from it goes with it.
        //  - neither the asset's file nor its source exists any more (deleted in the file explorer, left by an older import);
        //  - an imported asset (cooked mesh / texture) whose source is gone: the cooked file is a cache of something that no longer
        //    exists -- dropped, and the cooked file deleted;
        //  - the materials a deleted model's import generated (<model folder>/../Materials/<mesh>_*.nmat, MeshImporter::
        //    MaterialAssetPath): they live outside the model's folder, so deleting the folder left hundreds behind.
        // Deleting an entity never unregisters its assets (other entities, scenes, prefabs and the undo history may use them; their
        // memory is freed by UnloadUnusedAssets). Only when nothing of the registry exists at all is it a moved or unreachable asset
        // folder, left as it is (this used to apply as soon as half was missing -- deleting a model that was most of the project
        // removed nothing). The registry before the clean-up is kept next to it as .bak.
        bool registryPruned = false;
        if (!m_ReadOnly)
        {
            const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
            std::error_code error;
            auto existsInAssets = [&](const std::filesystem::path& relative)
            {
                return !relative.empty() && std::filesystem::exists(assetDirectory / relative, error);
            };
            auto isMeshType = [](AssetType type)
            {
                return type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::MeshSource || type == AssetType::SkeletalMesh;
            };
            // The generated materials of a mesh: their folder and file name prefix (MeshImporter::MaterialAssetPath).
            auto materialPrefix = [](const AssetMetadata& mesh)
            {
                const std::filesystem::path base = mesh.MeshSettings.MaterialBasePath.empty() ? mesh.FilePath : std::filesystem::path(mesh.MeshSettings.MaterialBasePath);
                return std::make_pair((base.parent_path().parent_path() / "Materials").lexically_normal().generic_string(), base.stem().string() + "_");
            };

            std::unordered_set<AssetHandle> stale;
            std::vector<std::filesystem::path> orphanedFiles; // cooked caches and generated materials of deleted sources
            size_t existing = 0;
            for (const auto& [handle, metadata] : m_AssetRegistry)
            {
                const bool fileExists = existsInAssets(metadata.FilePath);
                const bool sourceExists = existsInAssets(metadata.SourceFilePath);
                if (fileExists || sourceExists)
                    ++existing;
                const bool imported = !metadata.SourceFilePath.empty() && metadata.SourceFilePath != metadata.FilePath;
                if (!fileExists && !sourceExists)
                    stale.insert(handle);
                else if (imported && fileExists && !sourceExists && (isMeshType(metadata.Type) || metadata.Type == AssetType::Texture2D))
                {
                    stale.insert(handle);
                    orphanedFiles.push_back(metadata.FilePath);
                }
            }

            if (existing == 0 && !m_AssetRegistry.empty())
            {
                NOX_CORE_WARN("Asset registry: none of its {} entries has a file in '{}' -- is the asset folder moved? Nothing was removed.",
                              m_AssetRegistry.size(), assetDirectory.generic_string());
                stale.clear();
                orphanedFiles.clear();
            }
            else if (!stale.empty())
            {
                // The generated materials of the meshes that went, unless a mesh that stays generates the same names.
                std::vector<std::pair<std::string, std::string>> gonePrefixes;
                std::vector<std::pair<std::string, std::string>> keptPrefixes;
                for (const auto& [handle, metadata] : m_AssetRegistry)
                {
                    if (isMeshType(metadata.Type))
                        (stale.contains(handle) ? gonePrefixes : keptPrefixes).push_back(materialPrefix(metadata));
                }
                if (!gonePrefixes.empty())
                {
                    for (const auto& [handle, metadata] : m_AssetRegistry)
                    {
                        if (metadata.Type != AssetType::Material || stale.contains(handle))
                            continue;
                        const std::string folder = metadata.FilePath.parent_path().lexically_normal().generic_string();
                        const std::string name = metadata.FilePath.filename().string();
                        auto matches = [&](const std::pair<std::string, std::string>& prefix)
                        {
                            return prefix.first == folder && name.rfind(prefix.second, 0) == 0;
                        };
                        if (std::none_of(gonePrefixes.begin(), gonePrefixes.end(), matches) ||
                            std::any_of(keptPrefixes.begin(), keptPrefixes.end(), matches))
                            continue;
                        stale.insert(handle);
                        if (existsInAssets(metadata.FilePath))
                            orphanedFiles.push_back(metadata.FilePath);
                    }
                }

                std::filesystem::path backup = path;
                backup += ".bak";
                std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, error);
                size_t logged = 0;
                for (AssetHandle handle : stale)
                {
                    if (logged++ < 20)
                        NOX_CORE_INFO("Asset registry: removed '{}' (its source no longer exists)", m_AssetRegistry[handle].FilePath.generic_string());
                    m_AssetRegistry.erase(handle);
                }
                // The orphaned files themselves: only inside the asset folder, with a cooked file's hash beside it.
                size_t deleted = 0;
                for (const std::filesystem::path& relative : orphanedFiles)
                {
                    const std::filesystem::path file = (assetDirectory / relative).lexically_normal();
                    const std::string fileString = file.generic_string();
                    if (fileString.rfind(assetDirectory.lexically_normal().generic_string(), 0) != 0)
                        continue;
                    if (std::filesystem::remove(file, error))
                        ++deleted;
                    std::filesystem::remove(fileString + ".hash", error);
                }
                NOX_CORE_INFO("Asset registry: removed {} entries whose source no longer exists, deleted {} files left from them "
                              "(previous registry kept as {})", stale.size(), deleted, backup.filename().string());
                registryPruned = true;
            }
        }

        m_HandleByPath.clear();
        for (const auto& [indexedHandle, indexedMetadata] : m_AssetRegistry)
            IndexPath(indexedHandle, indexedMetadata);

        if (registryPruned)
            SerializeAssetRegistry();
        return true;
    }
}
