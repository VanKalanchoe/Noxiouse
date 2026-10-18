#include "AssetPreviewRenderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

#include "NoxCore/Animation/AnimPose.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t ThumbnailSize = 192;
        // Frames a thumbnail is drawn before its image is kept: the GPU scene upload, then materials and textures settling.
        constexpr uint32_t SettleFrames = 8;
        // Waiting for an asset to load: after this many frames a mesh is drawn with its embedded materials standing in for .nmat
        // files still loading; after the second, the asset is given up (its tile keeps the file icon).
        constexpr uint32_t StandInFrames = 120;
        constexpr uint32_t GiveUpFrames = 600;
        constexpr size_t MaxThumbnails = 512;
        // Cached thumbnails uploaded per frame (each a small synchronous upload).
        constexpr size_t DiskLoadsPerFrame = 4;
        constexpr char CacheMagic[4] = { 'N', 'T', 'H', '1' };

        // The preview stage: 10 km below the world's origin (whatever the project's unit), thumbnails and the live preview 2 km apart.
        glm::vec3 ThumbnailStage() { return glm::vec3(0.0f, -WorldUnits::FromMeters(10000.0f), 0.0f); }
        glm::vec3 LiveStage() { return glm::vec3(WorldUnits::FromMeters(2000.0f), -WorldUnits::FromMeters(10000.0f), 0.0f); }
        glm::vec3 AnimationStage() { return glm::vec3(WorldUnits::FromMeters(4000.0f), -WorldUnits::FromMeters(10000.0f), 0.0f); }

        Renderer* GetRenderer() { return Application::Get().GetRenderer(); }
    }

    AssetPreviewRenderer& AssetPreviewRenderer::Get()
    {
        static AssetPreviewRenderer instance;
        return instance;
    }

    bool AssetPreviewRenderer::CanRender(AssetType type)
    {
        return type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::Material;
    }

    Ref<Texture2D> AssetPreviewRenderer::GetThumbnail(AssetHandle handle)
    {
        if (handle == 0)
            return nullptr;
        m_LastUsed[handle] = m_Frame;
        if (auto found = m_Thumbnails.find(handle); found != m_Thumbnails.end())
            return found->second;
        // Drawn in an earlier session: read from the cache on disk first (Update), drawn only when it is not there.
        if (m_DiskChecked.insert(handle).second)
        {
            m_DiskQueue.push_back(handle);
            return nullptr;
        }
        if (!m_Failed.contains(handle))
            m_Requests[handle] = m_Frame;
        return nullptr;
    }

    std::filesystem::path AssetPreviewRenderer::CachePath(AssetHandle handle)
    {
        return Project::GetProjectDirectory() / "Cache" / "Thumbnails" / (std::to_string(static_cast<uint64_t>(handle)) + ".nthumb");
    }

    bool AssetPreviewRenderer::LoadCached(AssetHandle handle)
    {
        Renderer* renderer = GetRenderer();
        if (!renderer || !Project::GetActive() || !AssetManager::IsAssetHandleValid(handle))
            return false;
        const std::filesystem::path path = CachePath(handle);
        const std::filesystem::path assetPath = Project::GetActiveAssetDirectory() / AssetManager::GetMetadata(handle).FilePath;
        std::error_code error;
        const auto cachedTime = std::filesystem::last_write_time(path, error);
        if (error)
            return false;
        const auto assetTime = std::filesystem::last_write_time(assetPath, error);
        if (error || cachedTime < assetTime)
            return false;

        std::ifstream file(path, std::ios::binary);
        char magic[4] = {};
        uint32_t width = 0, height = 0;
        file.read(magic, sizeof(magic));
        file.read(reinterpret_cast<char*>(&width), sizeof(width));
        file.read(reinterpret_cast<char*>(&height), sizeof(height));
        if (!file || std::memcmp(magic, CacheMagic, sizeof(magic)) != 0 || width == 0 || height == 0 || width > 1024 || height > 1024)
            return false;
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
        file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!file)
            return false;

        // The extra views' image format (the surface's), the order the texels were read back in.
        TextureData data;
        data.Width = width;
        data.Height = height;
        data.MipLevels = 1;
        data.Format = NRI::ImageFormat::Surface;
        data.DirectFormat = UINT32_MAX;
        data.Data.Data = pixels.data();
        data.Data.Size = pixels.size();
        data.MipOffsets = { 0 };
        Ref<Texture2D> texture = renderer->UploadTexture(data);
        if (!texture)
            return false;
        m_Thumbnails[handle] = texture;
        return true;
    }

    void AssetPreviewRenderer::SaveCached(AssetHandle handle, Ref<Texture2D> image)
    {
        Renderer* renderer = GetRenderer();
        if (!renderer || !image || !Project::GetActive())
            return;
        const std::filesystem::path path = CachePath(handle);
        const uint32_t generation = m_Generations[handle];
        renderer->ReadbackTexture(std::move(image), [path, handle, generation](std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
        {
            AssetPreviewRenderer& previews = AssetPreviewRenderer::Get();
            if (pixels.size() != static_cast<size_t>(width) * height * 4 || previews.m_Generations[handle] != generation)
                return;
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(CacheMagic, sizeof(CacheMagic));
            file.write(reinterpret_cast<const char*>(&width), sizeof(width));
            file.write(reinterpret_cast<const char*>(&height), sizeof(height));
            file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        });
    }

    void AssetPreviewRenderer::Invalidate(AssetHandle handle)
    {
        // ImGui frames still in flight may show the old image: released once they are done.
        if (auto found = m_Thumbnails.find(handle); found != m_Thumbnails.end())
        {
            Renderer::DeferAssetRelease(found->second);
            m_Thumbnails.erase(found);
        }
        m_Failed.erase(handle);
        // The cached image is out of date too (and one still being read back is not saved).
        ++m_Generations[handle];
        m_DiskChecked.insert(handle);
        if (Project::GetActive())
        {
            std::error_code error;
            std::filesystem::remove(CachePath(handle), error);
        }
    }

    Texture2D* AssetPreviewRenderer::GetLivePreview(AssetHandle material, uint32_t size)
    {
        Renderer* renderer = GetRenderer();
        if (!renderer || material == 0 || size == 0)
            return nullptr;

        // One live sphere: a second Material Editor shown in the same frame gets its material's thumbnail instead.
        if (m_LiveRequestedFrame == m_Frame && m_Live.Asset != 0 && m_Live.Asset != material)
            return GetThumbnail(material).get();

        m_LiveRequestedFrame = m_Frame;
        if (m_Live.Asset != material)
        {
            Remove(m_Live);
            renderer->ReleaseExtraView(Renderer::LivePreviewView);
            if (!Place(material, LiveStage(), m_Live, true))
                return nullptr;
        }
        Submit(Renderer::LivePreviewView, m_Live, size);
        return renderer->GetExtraViewTexture(Renderer::LivePreviewView);
    }

    Texture2D* AssetPreviewRenderer::GetAnimationPreview(AssetHandle mesh, AssetHandle skeleton, AssetHandle clip, float time, uint32_t size)
    {
        Renderer* renderer = GetRenderer();
        if (!renderer || mesh == 0 || size == 0)
            return nullptr;

        m_AnimatedRequestedFrame = m_Frame;
        if (m_Animated.Asset != mesh)
        {
            Remove(m_Animated);
            renderer->ReleaseExtraView(Renderer::AnimationPreviewView);
            if (!Place(mesh, AnimationStage(), m_Animated, true))
                return nullptr;
        }
        m_AnimatedSources = { skeleton, clip };

        // The clip's pose at `time`, skinned like an Animator without joint entities does (bone matrices in the mesh's space); the
        // rest pose until the skeleton and the clip have loaded.
        const Skeleton* loadedSkeleton = skeleton != 0 && AssetManager::RequestAsset(skeleton) == AssetState::Ready ? AssetManager::FindLoadedAsset<Skeleton>(skeleton) : nullptr;
        const AnimationSequence* loadedClip = clip != 0 && AssetManager::RequestAsset(clip) == AssetState::Ready ? AssetManager::FindLoadedAsset<AnimationSequence>(clip) : nullptr;
        if (loadedSkeleton && loadedClip && !loadedSkeleton->AllNodes.empty())
        {
            AnimPose pose;
            SampleClipPose(*loadedClip, time, *loadedSkeleton, pose);
            ApplyPoseToSkeleton(pose, *loadedSkeleton, m_AnimatedBones);
            renderer->SetMeshInstancesBones(m_Animated.Instances, m_AnimatedBones);
        }
        else
            renderer->SetMeshInstancesBones(m_Animated.Instances, {});

        // Framed a little wider than the rest pose's bounds: the animation moves out of them.
        Submit(Renderer::AnimationPreviewView, m_Animated, size, 1.3f);
        return renderer->GetExtraViewTexture(Renderer::AnimationPreviewView);
    }

    void AssetPreviewRenderer::CollectAssetReferences(std::unordered_set<AssetHandle>& out) const
    {
        auto add = [&](const Placement& placement)
        {
            for (AssetHandle asset : placement.Assets)
                out.insert(asset);
        };
        if (m_Job)
            add(m_Job->Placed);
        add(m_Live);
        if (m_Animated.Asset != 0)
        {
            add(m_Animated);
            for (AssetHandle asset : m_AnimatedSources)
                out.insert(asset);
        }
    }

    void AssetPreviewRenderer::Update()
    {
        ++m_Frame;
        Renderer* renderer = GetRenderer();
        if (!renderer)
            return;

        // The live preview is drawn while a Material Editor shows it; it goes when none did last frame.
        if (m_Live.Asset != 0 && m_LiveRequestedFrame + 1 < m_Frame)
        {
            Remove(m_Live);
            renderer->ReleaseExtraView(Renderer::LivePreviewView);
        }
        // The animation preview likewise. It goes before this frame's renderer update reads its bone matrices: they are set
        // again every frame it is shown, and a skinned instance must not keep last frame's offset into them.
        if (m_Animated.Asset != 0 && m_AnimatedRequestedFrame + 1 < m_Frame)
        {
            Remove(m_Animated);
            m_AnimatedSources.clear();
            renderer->ReleaseExtraView(Renderer::AnimationPreviewView);
        }

        // Thumbnails from the cache on disk, a few a frame; what is not there (or is older than its asset) is drawn.
        for (size_t loads = 0; loads < DiskLoadsPerFrame && !m_DiskQueue.empty(); ++loads)
        {
            const AssetHandle asset = m_DiskQueue.back();
            m_DiskQueue.pop_back();
            if (m_Thumbnails.contains(asset))
                continue;
            if (LoadCached(asset))
                m_Requests.erase(asset);
            else if (!m_Failed.contains(asset))
                m_Requests[asset] = m_Frame;
        }

        if (!m_Job)
        {
            // The thumbnail asked for most recently (the tiles on screen now) goes first.
            auto next = m_Requests.end();
            for (auto it = m_Requests.begin(); it != m_Requests.end(); ++it)
            {
                if (next == m_Requests.end() || it->second > next->second)
                    next = it;
            }
            if (next == m_Requests.end())
                return;
            const AssetHandle asset = next->first;
            m_Requests.erase(next);
            if (m_Thumbnails.contains(asset) || !AssetManager::IsAssetHandleValid(asset) || !CanRender(AssetManager::GetAssetType(asset)))
                return;
            m_Job = Job{};
            m_Job->Asset = asset;
        }

        Job& job = *m_Job;
        if (!job.IsPlaced)
        {
            if (!Place(job.Asset, ThumbnailStage(), job.Placed, job.WaitFrames > StandInFrames))
            {
                if (++job.WaitFrames > GiveUpFrames)
                {
                    m_Failed[job.Asset] = true;
                    m_Job.reset();
                }
                return;
            }
            job.IsPlaced = true;
        }

        if (job.Frames < SettleFrames)
        {
            Submit(Renderer::ThumbnailView, job.Placed, ThumbnailSize);
            ++job.Frames;
            return;
        }

        // Drawn for long enough: the image is the thumbnail, the stage is cleared for the next one.
        Ref<Texture2D> image = renderer->TakeExtraViewTexture(Renderer::ThumbnailView);
        Remove(job.Placed);
        if (image)
        {
            m_Thumbnails[job.Asset] = image;
            SaveCached(job.Asset, image);
            if (m_Thumbnails.size() > MaxThumbnails)
            {
                // The one not asked for the longest goes (its tile draws it again when it is back on screen).
                auto oldest = m_Thumbnails.end();
                for (auto it = m_Thumbnails.begin(); it != m_Thumbnails.end(); ++it)
                {
                    if (it->first == job.Asset)
                        continue;
                    if (oldest == m_Thumbnails.end() || m_LastUsed[it->first] < m_LastUsed[oldest->first])
                        oldest = it;
                }
                if (oldest != m_Thumbnails.end())
                {
                    Renderer::DeferAssetRelease(oldest->second);
                    m_DiskChecked.erase(oldest->first); // back from the disk cache when it is asked for again
                    m_Thumbnails.erase(oldest);
                }
            }
        }
        m_Job.reset();
    }

    void AssetPreviewRenderer::Clear()
    {
        Remove(m_Live);
        Remove(m_Animated);
        m_AnimatedSources.clear();
        if (m_Job)
            Remove(m_Job->Placed);
        m_Job.reset();
        for (auto& [handle, image] : m_Thumbnails)
            Renderer::DeferAssetRelease(image);
        m_Thumbnails.clear();
        m_Requests.clear();
        m_Failed.clear();
        m_LastUsed.clear();
        m_DiskChecked.clear();
        m_DiskQueue.clear();
        if (Renderer* renderer = GetRenderer())
        {
            renderer->ReleaseExtraView(Renderer::ThumbnailView);
            renderer->ReleaseExtraView(Renderer::LivePreviewView);
            renderer->ReleaseExtraView(Renderer::AnimationPreviewView);
        }
    }

    bool AssetPreviewRenderer::Place(AssetHandle asset, const glm::vec3& origin, Placement& out, bool allowStandIns)
    {
        Renderer* renderer = GetRenderer();
        if (!renderer || !AssetManager::IsAssetHandleValid(asset))
            return false;

        // A material shows on the sphere; a mesh is itself.
        const AssetType type = AssetManager::GetAssetType(asset);
        MeshComponent mesh;
        mesh.SubmeshCount = UINT32_MAX;
        if (type == AssetType::Material)
        {
            mesh.Mesh = m_SphereProvider ? m_SphereProvider() : AssetHandle(0);
            if (mesh.Mesh == 0 || AssetManager::RequestAsset(asset) != AssetState::Ready)
                return false;
        }
        else
            mesh.Mesh = asset;
        if (AssetManager::RequestAsset(mesh.Mesh) != AssetState::Ready)
            return false;

        // Its local bounds (the stage puts it unrotated at the origin given).
        glm::vec3 lowest(FLT_MAX), highest(-FLT_MAX);
        size_t submeshCount = 0;
        auto gather = [&](const auto& loaded)
        {
            submeshCount = loaded.GetSubMeshCount();
            for (size_t index = 0; index < submeshCount; ++index)
            {
                const MeshHandle& submesh = loaded.GetSubMesh(index);
                if (!submesh.hasBounds)
                    continue;
                lowest = glm::min(lowest, submesh.boundsMin);
                highest = glm::max(highest, submesh.boundsMax);
            }
        };
        if (AssetManager::GetAssetType(mesh.Mesh) == AssetType::StaticMesh)
        {
            const StaticMesh* loaded = AssetManager::FindLoadedAsset<StaticMesh>(mesh.Mesh);
            if (!loaded)
                return false;
            gather(*loaded);
        }
        else
        {
            const Mesh* loaded = AssetManager::FindLoadedAsset<Mesh>(mesh.Mesh);
            if (!loaded)
                return false;
            gather(*loaded);
        }
        if (lowest.x > highest.x)
        {
            lowest = glm::vec3(-WorldUnits::FromMeters(0.5f));
            highest = glm::vec3(WorldUnits::FromMeters(0.5f));
        }

        MaterialComponent materials;
        if (type == AssetType::Material)
            materials.MaterialAssets.assign(std::max<size_t>(submeshCount, 1), asset);

        Placement placement;
        std::vector<AssetHandle> missing;
        renderer->AddMeshInstances(glm::translate(glm::mat4(1.0f), origin), mesh, type == AssetType::Material ? &materials : nullptr, -1,
                                   placement.Instances, missing);
        if (!missing.empty() && !allowStandIns)
        {
            // The mesh's own materials are still loading: wait for them rather than show the stand-ins.
            for (AssetHandle pending : missing)
                AssetManager::RequestAsset(pending);
            renderer->RemoveMeshInstances(placement.Instances);
            return false;
        }

        placement.Asset = asset;
        placement.Assets = { asset, mesh.Mesh };
        placement.Assets.insert(placement.Assets.end(), missing.begin(), missing.end());
        placement.Center = origin + (lowest + highest) * 0.5f;
        placement.Radius = std::max(glm::length(highest - lowest) * 0.5f, WorldUnits::FromMeters(0.001f));
        out = std::move(placement);
        return true;
    }

    void AssetPreviewRenderer::Remove(Placement& placement)
    {
        if (Renderer* renderer = GetRenderer(); renderer && !placement.Instances.empty())
            renderer->RemoveMeshInstances(placement.Instances);
        placement = Placement{};
    }

    void AssetPreviewRenderer::Submit(uint32_t view, const Placement& placement, uint32_t size, float framing)
    {
        Renderer* renderer = GetRenderer();
        if (!renderer)
            return;

        // From the front right and a little above, framed so the bounds' sphere fills the 30 degree view.
        constexpr float FieldOfView = 30.0f;
        const float distance = placement.Radius * framing / std::sin(glm::radians(FieldOfView * 0.5f)) * 1.08f;
        const float nearClip = std::min(WorldUnits::FromMeters(0.01f), distance * 0.1f);
        EditorCamera camera(FieldOfView, 1.0f, nearClip, distance * 4.0f);
        camera.SetViewportSize(static_cast<float>(size), static_cast<float>(size));
        camera.SetFocalPoint(placement.Center);
        camera.SetDistance(distance);
        camera.SetOrientation(glm::radians(22.0f), glm::radians(-35.0f));
        renderer->SubmitExtraView(view, camera, NRI::Extent2D{ size, size }, nullptr, true);
    }
}
