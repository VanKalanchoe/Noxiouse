#pragma once

#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    namespace SerializerUtils
    {
        static void WriteString(std::ofstream& stream, const std::string& str)
        {
            uint32_t length = static_cast<uint32_t>(str.size());
            stream.write(reinterpret_cast<const char*>(&length), sizeof(uint32_t));
            if (length > 0)
                stream.write(str.data(), length);
        }

        static void ReadString(std::ifstream& stream, std::string& outStr)
        {
            uint32_t length = 0;
            stream.read(reinterpret_cast<char*>(&length), sizeof(uint32_t));
            outStr.resize(length);
            if (length > 0)
                stream.read(&outStr[0], length);
        }

        template<typename T>
        static void WriteVector(std::ofstream& stream, const std::vector<T>& vec)
        {
            uint32_t size = static_cast<uint32_t>(vec.size());
            stream.write(reinterpret_cast<const char*>(&size), sizeof(uint32_t));
            if (size > 0)
                stream.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
        }

        template<typename T>
        static void ReadVector(std::ifstream& stream, std::vector<T>& vec)
        {
            uint32_t size = 0;
            stream.read(reinterpret_cast<char*>(&size), sizeof(uint32_t));
            vec.resize(size);
            if (size > 0)
                stream.read(reinterpret_cast<char*>(vec.data()), size * sizeof(T));
        }
    }

    // ==========================================
    // SKELETON SERIALIZER (.nskel)
    // ==========================================
    class SkeletonSerializer
    {
    public:
        static void Serialize(const std::filesystem::path& filepath, const Skeleton& skeleton)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            const char magic[5] = "NSKL";
            stream.write(magic, 4);

            // 1. Serialize AllNodes
            uint32_t nodeCount = static_cast<uint32_t>(skeleton.AllNodes.size());
            stream.write(reinterpret_cast<const char*>(&nodeCount), sizeof(uint32_t));

            for (const auto& node : skeleton.AllNodes)
            {
                bool valid = (node != nullptr);
                stream.write(reinterpret_cast<const char*>(&valid), sizeof(bool));
                if (!valid) continue;

                stream.write(reinterpret_cast<const char*>(&node->Index), sizeof(int32_t));
                SerializerUtils::WriteString(stream, node->Name);
                
                int32_t parentIndex = node->Parent ? node->Parent->Index : -1;
                stream.write(reinterpret_cast<const char*>(&parentIndex), sizeof(int32_t));

                stream.write(reinterpret_cast<const char*>(&node->RestTranslation), sizeof(glm::vec3));
                stream.write(reinterpret_cast<const char*>(&node->RestRotation), sizeof(glm::quat));
                stream.write(reinterpret_cast<const char*>(&node->RestScale), sizeof(glm::vec3));
                stream.write(reinterpret_cast<const char*>(&node->RestMatrix), sizeof(glm::mat4));
                stream.write(reinterpret_cast<const char*>(&node->HasRestMatrix), sizeof(bool));
            }

            // 2. Serialize Skins
            uint32_t skinCount = static_cast<uint32_t>(skeleton.Skins.size());
            stream.write(reinterpret_cast<const char*>(&skinCount), sizeof(uint32_t));

            for (const auto& skin : skeleton.Skins)
            {
                if (!skin) continue;
                SerializerUtils::WriteString(stream, skin->Name);

                uint32_t jointCount = static_cast<uint32_t>(skin->Joints.size());
                stream.write(reinterpret_cast<const char*>(&jointCount), sizeof(uint32_t));
                for (const auto& jointNode : skin->Joints)
                {
                    int32_t jointNodeIndex = jointNode ? jointNode->Index : -1;
                    stream.write(reinterpret_cast<const char*>(&jointNodeIndex), sizeof(int32_t));
                }

                SerializerUtils::WriteVector(stream, skin->InverseBindMatrices);
            }
        }

        static bool Deserialize(const std::filesystem::path& filepath, Skeleton& outSkeleton)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;

            char magic[5] = { 0 };
            stream.read(magic, 4);
            if (strcmp(magic, "NSKL") != 0) return false;

            // Clean up existing nodes/skins if any
            for (auto node : outSkeleton.AllNodes) delete node;
            for (auto skin : outSkeleton.Skins) delete skin;
            outSkeleton.AllNodes.clear();
            outSkeleton.RootNodes.clear();
            outSkeleton.Skins.clear();

            // 1. Deserialize AllNodes
            uint32_t nodeCount = 0;
            stream.read(reinterpret_cast<char*>(&nodeCount), sizeof(uint32_t));
            outSkeleton.AllNodes.resize(nodeCount, nullptr);

            std::vector<int32_t> parentIndices(nodeCount, -1);

            for (uint32_t i = 0; i < nodeCount; i++)
            {
                bool valid = false;
                stream.read(reinterpret_cast<char*>(&valid), sizeof(bool));
                if (!valid) continue;

                Node* node = new Node();
                stream.read(reinterpret_cast<char*>(&node->Index), sizeof(int32_t));
                SerializerUtils::ReadString(stream, node->Name);
                
                stream.read(reinterpret_cast<char*>(&parentIndices[i]), sizeof(int32_t));

                stream.read(reinterpret_cast<char*>(&node->RestTranslation), sizeof(glm::vec3));
                stream.read(reinterpret_cast<char*>(&node->RestRotation), sizeof(glm::quat));
                stream.read(reinterpret_cast<char*>(&node->RestScale), sizeof(glm::vec3));
                stream.read(reinterpret_cast<char*>(&node->RestMatrix), sizeof(glm::mat4));
                stream.read(reinterpret_cast<char*>(&node->HasRestMatrix), sizeof(bool));

                // Initialize current fields to rest pose
                node->Translation = node->RestTranslation;
                node->Rotation = node->RestRotation;
                node->Scale = node->RestScale;
                node->Matrix = node->RestMatrix;
                node->HasMatrix = node->HasRestMatrix;

                outSkeleton.AllNodes[i] = node;
            }

            // Wire up tree hierarchy and root nodes
            for (uint32_t i = 0; i < nodeCount; i++)
            {
                Node* node = outSkeleton.AllNodes[i];
                if (!node) continue;

                int32_t pIdx = parentIndices[i];
                if (pIdx >= 0 && pIdx < static_cast<int32_t>(nodeCount))
                {
                    node->Parent = outSkeleton.AllNodes[pIdx];
                    if (node->Parent)
                    {
                        node->Parent->Children.push_back(node);
                    }
                }
                else
                {
                    outSkeleton.RootNodes.push_back(node);
                }
            }

            // 2. Deserialize Skins
            uint32_t skinCount = 0;
            stream.read(reinterpret_cast<char*>(&skinCount), sizeof(uint32_t));
            outSkeleton.Skins.resize(skinCount);

            for (uint32_t i = 0; i < skinCount; i++)
            {
                Skin* skin = new Skin();
                SerializerUtils::ReadString(stream, skin->Name);

                uint32_t jointCount = 0;
                stream.read(reinterpret_cast<char*>(&jointCount), sizeof(uint32_t));
                skin->Joints.resize(jointCount);

                for (uint32_t j = 0; j < jointCount; j++)
                {
                    int32_t jointNodeIndex = -1;
                    stream.read(reinterpret_cast<char*>(&jointNodeIndex), sizeof(int32_t));
                    if (jointNodeIndex >= 0 && jointNodeIndex < static_cast<int32_t>(outSkeleton.AllNodes.size()))
                    {
                        skin->Joints[j] = outSkeleton.AllNodes[jointNodeIndex];
                    }
                    else
                    {
                        skin->Joints[j] = nullptr;
                    }
                }

                SerializerUtils::ReadVector(stream, skin->InverseBindMatrices);
                outSkeleton.Skins[i] = skin;
            }

            return true;
        }
    };

    // ==========================================
    // ANIMATION SERIALIZER (.nanim)
    // ==========================================
    class AnimationSerializer
    {
    public:
        static void Serialize(const std::filesystem::path& filepath, const AnimationSequence& anim)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            const char magic[5] = "NANM";
            stream.write(magic, 4);

            SerializerUtils::WriteString(stream, anim.Name);
            stream.write(reinterpret_cast<const char*>(&anim.Duration), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&anim.TicksPerSecond), sizeof(float));

            uint32_t channelCount = static_cast<uint32_t>(anim.Channels.size());
            stream.write(reinterpret_cast<const char*>(&channelCount), sizeof(uint32_t));

            for (const auto& channel : anim.Channels)
            {
                SerializerUtils::WriteString(stream, channel.NodeName);
                stream.write(reinterpret_cast<const char*>(&channel.TargetNodeIndex), sizeof(int32_t));

                uint8_t interp = static_cast<uint8_t>(channel.Interpolation);
                stream.write(reinterpret_cast<const char*>(&interp), sizeof(uint8_t));

                SerializerUtils::WriteVector(stream, channel.PositionKeys);
                SerializerUtils::WriteVector(stream, channel.RotationKeys);
                SerializerUtils::WriteVector(stream, channel.ScaleKeys);
            }
        }

        static bool Deserialize(const std::filesystem::path& filepath, AnimationSequence& outAnim)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;

            char magic[5] = { 0 };
            stream.read(magic, 4);
            if (strcmp(magic, "NANM") != 0) return false;

            SerializerUtils::ReadString(stream, outAnim.Name);
            stream.read(reinterpret_cast<char*>(&outAnim.Duration), sizeof(float));
            stream.read(reinterpret_cast<char*>(&outAnim.TicksPerSecond), sizeof(float));

            uint32_t channelCount = 0;
            stream.read(reinterpret_cast<char*>(&channelCount), sizeof(uint32_t));
            outAnim.Channels.resize(channelCount);

            for (uint32_t i = 0; i < channelCount; i++)
            {
                NodeAnimationChannel& channel = outAnim.Channels[i];
                SerializerUtils::ReadString(stream, channel.NodeName);
                stream.read(reinterpret_cast<char*>(&channel.TargetNodeIndex), sizeof(int32_t));

                uint8_t interp = 0;
                stream.read(reinterpret_cast<char*>(&interp), sizeof(uint8_t));
                channel.Interpolation = static_cast<AnimationInterpolation>(interp);

                SerializerUtils::ReadVector(stream, channel.PositionKeys);
                SerializerUtils::ReadVector(stream, channel.RotationKeys);
                SerializerUtils::ReadVector(stream, channel.ScaleKeys);
            }

            return true;
        }
    };

    // ==========================================
    // MESH SERIALIZER (.nsmesh / .nmesh)
    // ==========================================
    class MeshSerializer
    {
    private:
        static void WriteMeshData(std::ofstream& stream, const MeshData& data)
        {
            SerializerUtils::WriteString(stream, data.Name);
            SerializerUtils::WriteVector(stream, data.Vertices);
            SerializerUtils::WriteVector(stream, data.Bounds);
            SerializerUtils::WriteVector(stream, data.MeshletVertices);
            SerializerUtils::WriteVector(stream, data.MeshletTriangles);
            SerializerUtils::WriteVector(stream, data.Draws);
        }

        static void ReadMeshData(std::ifstream& stream, MeshData& outData)
        {
            SerializerUtils::ReadString(stream, outData.Name);
            SerializerUtils::ReadVector(stream, outData.Vertices);
            SerializerUtils::ReadVector(stream, outData.Bounds);
            SerializerUtils::ReadVector(stream, outData.MeshletVertices);
            SerializerUtils::ReadVector(stream, outData.MeshletTriangles);
            SerializerUtils::ReadVector(stream, outData.Draws);
        }

       static void WriteMaterials(std::ofstream& stream, const std::vector<MaterialData>& materialList)
        {
            uint32_t matCount = static_cast<uint32_t>(materialList.size());
            stream.write(reinterpret_cast<const char*>(&matCount), sizeof(uint32_t));
            for (const auto& mat : materialList)
            {
                SerializerUtils::WriteString(stream, mat.Name);
                
                // Base Color
                stream.write(reinterpret_cast<const char*>(&mat.AlbedoColor), sizeof(glm::vec4));
                SerializerUtils::WriteString(stream, mat.AlbedoTexturePath);
                
                // PBR Properties
                stream.write(reinterpret_cast<const char*>(&mat.MetallicFactor), sizeof(float));
                stream.write(reinterpret_cast<const char*>(&mat.RoughnessFactor), sizeof(float));
                SerializerUtils::WriteString(stream, mat.MetallicRoughnessTexturePath);
                
                // Additional Maps
                SerializerUtils::WriteString(stream, mat.NormalTexturePath);
                SerializerUtils::WriteString(stream, mat.OcclusionTexturePath);
                
                // Emission
                stream.write(reinterpret_cast<const char*>(&mat.EmissiveFactor), sizeof(glm::vec3));
                SerializerUtils::WriteString(stream, mat.EmissiveTexturePath);
                
                // Alpha & Render settings
                uint32_t modeVal = static_cast<uint32_t>(mat.Mode);
                stream.write(reinterpret_cast<const char*>(&modeVal), sizeof(uint32_t));
                stream.write(reinterpret_cast<const char*>(&mat.AlphaCutoff), sizeof(float));
                
                uint8_t doubleSidedVal = mat.DoubleSided ? 1 : 0;
                stream.write(reinterpret_cast<const char*>(&doubleSidedVal), sizeof(uint8_t));
            }
        }

        static void ReadMaterials(std::ifstream& stream, std::vector<MaterialData>& outMaterialList)
        {
            uint32_t matCount = 0;
            stream.read(reinterpret_cast<char*>(&matCount), sizeof(uint32_t));
            if (stream.fail()) return;

            outMaterialList.resize(matCount);
            for (uint32_t i = 0; i < matCount; i++)
            {
                SerializerUtils::ReadString(stream, outMaterialList[i].Name);
                
                // Base Color
                stream.read(reinterpret_cast<char*>(&outMaterialList[i].AlbedoColor), sizeof(glm::vec4));
                SerializerUtils::ReadString(stream, outMaterialList[i].AlbedoTexturePath);
                
                // PBR Properties
                stream.read(reinterpret_cast<char*>(&outMaterialList[i].MetallicFactor), sizeof(float));
                stream.read(reinterpret_cast<char*>(&outMaterialList[i].RoughnessFactor), sizeof(float));
                SerializerUtils::ReadString(stream, outMaterialList[i].MetallicRoughnessTexturePath);
                
                // Additional Maps
                SerializerUtils::ReadString(stream, outMaterialList[i].NormalTexturePath);
                SerializerUtils::ReadString(stream, outMaterialList[i].OcclusionTexturePath);
                
                // Emission
                stream.read(reinterpret_cast<char*>(&outMaterialList[i].EmissiveFactor), sizeof(glm::vec3));
                SerializerUtils::ReadString(stream, outMaterialList[i].EmissiveTexturePath);
                
                // Alpha & Render settings
                uint32_t modeVal = 0;
                stream.read(reinterpret_cast<char*>(&modeVal), sizeof(uint32_t));
                outMaterialList[i].Mode = static_cast<AlphaMode>(modeVal);
                stream.read(reinterpret_cast<char*>(&outMaterialList[i].AlphaCutoff), sizeof(float));
                
                uint8_t doubleSidedVal = 0;
                stream.read(reinterpret_cast<char*>(&doubleSidedVal), sizeof(uint8_t));
                outMaterialList[i].DoubleSided = (doubleSidedVal != 0);
            }
        }

    public:
        static void SerializeStaticMesh(const std::filesystem::path& filepath, const std::vector<MeshData>& dataList, const std::vector<MaterialData>& materialList)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            const char magic[5] = "NSMS";
            stream.write(magic, 4);

            uint32_t count = static_cast<uint32_t>(dataList.size());
            stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));

            for (const auto& data : dataList)
            {
                WriteMeshData(stream, data);
            }

            WriteMaterials(stream, materialList);
        }

        static bool DeserializeStaticMesh(const std::filesystem::path& filepath, std::vector<MeshData>& outDataList, std::vector<MaterialData>& outMaterialList)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;

            char magic[5] = { 0 };
            stream.read(magic, 4);
            if (strcmp(magic, "NSMS") != 0) return false;

            uint32_t count = 0;
            stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
            outDataList.resize(count);

            for (uint32_t i = 0; i < count; i++)
            {
                ReadMeshData(stream, outDataList[i]);
            }

            ReadMaterials(stream, outMaterialList);
            return true;
        }

        static void SerializeMesh(const std::filesystem::path& filepath, const std::vector<MeshData>& dataList, const std::vector<MaterialData>& materialList)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            const char magic[5] = "NMSH";
            stream.write(magic, 4);

            uint32_t count = static_cast<uint32_t>(dataList.size());
            stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));

            for (const auto& data : dataList)
            {
                WriteMeshData(stream, data);
            }

            WriteMaterials(stream, materialList);
        }

        static bool DeserializeMesh(const std::filesystem::path& filepath, std::vector<MeshData>& outDataList, std::vector<MaterialData>& outMaterialList)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;

            char magic[5] = { 0 };
            stream.read(magic, 4);
            if (strcmp(magic, "NMSH") != 0) return false;

            uint32_t count = 0;
            stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
            outDataList.resize(count);

            for (uint32_t i = 0; i < count; i++)
            {
                ReadMeshData(stream, outDataList[i]);
            }

            ReadMaterials(stream, outMaterialList);
            return true;
        }
    };
}