#include "GpuScene.h"

#include <algorithm>
#include <cstring>

#include "NRI/Device.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t MinimumTableCapacity = 64;

        RenderBucket SelectRenderBucket(const shaderio::GpuMaterial& material)
        {
            const bool doubleSided = material.doubleSided != 0;
            const bool unlit = material.unlit != 0;

            // A closed, single-sided translucent shape needs its backface culled like the opaque buckets do; otherwise
            // both hemispheres blend on top of each other (meshlets are not depth-sorted), so transparent buckets respect
            // doubleSided too.
            if (material.alphaMode == static_cast<uint32_t>(AlphaMode::Blend))
            {
                if (unlit)
                    return doubleSided ? RenderBucket::TransparentUnlitDoubleSided : RenderBucket::TransparentUnlit;
                return doubleSided ? RenderBucket::TransparentDoubleSided : RenderBucket::Transparent;
            }

            // Opaque and alpha-mask unlit materials share the unlit buckets.
            if (unlit)
                return doubleSided ? RenderBucket::UnlitDoubleSided : RenderBucket::Unlit;
            if (material.alphaMode == static_cast<uint32_t>(AlphaMode::Mask))
                return doubleSided ? RenderBucket::MaskDoubleSided : RenderBucket::Mask;
            return doubleSided ? RenderBucket::OpaqueDoubleSided : RenderBucket::Opaque;
        }

        bool IsTransparentBucket(RenderBucket bucket)
        {
            return bucket >= RenderBucket::Transparent && bucket < RenderBucket::Count;
        }

        // Ray tracing sees opaque and alpha-mask geometry.
        bool IsRayTracedBucket(RenderBucket bucket)
        {
            return bucket <= RenderBucket::MaskDoubleSided;
        }

        bool SameBucketInputs(const shaderio::GpuMaterial& a, const shaderio::GpuMaterial& b)
        {
            return a.alphaMode == b.alphaMode && a.unlit == b.unlit && a.doubleSided == b.doubleSided;
        }
    }

    template <typename Record>
    void GpuSceneTable<Record>::Initialize(NRI::Device& device, uint32_t framesInFlight)
    {
        m_Device = &device;
        m_Staging.resize(framesInFlight);
    }

    template <typename Record>
    uint32_t GpuSceneTable<Record>::Allocate()
    {
        if (!m_FreeSlots.empty())
        {
            const uint32_t slot = m_FreeSlots.back();
            m_FreeSlots.pop_back();
            return slot;
        }

        const uint32_t slot = static_cast<uint32_t>(m_Records.size());
        Grow(slot + 1);
        return slot;
    }

    template <typename Record>
    void GpuSceneTable<Record>::Free(uint32_t slot)
    {
        m_FreeSlots.push_back(slot);
    }

    template <typename Record>
    void GpuSceneTable<Record>::Grow(uint32_t count)
    {
        if (count <= m_Records.size())
            return;
        m_Records.resize(count);
        m_Dirty.resize(count, 0);
    }

    template <typename Record>
    Record& GpuSceneTable<Record>::Edit(uint32_t slot)
    {
        if (!m_Dirty[slot])
        {
            m_Dirty[slot] = 1;
            m_DirtySlots.push_back(slot);
        }
        return m_Records[slot];
    }

    template <typename Record>
    void GpuSceneTable<Record>::PrepareUpload(uint32_t frameSlot, std::vector<std::unique_ptr<NRI::Buffer>>& outReleased)
    {
        Staging& staging = m_Staging[frameSlot];
        staging.Regions.clear();

        if (m_Records.size() > m_Capacity)
        {
            // Frames in flight keep reading the old buffer (their uniforms hold its address).
            if (m_Buffer)
                outReleased.push_back(std::move(m_Buffer));
            m_Capacity = std::max<uint64_t>({ MinimumTableCapacity, m_Records.size(), m_Capacity * 2 });
            m_Buffer = m_Device->createBuffer(NRI::BufferDesc{
                .size = sizeof(Record) * m_Capacity,
                .usage = NRI::BufferUsage::Storage
            });

            m_DirtySlots.clear();
            for (uint32_t slot = 0; slot < m_Records.size(); ++slot)
            {
                m_Dirty[slot] = 1;
                m_DirtySlots.push_back(slot);
            }
        }

        if (m_DirtySlots.empty())
            return;

        // This slot's previous frame has finished (the frame slot was acquired), so its staging buffer is free.
        if (staging.Capacity < m_DirtySlots.size())
        {
            staging.Capacity = std::max<uint64_t>({ MinimumTableCapacity, m_DirtySlots.size(), staging.Capacity * 2 });
            staging.Buffer = m_Device->createBuffer(NRI::BufferDesc{
                .size = sizeof(Record) * staging.Capacity,
                .usage = NRI::BufferUsage::Staging
            });
        }

        // Sorted slots merge into contiguous copy regions.
        std::sort(m_DirtySlots.begin(), m_DirtySlots.end());

        const uint64_t byteCount = sizeof(Record) * m_DirtySlots.size();
        auto* mapped = static_cast<Record*>(staging.Buffer->map(0, byteCount));
        for (size_t index = 0; index < m_DirtySlots.size(); ++index)
        {
            const uint32_t slot = m_DirtySlots[index];
            mapped[index] = m_Records[slot];
            m_Dirty[slot] = 0;

            if (index > 0 && slot == m_DirtySlots[index - 1] + 1)
            {
                staging.Regions.back().size += sizeof(Record);
                continue;
            }
            staging.Regions.push_back({
                .srcOffset = sizeof(Record) * index,
                .dstOffset = sizeof(Record) * slot,
                .size = sizeof(Record)
            });
        }
        staging.Buffer->unmap();
        m_DirtySlots.clear();
    }

    template <typename Record>
    void GpuSceneTable<Record>::RecordUpload(NRI::CommandBuffer& cmd, uint32_t frameSlot) const
    {
        const Staging& staging = m_Staging[frameSlot];
        if (!staging.Regions.empty())
            cmd.copyBuffer(*staging.Buffer, *m_Buffer, std::span<const NRI::BufferCopyRegion>(staging.Regions));
    }

    template class GpuSceneTable<shaderio::GpuInstance>;
    template class GpuSceneTable<shaderio::GpuTransform>;
    template class GpuSceneTable<shaderio::GpuMaterial>;
    template class GpuSceneTable<shaderio::GpuMesh>;
    template class GpuSceneTable<shaderio::GpuRayTracingInstance>;

    void GpuScene::Initialize(NRI::Device& device, uint32_t framesInFlight)
    {
        m_Instances.Initialize(device, framesInFlight);
        m_Transforms.Initialize(device, framesInFlight);
        m_Materials.Initialize(device, framesInFlight);
        m_Meshes.Initialize(device, framesInFlight);
        m_RayTracingInstances.Initialize(device, framesInFlight);
    }

    uint32_t GpuScene::AddMesh(const shaderio::GpuMesh& mesh, uint64_t blasAddress)
    {
        const uint32_t slot = m_Meshes.Allocate();
        m_Meshes.Edit(slot) = mesh;
        if (m_MeshBlasAddresses.size() <= slot)
            m_MeshBlasAddresses.resize(slot + 1, 0);
        m_MeshBlasAddresses[slot] = blasAddress;
        return slot;
    }

    void GpuScene::RemoveMesh(uint32_t meshSlot, std::vector<uint32_t>& outDeactivatedInstances)
    {
        for (uint32_t instanceSlot = 0; instanceSlot < m_InstanceStates.size(); ++instanceSlot)
        {
            InstanceState& state = m_InstanceStates[instanceSlot];
            if (state.Mesh != meshSlot)
                continue;

            RemoveFromBucket(instanceSlot);
            state.Mesh = InvalidSlot;
            WriteTlasInstance(instanceSlot);
            outDeactivatedInstances.push_back(instanceSlot);
        }

        m_MeshBlasAddresses[meshSlot] = 0;
        m_Meshes.Free(meshSlot);
    }

    uint32_t GpuScene::FindMaterial(const GpuMaterialKey& key) const
    {
        auto found = m_MaterialByKey.find(key);
        return found != m_MaterialByKey.end() ? found->second : InvalidSlot;
    }

    uint32_t GpuScene::AddMaterial(const GpuMaterialKey& key, const shaderio::GpuMaterial& material)
    {
        const uint32_t slot = m_Materials.Allocate();
        m_Materials.Edit(slot) = material;
        if (m_MaterialReferences.size() <= slot)
        {
            m_MaterialReferences.resize(slot + 1, 0);
            m_MaterialKeys.resize(slot + 1);
        }
        m_MaterialReferences[slot] = 0;
        m_MaterialKeys[slot] = key;
        m_MaterialByKey[key] = slot;
        return slot;
    }

    void GpuScene::UpdateMaterial(uint32_t materialSlot, const shaderio::GpuMaterial& material)
    {
        if (std::memcmp(&m_Materials.Get(materialSlot), &material, sizeof(material)) == 0)
            return;

        const bool bucketsChange = !SameBucketInputs(m_Materials.Get(materialSlot), material);
        m_Materials.Edit(materialSlot) = material;

        // Instances copy material fields into their ray tracing records; alpha mode, unlit and double-sided also pick the
        // draw bucket and the TLAS flags.
        for (uint32_t instanceSlot = 0; instanceSlot < m_InstanceStates.size(); ++instanceSlot)
        {
            const InstanceState& state = m_InstanceStates[instanceSlot];
            if (state.Material != materialSlot)
                continue;

            WriteRayTracingInstance(instanceSlot);
            if (!bucketsChange || state.Mesh == InvalidSlot)
                continue;

            RemoveFromBucket(instanceSlot);
            InsertIntoBucket(instanceSlot);
            WriteTlasInstance(instanceSlot);
        }
    }

    void GpuScene::ForEachMaterial(const std::function<void(uint32_t materialSlot, const GpuMaterialKey& key)>& function) const
    {
        for (const auto& [key, slot] : m_MaterialByKey)
            function(slot, key);
    }

    uint32_t GpuScene::AddInstance(uint32_t meshSlot, uint32_t materialSlot, const glm::mat4& world, int32_t entityID)
    {
        const uint32_t slot = m_Instances.Allocate();
        m_Transforms.Grow(slot + 1);
        m_RayTracingInstances.Grow(slot + 1);
        if (m_InstanceStates.size() <= slot)
            m_InstanceStates.resize(slot + 1);

        shaderio::GpuInstance& instance = m_Instances.Edit(slot);
        instance.meshIndex = meshSlot;
        instance.materialIndex = materialSlot;
        instance.boneMatrixOffset = NoBoneMatrices;
        instance.entityID = entityID;

        shaderio::GpuTransform& transform = m_Transforms.Edit(slot);
        transform.world = world;
        transform.normal = glm::transpose(glm::inverse(world));
        transform.previousWorld = world;

        m_InstanceStates[slot] = InstanceState{ .Mesh = meshSlot, .Material = materialSlot };
        ++m_MaterialReferences[materialSlot];
        ++m_InstanceCount;
        WriteRayTracingInstance(slot);
        InsertIntoBucket(slot);

        if (m_TlasInstances.size() <= slot)
        {
            m_TlasInstances.resize(slot + 1);
        }
        WriteTlasInstance(slot);
        return slot;
    }

    void GpuScene::RemoveInstance(uint32_t instanceSlot)
    {
        InstanceState& state = m_InstanceStates[instanceSlot];
        RemoveFromBucket(instanceSlot);
        ReleaseMaterial(state.Material);
        state = InstanceState{};
        WriteTlasInstance(instanceSlot);

        m_Instances.Free(instanceSlot);
        --m_InstanceCount;
    }

    void GpuScene::RemoveAllInstances()
    {
        for (uint32_t instanceSlot = 0; instanceSlot < m_InstanceStates.size(); ++instanceSlot)
        {
            if (m_InstanceStates[instanceSlot].Material != InvalidSlot)
                RemoveInstance(instanceSlot);
        }
    }

    void GpuScene::SetTransform(uint32_t instanceSlot, const glm::mat4& world)
    {
        InstanceState& state = m_InstanceStates[instanceSlot];
        shaderio::GpuTransform& transform = m_Transforms.Edit(instanceSlot);

        // The first move of a frame keeps last frame's world as the previous transform.
        if (state.LastMovedFrame != m_Frame)
        {
            if (state.LastMovedFrame == ~0ull)
                m_MovedInstances.push_back(instanceSlot);
            transform.previousWorld = transform.world;
            state.LastMovedFrame = m_Frame;
        }
        transform.world = world;
        transform.normal = glm::transpose(glm::inverse(world));

        m_RayTracingInstances.Edit(instanceSlot).normalMatrix = transform.normal;
        WriteTlasInstance(instanceSlot);
    }

    void GpuScene::SetBoneMatrixOffset(uint32_t instanceSlot, uint32_t boneMatrixOffset)
    {
        if (m_Instances.Get(instanceSlot).boneMatrixOffset != boneMatrixOffset)
            m_Instances.Edit(instanceSlot).boneMatrixOffset = boneMatrixOffset;
    }

    void GpuScene::BuildDrawLists(const glm::vec3& cameraPosition, std::vector<uint32_t>& outDrawInstances,
                                  std::vector<DrawMeshTasksIndirectCommand>& outCommands, std::array<uint32_t, RenderBucketCount>& outCounts)
    {
        outDrawInstances.clear();
        outCommands.clear();

        for (size_t bucketIndex = 0; bucketIndex < RenderBucketCount; ++bucketIndex)
        {
            const std::vector<uint32_t>& bucket = m_Buckets[bucketIndex];
            outCounts[bucketIndex] = static_cast<uint32_t>(bucket.size());

            if (IsTransparentBucket(static_cast<RenderBucket>(bucketIndex)))
            {
                // Back to front by instance origin.
                m_TransparentSort.clear();
                for (uint32_t instanceSlot : bucket)
                {
                    const glm::vec3 position(m_Transforms.Get(instanceSlot).world[3]);
                    m_TransparentSort.emplace_back(glm::length(position - cameraPosition), instanceSlot);
                }
                std::sort(m_TransparentSort.begin(), m_TransparentSort.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
                for (const auto& [distance, instanceSlot] : m_TransparentSort)
                    outDrawInstances.push_back(instanceSlot);
            }
            else
            {
                outDrawInstances.insert(outDrawInstances.end(), bucket.begin(), bucket.end());
            }
        }

        outCommands.reserve(outDrawInstances.size());
        for (uint32_t instanceSlot : outDrawInstances)
        {
            const uint32_t meshletCount = m_Meshes.Get(m_InstanceStates[instanceSlot].Mesh).meshletCount;
            outCommands.push_back({
                .groupCountX = (meshletCount + shaderio::TASK_SHADER_DISPATCH_X - 1) / shaderio::TASK_SHADER_DISPATCH_X,
                .groupCountY = 1,
                .groupCountZ = 1
            });
        }
    }

    void GpuScene::PrepareUploads(uint32_t frameSlot, std::vector<std::unique_ptr<NRI::Buffer>>& outReleased)
    {
        // Instances that did not move this frame: their previous transform becomes their world.
        std::erase_if(m_MovedInstances, [this](uint32_t instanceSlot)
        {
            const InstanceState& state = m_InstanceStates[instanceSlot];
            if (state.LastMovedFrame == m_Frame)
                return false;

            if (state.Material != InvalidSlot)
            {
                const shaderio::GpuTransform& transform = m_Transforms.Get(instanceSlot);
                if (transform.previousWorld != transform.world)
                    m_Transforms.Edit(instanceSlot).previousWorld = transform.world;
            }
            m_InstanceStates[instanceSlot].LastMovedFrame = ~0ull;
            return true;
        });
        ++m_Frame;

        m_Instances.PrepareUpload(frameSlot, outReleased);
        m_Transforms.PrepareUpload(frameSlot, outReleased);
        m_Materials.PrepareUpload(frameSlot, outReleased);
        m_Meshes.PrepareUpload(frameSlot, outReleased);
        m_RayTracingInstances.PrepareUpload(frameSlot, outReleased);
    }

    bool GpuScene::HasUploads(uint32_t frameSlot) const
    {
        return m_Instances.HasUpload(frameSlot) || m_Transforms.HasUpload(frameSlot) ||
               m_Materials.HasUpload(frameSlot) || m_Meshes.HasUpload(frameSlot) || m_RayTracingInstances.HasUpload(frameSlot);
    }

    void GpuScene::RecordUploads(NRI::CommandBuffer& cmd, uint32_t frameSlot) const
    {
        m_Instances.RecordUpload(cmd, frameSlot);
        m_Transforms.RecordUpload(cmd, frameSlot);
        m_Materials.RecordUpload(cmd, frameSlot);
        m_Meshes.RecordUpload(cmd, frameSlot);
        m_RayTracingInstances.RecordUpload(cmd, frameSlot);
    }

    void GpuScene::InsertIntoBucket(uint32_t instanceSlot)
    {
        InstanceState& state = m_InstanceStates[instanceSlot];
        state.Bucket = SelectRenderBucket(m_Materials.Get(state.Material));

        std::vector<uint32_t>& bucket = m_Buckets[static_cast<size_t>(state.Bucket)];
        state.BucketIndex = static_cast<uint32_t>(bucket.size());
        bucket.push_back(instanceSlot);
    }

    void GpuScene::RemoveFromBucket(uint32_t instanceSlot)
    {
        InstanceState& state = m_InstanceStates[instanceSlot];
        if (state.Bucket == RenderBucket::Count)
            return;

        std::vector<uint32_t>& bucket = m_Buckets[static_cast<size_t>(state.Bucket)];
        const uint32_t moved = bucket.back();
        bucket[state.BucketIndex] = moved;
        m_InstanceStates[moved].BucketIndex = state.BucketIndex;
        bucket.pop_back();
        state.Bucket = RenderBucket::Count;
    }

    void GpuScene::WriteTlasInstance(uint32_t instanceSlot)
    {
        if (instanceSlot >= m_TlasInstances.size())
            return;

        const InstanceState& state = m_InstanceStates[instanceSlot];
        NRI::AccelerationStructureInstance record{};
        const uint64_t blasAddress = state.Mesh != InvalidSlot ? m_MeshBlasAddresses[state.Mesh] : 0;
        if (blasAddress != 0 && IsRayTracedBucket(state.Bucket))
        {
            // Column-major world matrix -> row-major 3x4.
            const glm::mat4& world = m_Transforms.Get(instanceSlot).world;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 4; ++column)
                    record.transform.matrix[row][column] = world[column][row];
            }

            const shaderio::GpuMaterial& material = m_Materials.Get(state.Material);
            record.instanceCustomIndex = instanceSlot;
            record.mask = 0x01;
            record.instanceShaderBindingTableRecordOffset = 0;
            record.flags = 0;
            // Opaque geometry skips any-hit (FORCE_OPAQUE), alpha-mask geometry gets alpha testing (FORCE_NO_OPAQUE).
            if (material.alphaMode == static_cast<uint32_t>(AlphaMode::Opaque))
                record.flags |= 0x04;
            else if (material.alphaMode == static_cast<uint32_t>(AlphaMode::Mask))
                record.flags |= 0x08;
            if (material.doubleSided != 0)
                record.flags |= 0x01; // TRIANGLE_FACING_CULL_DISABLE
            record.accelerationStructureReference = blasAddress;
        }

        NRI::AccelerationStructureInstance& current = m_TlasInstances[instanceSlot];
        if (std::memcmp(&current, &record, sizeof(record)) == 0)
            return;

        m_TlasInstanceCount += (record.accelerationStructureReference != 0 ? 1 : 0) - (current.accelerationStructureReference != 0 ? 1 : 0);
        if (record.accelerationStructureReference != 0 || current.accelerationStructureReference != 0)
            m_TlasChanged = true;
        current = record;
    }

    void GpuScene::WriteTlasInstances(std::span<NRI::AccelerationStructureInstance> outInstances) const
    {
        size_t written = 0;
        for (size_t bucketIndex = 0; bucketIndex <= static_cast<size_t>(RenderBucket::MaskDoubleSided); ++bucketIndex)
        {
            for (uint32_t instanceSlot : m_Buckets[bucketIndex])
            {
                const NRI::AccelerationStructureInstance& record = m_TlasInstances[instanceSlot];
                if (record.accelerationStructureReference != 0)
                    outInstances[written++] = record;
            }
        }
    }

    void GpuScene::WriteRayTracingInstance(uint32_t instanceSlot)
    {
        const shaderio::GpuInstance& instance = m_Instances.Get(instanceSlot);
        const shaderio::GpuMaterial& material = m_Materials.Get(instance.materialIndex);
        const shaderio::GpuMesh& mesh = m_Meshes.Get(instance.meshIndex);

        shaderio::GpuRayTracingInstance& record = m_RayTracingInstances.Edit(instanceSlot);
        record.vertexBufferAddress = mesh.vertexBufferAddress;
        record.indexBufferAddress = mesh.indexBufferAddress;
        record.normalMatrix = m_Transforms.Get(instanceSlot).normal;
        record.baseColorFactor = material.baseColorFactor;
        record.emissiveFactor = glm::vec4(material.emissiveFactor, material.emissiveStrength);
        record.baseColorTextureIndex = material.baseColorTextureIndex;
        record.alphaCutoff = material.alphaMaskCutoff;
        record.alphaMode = material.alphaMode;
        record.doubleSided = material.doubleSided;
        record.metallicFactor = material.metallicFactor;
        record.roughnessFactor = material.roughnessFactor;
        record.metallicRoughnessTextureIndex = material.metallicRoughnessTextureIndex;
        record.normalTextureIndex = material.normalTextureIndex;
        record.transmissionFactor = material.transmissionFactor;
        record.transmissionTextureIndex = material.transmissionTextureIndex;
        record.workflow = material.workflow;
        record.ior = material.ior;
        record.thickness = material.thickness;
    }

    void GpuScene::ReleaseMaterial(uint32_t materialSlot)
    {
        if (--m_MaterialReferences[materialSlot] != 0)
            return;

        m_MaterialByKey.erase(m_MaterialKeys[materialSlot]);
        m_Materials.Free(materialSlot);
    }
}
