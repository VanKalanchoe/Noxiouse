#pragma once

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <string>

#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Core/VirtualFileSystem.h"
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

        static void ReadString(std::istream& stream, std::string& outStr)
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
        static void ReadVector(std::istream& stream, std::vector<T>& vec)
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
            // A loose file, else a mounted pack's entry (VirtualFileSystem.h).
            std::unique_ptr<std::istream> file = VFS::Open(filepath);
            if (!file) return false;
            std::istream& stream = *file;

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
            // A loose file, else a mounted pack's entry (VirtualFileSystem.h).
            std::unique_ptr<std::istream> file = VFS::Open(filepath);
            if (!file) return false;
            std::istream& stream = *file;

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
        // Cluster bounds as version 2 files hold them (before the streaming page fields).
        struct MeshletBoundsV2
        {
            glm::vec3 center;
            float radius;
            glm::vec3 coneApex;
            float coneCutoff;
            glm::vec3 coneAxis;
            glm::vec3 lodCenter;
            float lodRadius;
            float lodError;
            glm::vec3 parentCenter;
            float parentRadius;
            float parentError;
            uint32_t lodLevel;
            uint32_t triangleCount;
        };
        static_assert(sizeof(MeshletBoundsV2) == 92);

        static void ReadMeshDataV2(std::istream& stream, MeshData& outData)
        {
            SerializerUtils::ReadString(stream, outData.Name);
            SerializerUtils::ReadVector(stream, outData.Vertices);
            std::vector<MeshletBoundsV2> bounds;
            SerializerUtils::ReadVector(stream, bounds);
            SerializerUtils::ReadVector(stream, outData.MeshletVertices);
            SerializerUtils::ReadVector(stream, outData.MeshletTriangles);
            SerializerUtils::ReadVector(stream, outData.Draws);

            outData.Bounds.resize(bounds.size());
            for (size_t index = 0; index < bounds.size(); ++index)
            {
                static_assert(offsetof(shaderio::MeshletBounds, page) == sizeof(MeshletBoundsV2));
                std::memcpy(&outData.Bounds[index], &bounds[index], sizeof(MeshletBoundsV2));
                outData.Bounds[index].page = 0;
                outData.Bounds[index].refinedPage = shaderio::ClusterNoPage;
            }
        }

        // Version 3 (§5.7.2): a mesh's cluster records, page table and dependencies here; the pages' data (MeshPage::PayloadOffset,
        // from the payload section) after everything else, so a page is read on its own as it streams in. Geometry without pages
        // (skinned: one level) keeps its arrays here.
        // Version 4 (§5.8.2): the vertices packed as the geometry stream holds them (shaderio::PackedVertex, read into staging as
        // they are), the bone influences of a skinned mesh in a block of their own (shaderio::SkinVertex).
        static std::vector<shaderio::PackedVertex> PackVertices(const shaderio::Vertex* vertices, size_t count)
        {
            std::vector<shaderio::PackedVertex> packed(count);
            for (size_t index = 0; index < count; ++index)
                packed[index] = VertexPacking::Pack(vertices[index]);
            return packed;
        }

        static void WriteMeshData(std::ofstream& stream, const MeshData& data, uint64_t& payloadCursor)
        {
            SerializerUtils::WriteString(stream, data.Name);
            SerializerUtils::WriteVector(stream, data.Bounds);
            SerializerUtils::WriteVector(stream, data.Draws);
            std::vector<MeshPage> pages = data.Pages;
            for (MeshPage& page : pages)
            {
                page.PayloadOffset = payloadCursor;
                payloadCursor += page.PayloadBytes();
            }
            SerializerUtils::WriteVector(stream, pages);
            SerializerUtils::WriteVector(stream, data.PageDependencies);
            stream.write(reinterpret_cast<const char*>(&data.RootPageCount), sizeof(uint32_t));
            stream.write(reinterpret_cast<const char*>(&data.BoundsMin), sizeof(glm::vec3));
            stream.write(reinterpret_cast<const char*>(&data.BoundsMax), sizeof(glm::vec3));
            if (data.Pages.empty())
            {
                SerializerUtils::WriteVector(stream, PackVertices(data.Vertices.data(), data.Vertices.size()));
                std::vector<shaderio::SkinVertex> skin;
                if (VertexPacking::HasSkin(data.Vertices))
                {
                    skin.reserve(data.Vertices.size());
                    for (const shaderio::Vertex& vertex : data.Vertices)
                        skin.push_back(VertexPacking::PackSkin(vertex));
                }
                SerializerUtils::WriteVector(stream, skin);
                SerializerUtils::WriteVector(stream, data.MeshletVertices);
                SerializerUtils::WriteVector(stream, data.MeshletTriangles);
            }
        }

        static void WritePagePayloads(std::ofstream& stream, const MeshData& data)
        {
            static constexpr char Padding[16] = {};
            auto block = [&](const void* bytes, uint64_t size)
            {
                stream.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
                stream.write(Padding, static_cast<std::streamsize>(MeshPage::Align16(size) - size));
            };
            for (const MeshPage& page : data.Pages)
            {
                const std::vector<shaderio::PackedVertex> packed = PackVertices(data.Vertices.data() + page.FirstVertex, page.VertexCount);
                block(packed.data(), sizeof(shaderio::PackedVertex) * packed.size());
                block(data.MeshletVertices.data() + page.FirstMeshletVertex, sizeof(uint32_t) * uint64_t(page.MeshletVertexCount));
                block(data.MeshletTriangles.data() + page.FirstMeshletTriangle, page.MeshletTriangleCount);
            }
        }

        static void ReadMeshData(std::istream& stream, MeshData& outData, uint32_t version)
        {
            SerializerUtils::ReadString(stream, outData.Name);
            SerializerUtils::ReadVector(stream, outData.Bounds);
            SerializerUtils::ReadVector(stream, outData.Draws);
            SerializerUtils::ReadVector(stream, outData.Pages);
            SerializerUtils::ReadVector(stream, outData.PageDependencies);
            stream.read(reinterpret_cast<char*>(&outData.RootPageCount), sizeof(uint32_t));
            stream.read(reinterpret_cast<char*>(&outData.BoundsMin), sizeof(glm::vec3));
            stream.read(reinterpret_cast<char*>(&outData.BoundsMax), sizeof(glm::vec3));
            if (outData.Pages.empty())
            {
                if (version >= 4)
                {
                    std::vector<shaderio::PackedVertex> packed;
                    std::vector<shaderio::SkinVertex> skin;
                    SerializerUtils::ReadVector(stream, packed);
                    SerializerUtils::ReadVector(stream, skin);
                    outData.Vertices.resize(packed.size());
                    for (size_t index = 0; index < packed.size(); ++index)
                    {
                        outData.Vertices[index] = VertexPacking::Unpack(packed[index]);
                        if (index < skin.size())
                            VertexPacking::UnpackSkin(skin[index], outData.Vertices[index]);
                    }
                }
                else
                {
                    SerializerUtils::ReadVector(stream, outData.Vertices);
                }
                SerializerUtils::ReadVector(stream, outData.MeshletVertices);
                SerializerUtils::ReadVector(stream, outData.MeshletTriangles);
            }
        }

        // The pages' data into the arrays: every page, or with streamPages only the root pages (the rest stream from the file
        // as they are wanted, MeshData::StreamedFrom).
        static bool ReadPagePayloads(const std::filesystem::path& filepath, uint64_t payloadBase, MeshData& data, bool streamPages, uint32_t version)
        {
            if (data.Pages.empty())
                return true;
            // Version 3 payloads hold unpacked vertices; the streamer packs them as it reads (MeshData::StreamedVertexBytes).
            const uint32_t vertexBytes = version >= 4 ? sizeof(shaderio::PackedVertex) : sizeof(shaderio::Vertex);
            data.StreamedVertexBytes = vertexBytes;
            for (MeshPage& page : data.Pages)
                page.PayloadOffset += payloadBase;

            const bool streamed = streamPages && data.RootPageCount < data.Pages.size();
            const size_t readCount = streamed ? data.RootPageCount : data.Pages.size();
            const MeshPage& last = data.Pages[readCount == 0 ? 0 : readCount - 1];
            data.Vertices.resize(readCount == 0 ? 0 : last.FirstVertex + last.VertexCount);
            data.MeshletVertices.resize(readCount == 0 ? 0 : last.FirstMeshletVertex + last.MeshletVertexCount);
            data.MeshletTriangles.resize(readCount == 0 ? 0 : last.FirstMeshletTriangle + last.MeshletTriangleCount);
            if (readCount > 0)
            {
                // The pages read are contiguous in the file: one read, then each block to its array.
                const uint64_t begin = data.Pages[0].PayloadOffset;
                const uint64_t end = last.PayloadOffset + last.PayloadBytes(vertexBytes);
                std::vector<uint8_t> payload(end - begin);
                if (!VFS::ReadRange(filepath, begin, payload.size(), payload.data()))
                    return false;
                for (size_t index = 0; index < readCount; ++index)
                {
                    const MeshPage& page = data.Pages[index];
                    const uint8_t* source = payload.data() + (page.PayloadOffset - begin);
                    if (version >= 4)
                    {
                        const auto* packed = reinterpret_cast<const shaderio::PackedVertex*>(source);
                        for (uint32_t vertex = 0; vertex < page.VertexCount; ++vertex)
                            data.Vertices[page.FirstVertex + vertex] = VertexPacking::Unpack(packed[vertex]);
                    }
                    else
                    {
                        std::memcpy(data.Vertices.data() + page.FirstVertex, source, sizeof(shaderio::Vertex) * uint64_t(page.VertexCount));
                    }
                    source += page.VertexBlockBytes(vertexBytes);
                    std::memcpy(data.MeshletVertices.data() + page.FirstMeshletVertex, source, sizeof(uint32_t) * uint64_t(page.MeshletVertexCount));
                    source += page.MeshletVertexBlockBytes();
                    std::memcpy(data.MeshletTriangles.data() + page.FirstMeshletTriangle, source, page.MeshletTriangleCount);
                }
            }
            if (streamed)
                data.StreamedFrom = filepath;
            return true;
        }

       static void WriteMaterials(std::ofstream& stream, const std::vector<MaterialData>& materialList)
{
    uint32_t matCount = static_cast<uint32_t>(materialList.size());
    stream.write(reinterpret_cast<const char*>(&matCount), sizeof(uint32_t));
    for (const auto& mat : materialList)
    {
        SerializerUtils::WriteString(stream, mat.Name);
        
        // Workflow + Specular Glossines
        stream.write(reinterpret_cast<const char*>(&mat.Workflow), sizeof(float));
        stream.write(reinterpret_cast<const char*>(&mat.DiffuseFactor), sizeof(glm::vec4));
        stream.write(reinterpret_cast<const char*>(&mat.SpecularFactor), sizeof(glm::vec4));
        
        // Base Color
        stream.write(reinterpret_cast<const char*>(&mat.BaseColorFactor), sizeof(glm::vec4));
        SerializerUtils::WriteString(stream, mat.BaseColorTexturePath);
        stream.write(reinterpret_cast<const char*>(&mat.BaseColorTextureSet), sizeof(int32_t));
        
        // PBR Properties
        stream.write(reinterpret_cast<const char*>(&mat.MetallicFactor), sizeof(float));
        stream.write(reinterpret_cast<const char*>(&mat.RoughnessFactor), sizeof(float));
        SerializerUtils::WriteString(stream, mat.MetallicRoughnessTexturePath);
        stream.write(reinterpret_cast<const char*>(&mat.PhysicalDescriptorTextureSet), sizeof(int32_t));
        
        // Additional Maps
        SerializerUtils::WriteString(stream, mat.NormalTexturePath);
        stream.write(reinterpret_cast<const char*>(&mat.NormalTextureSet), sizeof(int32_t));
        
        SerializerUtils::WriteString(stream, mat.OcclusionTexturePath);
        stream.write(reinterpret_cast<const char*>(&mat.OcclusionTextureSet), sizeof(int32_t));
        
        // Emission
        stream.write(reinterpret_cast<const char*>(&mat.EmissiveFactor), sizeof(glm::vec3));
        SerializerUtils::WriteString(stream, mat.EmissiveTexturePath);
        stream.write(reinterpret_cast<const char*>(&mat.EmissiveTextureSet), sizeof(int32_t));
        stream.write(reinterpret_cast<const char*>(&mat.emissiveStrength), sizeof(float));
        
        // Transmission
        stream.write(reinterpret_cast<const char*>(&mat.TransmissionFactor), sizeof(float));
        SerializerUtils::WriteString(stream, mat.TransmissionTexturePath);
        stream.write(reinterpret_cast<const char*>(&mat.TransmissionTextureSet), sizeof(int32_t));
        
        // Alpha & Render settings
        uint32_t modeVal = static_cast<uint32_t>(mat.Mode);
        stream.write(reinterpret_cast<const char*>(&modeVal), sizeof(uint32_t));
        stream.write(reinterpret_cast<const char*>(&mat.AlphaMaskCutoff), sizeof(float));
        
        uint8_t doubleSidedVal = mat.DoubleSided ? 1 : 0;
        stream.write(reinterpret_cast<const char*>(&doubleSidedVal), sizeof(uint8_t));
        
        uint8_t unlitVal = mat.Unlit ? 1 : 0;
        stream.write(reinterpret_cast<const char*>(&unlitVal), sizeof(uint8_t));
    }
}

