#pragma once
#include <string>
#include <glm/vec4.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    struct BufferAllocation
    {
        uint32_t pageIndex = UINT32_MAX;
        uint32_t offset = 0;
        uint32_t count = 0;
        
        bool IsValid() const { return count > 0; };
    };
    
    struct MeshHandle
    {
        BufferAllocation vertices;
        BufferAllocation meshletDraws; // Also used for bounds (1:1 ratio)
        BufferAllocation meshletVertices;
        BufferAllocation meshletTriangles;
        
        uint32_t GetFirstMeshlet() const { return meshletDraws.offset; }
        uint32_t GetMeshletCount() const { return meshletDraws.count; }
        bool IsValid() const { return vertices.count > 0; }
    };
    
    struct MaterialData
    {
        glm::vec4 AlbedoColor = glm::vec4(1.0f);
        std::string AlbedoTexturePath;
    };
    
    class StaticMesh : public Asset
    {
    public:
        StaticMesh() = default;
        ~StaticMesh();
        
        AssetType GetType() const override { return AssetType::StaticMesh; }
        
        // Mesh
        const std::vector<MeshHandle>& GetSubMeshes() const { return m_SubMeshes; }
        const MeshHandle& GetSubMesh(size_t index) const { return m_SubMeshes[index]; }
        size_t GetSubMeshCount() const { return m_SubMeshes.size(); }
        
        const std::string& GetSubmeshName(size_t index) const 
        { 
            static std::string empty = "";
            return index < m_SubmeshNames.size() ? m_SubmeshNames[index] : empty; 
        }
        
        // Material
        const std::vector<MaterialData>& GetMaterials() const { return m_Materials; }
        const MaterialData& GetMaterial(size_t index) const { return m_Materials[index]; }
        
        static AssetType GetStaticType() { return AssetType::StaticMesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<MaterialData> m_Materials;
        std::vector<std::string> m_SubmeshNames;
    };
    
    class Mesh : public Asset
    {
    public:
        Mesh() = default;
        ~Mesh();
        
        AssetType GetType() const override { return AssetType::Mesh; }
        
        // Mesh
        const std::vector<MeshHandle>& GetSubMeshes() const { return m_SubMeshes; }
        const MeshHandle& GetSubMesh(size_t index) const { return m_SubMeshes[index]; }
        size_t GetSubMeshCount() const { return m_SubMeshes.size(); }
        
        const std::string& GetSubmeshName(size_t index) const 
        { 
            static std::string empty = "";
            return index < m_SubmeshNames.size() ? m_SubmeshNames[index] : empty; 
        }
        
        // Material
        const std::vector<MaterialData>& GetMaterials() const { return m_Materials; }
        const MaterialData& GetMaterial(size_t index) const { return m_Materials[index]; }
        
        static AssetType GetStaticType() { return AssetType::Mesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<MaterialData> m_Materials;
        std::vector<std::string> m_SubmeshNames;
    };
}
