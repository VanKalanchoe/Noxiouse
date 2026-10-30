#include "MeshImporter.h"
#include "NoxCore/Core/VirtualFileSystem.h"

#include <algorithm>
#include <cfloat>

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <vector>

#define TINYGLTF3_IMPLEMENTATION
#define TINYGLTF3_ENABLE_FS 
#include <glm/gtx/matrix_decompose.hpp>
#include "NoxCore/Animation/Animator.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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

#include <yaml-cpp/yaml.h>

namespace Nox
{
    namespace
    {
        // The members of Mesh and StaticMesh are the same.
        void fillMeshAsset(CookedMesh& cooked, std::vector<MeshHandle>& subMeshes, std::vector<std::string>& names,
                           std::vector<MaterialData>& materials, std::vector<LightNodeData>& lights, std::vector<CameraNodeData>& cameras,
                           std::vector<MeshNodeData>& nodes, std::vector<std::vector<MaterialVariantMapping>>& variants)
        {
            subMeshes.resize(cooked.Submeshes.size());
            names.reserve(cooked.Submeshes.size());
            variants.clear();
            for (MeshData& submesh : cooked.Submeshes)
            {
                names.push_back(submesh.Name);
                if (!submesh.MaterialVariants.empty())
                {
                    variants.resize(cooked.Submeshes.size());
                    variants[&submesh - cooked.Submeshes.data()] = std::move(submesh.MaterialVariants);
                }
            }
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

        // Vertices before and after indexVertices, per thread (BuildClusters reports a cook's totals).
        thread_local uint64_t s_VerticesBeforeIndexing = 0;
        thread_local uint64_t s_VerticesAfterIndexing = 0;

        // meshoptimizer's first step (its README's "Indexing", the start of its demo's pipeline): vertices identical in every byte
        // (exporters split them per face, per primitive or per attribute set) become one, and vertices no triangle uses go. For the
        // cluster LOD a duplicate is also a border the simplifier has to keep, so welding them lets it simplify across.
        // The morph targets' deltas follow their vertices into a new order (remap: old -> new index, as meshoptimizer's).
        void remapMorphs(MeshData& data, const unsigned int* remap, size_t oldCount, size_t newCount)
        {
            for (MorphTarget& morph : data.Morphs)
            {
                std::vector<glm::vec3> positions(newCount), normals(newCount);
                meshopt_remapVertexBuffer(positions.data(), morph.Positions.data(), oldCount, sizeof(glm::vec3), remap);
                meshopt_remapVertexBuffer(normals.data(), morph.Normals.data(), oldCount, sizeof(glm::vec3), remap);
                morph.Positions = std::move(positions);
                morph.Normals = std::move(normals);
            }
        }

        // The vertices in a new order (remap: old -> new index), their morph deltas with them.
        void remapVertices(MeshData& data, std::vector<uint32_t>& indices, const std::vector<unsigned int>& remap, size_t newCount)
        {
            meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
            std::vector<shaderio::Vertex> vertices(newCount);
            meshopt_remapVertexBuffer(vertices.data(), data.Vertices.data(), data.Vertices.size(), sizeof(shaderio::Vertex), remap.data());
            remapMorphs(data, remap.data(), data.Vertices.size(), newCount);
            data.Vertices = std::move(vertices);
        }

        void indexVertices(MeshData& data, std::vector<uint32_t>& indices)
        {
            s_VerticesBeforeIndexing += data.Vertices.size();
            std::vector<unsigned int> remap(data.Vertices.size());
            // Two vertices are one only if their morph deltas are the same too (a lip corner and the cheek point it touches).
            std::vector<meshopt_Stream> streams = { { data.Vertices.data(), sizeof(shaderio::Vertex), sizeof(shaderio::Vertex) } };
            for (const MorphTarget& morph : data.Morphs)
            {
                streams.push_back({ morph.Positions.data(), sizeof(glm::vec3), sizeof(glm::vec3) });
                streams.push_back({ morph.Normals.data(), sizeof(glm::vec3), sizeof(glm::vec3) });
            }
            const size_t unique = meshopt_generateVertexRemapMulti(remap.data(), indices.data(), indices.size(), data.Vertices.size(),
                                                                   streams.data(), streams.size());
            remapVertices(data, indices, remap, unique);
            s_VerticesAfterIndexing += data.Vertices.size();
        }

        // One level of clusters, always drawn (no coarser version).
        void buildMeshlets(MeshData& data, std::vector<uint32_t> indices)
        {
            // Indexed, the triangles in vertex cache order (the meshlets then gather neighbors) and the vertices in the order the
            // triangles first use them (meshoptimizer's vertex cache and vertex fetch steps): a cluster's vertices lie close in memory.
            indexVertices(data, indices);
            meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), data.Vertices.size());
            {
                std::vector<unsigned int> fetchOrder(data.Vertices.size());
                const size_t used = meshopt_optimizeVertexFetchRemap(fetchOrder.data(), indices.data(), indices.size(), data.Vertices.size());
                remapVertices(data, indices, fetchOrder, used);
            }

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
                bounds.page = 0;
                bounds.refinedPage = shaderio::ClusterNoPage;
                appendCluster(data, bounds, vertices, meshlet.vertex_count, triangles, meshlet.triangle_count);
            }
        }

        // A cluster of the DAG as clodBuild emits it, before it is placed in a page: its triangles over the submesh's vertices.
        struct DagCluster
        {
            shaderio::MeshletBounds Bounds{};
            std::vector<uint32_t> Vertices; // submesh vertex indices
            std::vector<uint8_t> Triangles; // into Vertices
            int Refined = -1;               // the group it was simplified from
        };

        struct DagGroup
        {
            clodGroup Group{};
            std::vector<uint32_t> Clusters; // DagCluster indices
            bool Terminal = false;          // nothing coarser replaces it (clusterlod: FLT_MAX error)
        };

        // Streaming pages (§5.7.2): about this much data each (Nanite's 128 KB pages; one group may exceed it alone).
        constexpr uint64_t PageTargetBytes = 128 * 1024;

        // Packs the DAG into streaming pages and writes the submesh's cluster data in page order. Groups go into pages whole
        // (a cut never splits a group, so a page is all a group's clusters need), terminal groups first -- they are the
        // root pages, always resident -- then the others coarsest first, so the pages a page depends on (those holding the
        // clusters its groups were simplified into) come before it. Each page holds its own copy of the vertices its clusters
        // use, so it is loaded and evicted on its own.
        void writePages(MeshData& data, const std::vector<shaderio::Vertex>& vertices, const std::vector<DagCluster>& clusters, const std::vector<DagGroup>& groups)
        {
            std::vector<uint32_t> order(groups.size());
            for (uint32_t index = 0; index < order.size(); ++index)
                order[index] = index;
            std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
            {
                if (groups[a].Terminal != groups[b].Terminal)
                    return groups[a].Terminal;
                return groups[a].Group.depth > groups[b].Group.depth;
            });

            // Groups into pages: a new page when the next group would take it past the target, and between root and streamed groups.
            std::vector<std::vector<uint32_t>> pageGroups;
            std::vector<uint32_t> groupPage(groups.size(), 0);
            uint64_t pageBytes = 0;
            bool pageRoot = true;
            for (uint32_t group : order)
            {
                uint64_t bytes = 0;
                for (uint32_t cluster : groups[group].Clusters)
                    bytes += (sizeof(shaderio::PackedVertex) + sizeof(uint32_t)) * clusters[cluster].Vertices.size() + clusters[cluster].Triangles.size();
                if (pageGroups.empty() || pageRoot != groups[group].Terminal || (pageBytes > 0 && pageBytes + bytes > PageTargetBytes))
                {
                    pageGroups.emplace_back();
                    pageBytes = 0;
                    pageRoot = groups[group].Terminal;
                }
                pageGroups.back().push_back(group);
                groupPage[group] = static_cast<uint32_t>(pageGroups.size() - 1);
                pageBytes += bytes;
            }

            // The clusters a page's groups were simplified into live in coarser pages (an earlier index): those must stay while the
            // page is resident, or no coarser version would be there when the LOD moves away from it.
            std::vector<std::vector<uint32_t>> pageDependencies(pageGroups.size());
            for (uint32_t group = 0; group < groups.size(); ++group)
            {
                for (uint32_t clusterIndex : groups[group].Clusters)
                {
                    const int refined = clusters[clusterIndex].Refined;
                    if (refined >= 0 && groupPage[refined] != groupPage[group])
                        pageDependencies[groupPage[refined]].push_back(groupPage[group]);
                }
            }

            data.BoundsMin = glm::vec3(FLT_MAX);
            data.BoundsMax = glm::vec3(-FLT_MAX);
            for (const shaderio::Vertex& vertex : vertices)
            {
                data.BoundsMin = glm::min(data.BoundsMin, vertex.pos);
                data.BoundsMax = glm::max(data.BoundsMax, vertex.pos);
            }

            data.Vertices.clear();
            data.Bounds.clear();
            data.Draws.clear();
            data.MeshletVertices.clear();
            data.MeshletTriangles.clear();
            data.Pages.clear();
            data.PageDependencies.clear();
            data.RootPageCount = 0;

            std::unordered_map<uint32_t, uint32_t> pageVertex; // submesh vertex -> page-local
            for (uint32_t pageIndex = 0; pageIndex < pageGroups.size(); ++pageIndex)
            {
                MeshPage page;
                page.Flags = data.VertexColors ? MeshPage::Colors : 0u;
                page.FirstCluster = static_cast<uint32_t>(data.Draws.size());
                page.FirstVertex = static_cast<uint32_t>(data.Vertices.size());
                page.FirstMeshletVertex = static_cast<uint32_t>(data.MeshletVertices.size());
                page.FirstMeshletTriangle = static_cast<uint32_t>(data.MeshletTriangles.size());
                pageVertex.clear();

                bool root = false;
                bool hasLod = false;
                glm::vec3 lodMin(FLT_MAX), lodMax(-FLT_MAX);
                for (uint32_t group : pageGroups[pageIndex])
                {
                    const DagGroup& dagGroup = groups[group];
                    root |= dagGroup.Terminal;
                    if (!dagGroup.Terminal)
                    {
                        const clodBounds& simplified = dagGroup.Group.simplified;
                        const glm::vec3 center(simplified.center[0], simplified.center[1], simplified.center[2]);
                        lodMin = glm::min(lodMin, center - glm::vec3(simplified.radius));
                        lodMax = glm::max(lodMax, center + glm::vec3(simplified.radius));
                        page.LodError = std::max(page.LodError, simplified.error);
                        hasLod = true;
                    }

                    for (uint32_t clusterIndex : dagGroup.Clusters)
                    {
                        const DagCluster& cluster = clusters[clusterIndex];
                        shaderio::MeshletDraw draw{};
                        draw.vertexOffset = static_cast<uint32_t>(data.MeshletVertices.size());
                        draw.triangleOffset = static_cast<uint32_t>(data.MeshletTriangles.size());
                        draw.vertexCount = static_cast<uint32_t>(cluster.Vertices.size());
                        draw.triangleCount = static_cast<uint32_t>(cluster.Triangles.size() / 3);
                        draw.globalVertexOffset = page.FirstVertex;
                        for (uint32_t vertex : cluster.Vertices)
                        {
                            auto [found, inserted] = pageVertex.try_emplace(vertex, static_cast<uint32_t>(data.Vertices.size()) - page.FirstVertex);
                            if (inserted)
                                data.Vertices.push_back(vertices[vertex]);
                            data.MeshletVertices.push_back(found->second);
                        }
                        data.MeshletTriangles.insert(data.MeshletTriangles.end(), cluster.Triangles.begin(), cluster.Triangles.end());

                        shaderio::MeshletBounds bounds = cluster.Bounds;
                        bounds.page = pageIndex;
                        bounds.refinedPage = cluster.Refined < 0 ? shaderio::ClusterNoPage : groupPage[cluster.Refined];
                        data.Draws.push_back(draw);
                        data.Bounds.push_back(bounds);
                    }
                }

                std::vector<uint32_t>& dependencies = pageDependencies[pageIndex];
                std::sort(dependencies.begin(), dependencies.end());
                dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

                page.ClusterCount = static_cast<uint32_t>(data.Draws.size()) - page.FirstCluster;
                page.VertexCount = static_cast<uint32_t>(data.Vertices.size()) - page.FirstVertex;
                page.MeshletVertexCount = static_cast<uint32_t>(data.MeshletVertices.size()) - page.FirstMeshletVertex;
                page.MeshletTriangleCount = static_cast<uint32_t>(data.MeshletTriangles.size()) - page.FirstMeshletTriangle;
                page.FirstDependency = static_cast<uint32_t>(data.PageDependencies.size());
                page.DependencyCount = static_cast<uint32_t>(dependencies.size());
                data.PageDependencies.insert(data.PageDependencies.end(), dependencies.begin(), dependencies.end());
                if (hasLod)
                {
                    page.LodCenter = (lodMin + lodMax) * 0.5f;
                    page.LodRadius = glm::length(lodMax - lodMin) * 0.5f;
                }
                if (root)
                {
                    page.Flags |= MeshPage::Root;
                    page.LodError = shaderio::ClusterTerminalError;
                    data.RootPageCount = pageIndex + 1;
                }
                data.Pages.push_back(page);
            }
        }

        // The cluster LOD DAG (§5.7) with meshoptimizer's clusterlod, set up as its demo/nanite.cpp: clusters of the
        // original geometry are merged into groups and simplified, level after level, until one cluster is left. Every
        // cluster keeps the bounds and error of the group it came from and of the group it went into, so the task shader
        // can pick the cut. The clusters are then packed into streaming pages (writePages).
        void buildClusterLod(MeshData& data, std::vector<uint32_t> indices)
        {
            // Indexed first (vertex order does not matter here: each page takes its own vertex block, writePages).
            indexVertices(data, indices);

            clodConfig config = clodDefaultConfig(shaderio::MAX_PRIMITIVES);
            config.max_vertices = shaderio::MAX_VERTICES;
            // No sloppy fallback when regular simplification gets stuck: sloppy ignores topology and orientation, and a
            // flipped triangle on a flat surface (a street) costs almost no error, so it was drawn at a distance and back
            // face culled (holes). A stuck group stays at its level instead.
            config.simplify_fallback_sloppy = false;

            // Attribute-aware: the normal (weights as the demo), and vertex colors where the mesh has them. Permissive
            // simplification may collapse across any attribute discontinuity it is not told about (clusterlod.h:
            // attribute_protect_mask), so every seam the shading uses is protected: hard edges (normal, attributes 0-2), both UV
            // sets (uv0 3-4, uv1 5-6) and color edges (19-22), in Vertex order from the normal on (the tangent and bone fields in
            // between weigh nothing).
            constexpr uint32_t ColorAttribute = (offsetof(shaderio::Vertex, color) - offsetof(shaderio::Vertex, normal)) / sizeof(float);
            static_assert(ColorAttribute == 19);
            float attributeWeights[ColorAttribute + 4] = { 0.5f, 0.5f, 0.5f };
            if (data.VertexColors)
            {
                for (uint32_t channel = 0; channel < 4; ++channel)
                    attributeWeights[ColorAttribute + channel] = 0.5f;
            }
            clodMesh mesh{};
            mesh.indices = indices.data();
            mesh.index_count = indices.size();
            mesh.vertex_count = data.Vertices.size();
            mesh.vertex_positions = &data.Vertices[0].pos.x;
            mesh.vertex_positions_stride = sizeof(shaderio::Vertex);
            mesh.vertex_attributes = &data.Vertices[0].normal.x;
            mesh.vertex_attributes_stride = sizeof(shaderio::Vertex);
            mesh.attribute_weights = attributeWeights;
            mesh.attribute_count = data.VertexColors ? ColorAttribute + 4 : 3;
            mesh.attribute_protect_mask = 0b1111111u | (data.VertexColors ? 0b1111u << ColorAttribute : 0u);

            std::vector<DagCluster> clusters;
            std::vector<DagGroup> groups;
            std::array<unsigned int, shaderio::MAX_VERTICES> vertices{};
            std::array<unsigned char, shaderio::MAX_PRIMITIVES * 3> triangles{};
            clodBuild(config, mesh, [&](clodGroup group, const clodCluster* groupClusters, size_t clusterCount) -> int
            {
                DagGroup& dagGroup = groups.emplace_back();
                dagGroup.Group = group;
                dagGroup.Terminal = group.simplified.error == FLT_MAX;
                for (size_t index = 0; index < clusterCount; ++index)
                {
                    const clodCluster& cluster = groupClusters[index];
                    const size_t triangleCount = cluster.index_count / 3;
                    const size_t vertexCount = clodLocalIndices(vertices.data(), triangles.data(), cluster.indices, cluster.index_count);

                    shaderio::MeshletBounds bounds = clusterBounds(data, vertices.data(), triangles.data(), triangleCount);
                    // The group this cluster was simplified from; the original clusters have no error.
                    const clodBounds& self = cluster.refined < 0 ? cluster.bounds : groups[cluster.refined].Group.simplified;
                    bounds.lodCenter = glm::vec3(self.center[0], self.center[1], self.center[2]);
                    bounds.lodRadius = self.radius;
                    bounds.lodError = cluster.refined < 0 ? 0.0f : self.error;
                    // The group it went into (FLT_MAX error: never simplified further).
                    bounds.parentCenter = glm::vec3(group.simplified.center[0], group.simplified.center[1], group.simplified.center[2]);
                    bounds.parentRadius = group.simplified.radius;
                    bounds.parentError = group.simplified.error;
                    bounds.lodLevel = cluster.refined < 0 ? 0u : static_cast<uint32_t>(groups[cluster.refined].Group.depth + 1);
                    bounds.page = shaderio::ClusterNoPage; // writePages
                    bounds.refinedPage = shaderio::ClusterNoPage;

                    dagGroup.Clusters.push_back(static_cast<uint32_t>(clusters.size()));
                    DagCluster& dagCluster = clusters.emplace_back();
                    dagCluster.Bounds = bounds;
                    dagCluster.Vertices.assign(vertices.begin(), vertices.begin() + vertexCount);
                    dagCluster.Triangles.assign(triangles.begin(), triangles.begin() + triangleCount * 3);
                    dagCluster.Refined = cluster.refined;
                }
                return static_cast<int>(groups.size() - 1);
            });

            const std::vector<shaderio::Vertex> sourceVertices = std::move(data.Vertices);
            writePages(data, sourceVertices, clusters, groups);
        }
    }

    std::optional<CookedMesh> MeshImporter::ReadCookedMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata, bool streamPages)
    {
        const std::filesystem::path cookedPath = assetDirectory / metadata.FilePath;
        const std::filesystem::path sourcePath = assetDirectory / metadata.SourceFilePath;

        // Same test as ImportMesh / ImportStaticMesh. A packaged game ships the cooked file without its source (Build > Package
        // Game): with the source gone, the cooked file is taken as it is -- as is one that never had a source (made by a tool,
        // BuildCookedGeometry), like a texture whose file is its .ntex.
        std::error_code error;
        const bool sourceMissing = metadata.SourceFilePath.empty() || !std::filesystem::exists(sourcePath, error);
        XXH128_hash_t cookedHash{};
        if (!VFS::Exists(cookedPath) ||
            (!sourceMissing && (!Utility::loadHashFromFile(cookedPath.string() + ".hash", cookedHash) ||
                                !XXH128_isEqual(Utility::calcul_hash_streaming(sourcePath.string()), cookedHash))))
        {
            return std::nullopt;
        }
        // A cook in an older layout is cooked again while its source is there; without one it cannot be read (no compatibility
        // readers: MeshSerializer reads the current version only).
        if (!MeshSerializer::IsCurrentVersion(cookedPath))
        {
            if (sourceMissing)
                NOX_CORE_ERROR("MeshImporter: {} is cooked in an old format and its source is gone: import it again", cookedPath.generic_string());
            return std::nullopt;
        }

        CookedMesh cooked;
        const bool success = metadata.Type == AssetType::StaticMesh
            ? MeshSerializer::DeserializeStaticMesh(cookedPath, cooked.Submeshes, cooked.Materials, cooked.Lights, cooked.Nodes, cooked.Cameras, streamPages)
            : MeshSerializer::DeserializeMesh(cookedPath, cooked.Submeshes, cooked.Materials, cooked.Lights, cooked.Nodes, cooked.Cameras, streamPages);
        if (!success)
            return std::nullopt;
        return cooked;
    }

    Ref<Asset> MeshImporter::CreateMeshAsset(AssetType type, CookedMesh& cooked)
    {
        if (type == AssetType::StaticMesh)
        {
            Ref<StaticMesh> mesh = CreateRef<StaticMesh>();
            fillMeshAsset(cooked, mesh->m_SubMeshes, mesh->m_SubmeshNames, mesh->m_Materials, mesh->m_Lights, mesh->m_Cameras, mesh->m_Nodes, mesh->m_MaterialVariants);
            return Ref<Asset>(mesh);
        }

        Ref<Mesh> mesh = CreateRef<Mesh>();
        fillMeshAsset(cooked, mesh->m_SubMeshes, mesh->m_SubmeshNames, mesh->m_Materials, mesh->m_Lights, mesh->m_Cameras, mesh->m_Nodes, mesh->m_MaterialVariants);
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

    // The part of an import that one asset holds: submeshes with their materials (1:1) and triangles not yet built
    // into clusters (also 1:1), and the node table pointing at them (nodes of the other kind keep no submeshes).
    struct MeshSubset
    {
        std::vector<MeshData> Meshes;
        std::vector<MaterialData> Materials;
        std::vector<PendingGeometry> Pending;
        std::vector<MeshNodeData> Nodes;
    };

    // Import settings (Unreal's static / skeletal split): takes the submeshes of the nodes `wanted` accepts -- a submesh
    // shared by several nodes stays while any wanted node uses it -- and renumbers what the nodes point at. The
    // arguments are consumed.
    template <typename Wanted>
    static MeshSubset ExtractSubset(std::vector<MeshData>& meshes, std::vector<MaterialData>& materials,
                                    std::vector<PendingGeometry>& pending, const std::vector<MeshNodeData>& nodes, Wanted wanted)
    {
        std::vector<bool> keep(meshes.size(), false);
        for (const MeshNodeData& node : nodes)
        {
            if (node.SubmeshCount == 0 || !wanted(node))
                continue;
            for (uint32_t i = node.FirstSubmesh; i < node.FirstSubmesh + node.SubmeshCount && i < keep.size(); ++i)
                keep[i] = true;
        }

        MeshSubset subset;
        std::vector<uint32_t> newIndex(meshes.size(), UINT32_MAX);
        for (size_t i = 0; i < meshes.size(); ++i)
        {
            if (!keep[i])
                continue;
            newIndex[i] = static_cast<uint32_t>(subset.Meshes.size());
            subset.Meshes.push_back(std::move(meshes[i]));
            if (i < materials.size())
                subset.Materials.push_back(std::move(materials[i]));
            if (i < pending.size())
                subset.Pending.push_back(std::move(pending[i]));
        }

        subset.Nodes = nodes;
        for (MeshNodeData& node : subset.Nodes)
        {
            if (node.SubmeshCount == 0)
                continue;
            if (!wanted(node))
            {
                node.FirstSubmesh = UINT32_MAX;
                node.SubmeshCount = 0;
            }
            else
            {
                node.FirstSubmesh = newIndex[node.FirstSubmesh];
            }
        }
        return subset;
    }

    // Two submeshes merge when they draw the same way: same material values and textures.
    static std::string MaterialKey(const MaterialData& material)
    {
        std::string lobeTextures;
        for (const MaterialLobeTextureSlot& slot : MaterialLobeTextures)
            lobeTextures += (material.*slot.Member).Path + '|';
        return lobeTextures + material.Name + '|' + material.BaseColorTexturePath + '|' + material.MetallicRoughnessTexturePath + '|' +
               material.NormalTexturePath + '|' + material.OcclusionTexturePath + '|' + material.EmissiveTexturePath + '|' +
               material.TransmissionTexturePath + '|' + std::to_string(static_cast<int>(material.Mode)) + '|' +
               std::to_string(material.DoubleSided);
    }

    // Import settings "Combine": merges the subset's submeshes into one per material. Static meshes get their node's world
    // transform baked into the vertices (a mesh used by several nodes is copied per node -- what merging costs); skinned
    // meshes stay in the bind pose (glTF ignores the mesh node's transform for them). Every submesh must be of the kind
    // `skinned` (an asset never mixes kinds). The node table is replaced by one node that owns the merged submeshes.
    static void CombineSubmeshes(MeshSubset& subset, bool skinned)
    {
        std::vector<glm::mat4> world(subset.Nodes.size(), glm::mat4(1.0f));
        std::vector<bool> done(subset.Nodes.size(), false);
        std::function<const glm::mat4&(size_t)> worldOf = [&](size_t index) -> const glm::mat4&
        {
            if (done[index])
                return world[index];
            const MeshNodeData& node = subset.Nodes[index];
            const glm::mat4 local = glm::translate(glm::mat4(1.0f), node.Translation) * glm::mat4_cast(node.Rotation) *
                                    glm::scale(glm::mat4(1.0f), node.Scale);
            const bool hasParent = node.Parent >= 0 && node.Parent < static_cast<int32_t>(subset.Nodes.size()) &&
                                   node.Parent != static_cast<int32_t>(index);
            world[index] = hasParent ? worldOf(static_cast<size_t>(node.Parent)) * local : local;
            done[index] = true;
            return world[index];
        };

        struct Group
        {
            MeshData Data;
            PendingGeometry Geometry;
            MaterialData Material;
        };
        std::vector<Group> groups;
        std::unordered_map<std::string, size_t> groupOfMaterial;

        for (size_t nodeIndex = 0; nodeIndex < subset.Nodes.size(); ++nodeIndex)
        {
            const MeshNodeData& node = subset.Nodes[nodeIndex];
            if (node.SubmeshCount == 0)
                continue;

            const glm::mat4 transform = skinned ? glm::mat4(1.0f) : worldOf(nodeIndex);
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(transform)));
            const bool flipWinding = glm::determinant(glm::mat3(transform)) < 0.0f;

            for (uint32_t i = node.FirstSubmesh; i < node.FirstSubmesh + node.SubmeshCount && i < subset.Meshes.size(); ++i)
            {
                // A submesh with morph targets is not merged (its targets are its own): a group of its own, per node using it. Submeshes
                // merge only when their material variants match too (a variant would otherwise change both).
                const bool morphed = !subset.Meshes[i].Morphs.empty();
                std::string key = morphed ? MaterialKey(subset.Materials[i]) + "#morph" + std::to_string(nodeIndex) + "." + std::to_string(i)
                                          : MaterialKey(subset.Materials[i]);
                for (const MaterialVariantMapping& mapping : subset.Meshes[i].MaterialVariants)
                    key += "#variant" + std::to_string(mapping.VariantIndex) + ":" + MaterialKey(mapping.Material);
                auto found = groupOfMaterial.find(key);
                if (found == groupOfMaterial.end())
                {
                    found = groupOfMaterial.emplace(key, groups.size()).first;
                    Group& created = groups.emplace_back();
                    created.Material = subset.Materials[i];
                    created.Data.Name = subset.Materials[i].Name.empty() ? "Combined_" + std::to_string(groups.size() - 1) : subset.Materials[i].Name;
                    created.Geometry.Skinned = skinned;
                    created.Data.MaterialVariants = subset.Meshes[i].MaterialVariants;
                }
                Group& group = groups[found->second];
                // One merged submesh with colors if any part has them (the others' vertices are white).
                group.Data.VertexColors |= subset.Meshes[i].VertexColors;
                if (morphed)
                {
                    // Its deltas in the merged space, as its vertices: positions by the node's linear part, normals by its normal matrix.
                    group.Data.Morphs = subset.Meshes[i].Morphs;
                    if (!skinned)
                    {
                        for (MorphTarget& morph : group.Data.Morphs)
                        {
                            for (glm::vec3& delta : morph.Positions)
                                delta = glm::mat3(transform) * delta;
                            for (glm::vec3& delta : morph.Normals)
                                delta = normalMatrix * delta;
                        }
                    }
                }

                const uint32_t baseVertex = static_cast<uint32_t>(group.Data.Vertices.size());
                for (shaderio::Vertex vertex : subset.Meshes[i].Vertices)
                {
                    if (!skinned)
                    {
                        vertex.pos = glm::vec3(transform * glm::vec4(vertex.pos, 1.0f));
                        const glm::vec3 normal = normalMatrix * vertex.normal;
                        vertex.normal = glm::length(normal) > 0.0f ? glm::normalize(normal) : vertex.normal;
                    }
                    group.Data.Vertices.push_back(vertex);
                }

                const size_t first = group.Geometry.Indices.size();
                for (uint32_t index : subset.Pending[i].Indices)
                    group.Geometry.Indices.push_back(baseVertex + index);
                if (flipWinding)
                {
                    for (size_t t = first; t + 2 < group.Geometry.Indices.size(); t += 3)
                        std::swap(group.Geometry.Indices[t + 1], group.Geometry.Indices[t + 2]);
                }
            }
        }

        subset.Meshes.clear();
        subset.Materials.clear();
        subset.Pending.clear();
        for (Group& group : groups)
        {
            subset.Meshes.push_back(std::move(group.Data));
            subset.Materials.push_back(std::move(group.Material));
            subset.Pending.push_back(std::move(group.Geometry));
        }

        // The file's node structure is gone with the baked geometry: the merged asset is one node (importing never makes
        // hierarchy; Import Into Level is what keeps the scene structure, lights and cameras).
        subset.Nodes.clear();
        MeshNodeData& combined = subset.Nodes.emplace_back();
        combined.Name = skinned ? "Combined Skeletal Meshes" : "Combined Static Meshes";
        combined.Parent = -1;
        combined.FirstSubmesh = 0;
        combined.SubmeshCount = static_cast<uint32_t>(subset.Meshes.size());
        combined.Skinned = skinned;
    }

    static const tg3_extension* FindExtension(const tg3_extras_ext& ext, std::string_view name)
    {
        for (uint32_t i = 0; i < ext.extensions_count; i++)
        {
            if (std::string_view(ext.extensions[i].name.data, ext.extensions[i].name.len) == name)
                return &ext.extensions[i];
        }
        return nullptr;
    }

    static const tg3_value* FindMember(const tg3_value& object, std::string_view key)
    {
        if (object.type != TG3_VALUE_OBJECT)
            return nullptr;
        for (uint32_t i = 0; i < object.object_count; i++)
        {
            if (std::string_view(object.object_data[i].key.data, object.object_data[i].key.len) == key)
                return &object.object_data[i].value;
        }
        return nullptr;
    }

    // An accessor's elements as floats (`components` per element): float, or normalized / plain integer components, with its sparse
    // substitutions applied (an accessor without a buffer view is zeros they are written into: what exporters do for morph targets
    // that move a few vertices). Empty when it cannot be read.
    static std::vector<float> ReadAccessorFloats(const tg3_model& model, int64_t accessorIndex, uint32_t components)
    {
        std::vector<float> values;
        if (accessorIndex < 0 || accessorIndex >= static_cast<int64_t>(model.accessors_count))
            return values;
        const tg3_accessor& accessor = model.accessors[accessorIndex];

        uint32_t componentSize = 0;
        switch (accessor.component_type)
        {
        case TG3_COMPONENT_TYPE_FLOAT: componentSize = 4; break;
        case TG3_COMPONENT_TYPE_BYTE: case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: componentSize = 1; break;
        case TG3_COMPONENT_TYPE_SHORT: case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: componentSize = 2; break;
        default: return values;
        }
        auto convert = [&](const uint8_t* component)
        {
            float value = 0.0f;
            switch (accessor.component_type)
            {
            case TG3_COMPONENT_TYPE_FLOAT: std::memcpy(&value, component, 4); break;
            case TG3_COMPONENT_TYPE_BYTE: value = accessor.normalized ? std::max(static_cast<int8_t>(*component) / 127.0f, -1.0f) : static_cast<int8_t>(*component); break;
            case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: value = accessor.normalized ? *component / 255.0f : *component; break;
            case TG3_COMPONENT_TYPE_SHORT: { int16_t v; std::memcpy(&v, component, 2); value = accessor.normalized ? std::max(v / 32767.0f, -1.0f) : v; break; }
            case TG3_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, component, 2); value = accessor.normalized ? v / 65535.0f : v; break; }
            }
            return value;
        };
        // A buffer view's bytes from `offset` on, when `bytes` of them exist.
        auto viewData = [&](int32_t viewIndex, uint64_t offset, uint64_t bytes) -> const uint8_t*
        {
            if (viewIndex < 0 || viewIndex >= static_cast<int32_t>(model.buffer_views_count))
                return nullptr;
            const tg3_buffer_view& view = model.buffer_views[viewIndex];
            if (view.buffer < 0 || view.buffer >= static_cast<int32_t>(model.buffers_count))
                return nullptr;
            const tg3_buffer& buffer = model.buffers[view.buffer];
            const uint64_t start = view.byte_offset + offset;
            return start + bytes <= buffer.data.count ? buffer.data.data + start : nullptr;
        };

        values.assign(accessor.count * components, 0.0f);
        if (accessor.count == 0)
            return values;
        if (accessor.buffer_view >= 0)
        {
            if (accessor.buffer_view >= static_cast<int32_t>(model.buffer_views_count))
                return {};
            const uint64_t viewStride = model.buffer_views[accessor.buffer_view].byte_stride;
            const uint64_t stride = viewStride != 0 ? viewStride : static_cast<uint64_t>(componentSize) * components;
            const uint8_t* data = viewData(accessor.buffer_view, accessor.byte_offset, stride * (accessor.count - 1) + componentSize * components);
            if (!data)
                return {};
            for (uint64_t element = 0; element < accessor.count; element++)
                for (uint32_t c = 0; c < components; c++)
                    values[element * components + c] = convert(data + stride * element + componentSize * c);
        }
        else if (!accessor.sparse.is_sparse)
        {
            return {};
        }

        if (accessor.sparse.is_sparse && accessor.sparse.count > 0)
        {
            const tg3_accessor_sparse& sparse = accessor.sparse;
            const uint32_t indexSize = sparse.indices.component_type == TG3_COMPONENT_TYPE_UNSIGNED_BYTE ? 1u
                                     : sparse.indices.component_type == TG3_COMPONENT_TYPE_UNSIGNED_SHORT ? 2u
                                     : sparse.indices.component_type == TG3_COMPONENT_TYPE_UNSIGNED_INT ? 4u : 0u;
            const uint64_t count = static_cast<uint64_t>(sparse.count);
            const uint8_t* indices = indexSize ? viewData(sparse.indices.buffer_view, sparse.indices.byte_offset, indexSize * count) : nullptr;
            const uint8_t* substitutes = viewData(sparse.values.buffer_view, sparse.values.byte_offset, uint64_t(componentSize) * components * count);
            if (!indices || !substitutes)
                return {};
            for (uint64_t k = 0; k < count; ++k)
            {
                uint32_t element = 0;
                std::memcpy(&element, indices + indexSize * k, indexSize); // little endian: the low bytes
                if (element >= accessor.count)
                    continue;
                for (uint32_t c = 0; c < components; c++)
                    values[uint64_t(element) * components + c] = convert(substitutes + (k * components + c) * componentSize);
            }
        }
        return values;
    }

    // A morph target's name: the mesh's extras.targetNames (Blender's shape key names), else "Target <index>".
    static std::string MorphTargetName(const tg3_mesh& mesh, uint32_t target)
    {
        const tg3_value* names = mesh.ext.extras ? FindMember(*mesh.ext.extras, "targetNames") : nullptr;
        if (names && names->type == TG3_VALUE_ARRAY && target < names->array_count && names->array_data[target].type == TG3_VALUE_STRING &&
            names->array_data[target].string_val.len > 0)
            return std::string(names->array_data[target].string_val.data, names->array_data[target].string_val.len);
        return "Target " + std::to_string(target);
    }

    // The glTF scene an import takes: the file's default ("scene"), else the first; -1 when the file has no scenes (every node is in
    // then). Only that one, as Unreal and Godot import it: a file's other scenes are not brought in.
    static int32_t ImportedScene(const tg3_model& model)
    {
        const int32_t count = static_cast<int32_t>(model.scenes_count);
        if (count == 0)
            return -1;
        return model.default_scene >= 0 && model.default_scene < count ? model.default_scene : 0;
    }

    // Per node of the file: whether the imported scene reaches it (its roots and everything below them). All in without scenes.
    static std::vector<uint8_t> NodesInScene(const tg3_model& model)
    {
        const int32_t imported = ImportedScene(model);
        std::vector<uint8_t> in(model.nodes_count, imported < 0 ? 1 : 0);
        if (imported < 0)
            return in;
        std::vector<int32_t> pending(model.scenes[imported].nodes, model.scenes[imported].nodes + model.scenes[imported].nodes_count);
        while (!pending.empty())
        {
            const int32_t node = pending.back();
            pending.pop_back();
            if (node < 0 || node >= static_cast<int32_t>(model.nodes_count) || in[node])
                continue;
            in[node] = 1;
            for (uint32_t c = 0; c < model.nodes[node].children_count; ++c)
                pending.push_back(model.nodes[node].children[c]);
        }
        return in;
    }

    // The nearest joint above each node of the file (a joint of any skin), -1 where there is none. An unskinned mesh under a joint (a
    // sword in the hand, a hat on the head) is a rigid part of the character: Unreal's Import Meshes in Bone Hierarchy.
    static std::vector<int32_t> JointAncestors(const tg3_model& model)
    {
        const int32_t count = static_cast<int32_t>(model.nodes_count);
        std::vector<uint8_t> isJoint(count, 0);
        for (uint32_t s = 0; s < model.skins_count; ++s)
            for (uint32_t j = 0; j < model.skins[s].joints_count; ++j)
                if (model.skins[s].joints[j] >= 0 && model.skins[s].joints[j] < count)
                    isJoint[model.skins[s].joints[j]] = 1;
        std::vector<int32_t> parent(count, -1);
        for (int32_t n = 0; n < count; ++n)
            for (uint32_t c = 0; c < model.nodes[n].children_count; ++c)
                if (model.nodes[n].children[c] >= 0 && model.nodes[n].children[c] < count)
                    parent[model.nodes[n].children[c]] = n;
        std::vector<int32_t> ancestor(count, -1);
        for (int32_t n = 0; n < count; ++n)
        {
            for (int32_t up = parent[n], depth = 0; up >= 0 && depth < 256; up = parent[up], ++depth)
            {
                if (isJoint[up])
                {
                    ancestor[n] = up;
                    break;
                }
            }
        }
        return ancestor;
    }

    MeshImporter::GltfContent MeshImporter::InspectGltf(const std::filesystem::path& sourcePath)
    {
        GltfContent content;
        std::unordered_set<int32_t> skinnedMeshes;
        std::unordered_set<int32_t> staticMeshes;

        tg3_parse_options options;
        tg3_error_stack errors;
        tg3_model model;
        tg3_parse_options_init(&options);
        tg3_error_stack_init(&errors);
        options.images_as_is = 1;
        if (tg3_parse_file(&model, &errors, sourcePath.string().c_str(), sourcePath.string().size(), &options) != TG3_OK)
        {
            tg3_error_stack_free(&errors);
            return content;
        }

        const int32_t importedScene = ImportedScene(model);
        const std::vector<uint8_t> inScene = NodesInScene(model);
        const std::vector<int32_t> jointAncestors = JointAncestors(model);

        // The size: every mesh of the imported scene, its POSITION accessor's min / max box through the node transforms.
        {
            glm::vec3 lowest(FLT_MAX), highest(-FLT_MAX);
            auto meshBounds = [&](int32_t meshIndex, const glm::mat4& world)
            {
                const tg3_mesh& mesh = model.meshes[meshIndex];
                for (uint32_t p = 0; p < mesh.primitives_count; ++p)
                {
                    const tg3_primitive& primitive = mesh.primitives[p];
                    for (uint32_t a = 0; a < primitive.attributes_count; ++a)
                    {
                        const tg3_str_int_pair& attribute = primitive.attributes[a];
                        if (attribute.key.len != 8 || std::strncmp(attribute.key.data, "POSITION", 8) != 0)
                            continue;
                        if (attribute.value < 0 || attribute.value >= static_cast<int32_t>(model.accessors_count))
                            continue;
                        const tg3_accessor& accessor = model.accessors[attribute.value];
                        if (accessor.min_values_count < 3 || accessor.max_values_count < 3)
                            continue;
                        for (int corner = 0; corner < 8; ++corner)
                        {
                            const glm::vec3 point(
                                static_cast<float>((corner & 1) ? accessor.max_values[0] : accessor.min_values[0]),
                                static_cast<float>((corner & 2) ? accessor.max_values[1] : accessor.min_values[1]),
                                static_cast<float>((corner & 4) ? accessor.max_values[2] : accessor.min_values[2]));
                            const glm::vec3 transformed = glm::vec3(world * glm::vec4(point, 1.0f));
                            lowest = glm::min(lowest, transformed);
                            highest = glm::max(highest, transformed);
                        }
                    }
                }
            };

            auto localMatrix = [](const tg3_node& node)
            {
                if (node.has_matrix)
                {
                    glm::mat4 matrix(1.0f);
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row)
                            matrix[column][row] = static_cast<float>(node.matrix[column * 4 + row]);
                    return matrix;
                }
                const glm::quat rotation(static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                                         static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]));
                return glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2])))
                     * glm::mat4_cast(rotation)
                     * glm::scale(glm::mat4(1.0f), glm::vec3(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2])));
            };

            std::function<void(int32_t, const glm::mat4&, int)> visit = [&](int32_t nodeIndex, const glm::mat4& parent, int depth)
            {
                if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(model.nodes_count) || depth > 64)
                    return;
                const tg3_node& node = model.nodes[nodeIndex];
                // KHR_node_visibility: a hidden node draws nothing, nor does anything below it (as the cook treats it).
                if (const tg3_extension* visibility = FindExtension(node.ext, "KHR_node_visibility"))
                {
                    const tg3_value* visible = FindMember(visibility->value, "visible");
                    if (visible && visible->type == TG3_VALUE_BOOL && !visible->bool_val)
                        return;
                }
                const glm::mat4 world = parent * localMatrix(node);
                if (node.mesh >= 0 && node.mesh < static_cast<int32_t>(model.meshes_count))
                {
                    // EXT_mesh_gpu_instancing: the mesh at each instance's transform instead of the node's own.
                    bool instanced = false;
                    if (const tg3_extension* instancing = node.skin < 0 ? FindExtension(node.ext, "EXT_mesh_gpu_instancing") : nullptr)
                    {
                        if (const tg3_value* attributes = FindMember(instancing->value, "attributes"))
                        {
                            auto accessorOf = [&](std::string_view name) -> int64_t
                            {
                                const tg3_value* value = FindMember(*attributes, name);
                                if (!value)
                                    return -1;
                                return value->type == TG3_VALUE_INT ? value->int_val : value->type == TG3_VALUE_REAL ? static_cast<int64_t>(value->real_val) : -1;
                            };
                            const std::vector<float> t = ReadAccessorFloats(model, accessorOf("TRANSLATION"), 3);
                            const std::vector<float> r = ReadAccessorFloats(model, accessorOf("ROTATION"), 4);
                            const std::vector<float> sc = ReadAccessorFloats(model, accessorOf("SCALE"), 3);
                            const size_t count = std::max({ t.size() / 3, r.size() / 4, sc.size() / 3 });
                            for (size_t k = 0; k < count; ++k)
                            {
                                glm::mat4 instance(1.0f);
                                if (k * 3 + 2 < t.size())
                                    instance = glm::translate(instance, glm::vec3(t[k * 3], t[k * 3 + 1], t[k * 3 + 2]));
                                if (k * 4 + 3 < r.size())
                                    instance *= glm::mat4_cast(glm::normalize(glm::quat(r[k * 4 + 3], r[k * 4], r[k * 4 + 1], r[k * 4 + 2])));
                                if (k * 3 + 2 < sc.size())
                                    instance = glm::scale(instance, glm::vec3(sc[k * 3], sc[k * 3 + 1], sc[k * 3 + 2]));
                                meshBounds(node.mesh, world * instance);
                            }
                            instanced = count > 0;
                        }
                    }
                    if (!instanced)
                        meshBounds(node.mesh, world);
                }
                for (uint32_t c = 0; c < node.children_count; ++c)
                    visit(node.children[c], world, depth + 1);
            };

            if (model.nodes_count == 0)
            {
                for (uint32_t m = 0; m < model.meshes_count; ++m)
                    meshBounds(static_cast<int32_t>(m), glm::mat4(1.0f));
            }
            else if (importedScene >= 0)
            {
                const tg3_scene& imported = model.scenes[importedScene];
                for (uint32_t n = 0; n < imported.nodes_count; ++n)
                    visit(imported.nodes[n], glm::mat4(1.0f), 0);
            }
            else
            {
                // No scene: the nodes nobody lists as a child are the roots.
                std::vector<bool> isChild(model.nodes_count, false);
                for (uint32_t n = 0; n < model.nodes_count; ++n)
                    for (uint32_t c = 0; c < model.nodes[n].children_count; ++c)
                        if (model.nodes[n].children[c] >= 0 && model.nodes[n].children[c] < static_cast<int32_t>(model.nodes_count))
                            isChild[model.nodes[n].children[c]] = true;
                for (uint32_t n = 0; n < model.nodes_count; ++n)
                    if (!isChild[n])
                        visit(static_cast<int32_t>(n), glm::mat4(1.0f), 0);
            }

            if (lowest.x <= highest.x)
            {
                content.HasBounds = true;
                content.BoundsMin = lowest;
                content.BoundsMax = highest;
            }
        }

        for (uint32_t i = 0; i < model.nodes_count; ++i)
        {
            const tg3_node& node = model.nodes[i];
            if (node.mesh < 0 || node.mesh >= static_cast<int32_t>(model.meshes_count) || !inScene[i])
                continue;
            // A rigid part under a joint belongs to the character (UnifySkins binds it to the joint).
            const bool skinned = node.skin >= 0 || jointAncestors[i] >= 0;
            (skinned ? content.HasSkinnedMeshes : content.HasStaticMeshes) = true;
            (skinned ? skinnedMeshes : staticMeshes).insert(node.mesh);
        }
        // A file without nodes draws its meshes as they are.
        if (model.nodes_count == 0 && model.meshes_count > 0)
            content.HasStaticMeshes = true;
        content.SkinnedMeshCount = static_cast<uint32_t>(skinnedMeshes.size());
        content.StaticMeshCount = model.nodes_count == 0 ? model.meshes_count : static_cast<uint32_t>(staticMeshes.size());

        tg3_model_free(&model);
        tg3_error_stack_free(&errors);
        return content;
    }

    // Import setting "Import Skin Weights" off: the file's skinned meshes are treated as plain geometry.
    static void ConvertSkinnedToStatic(std::vector<MeshNodeData>& nodes, std::vector<PendingGeometry>& pending)
    {
        for (MeshNodeData& node : nodes)
            node.Skinned = false;
        for (PendingGeometry& geometry : pending)
            geometry.Skinned = false;
    }

    // Clusters (§5.7): static geometry gets the cluster LOD DAG; skinned geometry one level (simplified in the bind pose,
    // it would deform wrongly).
    // Like Unreal's Import Uniform Scale: baked into the cooked geometry once, at cook time, instead of a per-placement multiplier
    // (docs/Units_And_World_Tools_Plan_2026.md, U4 follow-up). Every vertex position, and every node's local translation (so a
    // multi-node file's meshes keep their relative positions), are scaled; rotations and node/vertex-normal directions are unaffected
    // by a uniform scale. An entity this asset is attached to then defaults to Scale 1.0 -- "as authored" -- not a unit-conversion
    // multiplier the Inspector has to show and the reset button has to know about.
    static void ApplyImportScale(MeshSubset& subset, float scale)
    {
        if (std::abs(scale - 1.0f) < 1e-6f)
            return;
        for (MeshData& mesh : subset.Meshes)
        {
            for (shaderio::Vertex& vertex : mesh.Vertices)
                vertex.pos *= scale;
            for (MorphTarget& morph : mesh.Morphs)
                for (glm::vec3& delta : morph.Positions)
                    delta *= scale;
        }
        for (MeshNodeData& node : subset.Nodes)
        {
            node.Translation *= scale;
            node.Physics.Size *= scale; // in the node's space, like its mesh's vertices
            NodePhysics::Joint& joint = node.Physics.JointData;
            for (int axis = 0; axis < 3; ++axis) // translation limits and targets: lengths
            {
                joint.Min[axis] *= scale;
                joint.Max[axis] *= scale;
                joint.Drives[axis].PositionTarget *= scale;
                joint.Drives[axis].VelocityTarget *= scale;
            }
        }
        // The volume's lengths are the file's, like its vertices.
        for (MaterialData& material : subset.Materials)
        {
            material.Thickness *= scale;
            material.AttenuationDistance *= scale;
        }
        for (MeshData& mesh : subset.Meshes)
        {
            for (MaterialVariantMapping& mapping : mesh.MaterialVariants)
            {
                mapping.Material.Thickness *= scale;
                mapping.Material.AttenuationDistance *= scale;
            }
        }
    }

    // The skeleton (bone rest poses) and its animation clips need the same uniform scale as the mesh they drive, or a scaled-up
    // character's bones stay at their old (too small) positions relative to the now-larger geometry. Node::RestTranslation is
    // translation-only, like a MeshNodeData's, for the same reason (a rotation-preserving uniform scale of a whole TRS chain only
    // ever needs each level's own translation scaled -- see ApplyImportScale). Skin::InverseBindMatrices is different: it is the
    // inverse of a bind-pose world matrix, and (Scale(S) * M)^-1 = M^-1 * Scale(1/S), which -- multiplying on the right -- scales
    // only the matrix's rotation/scale columns (0-2) by 1/S and leaves its translation column (3) alone.
    static void ApplySkeletonScale(Skeleton& skeleton, float scale)
    {
        if (std::abs(scale - 1.0f) < 1e-6f)
            return;
        for (Node* node : skeleton.AllNodes)
        {
            if (!node)
                continue;
            node->RestTranslation *= scale;
            if (node->HasRestMatrix)
                node->RestMatrix[3] = glm::vec4(glm::vec3(node->RestMatrix[3]) * scale, node->RestMatrix[3].w);
        }
        for (Skin* skin : skeleton.Skins)
        {
            if (!skin)
                continue;
            for (glm::mat4& inverseBind : skin->InverseBindMatrices)
            {
                inverseBind[0] /= scale;
                inverseBind[1] /= scale;
                inverseBind[2] /= scale;
            }
        }
    }

    // A clip of the whole file for one per-mesh asset (Do Not Combine, CopySingleMesh's node table): the channels and morph curves of the
    // file's nodes that draw the mesh, onto the asset's nodes standing for them --
    //  - node 0 (what a plain drag places, standing for the first file node with the mesh): relative to that node's rest pose, so the
    //    mesh moves the way it did in the file wherever it is put (a fan spins in place, a door swings on its hinge);
    //  - the file-layout node of each file node with the mesh (a drag with the file layout): in model space, as the layout places it
    //    (the file node's parents held at their rest pose: an animated parent's motion is not carried over).
    // False when the clip animates none of the mesh's nodes.
    static bool RetargetClipToMesh(const AnimationSequence& clip, const std::vector<MeshNodeData>& fileNodes, const std::vector<glm::mat4>& worlds,
                                   int32_t meshIndex, bool skinned, AnimationSequence& out)
    {
        out.Name = clip.Name;
        out.Duration = clip.Duration;
        out.TicksPerSecond = clip.TicksPerSecond;
        out.Channels.clear();
        out.MorphCurves.clear();

        // File node -> its per-mesh nodes: 0 for the first user, then 1 + its place among the users (CopySingleMesh's order).
        std::unordered_map<int32_t, std::vector<int32_t>> targets;
        int32_t layoutIndex = 1;
        for (size_t index = 0; index < fileNodes.size(); ++index)
        {
            const MeshNodeData& node = fileNodes[index];
            if (node.MeshIndex != meshIndex || node.Skinned != skinned || node.SubmeshCount == 0)
                continue;
            std::vector<int32_t>& stand = targets[static_cast<int32_t>(index)];
            if (layoutIndex == 1)
                stand.push_back(0);
            stand.push_back(layoutIndex++);
        }

        auto restLocal = [&](int32_t node)
        {
            const MeshNodeData& data = fileNodes[node];
            return glm::translate(glm::mat4(1.0f), data.Translation) * glm::mat4_cast(data.Rotation) * glm::scale(glm::mat4(1.0f), data.Scale);
        };
        for (const NodeAnimationChannel& channel : clip.Channels)
        {
            const auto found = targets.find(channel.TargetNodeIndex);
            if (found == targets.end() || channel.TargetNodeIndex < 0 || channel.TargetNodeIndex >= static_cast<int32_t>(worlds.size()))
                continue;
            const MeshNodeData& rest = fileNodes[channel.TargetNodeIndex];
            const glm::mat4 local = restLocal(channel.TargetNodeIndex);
            const glm::mat4 parentWorld = worlds[channel.TargetNodeIndex] * glm::inverse(local);
            // Every key time of the channel: the node's whole transform is resampled there (a rotation-only channel moves the node in
            // the space it is put into).
            std::vector<float> times;
            for (const auto& key : channel.PositionKeys) times.push_back(key.Time);
            for (const auto& key : channel.RotationKeys) times.push_back(key.Time);
            for (const auto& key : channel.ScaleKeys) times.push_back(key.Time);
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());

            for (int32_t target : found->second)
            {
                NodeAnimationChannel& retargeted = out.Channels.emplace_back();
                retargeted.NodeName = rest.Name;
                retargeted.TargetNodeIndex = target;
                retargeted.Interpolation = channel.Interpolation == AnimationInterpolation::Step ? AnimationInterpolation::Step : AnimationInterpolation::Linear;
                const glm::mat4 space = target == 0 ? glm::inverse(local) : parentWorld; // relative to its rest, or in model space
                for (float time : times)
                {
                    const glm::vec3 translation = channel.PositionKeys.empty() ? rest.Translation : Animator::InterpolatePosition(time, channel);
                    const glm::quat rotation = channel.RotationKeys.empty() ? rest.Rotation : Animator::InterpolateRotation(time, channel);
                    const glm::vec3 scale = channel.ScaleKeys.empty() ? rest.Scale : Animator::InterpolateScale(time, channel);
                    const glm::mat4 animated = space * glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
                    glm::vec3 t, s, skew;
                    glm::quat r;
                    glm::vec4 perspective;
                    glm::decompose(animated, s, r, t, skew, perspective);
                    retargeted.PositionKeys.push_back({ time, t });
                    retargeted.RotationKeys.push_back({ time, glm::normalize(r) });
                    retargeted.ScaleKeys.push_back({ time, s });
                }
            }
        }
        for (const MorphCurve& curve : clip.MorphCurves)
        {
            const auto found = targets.find(curve.TargetNodeIndex);
            if (found == targets.end())
                continue;
            for (int32_t target : found->second)
            {
                MorphCurve& retargeted = out.MorphCurves.emplace_back(curve);
                retargeted.TargetNodeIndex = target;
            }
        }
        return !out.Channels.empty() || !out.MorphCurves.empty();
    }

    static void ApplyAnimationScale(AnimationSequence& animation, float scale)
    {
        if (std::abs(scale - 1.0f) < 1e-6f)
            return;
        for (NodeAnimationChannel& channel : animation.Channels)
            for (Keyframe<glm::vec3>& key : channel.PositionKeys)
                key.Value *= scale;
    }

    static void BuildClusters(MeshSubset& subset)
    {
        s_VerticesBeforeIndexing = 0;
        s_VerticesAfterIndexing = 0;
        for (size_t i = 0; i < subset.Meshes.size(); ++i)
        {
            if (subset.Pending[i].Skinned || !subset.Meshes[i].Morphs.empty())
                buildMeshlets(subset.Meshes[i], std::move(subset.Pending[i].Indices));
            else
                buildClusterLod(subset.Meshes[i], std::move(subset.Pending[i].Indices));
        }
        subset.Pending.clear();
        if (s_VerticesAfterIndexing < s_VerticesBeforeIndexing)
            NOX_CORE_INFO("MeshImporter: indexing welded {} -> {} vertices ({:.1f}% fewer)", s_VerticesBeforeIndexing, s_VerticesAfterIndexing,
                          100.0 * double(s_VerticesBeforeIndexing - s_VerticesAfterIndexing) / double(s_VerticesBeforeIndexing));
    }

    // One .nmat per material, written once: edits made to it later survive a recook of the model. `materialBase` is the
    // mesh path the material names derive from.
    static void WriteMaterials(const std::filesystem::path& assetDirectory, const std::filesystem::path& materialBase, const MeshSubset& subset)
    {
        std::error_code error;
        for (size_t index = 0; index < subset.Materials.size(); ++index)
        {
            const std::filesystem::path materialPath = assetDirectory / MeshImporter::MaterialAssetPath(materialBase, subset.Materials[index], index);
            if (!std::filesystem::exists(materialPath, error))
            {
                std::filesystem::create_directories(materialPath.parent_path(), error);
                MaterialSerializer::Serialize(materialPath, subset.Materials[index]);
            }
        }
        // The materials of the variants (named, as every glTF material: MaterialAssetPath does not fall back to the index).
        for (size_t index = 0; index < subset.Meshes.size(); ++index)
        {
            for (const MaterialVariantMapping& mapping : subset.Meshes[index].MaterialVariants)
            {
                const std::filesystem::path materialPath = assetDirectory / MeshImporter::MaterialAssetPath(materialBase, mapping.Material, index);
                if (!std::filesystem::exists(materialPath, error))
                {
                    std::filesystem::create_directories(materialPath.parent_path(), error);
                    MaterialSerializer::Serialize(materialPath, mapping.Material);
                }
            }
        }
    }

    // World matrix of every node of the file (parents before children).
    static std::vector<glm::mat4> NodeWorldMatrices(const std::vector<MeshNodeData>& nodes)
    {
        std::vector<glm::mat4> world(nodes.size(), glm::mat4(1.0f));
        std::vector<bool> done(nodes.size(), false);
        std::function<const glm::mat4&(size_t)> worldOf = [&](size_t index) -> const glm::mat4&
        {
            if (done[index])
                return world[index];
            const MeshNodeData& node = nodes[index];
            const glm::mat4 local = glm::translate(glm::mat4(1.0f), node.Translation) * glm::mat4_cast(node.Rotation) *
                                    glm::scale(glm::mat4(1.0f), node.Scale);
            const bool hasParent = node.Parent >= 0 && node.Parent < static_cast<int32_t>(nodes.size()) &&
                                   node.Parent != static_cast<int32_t>(index);
            world[index] = hasParent ? worldOf(static_cast<size_t>(node.Parent)) * local : local;
            done[index] = true;
            return world[index];
        };
        for (size_t index = 0; index < nodes.size(); ++index)
            worldOf(index);
        return world;
    }

    // One skin for the character, as Unreal's skeletal mesh has one skeleton: the file's skins merged (a joint shared by several skins
    // with the same inverse bind matrix is one entry, otherwise each its own), every skinned vertex's joint indices moved from its
    // skin's list into the merged one, and the rigid parts (unskinned meshes under a joint, MeshNodeData::RigidJoint) bound to their
    // joint with weight 1: a copy of their submeshes in model space at the rest pose, so the joint carries them as it moves. The
    // runtime then has the one skin it uses (Scene, AnimPose: Skins[0]), and the model is one character entity.
    static void UnifySkins(Skeleton& skeleton, std::vector<MeshData>& meshes, std::vector<MaterialData>& materials,
                           std::vector<PendingGeometry>& pending, std::vector<MeshNodeData>& nodes)
    {
        const bool rigidParts = std::any_of(nodes.begin(), nodes.end(), [](const MeshNodeData& node) { return node.RigidJoint >= 0 && node.SubmeshCount > 0; });
        if (skeleton.Skins.empty() || (skeleton.Skins.size() == 1 && !rigidParts))
            return;

        Skin* unified = new Skin();
        unified->Name = skeleton.Skins.size() == 1 && skeleton.Skins[0] ? skeleton.Skins[0]->Name : "Skin";
        auto entryOf = [&](Node* joint, const glm::mat4& inverseBind) -> uint32_t
        {
            for (size_t k = 0; k < unified->Joints.size(); ++k)
            {
                if (unified->Joints[k] != joint)
                    continue;
                float difference = 0.0f;
                for (int c = 0; c < 4; ++c)
                    difference = std::max(difference, glm::length(unified->InverseBindMatrices[k][c] - inverseBind[c]));
                if (difference < 1e-4f)
                    return static_cast<uint32_t>(k);
            }
            unified->Joints.push_back(joint);
            unified->InverseBindMatrices.push_back(inverseBind);
            return static_cast<uint32_t>(unified->Joints.size() - 1);
        };
        std::vector<std::vector<uint32_t>> remap(skeleton.Skins.size());
        for (size_t s = 0; s < skeleton.Skins.size(); ++s)
        {
            const Skin* skin = skeleton.Skins[s];
            if (!skin)
                continue;
            for (size_t j = 0; j < skin->Joints.size(); ++j)
                remap[s].push_back(entryOf(skin->Joints[j], j < skin->InverseBindMatrices.size() ? skin->InverseBindMatrices[j] : glm::mat4(1.0f)));
        }

        // Skinned submeshes, each once (a mesh instanced by nodes of different skins keeps the first one's).
        std::vector<int32_t> submeshSkin(meshes.size(), -1);
        for (const MeshNodeData& node : nodes)
        {
            if (!node.Skinned || node.RigidJoint >= 0 || node.SubmeshCount == 0 || node.SkinIndex < 0 ||
                node.SkinIndex >= static_cast<int32_t>(remap.size()))
                continue;
            for (uint32_t i = node.FirstSubmesh; i < node.FirstSubmesh + node.SubmeshCount && i < meshes.size(); ++i)
            {
                if (submeshSkin[i] >= 0)
                {
                    if (submeshSkin[i] != node.SkinIndex)
                        NOX_CORE_WARN("[Importer] mesh '{}' is skinned by several skins: the first one is kept", meshes[i].Name);
                    continue;
                }
                submeshSkin[i] = node.SkinIndex;
                const std::vector<uint32_t>& table = remap[node.SkinIndex];
                for (shaderio::Vertex& vertex : meshes[i].Vertices)
                {
                    for (int c = 0; c < 4; ++c)
                        vertex.boneIDs[c] = vertex.boneWeights[c] > 0.0f && vertex.boneIDs[c] < table.size() ? table[vertex.boneIDs[c]] : 0u;
                }
            }
        }

        // Rigid parts.
        const std::vector<glm::mat4> worlds = NodeWorldMatrices(nodes);
        for (size_t n = 0; n < nodes.size(); ++n)
        {
            MeshNodeData& node = nodes[n];
            if (node.RigidJoint < 0 || node.SubmeshCount == 0 || node.RigidJoint >= static_cast<int32_t>(skeleton.AllNodes.size()) ||
                !skeleton.AllNodes[node.RigidJoint])
                continue;
            const uint32_t entry = entryOf(skeleton.AllNodes[node.RigidJoint], glm::inverse(worlds[node.RigidJoint]));
            const glm::mat4 world = worlds[n];
            const glm::mat3 linear(world);
            const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
            const bool flip = glm::determinant(linear) < 0.0f;
            const uint32_t first = static_cast<uint32_t>(meshes.size());
            for (uint32_t i = node.FirstSubmesh; i < node.FirstSubmesh + node.SubmeshCount && i < first; ++i)
            {
                MeshData part = meshes[i];
                for (shaderio::Vertex& vertex : part.Vertices)
                {
                    vertex.pos = glm::vec3(world * glm::vec4(vertex.pos, 1.0f));
                    const glm::vec3 normal = normalMatrix * vertex.normal;
                    vertex.normal = glm::length(normal) > 0.0f ? glm::normalize(normal) : vertex.normal;
                    const glm::vec3 tangent = linear * glm::vec3(vertex.tangent);
                    vertex.tangent = glm::vec4(glm::length(tangent) > 0.0f ? glm::normalize(tangent) : glm::vec3(vertex.tangent),
                                               flip ? -vertex.tangent.w : vertex.tangent.w);
                    vertex.boneIDs = glm::uvec4(entry, 0, 0, 0);
                    vertex.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
                }
                for (MorphTarget& morph : part.Morphs)
                {
                    for (glm::vec3& delta : morph.Positions)
                        delta = linear * delta;
                    for (glm::vec3& delta : morph.Normals)
                        delta = normalMatrix * delta;
                }
                PendingGeometry geometry = i < pending.size() ? pending[i] : PendingGeometry{};
                geometry.Skinned = true;
                if (flip)
                    for (size_t t = 0; t + 2 < geometry.Indices.size(); t += 3)
                        std::swap(geometry.Indices[t + 1], geometry.Indices[t + 2]);
                MaterialData material = i < materials.size() ? materials[i] : MaterialData{};
                meshes.push_back(std::move(part));
                materials.push_back(std::move(material));
                pending.push_back(std::move(geometry));
            }
            node.FirstSubmesh = first;
            node.SubmeshCount = static_cast<uint32_t>(meshes.size()) - first;
        }

        for (Skin* skin : skeleton.Skins)
            delete skin;
        skeleton.Skins = { unified };
    }

    // A mesh collider's submeshes in this asset: those of a node of the asset drawing that glTF mesh (none: a count of 0, the collider
    // is left out where the asset is placed). Before the asset's clusters are built, after its subset was taken.
    static void ResolveColliderMeshes(MeshSubset& subset)
    {
        for (MeshNodeData& node : subset.Nodes)
        {
            NodePhysics& physics = node.Physics;
            if (physics.Shape != NodePhysics::ShapeType::Mesh)
                continue;
            physics.ColliderSubmeshCount = 0;
            for (const MeshNodeData& drawer : subset.Nodes)
            {
                if (drawer.MeshIndex == physics.ColliderMesh && drawer.SubmeshCount > 0)
                {
                    physics.ColliderFirstSubmesh = drawer.FirstSubmesh;
                    physics.ColliderSubmeshCount = drawer.SubmeshCount;
                    break;
                }
            }
        }
    }

    // A glTF mesh as an asset of its own (Do Not Combine): its submeshes, copied, under one identity node (what a drag places).
    // After it come the file's instances of the mesh, one node each at its world transform (FileLayoutParent): dragging
    // several assets together places all of them where the file had them. False when no unskinned node uses the mesh.
    static bool CopySingleMesh(const std::vector<MeshData>& meshes, const std::vector<MaterialData>& materials,
                               const std::vector<PendingGeometry>& pending, const std::vector<MeshNodeData>& nodes,
                               const std::vector<glm::mat4>& worlds,
                               int32_t meshIndex, bool skinned, MeshSubset& out)
    {
        const MeshNodeData* user = nullptr;
        for (const MeshNodeData& node : nodes)
        {
            if (node.MeshIndex == meshIndex && node.Skinned == skinned && node.SubmeshCount > 0)
            {
                user = &node;
                break;
            }
        }
        if (!user)
            return false;

        for (uint32_t i = user->FirstSubmesh; i < user->FirstSubmesh + user->SubmeshCount && i < meshes.size(); ++i)
        {
            out.Meshes.push_back(meshes[i]);
            if (i < materials.size())
                out.Materials.push_back(materials[i]);
            if (i < pending.size())
                out.Pending.push_back(pending[i]);
        }

        // The node's extras and its own body go along (a collider on a child node of the file stays with the file: Import Into
        // Level keeps those).
        auto carryNodeData = [](const MeshNodeData& from, MeshNodeData& to)
        {
            to.Extras = from.Extras;
            if (from.Physics.Body != NodePhysics::BodyType::None)
                to.Physics = from.Physics;
            to.Physics.JointData = {}; // its other end is a node of the file, not of this asset
        };

        MeshNodeData& node = out.Nodes.emplace_back();
        node.Name = user->MeshName;
        node.FirstSubmesh = 0;
        node.SubmeshCount = static_cast<uint32_t>(out.Meshes.size());
        node.MeshIndex = meshIndex;
        carryNodeData(*user, node);

        const uint32_t submeshCount = static_cast<uint32_t>(out.Meshes.size());
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            const MeshNodeData& instance = nodes[index];
            if (instance.MeshIndex != meshIndex || instance.Skinned != skinned || instance.SubmeshCount == 0)
                continue;

            MeshNodeData& placed = out.Nodes.emplace_back();
            placed.Name = instance.Name;
            placed.Parent = MeshNodeData::FileLayoutParent;
            glm::vec3 skew;
            glm::vec4 perspective;
            glm::decompose(worlds[index], placed.Scale, placed.Rotation, placed.Translation, skew, perspective);
            placed.FirstSubmesh = 0;
            placed.SubmeshCount = submeshCount;
            placed.MeshIndex = meshIndex;
            carryNodeData(instance, placed);
        }
        return !out.Meshes.empty();
    }

    // The clips a cook of `cookedBase` wrote, listed in <cookedBase>.clips: the ones a previous cook wrote and this one did not (a clip
    // renamed or removed in the source, animations turned off) are deleted, so no stale .nanim stays registered next to the model.
    // A list rather than a name pattern: <model>_*.nanim would also match another model named <model>_Something in the folder.
    static void ReplaceClipManifest(const std::filesystem::path& cookedBase, const std::vector<std::filesystem::path>& written)
    {
        const std::filesystem::path manifest = cookedBase.string() + ".clips";
        std::unordered_set<std::string> keep;
        for (const std::filesystem::path& clip : written)
            keep.insert(clip.filename().string());

        std::error_code error;
        if (std::ifstream previous(manifest); previous)
        {
            std::string name;
            while (std::getline(previous, name))
            {
                // Only names this model writes (a model renamed since keeps the clips its scenes may still use).
                if (name.empty() || keep.contains(name) || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                    name.rfind(cookedBase.stem().string() + "_", 0) != 0)
                    continue;
                const std::filesystem::path stale = cookedBase.parent_path() / name;
                if (stale.extension() == ".nanim" && std::filesystem::remove(stale, error))
                    NOX_CORE_INFO("[Importer] Removed the stale clip {} (no longer in the source)", stale.generic_string());
            }
        }
        if (written.empty())
        {
            std::filesystem::remove(manifest, error);
            return;
        }
        std::ofstream out(manifest, std::ios::trunc);
        for (const std::filesystem::path& clip : written)
            out << clip.filename().string() << "\n";
    }

    // The file's nodes that belong to a character: the skins' joints and every node above them (the armature). A clip that moves only
    // these is the skeletal asset's; one that moves any other node is (also) a node animation of the static meshes or the level.
    static std::vector<uint8_t> CharacterNodes(const Skeleton& skeleton, size_t nodeCount)
    {
        std::vector<uint8_t> character(nodeCount, 0);
        for (const Skin* skin : skeleton.Skins)
        {
            for (const Node* joint : skin->Joints)
            {
                for (const Node* node = joint; node; node = node->Parent)
                {
                    if (node->Index >= 0 && node->Index < static_cast<int32_t>(nodeCount))
                        character[node->Index] = 1;
                }
            }
        }
        return character;
    }

    // The nodes a clip moves that are not a character's, sorted, each once.
    static std::vector<int32_t> NodeAnimationTargets(const AnimationSequence& clip, const std::vector<uint8_t>& character)
    {
        std::vector<int32_t> nodes;
        auto add = [&](int32_t node)
        {
            if (node >= 0 && (node >= static_cast<int32_t>(character.size()) || !character[node]))
                nodes.push_back(node);
        };
        for (const NodeAnimationChannel& channel : clip.Channels)
            add(channel.TargetNodeIndex);
        for (const MorphCurve& curve : clip.MorphCurves)
            add(curve.TargetNodeIndex);
        std::sort(nodes.begin(), nodes.end());
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
        return nodes;
    }

    bool MeshImporter::CookMesh(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
    {
        const std::filesystem::path cookedPath = assetDirectory / metadata.FilePath;
        const std::filesystem::path sourcePath = assetDirectory / metadata.SourceFilePath;
        NOX_CORE_INFO("Cooking GLTF from {} to {}", sourcePath.string(), cookedPath.string());

        std::vector<MaterialData> materialDataList;
        std::vector<LightNodeData> lightDataList;
        std::vector<MeshNodeData> nodeDataList;
        std::vector<CameraNodeData> cameraDataList;
        Skeleton extractedSkeleton;
        std::vector<Ref<AnimationSequence>> extractedAnimations;

        std::vector<PendingGeometry> pendingGeometry; // clusters are built below, after the import settings picked and merged the meshes
        std::vector<MeshData> meshDataList = ParseGltfToMeshData(sourcePath, materialDataList, extractedSkeleton, extractedAnimations,
                                                                 lightDataList, nodeDataList, cameraDataList, &pendingGeometry);
        if (meshDataList.empty())
        {
            NOX_CORE_WARN("MeshImporter::CookMesh - Failed to load or empty mesh at source path: {}", sourcePath.string());
            return false;
        }

        // Import settings decide which meshes this asset holds (Unreal's static / skeletal split): a skeletal mesh
        // (.nmesh) the skinned meshes, a static mesh (.nsmesh) the rest. Lights and cameras belong to static meshes.
        // A per-mesh asset (SourceMeshIndex) holds one glTF mesh and nothing else.
        const MeshImportSettings& importSettings = metadata.MeshSettings;
        if (!importSettings.ImportSkinWeights)
            ConvertSkinnedToStatic(nodeDataList, pendingGeometry);
        else
            UnifySkins(extractedSkeleton, meshDataList, materialDataList, pendingGeometry, nodeDataList);
        const bool isStatic = metadata.Type == AssetType::StaticMesh;
        const bool singleMesh = importSettings.SourceMeshIndex >= 0;
        MeshSubset subset;
        if (singleMesh)
        {
            if (!CopySingleMesh(meshDataList, materialDataList, pendingGeometry, nodeDataList, NodeWorldMatrices(nodeDataList), importSettings.SourceMeshIndex, metadata.Type == AssetType::Mesh, subset))
            {
                NOX_CORE_WARN("MeshImporter::CookMesh - glTF mesh {} of {} has no static geometry", importSettings.SourceMeshIndex, sourcePath.string());
                return false;
            }
            lightDataList.clear();
            cameraDataList.clear();
        }
        else
        {
            subset = ExtractSubset(meshDataList, materialDataList, pendingGeometry, nodeDataList,
                [&](const MeshNodeData& node) { return node.Skinned ? importSettings.ImportSkeletalMeshes : importSettings.ImportStaticMeshes; });
        }
        if (subset.Meshes.empty())
        {
            NOX_CORE_WARN("MeshImporter::CookMesh - the import settings leave no meshes in {} (static: {}, skeletal: {})",
                          sourcePath.string(), importSettings.ImportStaticMeshes, importSettings.ImportSkeletalMeshes);
            return false;
        }

        ResolveColliderMeshes(subset);

        // Combine (Unreal): only for an asset that holds one kind, which the import always gives it.
        const bool onlyStatic = importSettings.ImportStaticMeshes && !importSettings.ImportSkeletalMeshes;
        const bool onlySkeletal = importSettings.ImportSkeletalMeshes && !importSettings.ImportStaticMeshes;
        bool bakedStatic = false; // merged static geometry: no node structure, lights, cameras or node animations survive
        if (!singleMesh && onlyStatic && importSettings.StaticCombine != MeshCombineMode::DoNotCombine)
        {
            CombineSubmeshes(subset, false);
            bakedStatic = true;
            lightDataList.clear();
            cameraDataList.clear();
        }
        else if (!singleMesh && onlySkeletal && importSettings.SkeletalCombine != MeshCombineMode::DoNotCombine)
            CombineSubmeshes(subset, true);

        // The mesh's own geometry and node structure, whatever kind: baked in either way, so a skeletal instance no longer needs
        // ImportScale applied through its placement Transform.Scale either (matches the static path -- see EditorLayer's drag-in).
        ApplyImportScale(subset, importSettings.ImportScale);

        BuildClusters(subset);

        std::error_code error;
        std::filesystem::create_directories(cookedPath.parent_path(), error);
        if (isStatic)
            MeshSerializer::SerializeStaticMesh(cookedPath, subset.Meshes, subset.Materials, lightDataList, subset.Nodes, cameraDataList);
        else
            MeshSerializer::SerializeMesh(cookedPath, subset.Meshes, subset.Materials, lightDataList, subset.Nodes, cameraDataList);

        // The skeleton goes with skeletal meshes, clips with whichever asset the import gave "Import Animations" (node
        // animations of a level, e.g. Bistro's fans, have no skeleton).
        if (!singleMesh && importSettings.ImportSkeletalMeshes && !extractedSkeleton.Skins.empty())
        {
            ApplySkeletonScale(extractedSkeleton, importSettings.ImportScale);
            std::filesystem::path skelPath = cookedPath;
            skelPath.replace_extension(".nskel");
            SkeletonSerializer::Serialize(skelPath, extractedSkeleton);
            NOX_CORE_INFO("[Importer] Extracted and cooked Skeleton ({} nodes) to {}", extractedSkeleton.AllNodes.size(), skelPath.string());
        }

        if (!singleMesh && !bakedStatic)
        {
            std::vector<std::filesystem::path> clips;
            if (importSettings.ImportAnimations)
            {
                const std::vector<uint8_t> character = CharacterNodes(extractedSkeleton, nodeDataList.size());
                for (const Ref<AnimationSequence>& animation : extractedAnimations)
                {
                    // A static asset takes the node animations only (a character's clips are its skeletal asset's).
                    if (isStatic && NodeAnimationTargets(*animation, character).empty())
                        continue;
                    ApplyAnimationScale(*animation, importSettings.ImportScale);
                    std::filesystem::path animPath = cookedPath.parent_path() / (cookedPath.stem().string() + "_" + animation->Name + ".nanim");
                    AnimationSerializer::Serialize(animPath, *animation);
                    clips.push_back(animPath);
                    NOX_CORE_INFO("[Importer] Extracted and cooked Animation Sequence '{}' to {}", animation->Name, animPath.string());
                }
            }
            ReplaceClipManifest(cookedPath, clips);
        }

        WriteMaterials(assetDirectory, importSettings.MaterialBasePath.empty() ? metadata.FilePath : importSettings.MaterialBasePath, subset);

        // Last: an interrupted cook leaves no hash, so the model is cooked again.
        Utility::saveHashToFile(cookedPath.string() + ".hash", Utility::calcul_hash_streaming(sourcePath.string()));
        return true;
    }

    std::vector<MeshImporter::SplitMesh> MeshImporter::CookSplitMeshes(const std::filesystem::path& assetDirectory, const AssetMetadata& wholeFile,
                                                                       const std::filesystem::path& directory, bool skinned, std::atomic<uint32_t>* total,
                                                                       std::atomic<uint32_t>* done, SplitLevel* level)
    {
        const std::filesystem::path sourcePath = assetDirectory / wholeFile.SourceFilePath;
        NOX_CORE_INFO("Cooking every {} mesh of {} into {}", skinned ? "skinned" : "static", sourcePath.string(), directory.generic_string());

        std::vector<MaterialData> materialDataList;
        std::vector<LightNodeData> lightDataList;
        std::vector<MeshNodeData> nodeDataList;
        std::vector<CameraNodeData> cameraDataList;
        Skeleton extractedSkeleton;
        std::vector<Ref<AnimationSequence>> extractedAnimations;
        std::vector<PendingGeometry> pendingGeometry;
        std::vector<MeshData> meshDataList = ParseGltfToMeshData(sourcePath, materialDataList, extractedSkeleton, extractedAnimations,
                                                                 lightDataList, nodeDataList, cameraDataList, &pendingGeometry);

        if (!wholeFile.MeshSettings.ImportSkinWeights)
            ConvertSkinnedToStatic(nodeDataList, pendingGeometry);
        else
            UnifySkins(extractedSkeleton, meshDataList, materialDataList, pendingGeometry, nodeDataList);

        if (level)
        {
            level->Nodes = nodeDataList;
            level->Lights = lightDataList;
            level->Cameras = cameraDataList;
        }

        std::vector<SplitMesh> result;
        if (meshDataList.empty())
            return result;

        const XXH128_hash_t sourceHash = Utility::calcul_hash_streaming(sourcePath.string());
        std::error_code error;
        std::filesystem::create_directories(assetDirectory / directory, error);

        if (total)
        {
            std::unordered_set<int32_t> unique;
            for (const MeshNodeData& node : nodeDataList)
            {
                if (node.Skinned == skinned && node.SubmeshCount > 0 && node.MeshIndex >= 0)
                    unique.insert(node.MeshIndex);
            }
            total->store(static_cast<uint32_t>(unique.size()));
        }

        const std::vector<glm::mat4> worlds = NodeWorldMatrices(nodeDataList);
        std::unordered_set<int32_t> seenMeshes;
        std::unordered_set<std::string> usedNames;
        for (const MeshNodeData& node : nodeDataList)
        {
            if (node.Skinned != skinned || node.SubmeshCount == 0 || node.MeshIndex < 0 || !seenMeshes.insert(node.MeshIndex).second)
                continue;

            MeshSubset subset;
            if (!CopySingleMesh(meshDataList, materialDataList, pendingGeometry, nodeDataList, worlds, node.MeshIndex, skinned, subset))
                continue;
            ResolveColliderMeshes(subset);
            // Not for Import Into Level (level != nullptr): that path scales the whole placed scene -- meshes, lights, cameras --
            // uniformly through the level root's own Transform.Scale (ModelInstance::SpawnLevel), unrelated to this baking; baking
            // it into the geometry here too would scale it twice. Only the plain "drag this cooked mesh in on its own" case gets it
            // (static or skeletal alike -- see the matching skeleton/animation scaling below for the skinned case).
            if (!level)
                ApplyImportScale(subset, wholeFile.MeshSettings.ImportScale);
            BuildClusters(subset);

            std::string name = node.MeshName.empty() ? "Mesh_" + std::to_string(node.MeshIndex) : node.MeshName;
            for (char& character : name)
            {
                if (std::strchr("<>:\"/\\|?*", character) != nullptr)
                    character = '_';
            }
            if (!usedNames.insert(name).second)
                name += "_" + std::to_string(node.MeshIndex);

            SplitMesh split;
            split.Name = name;
            split.MeshIndex = node.MeshIndex;
            split.FilePath = directory / (name + (skinned ? ".nmesh" : ".nsmesh"));
            const std::filesystem::path cookedPath = assetDirectory / split.FilePath;
            if (skinned)
                MeshSerializer::SerializeMesh(cookedPath, subset.Meshes, subset.Materials, {}, subset.Nodes, {});
            else
                MeshSerializer::SerializeStaticMesh(cookedPath, subset.Meshes, subset.Materials, {}, subset.Nodes, {});
            // A static mesh's node animations and morph target animations (a fan, a door, a blinking face card): its own clips,
            // retargeted to its nodes (a plain drag of it then animates as in the file). Import Into Level keeps the file's own
            // clips with the level instead (below); skinned meshes share the file's (they play on its skeleton).
            if (!skinned && !level)
            {
                std::vector<std::filesystem::path> clips;
                if (wholeFile.MeshSettings.ImportAnimations)
                {
                    for (const Ref<AnimationSequence>& animation : extractedAnimations)
                    {
                        AnimationSequence retargeted;
                        if (!RetargetClipToMesh(*animation, nodeDataList, worlds, node.MeshIndex, skinned, retargeted))
                            continue;
                        ApplyAnimationScale(retargeted, wholeFile.MeshSettings.ImportScale);
                        const std::filesystem::path animPath = cookedPath.parent_path() / (cookedPath.stem().string() + "_" + animation->Name + ".nanim");
                        AnimationSerializer::Serialize(animPath, retargeted);
                        clips.push_back(animPath);
                    }
                }
                ReplaceClipManifest(cookedPath, clips);
            }
            WriteMaterials(assetDirectory, wholeFile.FilePath, subset);
            Utility::saveHashToFile(cookedPath.string() + ".hash", sourceHash);
            result.push_back(std::move(split));
            if (done)
                done->fetch_add(1);
        }
        // Skinned meshes share the file's skeleton and clips, written once, named after the whole-file base path (what
        // LinkImportedAssets looks for through MaterialBasePath).
        if (skinned)
        {
            // Same "not for Import Into Level" reasoning as the geometry above.
            const float scale = level ? 1.0f : wholeFile.MeshSettings.ImportScale;
            const std::filesystem::path base = assetDirectory / wholeFile.FilePath;
            if (!extractedSkeleton.Skins.empty())
            {
                ApplySkeletonScale(extractedSkeleton, scale);
                std::filesystem::path skelPath = base;
                skelPath.replace_extension(".nskel");
                SkeletonSerializer::Serialize(skelPath, extractedSkeleton);
            }
            std::vector<std::filesystem::path> clips;
            if (wholeFile.MeshSettings.ImportAnimations)
            {
                for (const Ref<AnimationSequence>& animation : extractedAnimations)
                {
                    ApplyAnimationScale(*animation, scale);
                    const std::filesystem::path animPath = base.parent_path() / (base.stem().string() + "_" + animation->Name + ".nanim");
                    AnimationSerializer::Serialize(animPath, *animation);
                    clips.push_back(animPath);
                }
            }
            ReplaceClipManifest(base, clips);
        }
        // Import Into Level: the file's node animations (Bistro's fans, a door) go with the level, named after the whole-file
        // base path like a skeletal model's clips.
        if (!skinned && level && wholeFile.MeshSettings.ImportAnimations)
        {
            // Only what moves the level's own nodes: a character's joints are animated by its skeletal asset's animator.
            const std::vector<uint8_t> character = CharacterNodes(extractedSkeleton, nodeDataList.size());
            for (const Ref<AnimationSequence>& animation : extractedAnimations)
            {
                std::vector<int32_t> targets = NodeAnimationTargets(*animation, character);
                if (targets.empty())
                    continue;
                const std::filesystem::path relativePath = wholeFile.FilePath.parent_path() / (wholeFile.FilePath.stem().string() + "_" + animation->Name + ".nanim");
                AnimationSerializer::Serialize(assetDirectory / relativePath, *animation);

                SplitLevel::Clip clip;
                clip.FilePath = relativePath;
                clip.Nodes = std::move(targets);
                level->Clips.push_back(std::move(clip));
            }
        }
        return result;
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

                // Morph target weights (path "weights"): one value per target of the node's mesh at every key (three -- in tangent,
                // value, out tangent -- with CUBICSPLINE), floats or normalized integers. A curve per target, by its name.
                if (Tg3StrEquals(channel.target.path, "weights"))
                {
                    if (targetNode.mesh < 0 || targetNode.mesh >= static_cast<int32_t>(model.meshes_count))
                        continue;
                    const tg3_mesh& mesh = model.meshes[targetNode.mesh];
                    const uint32_t targetCount = mesh.primitives_count > 0 ? mesh.primitives[0].targets_count : 0u;
                    const std::vector<float> times = ReadAccessorFloats(model, sampler.input, 1);
                    const std::vector<float> values = ReadAccessorFloats(model, sampler.output, 1);
                    const bool cubic = Tg3StrEquals(sampler.interpolation, "CUBICSPLINE");
                    const size_t perKey = size_t(targetCount) * (cubic ? 3 : 1);
                    if (targetCount == 0 || times.empty() || values.size() < times.size() * perKey)
                        continue;
                    for (uint32_t target = 0; target < targetCount; ++target)
                    {
                        MorphCurve curve;
                        curve.NodeName = nodeName;
                        curve.TargetNodeIndex = targetNodeIdx;
                        curve.Target = MorphTargetName(mesh, target);
                        curve.Interpolation = Tg3StrEquals(sampler.interpolation, "STEP") ? AnimationInterpolation::Step
                                            : cubic ? AnimationInterpolation::CubicSpline : AnimationInterpolation::Linear;
                        for (size_t k = 0; k < times.size(); ++k)
                            curve.Keys.push_back({ times[k], values[k * perKey + (cubic ? targetCount : 0) + target] });
                        maxDuration = std::max(maxDuration, times.back());
                        animSeq->MorphCurves.push_back(std::move(curve));
                    }
                    continue;
                }

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

            std::filesystem::path sourcePath = modelPath.parent_path() / uri;
            if (std::filesystem::exists(sourcePath))
                return sourcePath;

            // Match Donut's glTF importer: Bistro contains a few malformed MSFT_texture_dds entries whose
            // extension points at another missing PNG. The matching DDS is present beside it and is what RTXPT
            // loads. This fallback also handles ordinary glTF PNG references distributed with DDS replacements.
            std::filesystem::path ddsPath = sourcePath;
            ddsPath.replace_extension(".dds");
            if (std::filesystem::exists(ddsPath))
                return ddsPath;

            return sourcePath;
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

    // Node extensions that change what the node table spawns:
    //  - KHR_node_visibility: a node with "visible": false and everything below it shows no mesh.
    //  - EXT_mesh_gpu_instancing: the node's mesh is drawn once per instance (TRANSLATION / ROTATION / SCALE accessors), each an
    //    entity of its own below the node; the node itself draws nothing (the extension's rule).
    static void ApplyNodeExtensions(const tg3_model& model, std::vector<MeshNodeData>& nodes)
    {
        const uint32_t fileNodeCount = std::min<uint32_t>(model.nodes_count, static_cast<uint32_t>(nodes.size()));

        std::vector<uint8_t> hidden(fileNodeCount, 0);
        for (uint32_t i = 0; i < fileNodeCount; i++)
        {
            if (const tg3_extension* visibility = FindExtension(model.nodes[i].ext, "KHR_node_visibility"))
            {
                const tg3_value* visible = FindMember(visibility->value, "visible");
                if (visible && visible->type == TG3_VALUE_BOOL && !visible->bool_val)
                    hidden[i] = 1;
            }
        }
        for (bool grew = true; grew;)
        {
            grew = false;
            for (uint32_t i = 0; i < fileNodeCount; i++)
            {
                const int32_t parent = nodes[i].Parent;
                if (!hidden[i] && parent >= 0 && parent < static_cast<int32_t>(fileNodeCount) && hidden[parent])
                {
                    hidden[i] = 1;
                    grew = true;
                }
            }
        }
        for (uint32_t i = 0; i < fileNodeCount; i++)
        {
            if (hidden[i] && nodes[i].FirstSubmesh != UINT32_MAX)
            {
                nodes[i].FirstSubmesh = UINT32_MAX;
                nodes[i].SubmeshCount = 0;
            }
        }

        for (uint32_t i = 0; i < fileNodeCount; i++)
        {
            const tg3_extension* instancing = FindExtension(model.nodes[i].ext, "EXT_mesh_gpu_instancing");
            if (!instancing || nodes[i].FirstSubmesh == UINT32_MAX || nodes[i].Skinned)
                continue;
            const tg3_value* attributes = FindMember(instancing->value, "attributes");
            if (!attributes)
                continue;
            auto accessorOf = [&](std::string_view name) -> int64_t
            {
                const tg3_value* value = FindMember(*attributes, name);
                if (!value)
                    return -1;
                return value->type == TG3_VALUE_INT ? value->int_val : value->type == TG3_VALUE_REAL ? static_cast<int64_t>(value->real_val) : -1;
            };
            const std::vector<float> translations = ReadAccessorFloats(model, accessorOf("TRANSLATION"), 3);
            const std::vector<float> rotations = ReadAccessorFloats(model, accessorOf("ROTATION"), 4);
            const std::vector<float> scales = ReadAccessorFloats(model, accessorOf("SCALE"), 3);
            const size_t count = std::max({ translations.size() / 3, rotations.size() / 4, scales.size() / 3 });
            if (count == 0)
                continue;

            const MeshNodeData source = nodes[i];
            for (size_t k = 0; k < count; k++)
            {
                MeshNodeData instance;
                instance.Name = source.Name + "_" + std::to_string(k);
                instance.Parent = static_cast<int32_t>(i);
                if (k * 3 + 2 < translations.size())
                    instance.Translation = glm::vec3(translations[k * 3], translations[k * 3 + 1], translations[k * 3 + 2]);
                if (k * 4 + 3 < rotations.size())
                    instance.Rotation = glm::normalize(glm::quat(rotations[k * 4 + 3], rotations[k * 4], rotations[k * 4 + 1], rotations[k * 4 + 2]));
                if (k * 3 + 2 < scales.size())
                    instance.Scale = glm::vec3(scales[k * 3], scales[k * 3 + 1], scales[k * 3 + 2]);
                instance.FirstSubmesh = source.FirstSubmesh;
                instance.SubmeshCount = source.SubmeshCount;
                instance.MeshIndex = source.MeshIndex;
                instance.MeshName = source.MeshName;
                nodes.push_back(std::move(instance));
            }
            nodes[i].FirstSubmesh = UINT32_MAX;
            nodes[i].SubmeshCount = 0;
        }
    }

    static void ExtractNodeTRS(const tg3_node& node, glm::vec3& translation, glm::quat& rotation, glm::vec3& scale)
    {
        if (node.has_matrix)
        {
            glm::mat4 m = glm::mat4(glm::make_mat4(node.matrix));
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

    // Fills in vertex.tangent for a primitive whose file didn't provide a TANGENT attribute, via meshoptimizer's
    // MikkTSpace-compatible generator (meshopt_TangentCompatible: normal maps are near-universally baked assuming
    // MikkTSpace tangents, so matching it at runtime is what makes the baked detail line up with the lighting).
    // meshopt_generateTangents outputs one tangent per triangle CORNER, not per vertex: a vertex shared by corners
    // whose tangents genuinely disagree (a mirrored UV island's seam, most often) needs to become two vertices, one per
    // side, each keeping its own -- exactly like a hard vertex normal, just for tangent space instead. Collapsing that
    // disagreement into one shared, wrong-on-one-side vertex (or discarding it and reconstructing a tangent frame from
    // scratch per pixel, at runtime, from raw UV/position derivatives, which is what GBufferMaterial.slang used to
    // fall back to for every mesh) is exactly what produced a visible seam in specular response, right where such a
    // mirror seam actually is, on an otherwise perfectly continuous surface.
    // outOrigins: for every vertex appended (a tangent seam's duplicate), the vertex it copies (its morph deltas are the same).
    static void GenerateMissingTangents(std::vector<shaderio::Vertex>& vertices, std::vector<uint32_t>& indices, std::vector<uint32_t>* outOrigins = nullptr)
    {
        if (vertices.empty() || indices.empty())
            return;

        std::vector<float> cornerTangents(indices.size() * 4);
        meshopt_generateTangents(cornerTangents.data(), indices.data(), indices.size(),
                                  &vertices[0].pos.x, vertices.size(), sizeof(shaderio::Vertex),
                                  &vertices[0].normal.x, sizeof(shaderio::Vertex),
                                  &vertices[0].uv0.x, sizeof(shaderio::Vertex),
                                  meshopt_TangentCompatible);

        auto tangentsMatch = [](const glm::vec4& a, const glm::vec4& b)
        {
            return a.w == b.w && glm::dot(glm::vec3(a), glm::vec3(b)) > 0.9999f;
        };

        // Every vertex's first-seen tangent goes straight onto it; a later corner with a different tangent gets a new,
        // duplicate vertex (reusing an already-made duplicate if this exact tangent already showed up before).
        const size_t originalVertexCount = vertices.size();
        std::vector<uint8_t> assigned(originalVertexCount, 0);
        std::vector<std::vector<std::pair<glm::vec4, uint32_t>>> variantsOf(originalVertexCount);

        for (size_t corner = 0; corner < indices.size(); ++corner)
        {
            const uint32_t originalIndex = indices[corner];
            const glm::vec4 tangent(cornerTangents[corner * 4 + 0], cornerTangents[corner * 4 + 1],
                                     cornerTangents[corner * 4 + 2], cornerTangents[corner * 4 + 3]);

            if (!assigned[originalIndex])
            {
                vertices[originalIndex].tangent = tangent;
                assigned[originalIndex] = 1;
                variantsOf[originalIndex].push_back({ tangent, originalIndex });
                continue;
            }

            uint32_t target = UINT32_MAX;
            for (const auto& variant : variantsOf[originalIndex])
            {
                if (tangentsMatch(variant.first, tangent))
                {
                    target = variant.second;
                    break;
                }
            }
            if (target == UINT32_MAX)
            {
                shaderio::Vertex duplicate = vertices[originalIndex];
                duplicate.tangent = tangent;
                target = static_cast<uint32_t>(vertices.size());
                vertices.push_back(duplicate);
                if (outOrigins)
                    outOrigins->push_back(originalIndex);
                variantsOf[originalIndex].push_back({ tangent, target });
            }
            indices[corner] = target;
        }
    }

    // NVIDIA RTXPT's hand-tuned materials for a model (RTXPT-Assets/Materials/<model>.<material>.material.json, which RTXPT's
    // MaterialsBaker prefers over the glTF's): roughness of foliage, fabric and plants, specular colors, and the emission of
    // the lamps (Bistro's bulbs are 30-50x their glTF emission there), leaves' and cloth's diffuse transmission. Optional: looked
    // up in a "Materials" folder next to the glTF or in any folder above it, nothing changes when there is none. Read with yaml-cpp
    // (JSON is YAML).
    static void ApplyRtxptMaterialOverride(const std::filesystem::path& gltfPath, MaterialData& material)
    {
        if (material.Name.empty())
            return;
        // The folder that has them, per model, found once (thread_local: imports run on jobs).
        thread_local std::unordered_map<std::string, std::filesystem::path> s_Folders;
        const std::string model = gltfPath.stem().string();
        const std::string key = gltfPath.parent_path().string() + "|" + model;
        auto found = s_Folders.find(key);
        if (found == s_Folders.end())
        {
            std::filesystem::path folder;
            std::error_code error;
            for (std::filesystem::path directory = gltfPath.parent_path(); !directory.empty(); directory = directory.parent_path())
            {
                const std::filesystem::path candidate = directory / "Materials";
                if (std::filesystem::is_directory(candidate, error))
                {
                    for (const auto& entry : std::filesystem::directory_iterator(candidate, error))
                    {
                        const std::string name = entry.path().filename().string();
                        if (name.rfind(model + ".", 0) == 0 && name.size() > 14 && name.ends_with(".material.json"))
                        {
                            folder = candidate;
                            break;
                        }
                    }
                }
                if (!folder.empty() || directory == directory.parent_path())
                    break;
            }
            found = s_Folders.emplace(key, folder).first;
            if (!folder.empty())
                NOX_CORE_INFO("Using RTXPT material overrides from {}", folder.string());
        }
        if (found->second.empty())
            return;

        const std::filesystem::path file = found->second / (model + "." + material.Name + ".material.json");
        std::error_code error;
        if (!std::filesystem::exists(file, error))
            return;
        try
        {
            const YAML::Node json = YAML::LoadFile(file.string());
            auto number = [&](const char* name, float fallback) { return json[name] ? json[name].as<float>() : fallback; };
            auto color = [&](const char* name, glm::vec3 fallback)
            {
                const YAML::Node node = json[name];
                return node && node.IsSequence() && node.size() >= 3 ? glm::vec3(node[0].as<float>(), node[1].as<float>(), node[2].as<float>())
                                                                     : fallback;
            };
            const bool specularGloss = json["UseSpecularGlossModel"] && json["UseSpecularGlossModel"].as<bool>();
            if (specularGloss)
            {
                // RTXPT: roughness = 1 - glossTexture * (1 - Roughness), i.e. its Roughness is 1 - the glossiness factor.
                material.Workflow = 1.0f;
                material.DiffuseFactor = glm::vec4(color("BaseOrDiffuseColor", glm::vec3(material.DiffuseFactor)), material.DiffuseFactor.a);
                material.SpecularFactor = glm::vec4(color("SpecularColor", glm::vec3(material.SpecularFactor)),
                                                    1.0f - number("Roughness", 1.0f - material.SpecularFactor.a));
            }
            else
            {
                // RTXPT's metal-rough model, also for a material the glTF authors as specular-glossiness (the Bistro's glass: its
                // white specular texture read as specular made the panes metal mirrors). Its texture is then read as RTXPT reads
                // it, occlusion-roughness-metallic, under these factors.
                const float alpha = material.Workflow == 1.0f ? material.DiffuseFactor.a : material.BaseColorFactor.a;
                material.Workflow = 0.0f;
                material.BaseColorFactor = glm::vec4(color("BaseOrDiffuseColor", glm::vec3(material.BaseColorFactor)), alpha);
                material.RoughnessFactor = number("Roughness", material.RoughnessFactor);
                material.MetallicFactor = number("Metalness", material.MetallicFactor);
            }
            // A normal map RTXPT scales to nothing (its glass) is not applied.
            if (json["NormalTextureScale"] && json["NormalTextureScale"].as<float>() <= 0.0f)
                material.NormalTexturePath.clear();
            material.IOR = number("IoR", material.IOR);
            material.DiffuseTransmission = std::clamp(number("DiffuseTransmissionFactor", material.DiffuseTransmission), 0.0f, 1.0f);
            // RTXPT's path tracer flags: thin-walled (no refraction, 1/IoR on both faces), excluded from shadow rays (window glass).
            material.PathTracerFlags = shaderio::PathTracerRtxptMaterial;
            if (json["ThinSurface"])
                material.PathTracerFlags |= shaderio::PathTracerThinSet | (json["ThinSurface"].as<bool>() ? shaderio::PathTracerThin : 0u);
            if (json["ExcludeFromNEE"] && json["ExcludeFromNEE"].as<bool>())
                material.PathTracerFlags |= shaderio::PathTracerExcludeFromNEE;
            // Which of a smooth pane's lobes DLSS-RR's guides follow (-1 the pane, 0 the transmission, 1 the reflection).
            const int dominantLobe = json["PSDDominantDeltaLobe"] ? json["PSDDominantDeltaLobe"].as<int>() : -1;
            if (dominantLobe < 0)
                material.PathTracerFlags |= shaderio::PathTracerPSDDominantSurface;
            else if (dominantLobe >= 1)
                material.PathTracerFlags |= shaderio::PathTracerPSDDominantReflection;
            if (json["EnableTransmission"])
                material.TransmissionFactor = json["EnableTransmission"].as<bool>() ? number("TransmissionFactor", 0.0f) : 0.0f;
            const glm::vec3 emissive = color("EmissiveColor", material.EmissiveFactor);
            const float emissiveIntensity = number("EmissiveIntensity", material.emissiveStrength);
            if (glm::dot(emissive, emissive) > 0.0f && emissiveIntensity > 0.0f)
            {
                material.EmissiveFactor = emissive;
                material.emissiveStrength = emissiveIntensity;
            }
            else if (json["EmissiveColor"])
            {
                material.EmissiveFactor = glm::vec3(0.0f);
            }
        }
        catch (const std::exception& exception)
        {
            NOX_CORE_WARN("RTXPT material override {} could not be read: {}", file.string(), exception.what());
        }
    }

    // A glTF material's name: what its .nmat is called (MeshImporter::MaterialAssetPath) and what combining groups by, so it tells
    // the file's materials apart. A material named like an earlier one gets its index (exporters reuse names: the three variants of
    // Khronos' MaterialsVariantsShoe are all "phong1SG"); an unnamed one is Material_<index>.
    static std::string GltfMaterialName(const tg3_model& model, int32_t materialIndex)
    {
        auto nameOf = [&](int32_t index)
        {
            const tg3_str& name = model.materials[index].name;
            return name.data ? std::string_view(name.data, name.len) : std::string_view();
        };
        const std::string_view name = nameOf(materialIndex);
        if (name.empty())
            return "Material_" + std::to_string(materialIndex);
        for (int32_t earlier = 0; earlier < materialIndex; ++earlier)
        {
            if (nameOf(earlier) == name)
                return std::string(name) + "_" + std::to_string(materialIndex);
        }
        return std::string(name);
    }

    // A glTF material as the engine's (textures extracted next to the cook); defaults when the index is out of range.
    static MaterialData ReadGltfMaterial(const tg3_model& model, int32_t materialIndex, const std::filesystem::path& path)
    {
        MaterialData materialData{};
        if (materialIndex >= 0 && materialIndex < (int32_t)model.materials_count)
        {
            const auto& gltfMaterial = model.materials[materialIndex];

            materialData.Name = GltfMaterialName(model, materialIndex);

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

            materialData.BaseColorFactor = glm::vec4(
                static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[0]),
                static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[1]),
                static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[2]),
                static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[3])
            );
            materialData.BaseColorTexturePath = GetTexturePath(gltfMaterial.pbr_metallic_roughness.base_color_texture.index);
            materialData.BaseColorTextureSet = gltfMaterial.pbr_metallic_roughness.base_color_texture.tex_coord;

            materialData.MetallicFactor = static_cast<float>(gltfMaterial.pbr_metallic_roughness.metallic_factor);
            materialData.RoughnessFactor = static_cast<float>(gltfMaterial.pbr_metallic_roughness.roughness_factor);

            // glTF packs roughness in G and metallic in B.
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

                if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_diffuse_transmission" &&
                    ext.value.type == TG3_VALUE_OBJECT)
                {
                    for (uint32_t j = 0; j < ext.value.object_count; j++)
                    {
                        const auto& kv = ext.value.object_data[j];
                        if (std::string_view(kv.key.data, kv.key.len) == "diffuseTransmissionFactor" &&
                            (kv.value.type == TG3_VALUE_REAL || kv.value.type == TG3_VALUE_INT))
                            materialData.DiffuseTransmission = std::clamp(getFloatVal(kv.value), 0.0f, 1.0f);
                    }
                }

                if (std::string_view(ext.name.data, ext.name.len) == "KHR_materials_volume")
                {
                    if (ext.value.type == TG3_VALUE_OBJECT)
                    {
                        for (uint32_t j = 0; j < ext.value.object_count; j++)
                        {
                            const auto& kv = ext.value.object_data[j];
                            const std::string_view key(kv.key.data, kv.key.len);
                            if (key == "thicknessFactor")
                            {
                                if (kv.value.type == TG3_VALUE_REAL)
                                    materialData.Thickness = static_cast<float>(kv.value.real_val);
                                else if (kv.value.type == TG3_VALUE_INT)
                                    materialData.Thickness = static_cast<float>(kv.value.int_val);
                            }
                            else if (key == "attenuationDistance" && (kv.value.type == TG3_VALUE_REAL || kv.value.type == TG3_VALUE_INT))
                            {
                                materialData.AttenuationDistance = getFloatVal(kv.value);
                            }
                            else if (key == "attenuationColor" && kv.value.type == TG3_VALUE_ARRAY && kv.value.array_count >= 3)
                            {
                                materialData.AttenuationColor = glm::vec3(getFloatVal(kv.value.array_data[0]), getFloatVal(kv.value.array_data[1]),
                                                                          getFloatVal(kv.value.array_data[2]));
                            }
                        }
                    }
                }
            }

            // The lobes past the core model (MaterialData): each extension's members, a member left out keeping the spec default.
            auto number = [](const tg3_value& object, std::string_view key, float& out)
            {
                const tg3_value* value = FindMember(object, key);
                if (value && (value->type == TG3_VALUE_REAL || value->type == TG3_VALUE_INT))
                    out = getFloatVal(*value);
            };
            auto color = [](const tg3_value& object, std::string_view key, glm::vec3& out)
            {
                const tg3_value* value = FindMember(object, key);
                if (value && value->type == TG3_VALUE_ARRAY && value->array_count >= 3)
                    out = glm::vec3(getFloatVal(value->array_data[0]), getFloatVal(value->array_data[1]), getFloatVal(value->array_data[2]));
            };
            auto texture = [&](const tg3_value& object, std::string_view key, MaterialTexture& out)
            {
                const tg3_value* info = FindMember(object, key);
                if (!info || info->type != TG3_VALUE_OBJECT)
                    return;
                const tg3_value* index = FindMember(*info, "index");
                const tg3_value* texCoord = FindMember(*info, "texCoord");
                if (index && index->type == TG3_VALUE_INT)
                    out.Path = GetTexturePath(static_cast<int32_t>(index->int_val));
                if (texCoord && texCoord->type == TG3_VALUE_INT)
                    out.Set = static_cast<int32_t>(texCoord->int_val);
            };
            if (const tg3_extension* clearcoat = FindExtension(gltfMaterial.ext, "KHR_materials_clearcoat"))
            {
                number(clearcoat->value, "clearcoatFactor", materialData.ClearcoatFactor);
                texture(clearcoat->value, "clearcoatTexture", materialData.ClearcoatTexture);
                number(clearcoat->value, "clearcoatRoughnessFactor", materialData.ClearcoatRoughnessFactor);
                texture(clearcoat->value, "clearcoatRoughnessTexture", materialData.ClearcoatRoughnessTexture);
                texture(clearcoat->value, "clearcoatNormalTexture", materialData.ClearcoatNormalTexture);
            }
            if (const tg3_extension* sheen = FindExtension(gltfMaterial.ext, "KHR_materials_sheen"))
            {
                color(sheen->value, "sheenColorFactor", materialData.SheenColorFactor);
                texture(sheen->value, "sheenColorTexture", materialData.SheenColorTexture);
                number(sheen->value, "sheenRoughnessFactor", materialData.SheenRoughnessFactor);
                texture(sheen->value, "sheenRoughnessTexture", materialData.SheenRoughnessTexture);
            }
            if (const tg3_extension* specular = FindExtension(gltfMaterial.ext, "KHR_materials_specular"))
            {
                number(specular->value, "specularFactor", materialData.SpecularStrength);
                texture(specular->value, "specularTexture", materialData.SpecularStrengthTexture);
                color(specular->value, "specularColorFactor", materialData.SpecularColorFactor);
                texture(specular->value, "specularColorTexture", materialData.SpecularColorTexture);
            }
            if (const tg3_extension* iridescence = FindExtension(gltfMaterial.ext, "KHR_materials_iridescence"))
            {
                number(iridescence->value, "iridescenceFactor", materialData.IridescenceFactor);
                texture(iridescence->value, "iridescenceTexture", materialData.IridescenceTexture);
                number(iridescence->value, "iridescenceIor", materialData.IridescenceIOR);
                number(iridescence->value, "iridescenceThicknessMinimum", materialData.IridescenceThicknessMinimum);
                number(iridescence->value, "iridescenceThicknessMaximum", materialData.IridescenceThicknessMaximum);
                texture(iridescence->value, "iridescenceThicknessTexture", materialData.IridescenceThicknessTexture);
            }
            if (const tg3_extension* anisotropy = FindExtension(gltfMaterial.ext, "KHR_materials_anisotropy"))
            {
                number(anisotropy->value, "anisotropyStrength", materialData.AnisotropyStrength);
                number(anisotropy->value, "anisotropyRotation", materialData.AnisotropyRotation);
                texture(anisotropy->value, "anisotropyTexture", materialData.AnisotropyTexture);
            }
            if (const tg3_extension* dispersion = FindExtension(gltfMaterial.ext, "KHR_materials_dispersion"))
                number(dispersion->value, "dispersion", materialData.Dispersion);

            // tiny_gltf_v3 exposes initialized metallic/roughness values but not whether the optional
            // pbrMetallicRoughness object existed. Its synthetic 1/1 defaults make a textureless
            // KHR_materials_transmission material fully metallic, which removes the entire transmission lobe.
            // Donut/RTXPT leaves such transmission-only materials at its engine defaults (0/0). Keep this
            // compatibility in Nox instead of modifying the vendored parser.
            if (materialData.TransmissionFactor > 0.0f && materialData.Workflow == 0.0f &&
                materialData.MetallicFactor == 1.0f && materialData.RoughnessFactor == 1.0f &&
                materialData.BaseColorTexturePath.empty() && materialData.MetallicRoughnessTexturePath.empty())
            {
                materialData.MetallicFactor = 0.0f;
                materialData.RoughnessFactor = 0.0f;
            }
            materialData.emissiveStrength = emissiveStrength;
            ApplyRtxptMaterialOverride(path, materialData);
        }
        return materialData;
    }

    // KHR_materials_variants: the file's variant names (the root extension's "variants"), in order.
    static std::vector<std::string> ReadMaterialVariantNames(const tg3_model& model)
    {
        std::vector<std::string> names;
        const tg3_extension* extension = FindExtension(model.ext, "KHR_materials_variants");
        const tg3_value* variants = extension ? FindMember(extension->value, "variants") : nullptr;
        if (!variants || variants->type != TG3_VALUE_ARRAY)
            return names;
        for (uint32_t i = 0; i < variants->array_count; i++)
        {
            const tg3_value* name = FindMember(variants->array_data[i], "name");
            names.push_back(name && name->type == TG3_VALUE_STRING && name->string_val.len > 0
                                ? std::string(name->string_val.data, name->string_val.len)
                                : "Variant " + std::to_string(i));
        }
        return names;
    }

    // A primitive's KHR_materials_variants mappings ({ "material", "variants": [indices] }) as the submesh's MaterialVariants, ordered
    // by variant. A variant named by several mappings takes the first (the extension allows one material per variant).
    static void ReadMaterialVariantMappings(const tg3_model& model, const tg3_primitive& primitive, const std::vector<std::string>& names,
                                            const std::filesystem::path& path, MeshData& out)
    {
        const tg3_extension* extension = FindExtension(primitive.ext, "KHR_materials_variants");
        const tg3_value* mappings = extension ? FindMember(extension->value, "mappings") : nullptr;
        if (!mappings || mappings->type != TG3_VALUE_ARRAY)
            return;
        auto integer = [](const tg3_value* value) -> int64_t
        {
            if (!value)
                return -1;
            return value->type == TG3_VALUE_INT ? value->int_val : value->type == TG3_VALUE_REAL ? static_cast<int64_t>(value->real_val) : -1;
        };
        for (uint32_t m = 0; m < mappings->array_count; m++)
        {
            const tg3_value& mapping = mappings->array_data[m];
            const int64_t material = integer(FindMember(mapping, "material"));
            const tg3_value* variants = FindMember(mapping, "variants");
            if (material < 0 || material >= static_cast<int64_t>(model.materials_count) || !variants || variants->type != TG3_VALUE_ARRAY)
                continue;
            const MaterialData data = ReadGltfMaterial(model, static_cast<int32_t>(material), path);
            for (uint32_t v = 0; v < variants->array_count; v++)
            {
                const int64_t variant = integer(&variants->array_data[v]);
                if (variant < 0 || variant >= static_cast<int64_t>(names.size()))
                    continue;
                const bool taken = std::any_of(out.MaterialVariants.begin(), out.MaterialVariants.end(),
                                               [&](const MaterialVariantMapping& existing) { return existing.VariantIndex == variant; });
                if (!taken)
                    out.MaterialVariants.push_back({ names[variant], static_cast<uint32_t>(variant), data });
            }
        }
        std::sort(out.MaterialVariants.begin(), out.MaterialVariants.end(),
                  [](const MaterialVariantMapping& a, const MaterialVariantMapping& b) { return a.VariantIndex < b.VariantIndex; });
    }

    // A glTF JSON value as compact JSON text (extras keep what the file wrote).
    static void AppendJson(const tg3_value& value, std::string& out)
    {
        switch (value.type)
        {
        case TG3_VALUE_BOOL: out += value.bool_val ? "true" : "false"; break;
        case TG3_VALUE_INT: out += std::to_string(value.int_val); break;
        case TG3_VALUE_REAL:
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.9g", value.real_val);
            out += text;
            break;
        }
        case TG3_VALUE_STRING:
        {
            out += '"';
            for (uint32_t i = 0; i < value.string_val.len; ++i)
            {
                const char c = value.string_val.data[i];
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (c == '\n') out += "\\n";
                else if (c == '\t') out += "\\t";
                else if (static_cast<unsigned char>(c) < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", c); out += code; }
                else out += c;
            }
            out += '"';
            break;
        }
        case TG3_VALUE_ARRAY:
            out += '[';
            for (uint32_t i = 0; i < value.array_count; ++i)
            {
                if (i) out += ',';
                AppendJson(value.array_data[i], out);
            }
            out += ']';
            break;
        case TG3_VALUE_OBJECT:
            out += '{';
            for (uint32_t i = 0; i < value.object_count; ++i)
            {
                if (i) out += ',';
                AppendJson(tg3_value{ .type = TG3_VALUE_STRING, .string_val = value.object_data[i].key }, out);
                out += ':';
                AppendJson(value.object_data[i].value, out);
            }
            out += '}';
            break;
        default: out += "null"; break;
        }
    }

    // A node's glTF extras as key / value text (Godot keeps them as the node's metadata, Unreal as user-defined attributes): an object's
    // members one each, a string as it is, anything else as its JSON. Extras that are not an object are one entry named "extras".
    static std::vector<std::pair<std::string, std::string>> FlattenExtras(const tg3_value* extras)
    {
        std::vector<std::pair<std::string, std::string>> result;
        if (!extras || extras->type == TG3_VALUE_NULL)
            return result;
        auto text = [](const tg3_value& value)
        {
            if (value.type == TG3_VALUE_STRING)
                return value.string_val.data ? std::string(value.string_val.data, value.string_val.len) : std::string();
            std::string json;
            AppendJson(value, json);
            return json;
        };
        if (extras->type != TG3_VALUE_OBJECT)
        {
            result.emplace_back("extras", text(*extras));
            return result;
        }
        for (uint32_t i = 0; i < extras->object_count; ++i)
        {
            const tg3_kv_pair& member = extras->object_data[i];
            result.emplace_back(std::string(member.key.data ? member.key.data : "", member.key.len), text(member.value));
        }
        return result;
    }

    // KHR_physics_rigid_bodies on the file's nodes (shapes from KHR_implicit_shapes) as NodePhysics: bodies, every shape (box,
    // sphere, capsule and cylinder, tapered too, plane, mesh and convex hull), materials, collision filters and joints. What Nox has
    // no equivalent for (initial velocities, a set center of mass or inertia, compound triggers) is named once in the log and left out. Colliders and triggers with no body above
    // them get a static body of their own (the spec's static colliders).
    static void ReadNodePhysics(const tg3_model& model, std::vector<MeshNodeData>& nodes)
    {
        const uint32_t fileNodes = std::min<uint32_t>(model.nodes_count, static_cast<uint32_t>(nodes.size()));
        const tg3_extension* shapesExtension = FindExtension(model.ext, "KHR_implicit_shapes");
        const tg3_value* shapes = shapesExtension ? FindMember(shapesExtension->value, "shapes") : nullptr;
        const tg3_extension* bodiesExtension = FindExtension(model.ext, "KHR_physics_rigid_bodies");
        const tg3_value* materials = bodiesExtension ? FindMember(bodiesExtension->value, "physicsMaterials") : nullptr;
        const tg3_value* filters = bodiesExtension ? FindMember(bodiesExtension->value, "collisionFilters") : nullptr;
        const tg3_value* joints = bodiesExtension ? FindMember(bodiesExtension->value, "physicsJoints") : nullptr;

        auto number = [](const tg3_value* value, double fallback)
        {
            if (!value)
                return fallback;
            return value->type == TG3_VALUE_REAL ? value->real_val : value->type == TG3_VALUE_INT ? static_cast<double>(value->int_val) : fallback;
        };
        auto index = [&](const tg3_value& object, std::string_view key) { return static_cast<int64_t>(number(FindMember(object, key), -1.0)); };
        auto element = [](const tg3_value* array, int64_t at) -> const tg3_value*
        {
            return array && array->type == TG3_VALUE_ARRAY && at >= 0 && at < static_cast<int64_t>(array->array_count) ? &array->array_data[at] : nullptr;
        };

        std::set<std::string> leftOut;
        // The collider's shape into `physics`; false (and named in leftOut) when Nox has no component for it.
        auto readGeometry = [&](const tg3_value& geometry, NodePhysics& physics) -> bool
        {
            if (FindMember(geometry, "mesh"))
            {
                // The mesh's triangles, or their convex hull (MeshCollider3DComponent); its submeshes are found when the asset is cooked.
                const tg3_value* hull = FindMember(geometry, "convexHull");
                physics.Shape = NodePhysics::ShapeType::Mesh;
                physics.ColliderMesh = static_cast<int32_t>(index(geometry, "mesh"));
                physics.Convex = hull && hull->type == TG3_VALUE_BOOL && hull->bool_val;
                return physics.ColliderMesh >= 0;
            }
            const tg3_value* shape = element(shapes, index(geometry, "shape"));
            const tg3_value* type = shape ? FindMember(*shape, "type") : nullptr;
            const std::string_view kind = type && type->type == TG3_VALUE_STRING ? std::string_view(type->string_val.data, type->string_val.len) : "";
            const tg3_value* parameters = shape && !kind.empty() ? FindMember(*shape, kind) : nullptr;
            if (!parameters)
            {
                leftOut.insert("colliders without a shape");
                return false;
            }
            if (kind == "box")
            {
                const tg3_value* size = FindMember(*parameters, "size");
                physics.Shape = NodePhysics::ShapeType::Box;
                for (int axis = 0; axis < 3; ++axis)
                    physics.Size[axis] = 0.5f * static_cast<float>(number(element(size, axis), 1.0));
                return true;
            }
            if (kind == "sphere")
            {
                physics.Shape = NodePhysics::ShapeType::Sphere;
                physics.Size = glm::vec3(static_cast<float>(number(FindMember(*parameters, "radius"), 0.5)), 0.0f, 0.0f);
                return true;
            }
            if (kind == "capsule")
            {
                // height: between the centers of the two capping spheres (Jolt's and Nox's half height is half of it).
                const float top = static_cast<float>(number(FindMember(*parameters, "radiusTop"), 0.25));
                const float bottom = static_cast<float>(number(FindMember(*parameters, "radiusBottom"), 0.25));
                physics.Shape = NodePhysics::ShapeType::Capsule;
                physics.Size = glm::vec3(top, 0.5f * static_cast<float>(number(FindMember(*parameters, "height"), 0.5)),
                                         std::abs(top - bottom) > 1e-5f ? bottom : 0.0f); // tapered: the bottom radius in z
                return true;
            }
            if (kind == "cylinder")
            {
                const float top = static_cast<float>(number(FindMember(*parameters, "radiusTop"), 0.25));
                const float bottom = static_cast<float>(number(FindMember(*parameters, "radiusBottom"), 0.25));
                physics.Shape = NodePhysics::ShapeType::Cylinder;
                physics.Size = glm::vec3(top, 0.5f * static_cast<float>(number(FindMember(*parameters, "height"), 0.5)),
                                         std::abs(top - bottom) > 1e-5f ? std::max(bottom, 1e-6f) : 0.0f); // a cone: bottom radius in z
                return true;
            }
            if (kind == "plane")
            {
                // Unbounded by default; sizeX / sizeZ are total sizes. (One-sided or double-sided: Jolt's plane is a half-space.)
                physics.Shape = NodePhysics::ShapeType::Plane;
                physics.Size = glm::vec3(0.5f * static_cast<float>(number(FindMember(*parameters, "sizeX"), 0.0)), 0.0f,
                                         0.5f * static_cast<float>(number(FindMember(*parameters, "sizeZ"), 0.0)));
                return true;
            }
            leftOut.insert(std::string(kind) + " colliders");
            return false;
        };

        for (uint32_t i = 0; i < fileNodes; ++i)
        {
            const tg3_extension* extension = FindExtension(model.nodes[i].ext, "KHR_physics_rigid_bodies");
            if (!extension || nodes[i].Parent == MeshNodeData::OutsideScene)
                continue;
            NodePhysics& physics = nodes[i].Physics;
            if (const tg3_value* motion = FindMember(extension->value, "motion"))
            {
                const tg3_value* kinematic = FindMember(*motion, "isKinematic");
                const double mass = number(FindMember(*motion, "mass"), -1.0);
                // A mass of 0 is the spec's infinite mass: nothing moves it, as a kinematic body.
                physics.Body = (kinematic && kinematic->type == TG3_VALUE_BOOL && kinematic->bool_val) || mass == 0.0
                    ? NodePhysics::BodyType::Kinematic : NodePhysics::BodyType::Dynamic;
                physics.Mass = mass > 0.0 ? static_cast<float>(mass) : 0.0f;
                physics.GravityFactor = static_cast<float>(number(FindMember(*motion, "gravityFactor"), 1.0));
                for (const char* unsupported : { "linearVelocity", "angularVelocity", "centerOfMass", "inertiaDiagonal", "inertiaOrientation" })
                    if (FindMember(*motion, unsupported))
                        leftOut.insert(std::string("motion.") + unsupported);
            }
            const tg3_value* collider = FindMember(extension->value, "collider");
            const tg3_value* trigger = FindMember(extension->value, "trigger");
            const tg3_value* geometry = collider ? FindMember(*collider, "geometry") : trigger ? FindMember(*trigger, "geometry") : nullptr;
            if (geometry && readGeometry(*geometry, physics))
            {
                physics.Trigger = !collider && trigger;
                if (const tg3_value* material = collider ? element(materials, index(*collider, "physicsMaterial")) : nullptr)
                {
                    physics.Friction = static_cast<float>(number(FindMember(*material, "dynamicFriction"), number(FindMember(*material, "staticFriction"), 0.5)));
                    physics.Restitution = static_cast<float>(number(FindMember(*material, "restitution"), 0.0));
                }
            }
            else if (trigger && FindMember(*trigger, "nodes"))
            {
                leftOut.insert("compound triggers");
            }

            // The collision filter (top-level collisionFilters).
            const tg3_value* owner = collider ? collider : trigger;
            if (const tg3_value* filter = owner ? element(filters, index(*owner, "collisionFilter")) : nullptr)
            {
                auto names = [&](std::string_view key)
                {
                    std::vector<std::string> list;
                    if (const tg3_value* array = FindMember(*filter, key); array && array->type == TG3_VALUE_ARRAY)
                        for (uint32_t k = 0; k < array->array_count; ++k)
                            if (array->array_data[k].type == TG3_VALUE_STRING)
                                list.emplace_back(array->array_data[k].string_val.data, array->array_data[k].string_val.len);
                    return list;
                };
                physics.Systems = names("collisionSystems");
                physics.CollideWith = names("collideWithSystems");
                physics.NotCollideWith = names("notCollideWithSystems");
            }

            // The joint: its limits and drives (top-level physicsJoints) on the six axes of this node's frame.
            if (const tg3_value* joint = FindMember(extension->value, "joint"))
            {
                const int64_t connected = index(*joint, "connectedNode");
                const tg3_value* description = element(joints, index(*joint, "joint"));
                if (connected >= 0 && connected < static_cast<int64_t>(fileNodes) && description)
                {
                    NodePhysics::Joint& data = physics.JointData;
                    data.ConnectedNode = static_cast<int32_t>(connected);
                    const tg3_value* collide = FindMember(*joint, "enableCollision");
                    data.EnableCollision = collide && collide->type == TG3_VALUE_BOOL && collide->bool_val;
                    if (const tg3_value* limits = FindMember(*description, "limits"); limits && limits->type == TG3_VALUE_ARRAY)
                    {
                        for (uint32_t l = 0; l < limits->array_count; ++l)
                        {
                            const tg3_value& limit = limits->array_data[l];
                            const double low = number(FindMember(limit, "min"), -std::numeric_limits<double>::infinity());
                            const double high = number(FindMember(limit, "max"), std::numeric_limits<double>::infinity());
                            const double stiffness = number(FindMember(limit, "stiffness"), 0.0);
                            const double damping = number(FindMember(limit, "damping"), 0.0);
                            const tg3_value* linear = FindMember(limit, "linearAxes");
                            const tg3_value* angular = FindMember(limit, "angularAxes");
                            if ((linear && linear->array_count > 1) || (angular && angular->array_count > 1))
                                leftOut.insert("joint limits on several axes at once (limited per axis)");
                            for (int kind = 0; kind < 2; ++kind)
                            {
                                const tg3_value* axes = kind == 0 ? linear : angular;
                                for (uint32_t k = 0; axes && axes->type == TG3_VALUE_ARRAY && k < axes->array_count; ++k)
                                {
                                    const int64_t axis = static_cast<int64_t>(number(&axes->array_data[k], -1.0));
                                    if (axis < 0 || axis > 2)
                                        continue;
                                    const int slot = kind * 3 + static_cast<int>(axis);
                                    const bool bounded = std::isfinite(low) || std::isfinite(high);
                                    data.Axes[slot] = !bounded ? 0u : low >= high ? 1u : 2u; // Free, Locked, Limited
                                    data.Min[slot] = std::isfinite(low) ? static_cast<float>(low) : -1e6f;
                                    data.Max[slot] = std::isfinite(high) ? static_cast<float>(high) : 1e6f;
                                    if (kind == 0)
                                    {
                                        data.LimitStiffness[axis] = static_cast<float>(stiffness);
                                        data.LimitDamping[axis] = static_cast<float>(damping);
                                    }
                                    else if (stiffness > 0.0)
                                    {
                                        leftOut.insert("soft angular joint limits (made hard)");
                                    }
                                }
                            }
                        }
                    }
                    if (const tg3_value* drives = FindMember(*description, "drives"); drives && drives->type == TG3_VALUE_ARRAY)
                    {
                        for (uint32_t d = 0; d < drives->array_count; ++d)
                        {
                            const tg3_value& drive = drives->array_data[d];
                            const tg3_value* type = FindMember(drive, "type");
                            const bool angular = type && type->type == TG3_VALUE_STRING && std::string_view(type->string_val.data, type->string_val.len) == "angular";
                            const int64_t axis = static_cast<int64_t>(number(FindMember(drive, "axis"), -1.0));
                            if (axis < 0 || axis > 2)
                                continue;
                            if (const tg3_value* mode = FindMember(drive, "mode"); mode && mode->type == TG3_VALUE_STRING &&
                                std::string_view(mode->string_val.data, mode->string_val.len) == "acceleration")
                                leftOut.insert("acceleration-mode joint drives (driven as force)");
                            NodePhysics::Joint::Drive& target = data.Drives[(angular ? 3 : 0) + axis];
                            target.Enabled = true;
                            target.PositionTarget = static_cast<float>(number(FindMember(drive, "positionTarget"), 0.0));
                            target.VelocityTarget = static_cast<float>(number(FindMember(drive, "velocityTarget"), 0.0));
                            target.Stiffness = static_cast<float>(number(FindMember(drive, "stiffness"), 0.0));
                            target.Damping = static_cast<float>(number(FindMember(drive, "damping"), 0.0));
                            target.MaxForce = static_cast<float>(number(FindMember(drive, "maxForce"), 0.0));
                        }
                    }
                }
                else
                {
                    leftOut.insert("joints without a connected node or description");
                }
            }
        }

        // Only motions are bodies so far. A collider under one is part of it (the compound JoltPhysics3DScene builds); any other
        // collider is a static body of its own, and so is a trigger (a sensor): one under a moving body would have to move with it,
        // and Nox's sensors are whole bodies. Decided from the motions alone, so the order of the nodes does not matter.
        std::vector<uint8_t> underMotion(fileNodes, 0);
        for (uint32_t i = 0; i < fileNodes; ++i)
        {
            for (int32_t parent = nodes[i].Parent, depth = 0; parent >= 0 && parent < static_cast<int32_t>(fileNodes) && depth < 256;
                 parent = nodes[parent].Parent, ++depth)
                underMotion[i] |= nodes[parent].Physics.Body != NodePhysics::BodyType::None ? 1 : 0;
        }
        for (uint32_t i = 0; i < fileNodes; ++i)
        {
            NodePhysics& physics = nodes[i].Physics;
            if (physics.Shape == NodePhysics::ShapeType::None || physics.Body != NodePhysics::BodyType::None)
                continue;
            if (physics.Trigger && underMotion[i])
            {
                leftOut.insert("triggers on moving bodies");
                physics.Shape = NodePhysics::ShapeType::None;
                continue;
            }
            if (!underMotion[i])
                physics.Body = NodePhysics::BodyType::Static;
        }

        if (!leftOut.empty())
        {
            std::string list;
            for (const std::string& item : leftOut)
                list += (list.empty() ? "" : ", ") + item;
            NOX_CORE_WARN("[Importer] glTF physics: no Nox equivalent, left out: {}", list);
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
        std::vector<CameraNodeData>& outCameras,
        std::vector<PendingGeometry>* outPending
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
        const std::vector<std::string> variantNames = ReadMaterialVariantNames(model);

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

            const std::vector<int32_t> jointAncestors = JointAncestors(model);
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
                    nodeData.Skinned = node.skin >= 0;
                    nodeData.SkinIndex = node.skin;
                    if (node.skin < 0 && node.mesh >= 0 && jointAncestors[i] >= 0)
                    {
                        nodeData.Skinned = true; // a rigid part of the character
                        nodeData.RigidJoint = jointAncestors[i];
                    }
                    ExtractNodeTRS(node, nodeData.Translation, nodeData.Rotation, nodeData.Scale);
                    nodeData.Extras = FlattenExtras(node.ext.extras);
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
            // Only the imported scene's nodes: the others keep their place in the table (the file's indices) and nothing else.
            const std::vector<uint8_t> inScene = NodesInScene(model);
            for (uint32_t i = 0; i < model.nodes_count; i++)
            {
                if (!inScene[i])
                    outNodes[i].Parent = MeshNodeData::OutsideScene;
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
                    if (!inScene[instanceIdx])
                        continue; // another scene's: its meshes are not imported

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
                nodeData.MeshIndex = static_cast<int32_t>(meshIndex);
                nodeData.MeshName = (mesh.name.data && mesh.name.len > 0) ? std::string(mesh.name.data, mesh.name.len)
                                                                          : (meshName.empty() ? "Mesh_" + std::to_string(meshIndex) : meshName);
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

                // Authored tangent (xyz + handedness w): read directly when the file provides one. When it doesn't,
                // GenerateMissingTangents fills every vertex.tangent in below, after the vertex/index lists are complete.
                bool hasTangents = FindAttribute(primitive, "TANGENT") != -1;
                const tg3_accessor* tangentAccessor = nullptr;
                const tg3_buffer_view* tangentBufferView = nullptr;
                const tg3_buffer* tangentBuffer = nullptr;

                if (hasTangents)
                {
                    tangentAccessor = &model.accessors[FindAttribute(primitive, "TANGENT")];
                    tangentBufferView = &model.buffer_views[tangentAccessor->buffer_view];
                    tangentBuffer = &model.buffers[tangentBufferView->buffer];
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

                // Vertex colors (COLOR_0: float, or normalized unsigned byte / short; RGB or RGBA), multiplying the base color.
                std::vector<float> colors;
                uint32_t colorComponents = 0;
                if (const int32_t colorAttribute = FindAttribute(primitive, "COLOR_0"); colorAttribute != -1)
                {
                    colorComponents = model.accessors[colorAttribute].type == TG3_TYPE_VEC4 ? 4u : 3u;
                    colors = ReadAccessorFloats(model, colorAttribute, colorComponents);
                    if (colors.size() < primitiveVertexCount * colorComponents)
                    {
                        NOX_CORE_WARN("MeshImporter: COLOR_0 of {} could not be read, ignored", primitiveData.Name);
                        colors.clear();
                    }
                }
                primitiveData.VertexColors = !colors.empty();

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

                    if (hasTangents)
                    {
                        uint32_t tangentStride = tangentBufferView->byte_stride ? tangentBufferView->byte_stride : 16;
                        const float* tan = reinterpret_cast<const float*>(&tangentBuffer->data.data[tangentBufferView->byte_offset + tangentAccessor->byte_offset + (i * tangentStride)]);
                        vertex.tangent = {tan[0], tan[1], tan[2], tan[3]};
                    }
                    else
                    {
                        vertex.tangent = {0.0f, 0.0f, 0.0f, 0.0f}; // sentinel: GenerateMissingTangents fills this in below
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

                    if (!colors.empty())
                    {
                        const float* color = colors.data() + i * colorComponents;
                        vertex.color = glm::vec4(color[0], color[1], color[2], colorComponents == 4 ? color[3] : 1.0f);
                    }

                    primitiveData.Vertices.push_back(vertex);
                }

                // Morph targets (blend shapes): each target's POSITION and NORMAL deltas (its TANGENT deltas are left out: the tangent
                // follows the base shape). Their names from the mesh's extras.targetNames (Blender's shape key names), their default
                // weights from the mesh's weights.
                for (uint32_t target = 0; target < primitive.targets_count; ++target)
                {
                    MorphTarget morph;
                    morph.Name = MorphTargetName(mesh, target);
                    morph.DefaultWeight = target < mesh.weights_count ? static_cast<float>(mesh.weights[target]) : 0.0f;
                    morph.Positions.assign(primitiveVertexCount, glm::vec3(0.0f));
                    morph.Normals.assign(primitiveVertexCount, glm::vec3(0.0f));
                    for (uint32_t a = 0; a < primitive.target_attribute_counts[target]; ++a)
                    {
                        const tg3_str_int_pair& attribute = primitive.targets[target][a];
                        const std::string_view name(attribute.key.data, attribute.key.len);
                        std::vector<glm::vec3>* deltas = name == "POSITION" ? &morph.Positions : name == "NORMAL" ? &morph.Normals : nullptr;
                        if (!deltas)
                            continue;
                        const std::vector<float> values = ReadAccessorFloats(model, attribute.value, 3);
                        if (values.size() < primitiveVertexCount * 3)
                        {
                            NOX_CORE_WARN("MeshImporter: morph target {} of {} could not be read ({}), ignored", morph.Name, primitiveData.Name, name);
                            continue;
                        }
                        for (size_t i = 0; i < primitiveVertexCount; ++i)
                            (*deltas)[i] = glm::vec3(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
                    }
                    primitiveData.Morphs.push_back(std::move(morph));
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

                if (!hasTangents && hasTexCoords)
                {
                    std::vector<uint32_t> origins;
                    GenerateMissingTangents(primitiveData.Vertices, primitiveIndices, &origins);
                    for (MorphTarget& morph : primitiveData.Morphs)
                    {
                        for (uint32_t origin : origins)
                        {
                            morph.Positions.push_back(morph.Positions[origin]);
                            morph.Normals.push_back(morph.Normals[origin]);
                        }
                    }
                }

                // Clusters (§5.7): static geometry gets the cluster LOD DAG; skinned geometry one level (simplified in the bind
                // pose, it would deform wrongly).
                if (outPending)
                    outPending->push_back({ std::move(primitiveIndices), hasSkinning });
                else if (hasSkinning || !primitiveData.Morphs.empty())
                    buildMeshlets(primitiveData, primitiveIndices);
                else
                    buildClusterLod(primitiveData, primitiveIndices);

                const MaterialData materialData = ReadGltfMaterial(model, primitive.material, path);
                outMaterials.push_back(materialData);
                if (!variantNames.empty())
                    ReadMaterialVariantMappings(model, primitive, variantNames, path, primitiveData);
                result.push_back(std::move(primitiveData));
            }

            nodeData.SubmeshCount = static_cast<uint32_t>(result.size()) - nodeData.FirstSubmesh;
            sharedRange = { nodeData.FirstSubmesh, nodeData.SubmeshCount };
        }

        if (model.nodes_count > 0)
        {
            ApplyNodeExtensions(model, outNodes);
            ReadNodePhysics(model, outNodes);
            // Lights and cameras on another scene's nodes are not imported either.
            auto outside = [&](int32_t node)
            {
                return node >= 0 && node < static_cast<int32_t>(outNodes.size()) && outNodes[node].Parent == MeshNodeData::OutsideScene;
            };
            std::erase_if(outLights, [&](const LightNodeData& light) { return outside(light.NodeIndex); });
            std::erase_if(outCameras, [&](const CameraNodeData& camera) { return outside(camera.NodeIndex); });
        }

        tg3_model_free(&model);
        tg3_error_stack_free(&errors);

        return result;
    }

    void MeshImporter::BuildRuntimeGeometry(MeshData& data, std::vector<uint32_t> indices, bool generateTangents)
    {
        data.Bounds.clear();
        data.Draws.clear();
        data.MeshletVertices.clear();
        data.MeshletTriangles.clear();
        if (data.Vertices.empty() || indices.empty())
            return;
        if (generateTangents)
            GenerateMissingTangents(data.Vertices, indices);
        buildMeshlets(data, indices);
    }

    void MeshImporter::BuildCookedGeometry(MeshData& data, std::vector<uint32_t> indices, bool generateTangents)
    {
        data.Bounds.clear();
        data.Draws.clear();
        data.MeshletVertices.clear();
        data.MeshletTriangles.clear();
        if (data.Vertices.empty() || indices.empty())
            return;
        if (generateTangents)
            GenerateMissingTangents(data.Vertices, indices);
        buildClusterLod(data, indices);
    }
}
