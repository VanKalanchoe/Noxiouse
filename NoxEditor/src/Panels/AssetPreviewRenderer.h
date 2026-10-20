#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/Ref.h"
#include "NRI/Texture.h"

namespace Nox
{
    class Renderer;

    // Rendered previews of assets (docs/Editor_Features_Plan_2026-10.md B1 / B2), drawn by the renderer's extra views
    // (Renderer::SubmitExtraView with preview on). The asset is placed on a preview stage 10 km below the world -- out of every
    // camera's sight, and the preview views trace no shadow rays, so the scene above casts nothing on it -- and a camera framing it
    // from the front right draws it raster only, lit by the scene's lights and environment.
    //  - Thumbnails of meshes, static meshes and materials (on a sphere), one at a time over a few frames, kept as textures
    //    for the session (the Content Browser's tiles) and on disk in the project's Cache/Thumbnails (read back from the GPU),
    //    so the next session shows them at once; one older than its asset's file is drawn again.
    //  - The Material Editor's live sphere, drawn every frame it is shown, so edits show at once.
    class AssetPreviewRenderer
    {
    public:
        static AssetPreviewRenderer& Get();

        // The renderer the previews draw with (the editor's; it outlives the editor layer, which clears this on its way out).
        static void SetRenderer(Renderer* renderer);
        // Where material previews get their sphere (EditorLayer: the Sphere shape asset, made the first time it is needed).
        void SetSphereProvider(std::function<AssetHandle()> provider) { m_SphereProvider = std::move(provider); }
        // Once per frame before ImGui (EditorLayer::OnUpdate): advances the thumbnail being drawn and takes the live preview down
        // when nothing showed it last frame.
        void Update();
        // Drops the GPU instances and textures (the project or scene goes away).
        void Clear();

        static bool CanRender(AssetType type);
        // The rendered thumbnail, null until drawn: asking for it queues it (the ones asked for most recently go first).
        Ref<Texture2D> GetThumbnail(AssetHandle handle);
        // Drawn again on the next request (a material was edited).
        void Invalidate(AssetHandle handle);

        // The material on a sphere, square at `size` pixels, null until the first image is drawn. Call every frame it is shown.
        Texture2D* GetLivePreview(AssetHandle material, uint32_t size);
        // A skeletal mesh posed by a clip at `time` seconds (the Animation Clip window), square at `size` pixels, null until the first
        // image is drawn. Call every frame it is shown.
        Texture2D* GetAnimationPreview(AssetHandle mesh, AssetHandle skeleton, AssetHandle clip, float time, uint32_t size);
        // A skeletal mesh in a pose the caller worked out (skinning matrices, e.g. an animation graph's: the graph editor's
        // preview), square at `size` pixels; the same stage as GetAnimationPreview (one of them a frame). `sources`: assets it uses
        // (kept loaded). Call every frame it is shown.
        Texture2D* GetPosePreview(AssetHandle mesh, const std::vector<glm::mat4>& bones, const std::vector<AssetHandle>& sources, uint32_t size);
        // The assets on the stage right now (EditorLayer::UnloadUnusedAssets keeps them loaded).
        void CollectAssetReferences(std::unordered_set<AssetHandle>& out) const;

    private:
        AssetPreviewRenderer() = default;

        struct Placement
        {
            AssetHandle Asset = 0;
            std::vector<AssetHandle> Assets; // what it draws with (the asset, the sphere, materials)
            std::vector<uint32_t> Instances;
            glm::vec3 Center{ 0.0f };
            float Radius = 1.0f;
        };
        // Puts the asset (a mesh, or a material on the sphere) on the stage at `origin`; false while its assets still load
        // (allowStandIns: the mesh's embedded materials may stand in for .nmat files still loading).
        bool Place(AssetHandle asset, const glm::vec3& origin, Placement& out, bool allowStandIns);
        void Remove(Placement& placement);
        void Submit(uint32_t view, const Placement& placement, uint32_t size, float framing = 1.0f);

        // The thumbnail cache on disk: <project>/Cache/Thumbnails/<handle>.nthumb (width, height, the image's 4-byte texels).
        static std::filesystem::path CachePath(AssetHandle handle);
        // The cached image as a texture, when the file is there and newer than the asset's own file.
        bool LoadCached(AssetHandle handle);
        // Written once the GPU has copied the image back (a few frames later), unless the asset changed meanwhile.
        void SaveCached(AssetHandle handle, Ref<Texture2D> image);

        struct Job
        {
            Placement Placed;
            AssetHandle Asset = 0;
            uint32_t Frames = 0;      // frames since it was placed (drawn in each)
            uint32_t WaitFrames = 0;  // frames waiting for its assets
            bool IsPlaced = false;
        };

        std::function<AssetHandle()> m_SphereProvider;
        std::unordered_map<AssetHandle, Ref<Texture2D>> m_Thumbnails;
        std::unordered_map<AssetHandle, uint64_t> m_LastUsed;   // frame of the last request, for eviction
        std::unordered_map<AssetHandle, uint64_t> m_Requests;   // waiting thumbnails and the frame they were last asked for
        std::unordered_map<AssetHandle, bool> m_Failed;          // could not be drawn (no geometry, never loaded)
        std::unordered_set<AssetHandle> m_DiskChecked;           // looked for on disk already (this session)
        std::vector<AssetHandle> m_DiskQueue;                    // to look for on disk (a few a frame)
        std::unordered_map<AssetHandle, uint32_t> m_Generations; // bumped by Invalidate: a readback of an older image is not saved
        std::optional<Job> m_Job;

        Placement m_Live;
        uint64_t m_LiveRequestedFrame = 0;
        Placement m_Animated;
        std::vector<AssetHandle> m_AnimatedSources; // its skeleton and clip
        std::vector<glm::mat4> m_AnimatedBones;
        uint64_t m_AnimatedRequestedFrame = 0;
        uint64_t m_Frame = 0;
    };
}