static void ReadMaterials(std::istream& stream, std::vector<MaterialData>& outMaterialList)
{
    uint32_t matCount = 0;
    stream.read(reinterpret_cast<char*>(&matCount), sizeof(uint32_t));
    if (stream.fail()) return;

    outMaterialList.resize(matCount);
    for (uint32_t i = 0; i < matCount; i++)
    {
        SerializerUtils::ReadString(stream, outMaterialList[i].Name);
        
        // Workflow + Specular Glossines
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].Workflow), sizeof(float));
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].DiffuseFactor), sizeof(glm::vec4));
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].SpecularFactor), sizeof(glm::vec4));
        
        // Base Color
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].BaseColorFactor), sizeof(glm::vec4));
        SerializerUtils::ReadString(stream, outMaterialList[i].BaseColorTexturePath);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].BaseColorTextureSet), sizeof(int32_t));
        
        // PBR Properties
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].MetallicFactor), sizeof(float));
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].RoughnessFactor), sizeof(float));
        SerializerUtils::ReadString(stream, outMaterialList[i].MetallicRoughnessTexturePath);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].PhysicalDescriptorTextureSet), sizeof(int32_t));
        
        // Additional Maps
        SerializerUtils::ReadString(stream, outMaterialList[i].NormalTexturePath);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].NormalTextureSet), sizeof(int32_t));
        
        SerializerUtils::ReadString(stream, outMaterialList[i].OcclusionTexturePath);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].OcclusionTextureSet), sizeof(int32_t));
        
        // Emission
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].EmissiveFactor), sizeof(glm::vec3));
        SerializerUtils::ReadString(stream, outMaterialList[i].EmissiveTexturePath);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].EmissiveTextureSet), sizeof(int32_t));
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].emissiveStrength), sizeof(float));
        
        // Transmission
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].TransmissionFactor), sizeof(float));
        SerializerUtils::ReadString(stream, outMaterialList[i].TransmissionTexturePath);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].TransmissionTextureSet), sizeof(int32_t));
        
        // Alpha & Render settings
        uint32_t modeVal = 0;
        stream.read(reinterpret_cast<char*>(&modeVal), sizeof(uint32_t));
        outMaterialList[i].Mode = static_cast<AlphaMode>(modeVal);
        stream.read(reinterpret_cast<char*>(&outMaterialList[i].AlphaMaskCutoff), sizeof(float));
        
        uint8_t doubleSidedVal = 0;
        stream.read(reinterpret_cast<char*>(&doubleSidedVal), sizeof(uint8_t));
        outMaterialList[i].DoubleSided = (doubleSidedVal != 0);
        
        uint8_t unlitVal = 0;
        stream.read(reinterpret_cast<char*>(&unlitVal), sizeof(uint8_t));
        outMaterialList[i].Unlit = (unlitVal != 0);

        // Compatibility with textureless transmission-only materials that inherited the glTF parser's synthetic
        // 1/1 PBR defaults (see MaterialSerializer). Keep the repair in Nox, not the vendored parser.
        MaterialData& material = outMaterialList[i];
        if (material.TransmissionFactor > 0.0f && material.MetallicFactor == 1.0f &&
            material.RoughnessFactor == 1.0f && material.BaseColorTexturePath.empty() &&
            material.MetallicRoughnessTexturePath.empty())
        {
            material.MetallicFactor = 0.0f;
            material.RoughnessFactor = 0.0f;
        }
    }
}
        
        static void WriteLights(std::ofstream& stream, const std::vector<LightNodeData>& lights)
            {
                uint32_t count = static_cast<uint32_t>(lights.size());
                stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
                for (const auto& l : lights)
                {
                    SerializerUtils::WriteString(stream, l.Name);
                    uint32_t type = static_cast<uint32_t>(l.Type);
                    stream.write(reinterpret_cast<const char*>(&type), sizeof(uint32_t));
                    stream.write(reinterpret_cast<const char*>(&l.NodeIndex), sizeof(int32_t));
                    stream.write(reinterpret_cast<const char*>(&l.Color), sizeof(glm::vec3));
                    stream.write(reinterpret_cast<const char*>(&l.Intensity), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&l.Range), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&l.InnerConeAngle), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&l.OuterConeAngle), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&l.Translation), sizeof(glm::vec3));
                    stream.write(reinterpret_cast<const char*>(&l.Rotation), sizeof(glm::quat));
                    stream.write(reinterpret_cast<const char*>(&l.Scale), sizeof(glm::vec3));
                }
            }

            static void ReadLights(std::istream& stream, std::vector<LightNodeData>& outLights)
            {
                uint32_t count = 0;
                stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
                if (stream.fail()) return;

                outLights.resize(count);
                for (uint32_t i = 0; i < count; i++)
                {
                    SerializerUtils::ReadString(stream, outLights[i].Name);
                    uint32_t type = 0;
                    stream.read(reinterpret_cast<char*>(&type), sizeof(uint32_t));
                    outLights[i].Type = static_cast<GltfLightType>(type);
                    stream.read(reinterpret_cast<char*>(&outLights[i].NodeIndex), sizeof(int32_t));
                    stream.read(reinterpret_cast<char*>(&outLights[i].Color), sizeof(glm::vec3));
                    stream.read(reinterpret_cast<char*>(&outLights[i].Intensity), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outLights[i].Range), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outLights[i].InnerConeAngle), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outLights[i].OuterConeAngle), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outLights[i].Translation), sizeof(glm::vec3));
                    stream.read(reinterpret_cast<char*>(&outLights[i].Rotation), sizeof(glm::quat));
                    stream.read(reinterpret_cast<char*>(&outLights[i].Scale), sizeof(glm::vec3));
                }
            }

            static void WriteNodes(std::ofstream& stream, const std::vector<MeshNodeData>& nodes)
            {
                uint32_t count = static_cast<uint32_t>(nodes.size());
                stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
                for (const auto& node : nodes)
                {
                    SerializerUtils::WriteString(stream, node.Name);
                    stream.write(reinterpret_cast<const char*>(&node.Parent), sizeof(int32_t));
                    stream.write(reinterpret_cast<const char*>(&node.Translation), sizeof(glm::vec3));
                    stream.write(reinterpret_cast<const char*>(&node.Rotation), sizeof(glm::quat));
                    stream.write(reinterpret_cast<const char*>(&node.Scale), sizeof(glm::vec3));
                    stream.write(reinterpret_cast<const char*>(&node.FirstSubmesh), sizeof(uint32_t));
                    stream.write(reinterpret_cast<const char*>(&node.SubmeshCount), sizeof(uint32_t));
                }
            }

            static void WriteCameras(std::ofstream& stream, const std::vector<CameraNodeData>& cameras)
            {
                uint32_t count = static_cast<uint32_t>(cameras.size());
                stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
                for (const auto& camera : cameras)
                {
                    SerializerUtils::WriteString(stream, camera.Name);
                    uint32_t type = static_cast<uint32_t>(camera.Type);
                    stream.write(reinterpret_cast<const char*>(&type), sizeof(uint32_t));
                    stream.write(reinterpret_cast<const char*>(&camera.NodeIndex), sizeof(int32_t));
                    stream.write(reinterpret_cast<const char*>(&camera.VerticalFov), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&camera.OrthographicSize), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&camera.NearClip), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&camera.FarClip), sizeof(float));
                    stream.write(reinterpret_cast<const char*>(&camera.Translation), sizeof(glm::vec3));
                    stream.write(reinterpret_cast<const char*>(&camera.Rotation), sizeof(glm::quat));
                    stream.write(reinterpret_cast<const char*>(&camera.Scale), sizeof(glm::vec3));
                }
            }

            static void ReadCameras(std::istream& stream, std::vector<CameraNodeData>& outCameras)
            {
                uint32_t count = 0;
                stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
                if (stream.fail()) return;

                outCameras.resize(count);
                for (uint32_t i = 0; i < count; i++)
                {
                    SerializerUtils::ReadString(stream, outCameras[i].Name);
                    uint32_t type = 0;
                    stream.read(reinterpret_cast<char*>(&type), sizeof(uint32_t));
                    outCameras[i].Type = static_cast<GltfCameraType>(type);
                    stream.read(reinterpret_cast<char*>(&outCameras[i].NodeIndex), sizeof(int32_t));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].VerticalFov), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].OrthographicSize), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].NearClip), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].FarClip), sizeof(float));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].Translation), sizeof(glm::vec3));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].Rotation), sizeof(glm::quat));
                    stream.read(reinterpret_cast<char*>(&outCameras[i].Scale), sizeof(glm::vec3));
                }
            }

            static void ReadNodes(std::istream& stream, std::vector<MeshNodeData>& outNodes)
            {
                uint32_t count = 0;
                stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
                if (stream.fail()) return;

                outNodes.resize(count);
                for (uint32_t i = 0; i < count; i++)
                {
                    SerializerUtils::ReadString(stream, outNodes[i].Name);
                    stream.read(reinterpret_cast<char*>(&outNodes[i].Parent), sizeof(int32_t));
                    stream.read(reinterpret_cast<char*>(&outNodes[i].Translation), sizeof(glm::vec3));
                    stream.read(reinterpret_cast<char*>(&outNodes[i].Rotation), sizeof(glm::quat));
                    stream.read(reinterpret_cast<char*>(&outNodes[i].Scale), sizeof(glm::vec3));
                    stream.read(reinterpret_cast<char*>(&outNodes[i].FirstSubmesh), sizeof(uint32_t));
                    stream.read(reinterpret_cast<char*>(&outNodes[i].SubmeshCount), sizeof(uint32_t));
                }
            }

        static constexpr uint32_t HeaderBytes = 4 + sizeof(uint32_t) + sizeof(uint64_t);

        static void SerializeMeshes(const std::filesystem::path& filepath, const char* magic, const std::vector<MeshData>& dataList,
                                    const std::vector<MaterialData>& materialList, const std::vector<LightNodeData>& lightList,
                                    const std::vector<MeshNodeData>& nodeList, const std::vector<CameraNodeData>& cameraList)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            stream.write(magic, 4);
            uint32_t count = static_cast<uint32_t>(dataList.size());
            stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
            uint64_t payloadBase = 0; // written once known
            stream.write(reinterpret_cast<const char*>(&payloadBase), sizeof(uint64_t));

            uint64_t payloadCursor = 0;
            for (const auto& data : dataList)
                WriteMeshData(stream, data, payloadCursor);
            WriteMaterials(stream, materialList);
            WriteLights(stream, lightList);
            WriteNodes(stream, nodeList);
            WriteCameras(stream, cameraList);

            static constexpr char Padding[16] = {};
            const uint64_t metadataEnd = static_cast<uint64_t>(stream.tellp());
            payloadBase = MeshPage::Align16(metadataEnd);
            stream.write(Padding, static_cast<std::streamsize>(payloadBase - metadataEnd));
            for (const auto& data : dataList)
                WritePagePayloads(stream, data);

            stream.seekp(4 + sizeof(uint32_t));
            stream.write(reinterpret_cast<const char*>(&payloadBase), sizeof(uint64_t));
        }

        // magic: this kind's version 4; magicV3: its version 3 (unpacked vertices); legacyMagic: its version 2 (no pages, read as one level
        // of always resident clusters).
        static bool DeserializeMeshes(const std::filesystem::path& filepath, const char* magic, const char* magicV3, const char* legacyMagic, bool streamPages,
                                      std::vector<MeshData>& outDataList, std::vector<MaterialData>& outMaterialList,
                                      std::vector<LightNodeData>& outLightList, std::vector<MeshNodeData>& outNodeList,
                                      std::vector<CameraNodeData>& outCameraList)
        {
            char header[HeaderBytes] = {};
            if (!VFS::ReadRange(filepath, 0, 4, header))
                return false;

            if (std::memcmp(header, legacyMagic, 4) == 0)
            {
                std::unique_ptr<std::istream> file = VFS::Open(filepath);
                if (!file) return false;
                std::istream& stream = *file;
                stream.seekg(4);
                uint32_t count = 0;
                stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
                outDataList.resize(count);
                for (uint32_t i = 0; i < count; i++)
                    ReadMeshDataV2(stream, outDataList[i]);
                ReadMaterials(stream, outMaterialList);
                ReadLights(stream, outLightList);
                ReadNodes(stream, outNodeList);
                ReadCameras(stream, outCameraList);
                return true;
            }
            const uint32_t version = std::memcmp(header, magic, 4) == 0 ? 4u : std::memcmp(header, magicV3, 4) == 0 ? 3u : 0u;
            if (version == 0 || !VFS::ReadRange(filepath, 0, HeaderBytes, header))
                return false;

            // Only what comes before the pages' data is read whole (from a pack as from a loose file).
            uint64_t payloadBase = 0;
            std::memcpy(&payloadBase, header + 4 + sizeof(uint32_t), sizeof(uint64_t));
            std::string metadata(payloadBase, '\0');
            if (payloadBase < HeaderBytes || !VFS::ReadRange(filepath, 0, payloadBase, metadata.data()))
                return false;
            std::istringstream stream(std::move(metadata));
            stream.seekg(HeaderBytes);

            uint32_t count = 0;
            std::memcpy(&count, header + 4, sizeof(uint32_t));
            outDataList.resize(count);
            for (uint32_t i = 0; i < count; i++)
                ReadMeshData(stream, outDataList[i], version);
            ReadMaterials(stream, outMaterialList);
            ReadLights(stream, outLightList);
            ReadNodes(stream, outNodeList);
            ReadCameras(stream, outCameraList);
            if (stream.fail())
                return false;

            for (MeshData& data : outDataList)
            {
                if (!ReadPagePayloads(filepath, payloadBase, data, streamPages, version))
                    return false;
            }
            return true;
        }

    public:
        // The cooked format's version (2, 3 or 4) from the magic, 0 when the file is not a mesh.
        static uint32_t ReadVersion(const std::filesystem::path& filepath)
        {
            char magic[4] = {};
            if (!VFS::ReadRange(filepath, 0, 4, magic))
                return 0;
            if (std::memcmp(magic, "NSM4", 4) == 0 || std::memcmp(magic, "NMS4", 4) == 0)
                return 4;
            if (std::memcmp(magic, "NSM3", 4) == 0 || std::memcmp(magic, "NMS3", 4) == 0)
                return 3;
            if (std::memcmp(magic, "NSM2", 4) == 0 || std::memcmp(magic, "NMS2", 4) == 0)
                return 2;
            return 0;
        }

        static void SerializeStaticMesh
        (
            const std::filesystem::path& filepath,
            const std::vector<MeshData>& dataList,
            const std::vector<MaterialData>& materialList,
            const std::vector<LightNodeData>& lightList = {},
            const std::vector<MeshNodeData>& nodeList = {},
            const std::vector<CameraNodeData>& cameraList = {}
        )
        {
            SerializeMeshes(filepath, "NSM4", dataList, materialList, lightList, nodeList, cameraList);
        }

        static bool DeserializeStaticMesh(const std::filesystem::path& filepath, std::vector<MeshData>& outDataList, std::vector<MaterialData>& outMaterialList)
        {
            std::vector<LightNodeData> dummyLights;
            std::vector<MeshNodeData> dummyNodes;
            std::vector<CameraNodeData> dummyCameras;
            return DeserializeStaticMesh(filepath, outDataList, outMaterialList, dummyLights, dummyNodes, dummyCameras);
        }
        
        static bool DeserializeStaticMesh
        (
            const std::filesystem::path& filepath,
            std::vector<MeshData>& outDataList,
            std::vector<MaterialData>& outMaterialList,
            std::vector<LightNodeData>& outLightList
            )
        {
            std::vector<MeshNodeData> dummyNodes;
            std::vector<CameraNodeData> dummyCameras;
            return DeserializeStaticMesh(filepath, outDataList, outMaterialList, outLightList, dummyNodes, dummyCameras);
        }

        // streamPages: only the root pages of streamed geometry are read (§5.7.2); otherwise every page.
        static bool DeserializeStaticMesh
        (
            const std::filesystem::path& filepath,
            std::vector<MeshData>& outDataList,
            std::vector<MaterialData>& outMaterialList,
            std::vector<LightNodeData>& outLightList,
            std::vector<MeshNodeData>& outNodeList,
            std::vector<CameraNodeData>& outCameraList,
            bool streamPages = false
            )
        {
            return DeserializeMeshes(filepath, "NSM4", "NSM3", "NSM2", streamPages, outDataList, outMaterialList, outLightList, outNodeList, outCameraList);
        }

        static void SerializeMesh
        (
            const std::filesystem::path& filepath,
            const std::vector<MeshData>& dataList,
            const std::vector<MaterialData>& materialList,
            const std::vector<LightNodeData>& lightList = {},
            const std::vector<MeshNodeData>& nodeList = {},
            const std::vector<CameraNodeData>& cameraList = {}
        )
        {
            SerializeMeshes(filepath, "NMS4", dataList, materialList, lightList, nodeList, cameraList);
        }
        
        static bool DeserializeMesh(const std::filesystem::path& filepath, std::vector<MeshData>& outDataList, std::vector<MaterialData>& outMaterialList)
        {
            std::vector<LightNodeData> dummyLights;
            std::vector<MeshNodeData> dummyNodes;
            std::vector<CameraNodeData> dummyCameras;
            return DeserializeMesh(filepath, outDataList, outMaterialList, dummyLights, dummyNodes, dummyCameras);
        }

        static bool DeserializeMesh
        (
            const std::filesystem::path& filepath,
            std::vector<MeshData>& outDataList,
            std::vector<MaterialData>& outMaterialList,
            std::vector<LightNodeData>& outLightList
        )
        {
            std::vector<MeshNodeData> dummyNodes;
            std::vector<CameraNodeData> dummyCameras;
            return DeserializeMesh(filepath, outDataList, outMaterialList, outLightList, dummyNodes, dummyCameras);
        }

        static bool DeserializeMesh
        (
            const std::filesystem::path& filepath,
            std::vector<MeshData>& outDataList,
            std::vector<MaterialData>& outMaterialList,
            std::vector<LightNodeData>& outLightList,
            std::vector<MeshNodeData>& outNodeList,
            std::vector<CameraNodeData>& outCameraList,
            bool streamPages = false
        )
        {
            return DeserializeMeshes(filepath, "NMS4", "NMS3", "NMS2", streamPages, outDataList, outMaterialList, outLightList, outNodeList, outCameraList);
        }
    };
}
