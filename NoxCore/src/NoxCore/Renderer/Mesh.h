#pragma once
#include <string>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
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
        
        // Lights
        const std::vector<LightNodeData>& GetLights() const { return m_Lights; }
        
        static AssetType GetStaticType() { return AssetType::StaticMesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<MaterialData> m_Materials;
        std::vector<LightNodeData> m_Lights;
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
        
        // Lights
        const std::vector<LightNodeData>& GetLights() const { return m_Lights; }
        
        static AssetType GetStaticType() { return AssetType::Mesh; }
        virtual AssetType GetAssetType() const { return GetStaticType(); }
        
    private:
        friend class MeshImporter;
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<MaterialData> m_Materials;
        std::vector<LightNodeData> m_Lights;
        std::vector<std::string> m_SubmeshNames;
    };
}
