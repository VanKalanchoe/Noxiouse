#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "DataTypes.h"
#include "NRI/AccelerationStructure.h"
#include "NRI/Buffer.h"
#include "NRI/CommandBuffer.h"

namespace NRI
{
    class Device;
}

namespace Nox
{
    // Draw buckets in draw-list order (pipeline, cull mode and blending differ per bucket).
    enum class RenderBucket : uint8_t
    {
        Opaque,
        OpaqueDoubleSided,
        Mask,
        MaskDoubleSided,
        Unlit,
        UnlitDoubleSided,
        Transparent,
        TransparentDoubleSided,
        TransparentUnlit,
        TransparentUnlitDoubleSided,
        Count
    };

    constexpr size_t RenderBucketCount = static_cast<size_t>(RenderBucket::Count);
    static_assert(RenderBucketCount == shaderio::CULL_BUCKET_COUNT);

    // Identity of a shared material record: a material asset (Submesh = AssetMaterial) or the material embedded in a
    // mesh asset's submesh.
    struct GpuMaterialKey
    {
        static constexpr uint32_t AssetMaterial = ~0u;

        uint64_t Asset = 0;
        uint32_t Submesh = AssetMaterial;

        bool operator==(const GpuMaterialKey&) const = default;
    };

    struct GpuMaterialKeyHash
    {
        size_t operator()(const GpuMaterialKey& key) const { return std::hash<uint64_t>()(key.Asset ^ (static_cast<uint64_t>(key.Submesh) * 0x9E3779B97F4A7C15ull)); }
    };

    // One persistent GPU table of fixed-size records (slot index = GPU id). A CPU mirror keeps the records; only slots
    // edited since the last upload are staged and copied into the device buffer by the frame that stages them (§5.5.2).
    template <typename Record>
    class GpuSceneTable
    {
    public:
        void Initialize(NRI::Device& device, uint32_t framesInFlight);

        uint32_t Allocate();
        void Free(uint32_t slot);
        // Tables that share another table's slots (transforms follow instances).
        void Grow(uint32_t count);
        const Record& Get(uint32_t slot) const { return m_Records[slot]; }
        // Marks the slot for upload.
        Record& Edit(uint32_t slot);

        uint64_t GetDeviceAddress() const { return m_Buffer ? m_Buffer->getDeviceAddress() : 0; }
        NRI::Buffer* GetBuffer() const { return m_Buffer.get(); }
        NRI::Buffer* GetStagingBuffer(uint32_t frameSlot) const { return m_Staging[frameSlot].Buffer.get(); }

        // Main thread before recording. Grows the device buffer when needed (the old one is handed back for deferred
        // release and every record is uploaded again), then stages this frame's edited records.
        void PrepareUpload(uint32_t frameSlot, std::vector<std::unique_ptr<NRI::Buffer>>& outReleased);
        bool HasUpload(uint32_t frameSlot) const { return !m_Staging[frameSlot].Regions.empty(); }
        void RecordUpload(NRI::CommandBuffer& cmd, uint32_t frameSlot) const;

    private:
        struct Staging
        {
            std::unique_ptr<NRI::Buffer> Buffer;
            uint64_t Capacity = 0; // records
            std::vector<NRI::BufferCopyRegion> Regions;
        };

    private:
        NRI::Device* m_Device = nullptr;
        std::vector<Record> m_Records;
        std::vector<uint8_t> m_Dirty;
        std::vector<uint32_t> m_DirtySlots;
        std::vector<uint32_t> m_FreeSlots;
        std::unique_ptr<NRI::Buffer> m_Buffer;
        uint64_t m_Capacity = 0; // records
        std::vector<Staging> m_Staging;
    };

    // Ray tracing culling (§5.14, Unreal's ray tracing culling radius and angle): an instance farther than Radius whose bounds look
    // smaller than MinAngle from the camera is left out of the TLAS -- far small things add traversal cost and memory for little
    // light. Everything within the radius, and anything large enough, stays, in front of the camera or behind it (reflections and
    // GI see behind).
    struct TlasCulling
    {
        bool Enabled = true;
        glm::vec3 Camera{ 0.0f };
        float Radius = 0.0f;   // world units
        float MinAngle = 0.0f; // radians, the bounding sphere's angular diameter
    };

    // The renderer's persistent scene (§5.5): instances (one per mesh entity submesh) with their transforms, shared
    // materials and meshes, draw buckets and the TLAS instance records. Changed only through this API, so uploads and
    // acceleration structure work follow what changed. Main thread.
    class GpuScene
    {
    public:
        static constexpr uint32_t InvalidSlot = ~0u;
        static constexpr uint32_t NoBoneMatrices = 0xFFFFFFFF;

