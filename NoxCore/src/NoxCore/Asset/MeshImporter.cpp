#include "MeshImporter.h"

#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <vector>

#define TINYGLTF3_IMPLEMENTATION
#define TINYGLTF3_ENABLE_FS 
#include <glm/gtx/matrix_decompose.hpp>

#include "tiny_gltf_v3.h"

#include "meshoptimizer.h"
// meshoptimizer's cluster LOD builder (demo/clusterlod.h), implemented in this file as its header asks.
#define CLUSTERLOD_IMPLEMENTATION
#include "clusterlod.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"

#include "NoxCore/Project/Project.h"
#include "NoxCore/Asset/MeshSerializer.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    namespace
    {
        // The members of Mesh and StaticMesh are the same.
        void fillMeshAsset(CookedMesh& cooked, std::vector<MeshHandle>& subMeshes, std::vector<std::string>& names,
                           std::vector<MaterialData>& materials, std::vector<LightNodeData>& lights, std::vector<CameraNodeData>& cameras,
                           std::vector<MeshNodeData>& nodes)
        {
            subMeshes.resize(cooked.Submeshes.size());
            names.reserve(cooked.Submeshes.size());
            for (const MeshData& submesh : cooked.Submeshes)
                names.push_back(submesh.Name);
            materials = std::move(cooked.Materials);
            lights = std::move(cooked.Lights);
            cameras = std::move(cooked.Cameras);
            nodes = std::move(cooked.Nodes);
        }

        // Culling bounds of one cluster from its local indices.
        shaderio::MeshletBounds clusterBounds(const MeshData& data, const unsigned int* vertices, const unsigned char* triangles, size_t triangleCount)
        {
            const meshopt_Bounds bounds = meshopt_computeMeshletBounds(vertices, triangles, triangleCount, &data.Vertices[0].pos.x,
                                                                       data.Vertices.size(), sizeof(shaderio::Vertex));
            shaderio::MeshletBounds result{};
            result.center = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
            result.radius = bounds.radius;
            result.coneApex = glm::vec3(bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]);
            result.coneCutoff = bounds.cone_cutoff;
            result.coneAxis = glm::vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]);
            result.triangleCount = static_cast<uint32_t>(triangleCount);
            return result;
        }

        // Appends one cluster: its local vertices and triangles, draw record and bounds.
        void appendCluster(MeshData& data, const shaderio::MeshletBounds& bounds, const unsigned int* vertices, size_t vertexCount,
                           const unsigned char* triangles, size_t triangleCount)
        {
            shaderio::MeshletDraw draw{};
            draw.vertexOffset = static_cast<uint32_t>(data.MeshletVertices.size());
            draw.triangleOffset = static_cast<uint32_t>(data.MeshletTriangles.size());
            draw.vertexCount = static_cast<uint32_t>(vertexCount);
            draw.triangleCount = static_cast<uint32_t>(triangleCount);
            draw.globalVertexOffset = 0;
            data.Draws.push_back(draw);
            data.Bounds.push_back(bounds);
            data.MeshletVertices.insert(data.MeshletVertices.end(), vertices, vertices + vertexCount);
            data.MeshletTriangles.insert(data.MeshletTriangles.end(), triangles, triangles + triangleCount * 3);
        }

        // One level of clusters, always drawn (no coarser version).
        void buildMeshlets(MeshData& data, const std::vector<uint32_t>& indices)
        {
            std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(indices.size(), shaderio::MAX_VERTICES, shaderio::MAX_PRIMITIVES));
            std::vector<unsigned int> meshletVertices(indices.size());
            std::vector<unsigned char> meshletTriangles(indices.size());
            meshlets.resize(meshopt_buildMeshlets(meshlets.data(), meshletVertices.data(), meshletTriangles.data(), indices.data(), indices.size(),
                                                  &data.Vertices[0].pos.x, data.Vertices.size(), sizeof(shaderio::Vertex),
                                                  shaderio::MAX_VERTICES, shaderio::MAX_PRIMITIVES, 0.0f));

            for (const meshopt_Meshlet& meshlet : meshlets)
            {
                unsigned int* vertices = &meshletVertices[meshlet.vertex_offset];
                unsigned char* triangles = &meshletTriangles[meshlet.triangle_offset];
                meshopt_optimizeMeshlet(vertices, triangles, meshlet.triangle_count, meshlet.vertex_count);

                shaderio::MeshletBounds bounds = clusterBounds(data, vertices, triangles, meshlet.triangle_count);
                bounds.lodCenter = bounds.center;
                bounds.lodRadius = bounds.radius;
                bounds.lodError = 0.0f;
                bounds.parentCenter = bounds.center;
                bounds.parentRadius = bounds.radius;
                bounds.parentError = shaderio::ClusterTerminalError;
                bounds.lodLevel = 0;
                appendCluster(data, bounds, vertices, meshlet.vertex_count, triangles, meshlet.triangle_count);
            }
        }

        // The cluster LOD DAG (§5.7) with meshoptimizer's clusterlod, set up as its demo/nanite.cpp: clusters of the
        // original geometry are merged into groups and simplified, level after level, until one cluster is left. Every
        // cluster keeps the bounds and error of the group it came from and of the group it went into, so the task shader
        // can pick the cut. Clusters index the submesh's vertices, like the meshlets.
        void buildClusterLod(MeshData& data, const std::vector<uint32_t>& indices)
        {
            clodConfig config = clodDefaultConfig(shaderio::MAX_PRIMITIVES);
            config.max_vertices = shaderio::MAX_VERTICES;
            // No sloppy fallback when regular simplification gets stuck: sloppy ignores topology and orientation, and a
            // flipped triangle on a flat surface (a street) costs almost no error, so it was drawn at a distance and back
            // face culled (holes). A stuck group stays at its level instead.
            config.simplify_fallback_sloppy = false;

            // Attribute-aware: the normal (weights as the demo). Permissive simplification may collapse across any
            // attribute discontinuity it is not told about (clusterlod.h: attribute_protect_mask), so every seam the
            // shading uses is protected: hard edges (normal, attributes 0-2) and both UV sets (uv0 3-4, uv1 5-6), in
            // Vertex order from the normal on.
            const float attributeWeights[3] = { 0.5f, 0.5f, 0.5f };
            clodMesh mesh{};
            mesh.indices = indices.data();
            mesh.index_count = indices.size();
            mesh.vertex_count = data.Vertices.size();
            mesh.vertex_positions = &data.Vertices[0].pos.x;
            mesh.vertex_positions_stride = sizeof(shaderio::Vertex);
            mesh.vertex_attributes = &data.Vertices[0].normal.x;
            mesh.vertex_attributes_stride = sizeof(shaderio::Vertex);
            mesh.attribute_weights = attributeWeights;
            mesh.attribute_count = std::size(attributeWeights);
            mesh.attribute_protect_mask = 0b1111111;

            std::vector<clodGroup> groups;
            std::array<unsigned int, shaderio::MAX_VERTICES> vertices{};
            std::array<unsigned char, shaderio::MAX_PRIMITIVES * 3> triangles{};
            clodBuild(config, mesh, [&](clodGroup group, const clodCluster* clusters, size_t clusterCount) -> int
            {
                for (size_t index = 0; index < clusterCount; ++index)
                {
                    const clodCluster& cluster = clusters[index];
                    const size_t triangleCount = cluster.index_count / 3;
                    const size_t vertexCount = clodLocalIndices(vertices.data(), triangles.data(), cluster.indices, cluster.index_count);

                    shaderio::MeshletBounds bounds = clusterBounds(data, vertices.data(), triangles.data(), triangleCount);
                    // The group this cluster was simplified from; the original clusters have no error.
                    const clodBounds& self = cluster.refined < 0 ? cluster.bounds : groups[cluster.refined].simplified;
                    bounds.lodCenter = glm::vec3(self.center[0], self.center[1], self.center[2]);
                    bounds.lodRadius = self.radius;
                    bounds.lodError = cluster.refined < 0 ? 0.0f : self.error;
                    // The group it went into (FLT_MAX error: never simplified further).
                    bounds.parentCenter = glm::vec3(group.simplified.center[0], group.simplified.center[1], group.simplified.center[2]);
                    bounds.parentRadius = group.simplified.radius;
                    bounds.parentError = group.simplified.error;
                    bounds.lodLevel = cluster.refined < 0 ? 0u : static_cast<uint32_t>(groups[cluster.refined].depth + 1);
                    appendCluster(data, bounds, vertices.data(), vertexCount, triangles.data(), triangleCount);
                }

                groups.push_back(group);
                return static_cast<int>(groups.size() - 1);
            });
        }
    }

    std::optional<CookedMesh> MeshImporter::ReadCookedMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        const std::filesystem::path cookedPath = assetDirectory / metadata.FilePath;
        const std::filesystem::path sourcePath = assetDirectory / metadata.SourceFilePath;

        // Same test as ImportMesh / ImportStaticMesh.
        XXH128_hash_t cookedHash{};
        if (!std::filesystem::exists(cookedPath) || !Utility::loadHashFromFile(cookedPath.string() + ".hash", cookedHash) ||
            !XXH128_isEqual(Utility::calcul_hash_streaming(sourcePath.string()), cookedHash))
        {
            return std::nullopt;
        }

        CookedMesh cooked;
        const bool success = metadata.Type == AssetType::StaticMesh
            ? MeshSerializer::DeserializeStaticMesh(cookedPath, cooked.Submeshes, cooked.Materials, cooked.Lights, cooked.Nodes, cooked.Cameras)
            : MeshSerializer::DeserializeMesh(cookedPath, cooked.Submeshes, cooked.Materials, cooked.Lights, cooked.Nodes, cooked.Cameras);
        if (!success)
            return std::nullopt;
        return cooked;
    }

    Ref<Asset> MeshImporter::CreateMeshAsset(AssetType type, CookedMesh& cooked)
    {
        if (type == AssetType::StaticMesh)
        {
            Ref<StaticMesh> mesh = CreateRef<StaticMesh>();
            fillMeshAsset(cooked, mesh->m_SubMeshes, mesh->m_SubmeshNames, mesh->m_Materials, mesh->m_Lights, mesh->m_Cameras, mesh->m_Nodes);
            return Ref<Asset>(mesh);
        }

        Ref<Mesh> mesh = CreateRef<Mesh>();
        fillMeshAsset(cooked, mesh->m_SubMeshes, mesh->m_SubmeshNames, mesh->m_Materials, mesh->m_Lights, mesh->m_Cameras, mesh->m_Nodes);
        return Ref<Asset>(mesh);
    }

    void MeshImporter::SetSubMesh(Asset& mesh, size_t index, const MeshHandle& handle)
    {
        if (mesh.GetType() == AssetType::StaticMesh)
            static_cast<StaticMesh&>(mesh).m_SubMeshes[index] = handle;
        else
            static_cast<Mesh&>(mesh).m_SubMeshes[index] = handle;
    }

    std::filesystem::path MeshImporter::MaterialAssetPath(const std::filesystem::path& meshFilePath, const MaterialData& material, size_t index)
    {
        std::string name = material.Name;
        constexpr const char* invalid = "<>:\"/\\|?*";
        for (char& character : name)
        {
            if (std::strchr(invalid, character) != nullptr)
                character = '_';
        }
        if (name.empty())
            name = "Material_" + std::to_string(index);
        return meshFilePath.parent_path().parent_path() / "Materials" / (meshFilePath.stem().string() + "_" + name + ".nmat");
    }

    bool MeshImporter::CookMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        const std::filesystem::path cookedPath = assetDirectory / metadata.FilePath;
        const std::filesystem::path sourcePath = assetDirectory / metadata.SourceFilePath;
        const bool isStatic = metadata.Type == AssetType::StaticMesh;
        NOX_CORE_INFO("Cooking GLTF from {} to {}", sourcePath.string(), cookedPath.string());

        std::vector<MaterialData> materialDataList;
        std::vector<LightNodeData> lightDataList;
        std::vector<MeshNodeData> nodeDataList;
        std::vector<CameraNodeData> cameraDataList;
        Skeleton extractedSkeleton;
        std::vector<Ref<AnimationSequence>> extractedAnimations;

        const std::vector<MeshData> meshDataList = ParseGltfToMeshData(sourcePath, materialDataList, extractedSkeleton, extractedAnimations,
                                                                       lightDataList, nodeDataList, cameraDataList);
        if (meshDataList.empty())
        {
            NOX_CORE_WARN("MeshImporter::CookMesh - Failed to load or empty mesh at source path: {}", sourcePath.string());
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(cookedPath.parent_path(), error);
        if (isStatic)
            MeshSerializer::SerializeStaticMesh(cookedPath, meshDataList, materialDataList, lightDataList, nodeDataList, cameraDataList);
        else
            MeshSerializer::SerializeMesh(cookedPath, meshDataList, materialDataList, lightDataList, nodeDataList, cameraDataList);

        if (!isStatic)
        {
            // A node/object animation does not require a skeleton. Only write .nskel when the glTF contains actual skin
            // data; animation clips are serialized independently below.
            if (!extractedSkeleton.Skins.empty())
            {
                std::filesystem::path skelPath = cookedPath;
                skelPath.replace_extension(".nskel");
                SkeletonSerializer::Serialize(skelPath, extractedSkeleton);
                NOX_CORE_INFO("[Importer] Extracted and cooked Skeleton ({} nodes) to {}", extractedSkeleton.AllNodes.size(), skelPath.string());
            }

            for (const Ref<AnimationSequence>& animation : extractedAnimations)
            {
                std::filesystem::path animPath = cookedPath.parent_path() / (cookedPath.stem().string() + "_" + animation->Name + ".nanim");
                AnimationSerializer::Serialize(animPath, *animation);
                NOX_CORE_INFO("[Importer] Extracted and cooked Animation Sequence '{}' to {}", animation->Name, animPath.string());
            }
        }

        // One .nmat per material, written once: edits made to it later survive a recook of the model.
        for (size_t index = 0; index < materialDataList.size(); ++index)
        {
            const std::filesystem::path materialPath = assetDirectory / MaterialAssetPath(metadata.FilePath, materialDataList[index], index);
            if (!std::filesystem::exists(materialPath, error))
            {
                std::filesystem::create_directories(materialPath.parent_path(), error);
                MaterialSerializer::Serialize(materialPath, materialDataList[index]);
            }
        }

        // Last: an interrupted cook leaves no hash, so the model is cooked again.
        Utility::saveHashToFile(cookedPath.string() + ".hash", Utility::calcul_hash_streaming(sourcePath.string()));
        return true;
    }

    Ref<Mesh> MeshImporter::ImportMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        NOX_CORE_INFO("MeshImporter::ImportMesh loading mesh from {}", (assetDirectory / metadata.FilePath).string());

        std::optional<CookedMesh> cooked = ReadCookedMesh(assetDirectory, metadata);
        if (!cooked && CookMesh(assetDirectory, metadata))
            cooked = ReadCookedMesh(assetDirectory, metadata);
        if (!cooked)
        {
            NOX_CORE_ERROR("MeshImporter::ImportMesh - could not load {}", metadata.FilePath.generic_string());
            return Ref<Mesh>(nullptr);
        }
        return Ref<Mesh>(uploadMeshAsset(AssetType::Mesh, *cooked));
    }

    Ref<StaticMesh> MeshImporter::ImportStaticMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        NOX_CORE_INFO("MeshImporter::ImportStaticMesh loading static mesh from {}", (assetDirectory / metadata.FilePath).string());

        std::optional<CookedMesh> cooked = ReadCookedMesh(assetDirectory, metadata);
        if (!cooked && CookMesh(assetDirectory, metadata))
            cooked = ReadCookedMesh(assetDirectory, metadata);
        if (!cooked)
        {
            NOX_CORE_ERROR("MeshImporter::ImportStaticMesh - could not load {}", metadata.FilePath.generic_string());
            return Ref<StaticMesh>(nullptr);
        }
        return Ref<StaticMesh>(uploadMeshAsset(AssetType::StaticMesh, *cooked));
    }

    Ref<Asset> MeshImporter::uploadMeshAsset(AssetType type, CookedMesh& cooked)
    {
        // Synchronous: every submesh uploaded and published now (the frame waits for the copies).
        std::vector<bool> opaque;
        for (size_t index = 0; index < cooked.Submeshes.size(); ++index)
            opaque.push_back(index < cooked.Materials.size() ? cooked.Materials[index].Mode == AlphaMode::Opaque : true);

        Ref<Asset> mesh = CreateMeshAsset(type, cooked);
        const std::vector<MeshData>& submeshes = cooked.Submeshes;

        const auto uploadStart = std::chrono::steady_clock::now();
        {
            NOX_PROFILE_SCOPE("Mesh GPU Upload");
            for (size_t index = 0; index < submeshes.size(); ++index)
                SetSubMesh(*mesh, index, Renderer::UploadMesh(submeshes[index], opaque[index]));
        }
        NOX_CORE_INFO("[AssetLoad] GPU upload of {} submesh(es) (geometry + BLAS) took {:.1f} ms", submeshes.size(),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count());
        return mesh;
    }

    Ref<Mesh> MeshImporter::LoadMesh(const std::filesystem::path& path)
    {
        NOX_ASSERT(fasle, "broken");
        /*NOX_CORE_INFO("MeshImporter::LoadMesh loading raw mesh from {}", path.string());
        
        std::filesystem::path sourcePath = path;
        std::filesystem::path cookedPath = sourcePath;
        cookedPath.replace_extension(".nmesh");
        
        std::vector<MeshData> meshDataList;
        std::vector<MaterialData> materialDataList;
        
        if (std::filesystem::exists(cookedPath))
        {
            bool success = MeshSerializer::DeserializeMesh(path, meshDataList, materialDataList);
            if (!success) NOX_CORE_ASSERT(false, "MeshImporter::LoadMesh - Failed to deserialize .nmesh file");
        }
        else
        {
            meshDataList = ParseGltfToMeshData(path, materialDataList, TODO, TODO);
            if (meshDataList.empty())
            {
                NOX_CORE_ASSERT("MeshImporter::LoadMesh - Failed to load or empty mesh at path: {}", path.string());
                return Ref<Mesh>(nullptr);
            }
            
            MeshSerializer::SerializeMesh(cookedPath, meshDataList, materialDataList);
        }
        
        Ref<Mesh> meshAsset = CreateRef<Mesh>();
        
        // Upload each sub-mesh independently -> Vector of Handles
        for (const auto& data : meshDataList)
        {
            MeshHandle subMeshHandle = Renderer::UploadMesh(data);
            meshAsset->m_SubMeshes.push_back(subMeshHandle);
            meshAsset->m_SubmeshNames.push_back(data.Name);
        }
        meshAsset->m_Materials = std::move(materialDataList);
        
        return meshAsset;*/
    }

    int32_t FindAttribute(const tg3_primitive& primitive, const char* name)
    {
        for (uint32_t i = 0; i < primitive.attributes_count; i++)
        {
            const tg3_str_int_pair& attr = primitive.attributes[i];

            if (strcmp(attr.key.data, name) == 0)
            {
                return attr.value;
            }
        }

        return -1;
    }

    static void LoadNodeHierarchy(int32_t nodeIndex, Node* parent, const tg3_model& model, Skeleton& outSkeleton)
    {
        const tg3_node& tg3Node = model.nodes[nodeIndex];

        Node* newNode = new Node();
        newNode->Index = nodeIndex;
        newNode->Parent = parent;
        newNode->Name = (tg3Node.name.data && tg3Node.name.len > 0)
                            ? std::string(tg3Node.name.data, tg3Node.name.len)
                            : ("Node_" + std::to_string(nodeIndex));

        // 1. Extract base transform
        bool hasMatrix = (tg3Node.has_matrix != 0);

        if (hasMatrix)
        {
            newNode->HasRestMatrix = true;
            newNode->RestMatrix = glm::mat4(glm::make_mat4(tg3Node.matrix));

            glm::vec3 skew;
            glm::vec4 perspective;
            glm::decompose(newNode->RestMatrix, newNode->RestScale, newNode->RestRotation, newNode->RestTranslation, skew, perspective);
        }
        else
        {
            newNode->HasRestMatrix = false;
            newNode->RestTranslation = glm::vec3(static_cast<float>(tg3Node.translation[0]), static_cast<float>(tg3Node.translation[1]), static_cast<float>(tg3Node.translation[2]));
            newNode->RestRotation = glm::quat(static_cast<float>(tg3Node.rotation[3]), static_cast<float>(tg3Node.rotation[0]), static_cast<float>(tg3Node.rotation[1]),
                                              static_cast<float>(tg3Node.rotation[2]));
            newNode->RestScale = glm::vec3(static_cast<float>(tg3Node.scale[0]), static_cast<float>(tg3Node.scale[1]), static_cast<float>(tg3Node.scale[2]));

            newNode->RestMatrix = glm::translate(glm::mat4(1.0f), newNode->RestTranslation) *
                glm::toMat4(newNode->RestRotation) *
                glm::scale(glm::mat4(1.0f), newNode->RestScale);
        }

        // Sync active fields to rest values
        newNode->Translation = newNode->RestTranslation;
        newNode->Rotation = newNode->RestRotation;
        newNode->Scale = newNode->RestScale;
        newNode->Matrix = newNode->RestMatrix;
        newNode->HasMatrix = newNode->HasRestMatrix;

        // 2. Register node in the system
        outSkeleton.AllNodes[nodeIndex] = newNode;

        if (parent)
            parent->Children.push_back(newNode);
        else
            outSkeleton.RootNodes.push_back(newNode);

        // 3. Recurse children
        for (uint32_t c = 0; c < tg3Node.children_count; c++)
        {
            LoadNodeHierarchy(tg3Node.children[c], newNode, model, outSkeleton);
        }
    }

    static void ParseSkeletonFromGltf(const tg3_model& model, Skeleton& outSkeleton)
    {
        if (model.nodes_count == 0) return;

        // Pre-allocate node list so we can lookup by index
        outSkeleton.AllNodes.resize(model.nodes_count, nullptr);

        // Find root nodes (Nodes that have no parents)
        std::vector<bool> isChild(model.nodes_count, false);
        for (uint32_t i = 0; i < model.nodes_count; i++)
        {
            for (uint32_t c = 0; c < model.nodes[i].children_count; c++)
            {
                isChild[model.nodes[i].children[c]] = true;
            }
        }

        // Load full hierarchy starting from roots
        for (uint32_t i = 0; i < model.nodes_count; i++)
        {
            if (!isChild[i])
            {
                LoadNodeHierarchy(i, nullptr, model, outSkeleton);
            }
        }

        // Load Skins EXACTLY as the joints array specifies
        for (uint32_t i = 0; i < model.skins_count; i++)
        {
            const tg3_skin& tg3Skin = model.skins[i];
            Skin* newSkin = new Skin();
            newSkin->Name = (tg3Skin.name.data && tg3Skin.name.len > 0)
                                ? std::string(tg3Skin.name.data, tg3Skin.name.len)
                                : ("Skin_" + std::to_string(i));

            // This is the critical fix. We map joint index [j] DIRECTLY to the node pointer
            for (uint32_t j = 0; j < tg3Skin.joints_count; j++)
            {
                int32_t nodeIndex = tg3Skin.joints[j];
                newSkin->Joints.push_back(outSkeleton.AllNodes[nodeIndex]);
            }

            // Get Inverse Bind Matrices
            if (tg3Skin.inverse_bind_matrices >= 0)
            {
                const tg3_accessor& acc = model.accessors[tg3Skin.inverse_bind_matrices];
                const tg3_buffer_view& bv = model.buffer_views[acc.buffer_view];
                const tg3_buffer& buf = model.buffers[bv.buffer];
                const float* dataPtr = reinterpret_cast<const float*>(&buf.data.data[bv.byte_offset + acc.byte_offset]);

                newSkin->InverseBindMatrices.resize(acc.count);
                for (size_t m = 0; m < acc.count; m++)
                {
                    newSkin->InverseBindMatrices[m] = glm::make_mat4(dataPtr + (m * 16));
                }
            }

            outSkeleton.Skins.push_back(newSkin);
        }
    }

    static bool Tg3StrEquals(const tg3_str& str, std::string_view expected)
    {
        if (!str.data) return false;
        return std::string_view(str.data, str.len) == expected;
    }

    // Helper: Parses all animation tracks and safely handles Linear & CubicSpline interpolation
    static void ParseAnimationsFromGltf
    (
        const tg3_model& model,
        const Skeleton& skeleton,
        std::vector<Ref<AnimationSequence>>& outAnimations
    )
    {
        outAnimations.clear();

        for (uint32_t animIndex = 0; animIndex < model.animations_count; animIndex++)
        {
            const tg3_animation& tg3Anim = model.animations[animIndex];
            Ref<AnimationSequence> animSeq = CreateRef<AnimationSequence>();

            animSeq->Name = (tg3Anim.name.data && tg3Anim.name.len > 0)
                                ? std::string(tg3Anim.name.data, tg3Anim.name.len)
                                : ("Anim_" + std::to_string(animIndex));

            float maxDuration = 0.0f;

            for (uint32_t channelIdx = 0; channelIdx < tg3Anim.channels_count; channelIdx++)
            {
                const tg3_animation_channel& channel = tg3Anim.channels[channelIdx];

                if (channel.sampler < 0 || channel.sampler >= (int32_t)tg3Anim.samplers_count)
                    continue;

                const tg3_animation_sampler& sampler = tg3Anim.samplers[channel.sampler];

                // --- CRITICAL FIX: Direct Node Index Mapping ---
                int32_t targetNodeIdx = channel.target.node;

                // Ensure the node index is valid within our newly parsed skeleton
                if (targetNodeIdx < 0 || targetNodeIdx >= (int32_t)skeleton.AllNodes.size())
                    continue;

                const tg3_node& targetNode = model.nodes[targetNodeIdx];
                std::string nodeName = (targetNode.name.data && targetNode.name.len > 0)
                                           ? std::string(targetNode.name.data, targetNode.name.len)
                                           : ("Node_" + std::to_string(targetNodeIdx));

                // Find by TargetNodeIndex instead of string name
                auto channelIt = std::find_if(animSeq->Channels.begin(), animSeq->Channels.end(),
                                              [&](const NodeAnimationChannel& c) { return c.TargetNodeIndex == targetNodeIdx; });

                NodeAnimationChannel* animChannel = nullptr;
                if (channelIt != animSeq->Channels.end())
                {
                    animChannel = &(*channelIt);
                }
                else
                {
                    animSeq->Channels.push_back({});
                    animChannel = &animSeq->Channels.back();
                    animChannel->NodeName = nodeName;
                    animChannel->TargetNodeIndex = targetNodeIdx; // Matches Node::Index in Skeleton
                }

                // Interpolation setup
                if (Tg3StrEquals(sampler.interpolation, "STEP"))
                    animChannel->Interpolation = AnimationInterpolation::Step;
                else if (Tg3StrEquals(sampler.interpolation, "CUBICSPLINE"))
                    animChannel->Interpolation = AnimationInterpolation::CubicSpline;
                else
                    animChannel->Interpolation = AnimationInterpolation::Linear;

                const tg3_accessor& timeAcc = model.accessors[sampler.input];
                const tg3_buffer_view& timeView = model.buffer_views[timeAcc.buffer_view];
                const tg3_buffer& timeBuf = model.buffers[timeView.buffer];
                const float* timePtr = reinterpret_cast<const float*>(
                    &timeBuf.data.data[timeView.byte_offset + timeAcc.byte_offset]);

                const tg3_accessor& valAcc = model.accessors[sampler.output];
                const tg3_buffer_view& valView = model.buffer_views[valAcc.buffer_view];
                const tg3_buffer& valBuf = model.buffers[valView.buffer];
                const float* valPtr = reinterpret_cast<const float*>(
                    &valBuf.data.data[valView.byte_offset + valAcc.byte_offset]);

                size_t keyCount = timeAcc.count;
                bool isCubic = (animChannel->Interpolation == AnimationInterpolation::CubicSpline);
                size_t strideMultiplier = isCubic ? 3 : 1;
                size_t valueOffset = isCubic ? 1 : 0;

                // Keyframe extraction (Unchanged, your math here is already correct)
                if (Tg3StrEquals(channel.target.path, "translation"))
                {
                    for (size_t k = 0; k < keyCount; k++)
                    {
                        float t = timePtr[k];
                        size_t idx = (k * strideMultiplier + valueOffset) * 3;
                        animChannel->PositionKeys.push_back({t, glm::vec3(valPtr[idx], valPtr[idx + 1], valPtr[idx + 2])});
                        maxDuration = std::max(maxDuration, t);
                    }
                }
                else if (Tg3StrEquals(channel.target.path, "rotation"))
                {
                    for (size_t k = 0; k < keyCount; k++)
                    {
                        float t = timePtr[k];
                        size_t idx = (k * strideMultiplier + valueOffset) * 4;
                        glm::quat q(valPtr[idx + 3], valPtr[idx + 0], valPtr[idx + 1], valPtr[idx + 2]);
                        animChannel->RotationKeys.push_back({t, q});
                        maxDuration = std::max(maxDuration, t);
                    }
                }
                else if (Tg3StrEquals(channel.target.path, "scale"))
                {
                    for (size_t k = 0; k < keyCount; k++)
                    {
                        float t = timePtr[k];
                        size_t idx = (k * strideMultiplier + valueOffset) * 3;
                        animChannel->ScaleKeys.push_back({t, glm::vec3(valPtr[idx], valPtr[idx + 1], valPtr[idx + 2])});
                        maxDuration = std::max(maxDuration, t);
                    }
                }
            }

            animSeq->Duration = maxDuration;
            animSeq->TicksPerSecond = 1.0f; // Since glTF time is always in absolute seconds
            outAnimations.push_back(animSeq);
        }
    }


    static std::vector<uint8_t> DecodeBase64(std::string_view input)
    {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";

        std::vector<uint8_t> output;
        output.reserve((input.size() * 3) / 4);

        uint32_t buffer = 0;
        int bits = 0;

        for (unsigned char c : input)
        {
            // Ignore whitespace
            if (std::isspace(c))
                continue;

            // Padding
            if (c == '=')
                break;

            const char* pos = std::strchr(alphabet, c);
            if (!pos)
                continue;

            const uint32_t value =
                static_cast<uint32_t>(pos - alphabet);

            buffer = (buffer << 6) | value;
            bits += 6;

            if (bits >= 8)
            {
                bits -= 8;
                output.push_back(
                    static_cast<uint8_t>((buffer >> bits) & 0xFF)
                );
            }
        }

        return output;
    }

    static std::filesystem::path ExtractGltfImage(
        const tg3_model& model,
        const tg3_image& image,
        int32_t imageIndex,
        const std::filesystem::path& modelPath)
    {
        std::filesystem::path textureDir =
            modelPath.parent_path() / "Textures";

        std::filesystem::create_directories(textureDir);

        std::string extension = ".png";

        if (image.mime_type.data && image.mime_type.len > 0)
        {
            std::string mime(image.mime_type.data, image.mime_type.len);

            if (mime == "image/jpeg")
                extension = ".jpg";
            else if (mime == "image/png")
                extension = ".png";
            else if (mime == "image/webp")
                extension = ".webp";
            else if (mime == "image/ktx2")
                extension = ".ktx2";
            else
                NOX_CORE_WARN("Unknown embedded image MIME type: {}", mime);
        }

        std::filesystem::path outputPath =
            textureDir /
            (modelPath.stem().string() +
                "_tex_" +
                std::to_string(imageIndex) +
                extension);

        // ------------------------------------------------------------
        // 1. data:image/...;base64,...
        // ------------------------------------------------------------

        if (image.uri.data && image.uri.len > 0)
        {
            std::string uri(image.uri.data, image.uri.len);

            if (uri.starts_with("data:"))
            {
                const size_t commaPos = uri.find(',');

                if (commaPos == std::string::npos)
                {
                    NOX_CORE_ERROR(
                        "[Importer] Invalid data URI for image {}",
                        imageIndex
                    );

                    return {};
                }

                std::string_view encodedData =
                    std::string_view(uri).substr(commaPos + 1);

                std::vector<uint8_t> decoded =
                    DecodeBase64(encodedData);

                if (decoded.empty())
                {
                    NOX_CORE_ERROR(
                        "[Importer] Failed to decode base64 image {}",
                        imageIndex
                    );

                    return {};
                }

                std::ofstream outFile(
                    outputPath,
                    std::ios::binary
                );

                if (!outFile)
                {
                    NOX_CORE_ERROR(
                        "[Importer] Failed to create texture file: {}",
                        outputPath.string()
                    );

                    return {};
                }

                outFile.write(
                    reinterpret_cast<const char*>(decoded.data()),
                    static_cast<std::streamsize>(decoded.size())
                );

                outFile.close();

                NOX_CORE_INFO(
                    "[Importer] Extracted base64 texture {} -> {}",
                    imageIndex,
                    outputPath.string()
                );

                return outputPath;
            }

            // ------------------------------------------------------------
            // 2. Normal external URI
            // ------------------------------------------------------------

            return modelPath.parent_path() / uri;
        }

        // ------------------------------------------------------------
        // 3. GLB bufferView embedded image
        // ------------------------------------------------------------

        if (image.buffer_view >= 0 &&
            image.buffer_view < static_cast<int32_t>(model.buffer_views_count))
        {
            const auto& bufView =
                model.buffer_views[image.buffer_view];

            if (bufView.buffer < 0 ||
                bufView.buffer >= static_cast<int32_t>(model.buffers_count))
            {
                NOX_CORE_ERROR(
                    "[Importer] Invalid buffer index for image {}",
                    imageIndex
                );

                return {};
            }

            const auto& buffer =
                model.buffers[bufView.buffer];

            const uint8_t* imgData =
                buffer.data.data + bufView.byte_offset;

            const size_t imgSize =
                bufView.byte_length;

            std::ofstream outFile(
                outputPath,
                std::ios::binary
            );

            if (!outFile)
            {
                NOX_CORE_ERROR(
                    "[Importer] Failed to create texture file: {}",
                    outputPath.string()
                );

                return {};
            }

            outFile.write(
                reinterpret_cast<const char*>(imgData),
                static_cast<std::streamsize>(imgSize)
            );

            outFile.close();

            NOX_CORE_INFO(
                "[Importer] Extracted bufferView texture {} -> {}",
                imageIndex,
                outputPath.string()
            );

            return outputPath;
        }

        return {};
    }

    auto getFloatVal = [](const tg3_value& val) -> float {
        if (val.type == TG3_VALUE_REAL)
            return static_cast<float>(val.real_val);
        if (val.type == TG3_VALUE_INT)
            return static_cast<float>(val.int_val);
        return 1.0f; // Default fallback
    };

    static int32_t GetTextureSourceImageIndex(const tg3_texture& texture)
    {
        for (uint32_t i = 0; i < texture.ext.extensions_count; i++)
        {
            const tg3_extension& ext = texture.ext.extensions[i];
            if (std::string_view(ext.name.data, ext.name.len) != "MSFT_texture_dds" ||
                ext.value.type != TG3_VALUE_OBJECT)
            {
                continue;
            }

            for (uint32_t j = 0; j < ext.value.object_count; j++)
            {
                const auto& kv = ext.value.object_data[j];
                if (std::string_view(kv.key.data, kv.key.len) == "source" &&
                    kv.value.type == TG3_VALUE_INT)
                {
                    return static_cast<int32_t>(kv.value.int_val);
                }
            }
        }

        return texture.source;
    }

    static void ExtractNodeTRS(const tg3_node& node, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale)
    {
        if (node.has_matrix)
        {
            glm::mat4 m = glm::make_mat4(node.matrix);
            glm::vec3 skew;
            glm::vec4 perspective;
            glm::decompose(m, scale, rotation, translation, skew, perspective);
            return;
        }

        translation = glm::vec3(
            static_cast<float>(node.translation[0]),
            static_cast<float>(node.translation[1]),
            static_cast<float>(node.translation[2])
        );

        rotation = glm::quat(
            static_cast<float>(node.rotation[3]),
            static_cast<float>(node.rotation[0]),
            static_cast<float>(node.rotation[1]),
            static_cast<float>(node.rotation[2])
        );

        scale = glm::vec3(
            static_cast<float>(node.scale[0]),
            static_cast<float>(node.scale[1]),
            static_cast<float>(node.scale[2])
        );
    }
    
    static void ParseLightsFromGltf(const tg3_model& model, std::vector<LightNodeData>& outLights)
        {
            outLights.clear();
            for (uint32_t i = 0; i < model.nodes_count; i++)
            {
                const tg3_node& node = model.nodes[i];
                if (node.light < 0 || node.light >= static_cast<int32_t>(model.lights_count))
                    continue;

                const tg3_light& gltfLight = model.lights[node.light];
                LightNodeData l{};
                l.NodeIndex = static_cast<int32_t>(i);

                if (node.name.data && node.name.len > 0)
                    l.Name = std::string(node.name.data, node.name.len);
                else if (gltfLight.name.data && gltfLight.name.len > 0)
                    l.Name = std::string(gltfLight.name.data, gltfLight.name.len);
                else
                    l.Name = "Light_" + std::to_string(node.light);

                l.Color = glm::vec3(
                    static_cast<float>(gltfLight.color[0]),
                    static_cast<float>(gltfLight.color[1]),
                    static_cast<float>(gltfLight.color[2])
                );

                // glTF KHR_lights_punctual intensities are photometric (lux for directional,
                // candela for point/spot) - real-world values in the hundreds to thousands.
                // This engine's lighting/tonemap pipeline is calibrated in unitless radiance
                // multipliers around 1-10 (see DirectionalLightComponent/PointLightComponent's
                // own defaults), so raw photometric values would blow every surface out to white.
                // Dividing by 683 lm/W (luminous efficacy of 555nm light - the standard constant
                // Khronos' own glTF-Sample-Viewer and Filament use for this exact conversion)
                // brings them into that same range.
                static constexpr float LUMINOUS_EFFICACY_LM_PER_W = 683.0f;
                l.Intensity = static_cast<float>(gltfLight.intensity) / LUMINOUS_EFFICACY_LM_PER_W;
                l.Range = static_cast<float>(gltfLight.range);

                std::string_view typeStr = (gltfLight.type.data && gltfLight.type.len > 0)
                    ? std::string_view(gltfLight.type.data, gltfLight.type.len)
                    : std::string_view("point");

                if (typeStr == "directional")
                {
                    l.Type = GltfLightType::Directional;
                }
                else if (typeStr == "spot")
                {
                    l.Type = GltfLightType::Spot;
                    l.InnerConeAngle = glm::degrees(static_cast<float>(gltfLight.spot.inner_cone_angle));
                    l.OuterConeAngle = glm::degrees(static_cast<float>(gltfLight.spot.outer_cone_angle));
                    if (l.Range <= 0.0f) l.Range = 10.0f;
                }
                else // "point"
                {
                    l.Type = GltfLightType::Point;
                    if (l.Range <= 0.0f) l.Range = 10.0f;
                }

                ExtractNodeTRS(node, l.Translation, l.Rotation, l.Scale);

                outLights.push_back(l);
            }
        }
    
    // glTF 2.0 cameras look down their node's local -Z with +Y up, the same convention as the engine's cameras,
    // so the node transform can be used as the camera transform unchanged.
    static void ParseCamerasFromGltf(const tg3_model& model, std::vector<CameraNodeData>& outCameras)
    {
        outCameras.clear();
        for (uint32_t i = 0; i < model.nodes_count; i++)
        {
            const tg3_node& node = model.nodes[i];
            if (node.camera < 0 || node.camera >= static_cast<int32_t>(model.cameras_count))
                continue;

            const tg3_camera& gltfCamera = model.cameras[node.camera];
            CameraNodeData camera{};
            camera.NodeIndex = static_cast<int32_t>(i);

            if (node.name.data && node.name.len > 0)
                camera.Name = std::string(node.name.data, node.name.len);
            else if (gltfCamera.name.data && gltfCamera.name.len > 0)
                camera.Name = std::string(gltfCamera.name.data, gltfCamera.name.len);
            else
                camera.Name = "Camera_" + std::to_string(node.camera);

            std::string_view typeStr = (gltfCamera.type.data && gltfCamera.type.len > 0)
                ? std::string_view(gltfCamera.type.data, gltfCamera.type.len)
                : std::string_view("perspective");

            if (typeStr == "orthographic")
            {
                camera.Type = GltfCameraType::Orthographic;
                camera.OrthographicSize = 2.0f * static_cast<float>(gltfCamera.orthographic.ymag);
                camera.NearClip = static_cast<float>(gltfCamera.orthographic.znear);
                camera.FarClip = static_cast<float>(gltfCamera.orthographic.zfar);
            }
            else // "perspective"
            {
                camera.Type = GltfCameraType::Perspective;
                camera.VerticalFov = static_cast<float>(gltfCamera.perspective.yfov);
                camera.NearClip = static_cast<float>(gltfCamera.perspective.znear);
                if (gltfCamera.perspective.zfar > 0.0) // 0 = infinite projection in glTF
                    camera.FarClip = static_cast<float>(gltfCamera.perspective.zfar);
            }

            ExtractNodeTRS(node, camera.Translation, camera.Rotation, camera.Scale);
            outCameras.push_back(camera);
        }
    }

    std::vector<MeshData> MeshImporter::ParseGltfToMeshData
    (
        const std::filesystem::path& path,
        std::vector<MaterialData>& outMaterials,
        Skeleton& outSkeleton,
        std::vector<Ref<AnimationSequence>>& outAnimations,
        std::vector<LightNodeData>& outLights,
        std::vector<MeshNodeData>& outNodes,
        std::vector<CameraNodeData>& outCameras
    )
    {
        /*
            Here is the exact layout based on your EditorCamera class:
            Up: +Y
            Right: +X
            Forward: -Z
        */
        std::vector<MeshData> result;

        tg3_parse_options opts;
        tg3_error_stack errors;
        tg3_model model;

        tg3_parse_options_init(&opts);
        tg3_error_stack_init(&errors);

        tg3_error_code err = tg3_parse_file(&model, &errors, path.string().c_str(), path.string().size(), &opts);
        if (err != TG3_OK)
        {
            for (uint32_t i = 0; i < errors.count; i++)
            {
                NOX_CORE_ERROR("[{}] {}", (int)errors.entries[i].severity, errors.entries[i].message ? errors.entries[i].message : "(null)");
            }
            tg3_error_stack_free(&errors);
            return result;
        }

        ParseSkeletonFromGltf(model, outSkeleton);
        ParseAnimationsFromGltf(model, outSkeleton, outAnimations);
        ParseLightsFromGltf(model, outLights);
        ParseCamerasFromGltf(model, outCameras);
        outNodes.clear();

         // 1. Build parent hierarchy map to resolve accumulated world transforms
            std::vector<int32_t> parentMap(model.nodes_count, -1);
            for (uint32_t i = 0; i < model.nodes_count; i++)
            {
                const auto& n = model.nodes[i];
                for (uint32_t c = 0; c < n.children_count; c++)
                {
                    int32_t childIdx = n.children[c];
                    if (childIdx >= 0 && childIdx < static_cast<int32_t>(model.nodes_count))
                    {
                        parentMap[childIdx] = static_cast<int32_t>(i);
                    }
                }
            }

            if (model.nodes_count > 0)
            {
                outNodes.resize(model.nodes_count);
                for (uint32_t i = 0; i < model.nodes_count; i++)
                {
                    const tg3_node& node = model.nodes[i];
                    MeshNodeData& nodeData = outNodes[i];
                    nodeData.Name = (node.name.data && node.name.len > 0)
                                        ? std::string(node.name.data, node.name.len)
                                        : ("Node_" + std::to_string(i));
                    nodeData.Parent = parentMap[i];
                    ExtractNodeTRS(node, nodeData.Translation, nodeData.Rotation, nodeData.Scale);
                }
            }
            else
            {
                outNodes.resize(model.meshes_count);
                for (uint32_t i = 0; i < model.meshes_count; i++)
                {
                    MeshNodeData& nodeData = outNodes[i];
                    const tg3_mesh& mesh = model.meshes[i];
                    nodeData.Name = (mesh.name.data && mesh.name.len > 0)
                                        ? std::string(mesh.name.data, mesh.name.len)
                                        : ("Mesh_" + std::to_string(i));
                }
            }

            uint32_t totalInstances = (model.nodes_count > 0) ? model.nodes_count : model.meshes_count;

            // Instancing: every glTF mesh is cooked once; each node referencing it points at the same submesh range, so
            // geometry and BLAS exist once per unique mesh (glTF materials belong to primitives, so they are shared too).
            struct SubmeshRange
            {
                uint32_t First = UINT32_MAX;
                uint32_t Count = 0;
            };
            std::vector<SubmeshRange> meshSubmeshes(model.meshes_count);

            for (uint32_t instanceIdx = 0; instanceIdx < totalInstances; instanceIdx++)
            {
                uint32_t meshIndex = 0;
                std::string meshName;

                if (model.nodes_count > 0)
                {
                    const auto& node = model.nodes[instanceIdx];
                    if (node.mesh < 0 || node.mesh >= static_cast<int32_t>(model.meshes_count))
                        continue; // Skip nodes that don't reference a mesh (e.g. empty groupings, cameras)

                    meshIndex = static_cast<uint32_t>(node.mesh);
                    meshName = outNodes[instanceIdx].Name;
                }
                else
                {
                    meshIndex = instanceIdx;
                    meshName = outNodes[instanceIdx].Name;
                }

                const tg3_mesh& mesh = model.meshes[meshIndex];

                MeshNodeData& nodeData = outNodes[instanceIdx];
                SubmeshRange& sharedRange = meshSubmeshes[meshIndex];
                if (sharedRange.First != UINT32_MAX)
                {
                    nodeData.FirstSubmesh = sharedRange.First;
                    nodeData.SubmeshCount = sharedRange.Count;
                    continue;
                }

                // Submeshes are shared by every node using the mesh: name them after the mesh, not the first node.
                if (mesh.name.data && mesh.name.len > 0)
                    meshName = std::string(mesh.name.data, mesh.name.len);
                else if (meshName.empty())
                    meshName = "Instance_" + std::to_string(instanceIdx);

                nodeData.FirstSubmesh = static_cast<uint32_t>(result.size());
            
            for (uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives_count; primitiveIndex++)
            {
                const tg3_primitive& primitive = mesh.primitives[primitiveIndex];
                MeshData primitiveData{};

                std::string materialName;
                if (primitive.material >= 0 && primitive.material < static_cast<int32_t>(model.materials_count))
                {
                    const auto& gltfMaterial = model.materials[primitive.material];
                    if (gltfMaterial.name.data != nullptr && gltfMaterial.name.len > 0)
                        materialName = std::string(gltfMaterial.name.data, gltfMaterial.name.len);
                }

                if (mesh.primitives_count > 1 && !materialName.empty())
                {
                    primitiveData.Name = materialName;
                }
                else if (!meshName.empty())
                {
                    if (mesh.primitives_count > 1)
                        primitiveData.Name = meshName + "_" + std::to_string(primitiveIndex);
                    else
                        primitiveData.Name = meshName; // Results in "bunny", "fox", etc.
                }
                else
                {
                    // Ultimate fallback if even the node had no name
                    primitiveData.Name = "Submesh_" + std::to_string(meshIndex);
                }

                // Get vertex positions
                const tg3_accessor& posAccessor = model.accessors[FindAttribute(primitive, "POSITION")];
                const tg3_buffer_view& posBufferView = model.buffer_views[posAccessor.buffer_view];
                const tg3_buffer& posBuffer = model.buffers[posBufferView.buffer];

                // Get normals
                bool hasNormals = FindAttribute(primitive, "NORMAL") != -1;
                const tg3_accessor* normalAccessor = nullptr;
                const tg3_buffer_view* normalBufferView = nullptr;
                const tg3_buffer* normalBuffer = nullptr;

                if (hasNormals)
                {
                    normalAccessor = &model.accessors[FindAttribute(primitive, "NORMAL")];
                    normalBufferView = &model.buffer_views[normalAccessor->buffer_view];
                    normalBuffer = &model.buffers[normalBufferView->buffer];
                }

                // Get texture coordinates (TEXCOORD_0)
                bool hasTexCoords = FindAttribute(primitive, "TEXCOORD_0") != -1;
                const tg3_accessor* texCoordAccessor = nullptr;
                const tg3_buffer_view* texCoordBufferView = nullptr;
                const tg3_buffer* texCoordBuffer = nullptr;

                if (hasTexCoords)
                {
                    texCoordAccessor = &model.accessors[FindAttribute(primitive, "TEXCOORD_0")];
                    texCoordBufferView = &model.buffer_views[texCoordAccessor->buffer_view];
                    texCoordBuffer = &model.buffers[texCoordBufferView->buffer];
                }

                // Get texture coordinates (TEXCOORD_1)
                bool hasTexCoords1 = FindAttribute(primitive, "TEXCOORD_1") != -1;
                const tg3_accessor* texCoord1Accessor = nullptr;
                const tg3_buffer_view* texCoord1BufferView = nullptr;
                const tg3_buffer* texCoord1Buffer = nullptr;

                if (hasTexCoords1)
                {
                    texCoord1Accessor = &model.accessors[FindAttribute(primitive, "TEXCOORD_1")];
                    texCoord1BufferView = &model.buffer_views[texCoord1Accessor->buffer_view];
                    texCoord1Buffer = &model.buffers[texCoord1BufferView->buffer];
                }

                bool hasSkinning = (FindAttribute(primitive, "JOINTS_0") != -1 && FindAttribute(primitive, "WEIGHTS_0") != -1);
                const tg3_accessor* jointsAccessor = nullptr;
                const tg3_buffer_view* jointsBufferView = nullptr;
                const tg3_buffer* jointsBuffer = nullptr;
                const tg3_accessor* weightsAccessor = nullptr;
                const tg3_buffer_view* weightsBufferView = nullptr;
                const tg3_buffer* weightsBuffer = nullptr;

                if (hasSkinning)
                {
                    jointsAccessor = &model.accessors[FindAttribute(primitive, "JOINTS_0")];
                    jointsBufferView = &model.buffer_views[jointsAccessor->buffer_view];
                    jointsBuffer = &model.buffers[jointsBufferView->buffer];

                    weightsAccessor = &model.accessors[FindAttribute(primitive, "WEIGHTS_0")];
                    weightsBufferView = &model.buffer_views[weightsAccessor->buffer_view];
                    weightsBuffer = &model.buffers[weightsBufferView->buffer];
                }

                size_t primitiveVertexCount = posAccessor.count;
                primitiveData.Vertices.reserve(primitiveVertexCount);

                for (size_t i = 0; i < primitiveVertexCount; i++)
                {
                    shaderio::Vertex vertex{};

                    uint32_t posStride = posBufferView.byte_stride ? posBufferView.byte_stride : 12;
                    const float* pos = reinterpret_cast<const float*>(&posBuffer.data.data[posBufferView.byte_offset + posAccessor.byte_offset + (i * posStride)]);
                    // glTF uses a right-handed coordinate system with Y-up
                    // Vulkan uses a right-handed coordinate system with Y-down
                    // We need to flip the Y coordinate
                    // i dont need that look first line in load model
                    
                    if (!hasSkinning)
                    {
                        vertex.pos = {pos[0], pos[1], pos[2]};
                    }
                    else
                    {
                        vertex.pos = {pos[0], pos[1], pos[2]};
                    }

                    if (hasNormals)
                    {
                        uint32_t normalStride = normalBufferView->byte_stride ? normalBufferView->byte_stride : 12;
                        size_t normalOffset = normalBufferView->byte_offset + normalAccessor->byte_offset + (i * normalStride);
                        const float* norm = reinterpret_cast<const float*>(&normalBuffer->data.data[normalOffset]);

                        if (!hasSkinning)
                        {
                            vertex.normal = {norm[0], norm[1], norm[2]};
                        }
                        else
                        {
                            vertex.normal = {norm[0], norm[1], norm[2]};
                        }
                    }
                    else
                    {
                        vertex.normal = {0.0f, 1.0f, 0.0f};
                    }

                    if (hasTexCoords)
                    {
                        uint32_t texStride = texCoordBufferView->byte_stride ? texCoordBufferView->byte_stride : 8;
                        const float* texCoord = reinterpret_cast<const float*>(&texCoordBuffer->data.data[texCoordBufferView->byte_offset + texCoordAccessor->byte_offset + (i * texStride)]);

                        vertex.uv0 = {texCoord[0], texCoord[1]};
                    }
                    else
                    {
                        vertex.uv0 = {0.0f, 0.0f};
                    }

                    if (hasTexCoords1)
                    {
                        uint32_t texStride1 = texCoord1BufferView->byte_stride ? texCoord1BufferView->byte_stride : 8;
                        const float* texCoord1 = reinterpret_cast<const float*>(&texCoord1Buffer->data.data[texCoord1BufferView->byte_offset + texCoord1Accessor->byte_offset + (i * texStride1)]);
                        vertex.uv1 = {texCoord1[0], texCoord1[1]};
                    }
                    else
                    {
                        vertex.uv1 = {0.0f, 0.0f};
                    }

                    if (hasSkinning)
                    {
                        // --- 1. Resolve Strides (Accounts for interleaved buffers) ---
                        uint32_t jointStride = jointsBufferView->byte_stride ? jointsBufferView->byte_stride : (jointsAccessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT ? 8 : 4);

                        uint32_t weightStride = weightsBufferView->byte_stride
                                                    ? weightsBufferView->byte_stride
                                                    : (weightsAccessor->component_type == TG3_COMPONENT_TYPE_FLOAT ? 16 : weightsAccessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT ? 8 : 4);

                        // --- 2. Parse Joints ---
                        size_t jointOffset = jointsBufferView->byte_offset + jointsAccessor->byte_offset + (i * jointStride);
                        if (jointsAccessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT)
                        {
                            const uint16_t* joints = reinterpret_cast<const uint16_t*>(&jointsBuffer->data.data[jointOffset]);
                            vertex.boneIDs = glm::uvec4(joints[0], joints[1], joints[2], joints[3]);
                        }
                        else if (jointsAccessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_BYTE)
                        {
                            const uint8_t* joints = reinterpret_cast<const uint8_t*>(&jointsBuffer->data.data[jointOffset]);
                            vertex.boneIDs = glm::uvec4(joints[0], joints[1], joints[2], joints[3]);
                        }

                        // --- 3. Parse Weights & Normalize ---
                        size_t weightOffset = weightsBufferView->byte_offset + weightsAccessor->byte_offset + (i * weightStride);
                        glm::vec4 rawWeights(0.0f);

                        if (weightsAccessor->component_type == TG3_COMPONENT_TYPE_FLOAT)
                        {
                            const float* weights = reinterpret_cast<const float*>(&weightsBuffer->data.data[weightOffset]);
                            rawWeights = glm::vec4(weights[0], weights[1], weights[2], weights[3]);
                        }
                        else if (weightsAccessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT)
                        {
                            const uint16_t* weights = reinterpret_cast<const uint16_t*>(&weightsBuffer->data.data[weightOffset]);
                            float scale = weightsAccessor->normalized ? (1.0f / 65535.0f) : 1.0f;
                            rawWeights = glm::vec4(weights[0], weights[1], weights[2], weights[3]) * scale;
                        }
                        else if (weightsAccessor->component_type == TG3_COMPONENT_TYPE_UNSIGNED_BYTE)
                        {
                            const uint8_t* weights = reinterpret_cast<const uint8_t*>(&weightsBuffer->data.data[weightOffset]);
                            float scale = weightsAccessor->normalized ? (1.0f / 255.0f) : 1.0f;
                            rawWeights = glm::vec4(weights[0], weights[1], weights[2], weights[3]) * scale;
                        }

                        // Force normalization so weights always sum to 1.0 (prevents collapse/stretching)
                        float weightSum = rawWeights.x + rawWeights.y + rawWeights.z + rawWeights.w;
                        if (weightSum > 0.0f)
                        {
                            vertex.boneWeights = rawWeights / weightSum;
                        }
                        else
                        {
                            vertex.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                        }
                    }
                    else
                    {
                        vertex.boneIDs = glm::uvec4(0);
                        vertex.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                    }

                    primitiveData.Vertices.push_back(vertex);
                }

                std::vector<uint32_t> primitiveIndices;

                if (primitive.indices >= 0)
                {
                    // Get indices
                    const tg3_accessor& indexAccessor = model.accessors[primitive.indices];
                    const tg3_buffer_view& indexBufferView = model.buffer_views[indexAccessor.buffer_view];
                    const tg3_buffer& indexBuffer = model.buffers[indexBufferView.buffer];

                    const unsigned char* indexData = &indexBuffer.data.data[indexBufferView.byte_offset + indexAccessor.byte_offset];
                    size_t indexCount = indexAccessor.count;
                    size_t indexStride = 0;

                    // Determine index stride based on component type
                    if (indexAccessor.component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT) indexStride = sizeof(uint16_t);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_UNSIGNED_INT) indexStride = sizeof(uint32_t);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_UNSIGNED_BYTE) indexStride = sizeof(uint8_t);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_BYTE) indexStride = sizeof(int8_t);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_SHORT) indexStride = sizeof(int16_t);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_INT) indexStride = sizeof(int32_t);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_FLOAT) indexStride = sizeof(float);
                    else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_DOUBLE) indexStride = sizeof(double);
                    else
                    {
                        NOX_CORE_ERROR("Unsupported index component type encountered. Value: {}", indexAccessor.component_type);
                        throw std::runtime_error("Unsupported index component type");
                    }

                    primitiveIndices.reserve(indexCount);

                    for (size_t i = 0; i < indexCount; i++)
                    {
                        uint32_t index = 0;

                        if (indexAccessor.component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT) index = *reinterpret_cast<const uint16_t*>(indexData + i * indexStride);
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_UNSIGNED_INT) index = *reinterpret_cast<const uint32_t*>(indexData + i * indexStride);
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_UNSIGNED_BYTE) index = *reinterpret_cast<const uint8_t*>(indexData + i * indexStride);
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_BYTE) index = static_cast<uint32_t>(*reinterpret_cast<const int8_t*>(indexData + i * indexStride));
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_SHORT) index = static_cast<uint32_t>(*reinterpret_cast<const int16_t*>(indexData + i * indexStride));
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_INT) index = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(indexData + i * indexStride));
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_FLOAT) index = static_cast<uint32_t>(*reinterpret_cast<const float*>(indexData + i * indexStride));
                        else if (indexAccessor.component_type == TG3_COMPONENT_TYPE_DOUBLE) index = static_cast<uint32_t>(*reinterpret_cast<const double*>(indexData + i * indexStride));

                        primitiveIndices.push_back(index);
                    }
                }
                else
                {
                    // Non-indexed primitive fallback: generate sequential indices
                    primitiveIndices.reserve(primitiveVertexCount);
                    for (size_t i = 0; i < primitiveVertexCount; i++)
                    {
                        primitiveIndices.push_back(static_cast<uint32_t>(i));
                    }
                }

                // Clusters (§5.7): static geometry gets the cluster LOD DAG; skinned geometry one level (simplified in the bind
                // pose, it would deform wrongly).
                if (hasSkinning)
                    buildMeshlets(primitiveData, primitiveIndices);
                else
                    buildClusterLod(primitiveData, primitiveIndices);

                MaterialData materialData{};

                // Materials
                if (primitive.material >= 0 && primitive.material < (int32_t)model.materials_count)
                {
                    const auto& gltfMaterial = model.materials[primitive.material];

                    // Material Name
                    if (gltfMaterial.name.data != nullptr && gltfMaterial.name.len > 0)
                        materialData.Name = std::string(gltfMaterial.name.data, gltfMaterial.name.len);
                    else
                        materialData.Name = "Material_" + std::to_string(primitive.material);

                    // Alpha Mode ("OPAQUE", "MASK", "BLEND")
                    if (gltfMaterial.alpha_mode.data != nullptr && gltfMaterial.alpha_mode.len > 0)
                    {
                        std::string_view modeStr(gltfMaterial.alpha_mode.data, gltfMaterial.alpha_mode.len);
                        if (modeStr == "OPAQUE" || modeStr == "Opaque") materialData.Mode = AlphaMode::Opaque;
                        else if (modeStr == "MASK") materialData.Mode = AlphaMode::Mask;
                        else if (modeStr == "BLEND") materialData.Mode = AlphaMode::Blend;
                    }
                    else
                    {
                        materialData.Mode = AlphaMode::Opaque;
                    }

                    // Alpha Cutoff & Double Sided
                    materialData.AlphaMaskCutoff = static_cast<float>(gltfMaterial.alpha_cutoff);
                    materialData.DoubleSided = (gltfMaterial.double_sided != 0);

                    // --- Helper Lambda to safely extract textures ---
                    auto GetTexturePath = [&](int32_t texIndex) -> std::string
                    {
                        if (texIndex >= 0 && texIndex < (int32_t)model.textures_count)
                        {
                            int32_t imageIndex = GetTextureSourceImageIndex(model.textures[texIndex]);
                            if (imageIndex >= 0 && imageIndex < (int32_t)model.images_count)
                            {
                                std::filesystem::path texPath = ExtractGltfImage(model, model.images[imageIndex], imageIndex, path);
                                return texPath.string();
                            }
                        }
                        return "";
                    };

                    // Base Color Factor
                    materialData.BaseColorFactor = glm::vec4(
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[0]),
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[1]),
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[2]),
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[3])
                    );
                    materialData.BaseColorTexturePath = GetTexturePath(gltfMaterial.pbr_metallic_roughness.base_color_texture.index);
                    materialData.BaseColorTextureSet = gltfMaterial.pbr_metallic_roughness.base_color_texture.tex_coord;

                    // Metallic & Roughness Factors
                    materialData.MetallicFactor = static_cast<float>(gltfMaterial.pbr_metallic_roughness.metallic_factor);
                    materialData.RoughnessFactor = static_cast<float>(gltfMaterial.pbr_metallic_roughness.roughness_factor);

                    // Metallic-Roughness Texture
                    // (Note: glTF packs Roughness in the Green channel, Metallic in the Blue channel)
                    materialData.MetallicRoughnessTexturePath = GetTexturePath(gltfMaterial.pbr_metallic_roughness.metallic_roughness_texture.index);
                    materialData.PhysicalDescriptorTextureSet = gltfMaterial.pbr_metallic_roughness.metallic_roughness_texture.tex_coord;

                    // Normal Texture
                    materialData.NormalTexturePath = GetTexturePath(gltfMaterial.normal_texture.index);
                    materialData.NormalTextureSet = gltfMaterial.normal_texture.tex_coord;

                    // Ambient Occlusion Texture
                    materialData.OcclusionTexturePath = GetTexturePath(gltfMaterial.occlusion_texture.index);
                    materialData.OcclusionTextureSet = gltfMaterial.occlusion_texture.tex_coord;

                    // Emissive Factor & Texture
                    materialData.EmissiveFactor = glm::vec3(
                        static_cast<float>(gltfMaterial.emissive_factor[0]),
                        static_cast<float>(gltfMaterial.emissive_factor[1]),
                        static_cast<float>(gltfMaterial.emissive_factor[2])
                    );
                    materialData.EmissiveTexturePath = GetTexturePath(gltfMaterial.emissive_texture.index);
                    materialData.EmissiveTextureSet = gltfMaterial.emissive_texture.tex_coord;
                    float emissiveStrength = 1.0f;

                    // Default setup
                    materialData.Workflow = 0.0f;
                    materialData.DiffuseFactor = glm::vec4(1.0f);
                    materialData.SpecularFactor = glm::vec4(1.0f); // Default: rgb = 1.0 (specular), a = 1.0 (glossiness)

                    // Check Extensions
                    for (uint32_t i = 0; i < gltfMaterial.ext.extensions_count; i++)
                    {
                        const tg3_extension& ext = gltfMaterial.ext.extensions[i];
                        
                        if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_unlit")
                        {
                            materialData.Unlit = true;
                        }
                        
                        if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_emissive_strength")
                        {
                            if (ext.value.type == TG3_VALUE_OBJECT)
                            {
                                for (uint32_t j = 0; j < ext.value.object_count; j++)
                                {
                                    const auto& kv = ext.value.object_data[j];
                                    if (std::string_view(kv.key.data, kv.key.len) == "emissiveStrength")
                                    {
                                        if (kv.value.type == TG3_VALUE_REAL)
                                        {
                                            emissiveStrength = static_cast<float>(kv.value.real_val);
                                        }
                                        else if (kv.value.type == TG3_VALUE_INT)
                                        {
                                            emissiveStrength = static_cast<float>(kv.value.int_val);
                                        }
                                    }
                                }
                            }
                        }

                        if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_pbrSpecularGlossiness")
                        {
                            materialData.Workflow = 1.0f;

                            if (ext.value.type == TG3_VALUE_OBJECT)
                            {
                                for (uint32_t j = 0; j < ext.value.object_count; j++)
                                {
                                    const auto& kv = ext.value.object_data[j];
                                    std::string_view key(kv.key.data, kv.key.len);

                                    // 1. diffuseFactor (vec4)
                                    if (key == "diffuseFactor" && kv.value.type == TG3_VALUE_ARRAY && kv.value.array_count >= 4)
                                    {
                                        materialData.DiffuseFactor = glm::vec4(
                                            getFloatVal(kv.value.array_data[0]),
                                            getFloatVal(kv.value.array_data[1]),
                                            getFloatVal(kv.value.array_data[2]),
                                            getFloatVal(kv.value.array_data[3])
                                        );
                                    }
                                    // 2. specularFactor (vec3 packed into rgb)
                                    else if (key == "specularFactor" && kv.value.type == TG3_VALUE_ARRAY && kv.value.array_count >= 3)
                                    {
                                        materialData.SpecularFactor.r = getFloatVal(kv.value.array_data[0]);
                                        materialData.SpecularFactor.g = getFloatVal(kv.value.array_data[1]);
                                        materialData.SpecularFactor.b = getFloatVal(kv.value.array_data[2]);
                                    }
                                    // 3. glossinessFactor (float packed into a)
                                    else if (key == "glossinessFactor")
                                    {
                                        materialData.SpecularFactor.a = getFloatVal(kv.value);
                                    }
                                    // 4. diffuseTexture
                                    else if (key == "diffuseTexture" && kv.value.type == TG3_VALUE_OBJECT)
                                    {
                                        for (uint32_t t = 0; t < kv.value.object_count; t++)
                                        {
                                            const auto& texKv = kv.value.object_data[t];
                                            if (std::string_view(texKv.key.data, texKv.key.len) == "index")
                                                materialData.BaseColorTexturePath = GetTexturePath(static_cast<int32_t>(texKv.value.int_val));
                                            else if (std::string_view(texKv.key.data, texKv.key.len) == "texCoord")
                                                materialData.BaseColorTextureSet = static_cast<int32_t>(texKv.value.int_val);
                                        }
                                    }
                                    // 5. specularGlossinessTexture
                                    else if (key == "specularGlossinessTexture" && kv.value.type == TG3_VALUE_OBJECT)
                                    {
                                        for (uint32_t t = 0; t < kv.value.object_count; t++)
                                        {
                                            const auto& texKv = kv.value.object_data[t];
                                            if (std::string_view(texKv.key.data, texKv.key.len) == "index")
                                                materialData.MetallicRoughnessTexturePath = GetTexturePath(static_cast<int32_t>(texKv.value.int_val));
                                            else if (std::string_view(texKv.key.data, texKv.key.len) == "texCoord")
                                                materialData.PhysicalDescriptorTextureSet = static_cast<int32_t>(texKv.value.int_val);
                                        }
                                    }
                                }
                            }
                        }
                        
                        if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_transmission")
                            {
                                if (ext.value.type == TG3_VALUE_OBJECT)
                                {
                                    for (uint32_t j = 0; j < ext.value.object_count; j++)
                                    {
                                        const auto& kv = ext.value.object_data[j];
                                        std::string_view key(kv.key.data, kv.key.len);

                                        if (key == "transmissionFactor")
                                        {
                                            if (kv.value.type == TG3_VALUE_REAL)
                                                materialData.TransmissionFactor = static_cast<float>(kv.value.real_val);
                                            else if (kv.value.type == TG3_VALUE_INT)
                                                materialData.TransmissionFactor = static_cast<float>(kv.value.int_val);
                                        }
                                        else if (key == "transmissionTexture" && kv.value.type == TG3_VALUE_OBJECT)
                                        {
                                            for (uint32_t t = 0; t < kv.value.object_count; t++)
                                            {
                                                const auto& texKv = kv.value.object_data[t];
                                                std::string_view tkey(texKv.key.data, texKv.key.len);
                                                if (tkey == "index")
                                                    materialData.TransmissionTexturePath = GetTexturePath(static_cast<int32_t>(texKv.value.int_val));
                                                else if (tkey == "texCoord")
                                                    materialData.TransmissionTextureSet = static_cast<int32_t>(texKv.value.int_val);
                                            }
                                        }
                                    }
                                }
                            }

                        if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_ior")
                        {
                            if (ext.value.type == TG3_VALUE_OBJECT)
                            {
                                for (uint32_t j = 0; j < ext.value.object_count; j++)
                                {
                                    const auto& kv = ext.value.object_data[j];
                                    if (std::string_view(kv.key.data, kv.key.len) == "ior")
                                    {
                                        if (kv.value.type == TG3_VALUE_REAL)
                                            materialData.IOR = static_cast<float>(kv.value.real_val);
                                        else if (kv.value.type == TG3_VALUE_INT)
                                            materialData.IOR = static_cast<float>(kv.value.int_val);
                                    }
                                }
                            }
                        }

                        if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_volume")
                        {
                            if (ext.value.type == TG3_VALUE_OBJECT)
                            {
                                for (uint32_t j = 0; j < ext.value.object_count; j++)
                                {
                                    const auto& kv = ext.value.object_data[j];
                                    if (std::string_view(kv.key.data, kv.key.len) == "thicknessFactor")
                                    {
                                        if (kv.value.type == TG3_VALUE_REAL)
                                            materialData.Thickness = static_cast<float>(kv.value.real_val);
                                        else if (kv.value.type == TG3_VALUE_INT)
                                            materialData.Thickness = static_cast<float>(kv.value.int_val);
                                    }
                                }
                            }
                        }
                    }
                    materialData.emissiveStrength = emissiveStrength;
                }

                outMaterials.push_back(materialData);
                result.push_back(std::move(primitiveData));
            }

            nodeData.SubmeshCount = static_cast<uint32_t>(result.size()) - nodeData.FirstSubmesh;
            sharedRange = { nodeData.FirstSubmesh, nodeData.SubmeshCount };
        }

        tg3_model_free(&model);
        tg3_error_stack_free(&errors);

        return result;
    }
}
