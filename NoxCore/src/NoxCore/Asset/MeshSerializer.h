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

            uint32_t boneCount = static_cast<uint32_t>(skeleton.Bones.size());
            stream.write(reinterpret_cast<const char*>(&boneCount), sizeof(uint32_t));

            for (const auto& bone : skeleton.Bones)
            {
                SerializerUtils::WriteString(stream, bone.Name);
                stream.write(reinterpret_cast<const char*>(&bone.ParentIndex), sizeof(int32_t));
                stream.write(reinterpret_cast<const char*>(&bone.InverseBindMatrix), sizeof(glm::mat4));
                stream.write(reinterpret_cast<const char*>(&bone.LocalRestTransform), sizeof(glm::mat4));
            }
        }

        static bool Deserialize(const std::filesystem::path& filepath, Skeleton& outSkeleton)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;

            char magic[5] = { 0 };
            stream.read(magic, 4);
            if (strcmp(magic, "NSKL") != 0) return false;

            uint32_t boneCount = 0;
            stream.read(reinterpret_cast<char*>(&boneCount), sizeof(uint32_t));
            outSkeleton.Bones.resize(boneCount);
            outSkeleton.BoneNameToIndexMap.clear();

            for (uint32_t i = 0; i < boneCount; i++)
            {
                BoneInfo& bone = outSkeleton.Bones[i];
                SerializerUtils::ReadString(stream, bone.Name);
                stream.read(reinterpret_cast<char*>(&bone.ParentIndex), sizeof(int32_t));
                stream.read(reinterpret_cast<char*>(&bone.InverseBindMatrix), sizeof(glm::mat4));
                stream.read(reinterpret_cast<char*>(&bone.LocalRestTransform), sizeof(glm::mat4));

                outSkeleton.BoneNameToIndexMap[bone.Name] = static_cast<int32_t>(i);
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
                SerializerUtils::WriteString(stream, channel.BoneName);
                stream.write(reinterpret_cast<const char*>(&channel.BoneIndex), sizeof(int32_t));

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
                BoneAnimationChannel& channel = outAnim.Channels[i];
                SerializerUtils::ReadString(stream, channel.BoneName);
                stream.read(reinterpret_cast<char*>(&channel.BoneIndex), sizeof(int32_t));

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
                stream.write(reinterpret_cast<const char*>(&mat.AlbedoColor), sizeof(glm::vec4));
                SerializerUtils::WriteString(stream, mat.AlbedoTexturePath);
            }
        }

        static void ReadMaterials(std::ifstream& stream, std::vector<MaterialData>& outMaterialList)
        {
            if (stream.peek() != EOF)
            {
                uint32_t matCount = 0;
                stream.read(reinterpret_cast<char*>(&matCount), sizeof(uint32_t));
                outMaterialList.resize(matCount);
                for (uint32_t i = 0; i < matCount; i++)
                {
                    stream.read(reinterpret_cast<char*>(&outMaterialList[i].AlbedoColor), sizeof(glm::vec4));
                    SerializerUtils::ReadString(stream, outMaterialList[i].AlbedoTexturePath);
                }
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