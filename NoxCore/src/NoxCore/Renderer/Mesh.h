#pragma once
#include <glm/vec4.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    struct MeshHandle
    {
        uint32_t firstMeshlet = 0;
        uint32_t meshletCount = 0;
    };
    
    struct MaterialData
    {
        glm::vec4 AlbedoColor = glm::vec4(1.0f);
        std::string AlbedoTexturePath;
        AssetHandle AlbedoMap = 0;
    };
    
    class StaticMesh : public Asset
    {
    public:
        StaticMesh() = default;
        virtual ~StaticMesh() = default;
        
        AssetType GetType() const override { return AssetType::StaticMesh; }
        
        // Mesh
        const std::vector<MeshHandle>& GetSubMeshes() const { return m_SubMeshes; }
        const MeshHandle& GetSubMesh(size_t index) const { return m_SubMeshes[index]; }
        size_t GetSubMeshCount() const { return m_SubMeshes.size(); }
        
        // Material
        const std::vector<MaterialData>& GetMaterials() const { return m_Materials; }
        const MaterialData& GetMaterial(size_t index) const { return m_Materials[index]; }
        
        static AssetType GetStaticType() { return AssetType::StaticMesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<MaterialData> m_Materials;
    };
    
    class Mesh : public Asset
    {
    public:
        Mesh() = default;
        virtual ~Mesh() = default;
        
        AssetType GetType() const override { return AssetType::Mesh; }
        
        // Mesh
        const std::vector<MeshHandle>& GetSubMeshes() const { return m_SubMeshes; }
        const MeshHandle& GetSubMesh(size_t index) const { return m_SubMeshes[index]; }
        size_t GetSubMeshCount() const { return m_SubMeshes.size(); }
        
        // Material
        const std::vector<MaterialData>& GetMaterials() const { return m_Materials; }
        const MaterialData& GetMaterial(size_t index) const { return m_Materials[index]; }
        
        static AssetType GetStaticType() { return AssetType::Mesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<MaterialData> m_Materials;
    };
}
