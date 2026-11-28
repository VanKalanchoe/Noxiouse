#include "VoxelMaterialTable.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <limits>

namespace Nox::Voxel
{
    namespace
    {
        void EmitVec(YAML::Emitter& out, const float* v, int count)
        {
            out << YAML::Flow << YAML::BeginSeq;
            for (int i = 0; i < count; ++i)
                out << v[i];
            out << YAML::EndSeq;
        }

        bool ReadVec(const YAML::Node& node, float* v, int count)
        {
            if (!node || !node.IsSequence() || static_cast<int>(node.size()) != count)
                return false;
            for (int i = 0; i < count; ++i)
                v[i] = node[i].as<float>();
            return true;
        }
    }

    VoxelMaterialTable::VoxelMaterialTable()
    {
        VoxelMaterial air;
        air.Name = "Air";
        air.BaseColor = glm::vec4(0.0f);
        air.Hardness = 0.0f;
        m_Materials.push_back(air);
    }

    MaterialId VoxelMaterialTable::Add(const VoxelMaterial& material)
    {
        if (m_Materials.size() > std::numeric_limits<MaterialId>::max())
            return Air;
        m_Materials.push_back(material);
        return static_cast<MaterialId>(m_Materials.size() - 1);
    }

    MaterialId VoxelMaterialTable::Find(const std::string& name) const
    {
        for (size_t id = 1; id < m_Materials.size(); ++id)
            if (m_Materials[id].Name == name)
                return static_cast<MaterialId>(id);
        return Air;
    }

    MaterialId VoxelMaterialTable::Nearest(const VoxelMaterial& look) const
    {
        MaterialId best = Air;
        float bestDistance = std::numeric_limits<float>::max();
        for (size_t id = 1; id < m_Materials.size(); ++id)
        {
            const VoxelMaterial& candidate = m_Materials[id];
            const glm::vec3 d = glm::vec3(candidate.BaseColor) - glm::vec3(look.BaseColor);
            float distance = glm::dot(d, d);
            // A lamp must not turn into a plain block (nor the reverse), nor metal into stone.
            distance += candidate.IsEmissive() != look.IsEmissive() ? 4.0f : 0.0f;
            distance += std::abs(candidate.Metallic - look.Metallic) * 0.5f + std::abs(candidate.Roughness - look.Roughness) * 0.1f;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = static_cast<MaterialId>(id);
            }
        }
        return best;
    }

    std::string VoxelMaterialTable::Serialize() const
    {
        YAML::Emitter out;
        out.SetFloatPrecision(9); // (floats round-trip exactly)
        out << YAML::BeginMap;
        out << YAML::Key << "VoxelMaterials" << YAML::Value << YAML::BeginSeq;
        for (size_t id = 1; id < m_Materials.size(); ++id) // (air is implied)
        {
            const VoxelMaterial& m = m_Materials[id];
            out << YAML::BeginMap;
            out << YAML::Key << "Id" << YAML::Value << id;
            out << YAML::Key << "Name" << YAML::Value << m.Name;
            out << YAML::Key << "BaseColor" << YAML::Value;
            EmitVec(out, &m.BaseColor.x, 4);
            out << YAML::Key << "Roughness" << YAML::Value << m.Roughness;
            out << YAML::Key << "Metallic" << YAML::Value << m.Metallic;
            out << YAML::Key << "Emission" << YAML::Value;
            EmitVec(out, &m.Emission.x, 3);
            out << YAML::Key << "EmissionStrength" << YAML::Value << m.EmissionStrength;
            out << YAML::Key << "Hardness" << YAML::Value << m.Hardness;
            out << YAML::Key << "FootstepSurface" << YAML::Value << m.FootstepSurface;
            out << YAML::Key << "TopTexture" << YAML::Value << m.TopTexture;
            out << YAML::Key << "SideTexture" << YAML::Value << m.SideTexture;
            out << YAML::Key << "BottomTexture" << YAML::Value << m.BottomTexture;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq << YAML::EndMap;
        return out.c_str();
    }

    bool VoxelMaterialTable::Deserialize(const std::string& text, VoxelMaterialTable& out, std::string& error)
    {
        try
        {
            const YAML::Node root = YAML::Load(text);
            const YAML::Node list = root["VoxelMaterials"];
            if (!list || !list.IsSequence())
            {
                error = "no VoxelMaterials list";
                return false;
            }
            VoxelMaterialTable table;
            for (const YAML::Node& node : list)
            {
                // Ids are saved so a reordered or hand-edited file cannot shuffle what the worlds' voxels mean.
                const size_t id = node["Id"].as<size_t>();
                if (id != table.m_Materials.size())
                {
                    error = "material ids must run 1, 2, 3, ... (found " + std::to_string(id) + " at position " + std::to_string(table.m_Materials.size()) + ")";
                    return false;
                }
                VoxelMaterial m;
                m.Name = node["Name"].as<std::string>("");
                if (!ReadVec(node["BaseColor"], &m.BaseColor.x, 4) || !ReadVec(node["Emission"], &m.Emission.x, 3))
                {
                    error = "material " + std::to_string(id) + ": BaseColor needs 4 numbers, Emission 3";
                    return false;
                }
                m.Roughness = node["Roughness"].as<float>(1.0f);
                m.Metallic = node["Metallic"].as<float>(0.0f);
                m.EmissionStrength = node["EmissionStrength"].as<float>(0.0f);
                m.Hardness = node["Hardness"].as<float>(1.0f);
                m.FootstepSurface = node["FootstepSurface"].as<std::string>("");
                m.TopTexture = node["TopTexture"].as<uint64_t>(0);
                m.SideTexture = node["SideTexture"].as<uint64_t>(0);
                m.BottomTexture = node["BottomTexture"].as<uint64_t>(0);
                if (table.Add(m) == Air)
                {
                    error = "more than 65535 voxel materials";
                    return false;
                }
            }
            out = std::move(table);
            return true;
        }
        catch (const YAML::Exception& e)
        {
            error = e.what();
            return false;
        }
    }
}