        void Initialize(NRI::Device& device, uint32_t framesInFlight);

        // Meshes: one per uploaded submesh. blasAddress 0: not ray traced.
        uint32_t AddMesh(const shaderio::GpuMesh& mesh, uint64_t blasAddress);
        // The mesh's BLAS is built (it is ray traced from the next TLAS on).
        void SetMeshBlas(uint32_t meshSlot, uint64_t blasAddress);
        // Where the geometry streams live now: the ray tracing records hold absolute addresses (the hit lookups read one
        // flat record), so they are rewritten whenever a stream that grew moved its contents.
        void SetGeometryBases(uint64_t vertexBase, uint64_t indexBase);
        // Instances still using the mesh stop drawing and tracing; their slots are reported so the owner registers them
        // again (with the reloaded mesh).
        void RemoveMesh(uint32_t meshSlot, std::vector<uint32_t>& outDeactivatedInstances);

        // Materials: shared by key and referenced by instances; released with their last instance.
        uint32_t FindMaterial(const GpuMaterialKey& key) const;
        // The new material has no reference until an instance uses it.
        uint32_t AddMaterial(const GpuMaterialKey& key, const shaderio::GpuMaterial& material);
        void UpdateMaterial(uint32_t materialSlot, const shaderio::GpuMaterial& material);
        void ForEachMaterial(const std::function<void(uint32_t materialSlot, const GpuMaterialKey& key)>& function) const;

        // Instances. AddInstance references the material, RemoveInstance releases it.
        uint32_t AddInstance(uint32_t meshSlot, uint32_t materialSlot, const glm::mat4& world, int32_t entityID);
        void RemoveInstance(uint32_t instanceSlot);
        void RemoveAllInstances();
        void SetTransform(uint32_t instanceSlot, const glm::mat4& world);
        void SetBoneMatrixOffset(uint32_t instanceSlot, uint32_t boneMatrixOffset);
        void SetSkinnedVertexOffset(uint32_t instanceSlot, uint32_t skinnedVertexOffset);
        // A skinned instance owns a dynamic BLAS while ray tracing is active. A zero override restores its mesh's
        // shared static BLAS; the optional skinned vertex address is used only by hit shading.
        void SetInstanceRayTracingGeometry(uint32_t instanceSlot, uint64_t blasAddressOverride, uint64_t skinnedVertexAddress);
        int32_t GetInstanceEntity(uint32_t instanceSlot) const { return m_Instances.Get(instanceSlot).entityID; }
        uint32_t GetInstanceCount() const { return m_InstanceCount; }

        // Per frame, main thread before recording.
        // The draw list every view culls: instance slots in bucket order (transparent buckets back to front), outBucketStarts
        // the entry range of each bucket. Rebuilt when bucket membership changed; otherwise only the transparent entries are
        // re-sorted. Returns whether the list changed.
        bool UpdateDrawList(const glm::vec3& cameraPosition, std::vector<uint32_t>& drawList, std::array<uint32_t, RenderBucketCount + 1>& outBucketStarts);
        // Settles the previous transforms of instances that stopped moving, then stages every table's changes.
        void PrepareUploads(uint32_t frameSlot, std::vector<std::unique_ptr<NRI::Buffer>>& outReleased);
        bool HasUploads(uint32_t frameSlot) const;
        void RecordUploads(NRI::CommandBuffer& cmd, uint32_t frameSlot) const;

        const GpuSceneTable<shaderio::GpuInstance>& GetInstances() const { return m_Instances; }
        const GpuSceneTable<shaderio::GpuTransform>& GetTransforms() const { return m_Transforms; }
        const GpuSceneTable<shaderio::GpuMaterial>& GetMaterials() const { return m_Materials; }
        const GpuSceneTable<shaderio::GpuMesh>& GetMeshes() const { return m_Meshes; }
        // Device bytes of the tables (the memory category the scene owns).
        uint64_t GetDeviceBytes() const;
        const GpuSceneTable<shaderio::GpuRayTracingInstance>& GetRayTracingInstances() const { return m_RayTracingInstances; }

