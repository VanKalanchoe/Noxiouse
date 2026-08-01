#pragma once
#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    struct MeshHandle
    {
        uint32_t firstMeshlet = 0;
        uint32_t meshletCount = 0;
    };
    
    class StaticMesh : public Asset
    {
    public:
        StaticMesh() = default;
        StaticMesh(MeshHandle handle) : m_Handle(handle) {}
        virtual ~StaticMesh() = default;
        
        AssetType GetType() const override { return AssetType::StaticMesh; }
        
        MeshHandle GetHandle() const { return m_Handle; }
        
        static AssetType GetStaticType() { return AssetType::StaticMesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        MeshHandle m_Handle;
    };
    
    class Mesh : public Asset
    {
    public:
        Mesh() = default;
        virtual ~Mesh() = default;
        
        AssetType GetType() const override { return AssetType::Mesh; }
        
        const std::vector<MeshHandle>& GetSubMeshes() const { return m_SubMeshes; }
        const MeshHandle& GetSubMesh(size_t index) const { return m_SubMeshes[index]; }
        size_t GetSubMeshCount() const { return m_SubMeshes.size(); }
        
        static AssetType GetStaticType() { return AssetType::Mesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
    };
}