        // Ray traced instances (all render buckets with a BLAS; instanceCustomIndex = instance slot). Opaque geometry
        // skips any-hit, while mask/blend geometry uses the alpha-aware hit group. The TLAS is compacted in bucket order.
        uint32_t GetTlasInstanceCount() const { return m_TlasInstanceCount; }
        // Once a frame before the TLAS is prepared: which ray traced instances culling leaves out (a change rebuilds the TLAS), and
        // which meshes ray tracing needs this frame (an instance in a traced bucket that is not culled, with or without its BLAS).
        // A culled instance comes back within 90% of the radius or above 110% of the angle (no rebuild after rebuild at the edge).
        void UpdateTlasCulling(const TlasCulling& culling);
        // The instances the TLAS is built from (ray traced, not culled), and those culling leaves out.
        uint32_t GetTlasBuildCount() const { return m_TlasInstanceCount - m_TlasCulledCount; }
        uint32_t GetTlasCulledCount() const { return m_TlasCulledCount; }
        // The last UpdateTlasCulling call that needed the mesh for ray tracing, and the current one (BLAS eviction).
        uint64_t GetMeshLastTracedFrame(uint32_t meshSlot) const { return meshSlot < m_MeshLastTraced.size() ? m_MeshLastTraced[meshSlot] : 0; }
        uint64_t GetCullingFrame() const { return m_CullingFrame; }
        // The build's instances: GetTlasBuildCount of them.
        void WriteTlasInstances(std::span<NRI::AccelerationStructureInstance> outInstances) const;
        // Whether any ray traced instance was added, removed, moved or re-flagged since the last call (the TLAS is rebuilt,
        // not refitted: NVIDIA RT best practices).
        bool ConsumeTlasChanged() { return std::exchange(m_TlasChanged, false); }
        // The ray traced instances whose material emits light, in slot order, for the path tracer to sample as lights
        // (shaderIO.h EmissiveLightInstance). Returns the sum of their weights.
        float CollectEmissiveLights(std::vector<shaderio::EmissiveLightInstance>& outLights) const;

    private:
        struct InstanceState
        {
            uint32_t Mesh = InvalidSlot;     // InvalidSlot: deactivated (mesh removed) or free
            uint32_t Material = InvalidSlot; // InvalidSlot: free
            RenderBucket Bucket = RenderBucket::Count; // Count: not drawn
            uint32_t BucketIndex = 0;
            uint64_t LastMovedFrame = ~0ull;
            uint64_t BlasAddressOverride = 0;
            uint64_t SkinnedVertexAddress = 0;
            // The world bounding sphere of its mesh's box (center, radius), kept with its transform: the ray tracing culling reads
            // it every frame for every instance (Unreal keeps a primitive's world bounds the same way).
            glm::vec4 WorldSphere{ 0.0f };
        };

        void InsertIntoBucket(uint32_t instanceSlot);
        void RemoveFromBucket(uint32_t instanceSlot);
        void WriteTlasInstance(uint32_t instanceSlot);
        // The instance's ray tracing record from its mesh, material and transform.
        void WriteRayTracingInstance(uint32_t instanceSlot);
        void ReleaseMaterial(uint32_t materialSlot);

    private:
        GpuSceneTable<shaderio::GpuInstance> m_Instances;
        GpuSceneTable<shaderio::GpuTransform> m_Transforms;
        GpuSceneTable<shaderio::GpuMaterial> m_Materials;
        GpuSceneTable<shaderio::GpuMesh> m_Meshes;
        GpuSceneTable<shaderio::GpuRayTracingInstance> m_RayTracingInstances; // same slots as m_Instances

        std::vector<InstanceState> m_InstanceStates;
        uint32_t m_InstanceCount = 0;
        std::array<std::vector<uint32_t>, RenderBucketCount> m_Buckets;
        bool m_BucketsChanged = true;
        std::vector<std::pair<float, uint32_t>> m_TransparentSort;

        std::vector<uint32_t> m_MaterialReferences;
        std::vector<GpuMaterialKey> m_MaterialKeys;
        std::unordered_map<GpuMaterialKey, uint32_t, GpuMaterialKeyHash> m_MaterialByKey;

        std::vector<uint64_t> m_MeshBlasAddresses;
        uint64_t m_GeometryVertexBase = 0;
        uint64_t m_GeometryIndexBase = 0;

        std::vector<NRI::AccelerationStructureInstance> m_TlasInstances; // by instance slot, inactive: no BLAS reference
        uint32_t m_TlasInstanceCount = 0;
        bool m_TlasChanged = false;
        std::vector<uint8_t> m_TlasCulled; // by instance slot
        uint32_t m_TlasCulledCount = 0;    // culled instances with a TLAS record
        std::vector<uint64_t> m_MeshLastTraced; // by mesh slot
        uint64_t m_CullingFrame = 0;

        // Per-object motion: an instance moved in frame N keeps the world of N-1 as its previous transform; once it stops
        // moving, its previous transform is settled to its world.
        uint64_t m_Frame = 0;
        std::vector<uint32_t> m_MovedInstances;
    };
}
