#include "MeshImporter.h"

#define TINYGLTF3_IMPLEMENTATION
#define TINYGLTF3_ENABLE_FS 
#include "tiny_gltf_v3.h"

#include "meshoptimizer.h"
#include "NoxCore/Core/Log.h"

#include "NoxCore/Project/Project.h"

namespace Nox
{
    class MeshSerializer
    {
    private:
        // Helper to write raw vector data safely
        template<typename T>
        static void WriteVector(std::ofstream& stream, const std::vector<T>& vec)
        {
            uint32_t size = static_cast<uint32_t>(vec.size());
            stream.write(reinterpret_cast<const char*>(&size), sizeof(uint32_t));
            if (size > 0)
                stream.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
        }

        // Helper to read raw vector data safely
        template<typename T>
        static void ReadVector(std::ifstream& stream, std::vector<T>& vec)
        {
            uint32_t size = 0;
            stream.read(reinterpret_cast<char*>(&size), sizeof(uint32_t));
            vec.resize(size);
            if (size > 0)
                stream.read(reinterpret_cast<char*>(vec.data()), size * sizeof(T));
        }

        // Writes a single MeshData struct
        static void WriteMeshData(std::ofstream& stream, const MeshData& data)
        {
            WriteVector(stream, data.Vertices);
            WriteVector(stream, data.Bounds);
            WriteVector(stream, data.MeshletVertices);
            WriteVector(stream, data.MeshletTriangles);
            WriteVector(stream, data.Draws);
        }

        // Reads a single MeshData struct
        static void ReadMeshData(std::ifstream& stream, MeshData& outData)
        {
            ReadVector(stream, outData.Vertices);
            ReadVector(stream, outData.Bounds);
            ReadVector(stream, outData.MeshletVertices);
            ReadVector(stream, outData.MeshletTriangles);
            ReadVector(stream, outData.Draws);
        }

    public:
        // ==========================================
        // STATIC MESH (.nsmesh) - Single flattened MeshData
        // ==========================================
        static void SerializeStaticMesh(const std::filesystem::path& filepath, const MeshData& data)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            const char magic[5] = "NSMS"; // Header for Static Mesh
            stream.write(magic, 4);
            
            WriteMeshData(stream, data);
        }

        static bool DeserializeStaticMesh(const std::filesystem::path& filepath, MeshData& outData)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;
            
            char magic[5] = {0};
            stream.read(magic, 4);
            if (strcmp(magic, "NSMS") != 0) return false;
            
            ReadMeshData(stream, outData);
            return true;
        }

        // ==========================================
        // DYNAMIC MESH (.nmesh) - Vector of MeshData
        // ==========================================
        static void SerializeMesh(const std::filesystem::path& filepath, const std::vector<MeshData>& dataList)
        {
            std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
            const char magic[5] = "NMSH"; // Header for regular Mesh
            stream.write(magic, 4);
            
            uint32_t count = static_cast<uint32_t>(dataList.size());
            stream.write(reinterpret_cast<const char*>(&count), sizeof(uint32_t));
            
            for (const auto& data : dataList)
            {
                WriteMeshData(stream, data);
            }
        }

        static bool DeserializeMesh(const std::filesystem::path& filepath, std::vector<MeshData>& outDataList)
        {
            std::ifstream stream(filepath, std::ios::binary);
            if (!stream.is_open()) return false;
            
            char magic[5] = {0};
            stream.read(magic, 4);
            if (strcmp(magic, "NMSH") != 0) return false;
            
            uint32_t count = 0;
            stream.read(reinterpret_cast<char*>(&count), sizeof(uint32_t));
            outDataList.resize(count);
            
            for (uint32_t i = 0; i < count; i++)
            {
                ReadMeshData(stream, outDataList[i]);
            }
            return true;
        }
    };

    struct MeshData;

    Ref<Mesh> MeshImporter::ImportMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
        std::filesystem::path sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
        
        NOX_CORE_INFO("MeshImporter::ImportMesh loading mesh from {}", cookedPath.string());
        
        std::vector<MeshData> meshDataList;
        
        if (std::filesystem::exists(cookedPath))
        {
            NOX_CORE_INFO("Loading cooked dynamic mesh from {}", cookedPath.string());
            bool success = MeshSerializer::DeserializeMesh(cookedPath, meshDataList);
            if (!success) NOX_CORE_ASSERT(false, "MeshImporter::ImportMesh - Failed to deserialize .nmesh file: {}", cookedPath.string());
        }
        
        else
        {
            // It doesn't exist, cook it from the source!
            NOX_CORE_INFO("Cooking GLTF from {} to {}", sourcePath.string(), cookedPath.string());
            
            meshDataList = ParseGltfToMeshData(sourcePath);
            if (meshDataList.empty())
            {
                NOX_CORE_ASSERT(false, "MeshImporter::ImportMesh - Failed to load or empty mesh at source path: {}", sourcePath.string());
                return Ref<Mesh>(nullptr);
            }
            
            // Ensure the directory for the cooked path actually exists before saving!
            if (!std::filesystem::exists(cookedPath.parent_path()))
                std::filesystem::create_directories(cookedPath.parent_path());
            
            MeshSerializer::SerializeMesh(cookedPath, meshDataList);
        }
        
        Ref<Mesh> meshAsset = CreateRef<Mesh>();
        
        // Upload each sub-mesh independently -> Vector of Handles
        for (const auto& data : meshDataList)
        {
            MeshHandle subMeshHandle = Renderer::UploadMeshGeometry(data);
            meshAsset->m_SubMeshes.push_back(subMeshHandle);
        }
        
        Renderer::UpdateMeshletBuffers();
        
        return meshAsset;
    }

    Ref<StaticMesh> MeshImporter::ImportStaticMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
        std::filesystem::path sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;
        
        NOX_CORE_INFO("MeshImporter::ImportStaticMesh loading static mesh from {}", cookedPath.string());
        
        MeshData combinedData;
        
        if (std::filesystem::exists(cookedPath))
        {
            NOX_CORE_INFO("Loading cooked static mesh from {}", cookedPath.string());
            bool success = MeshSerializer::DeserializeStaticMesh(cookedPath, combinedData);
            if (!success) 
            {
                NOX_CORE_ASSERT(false, "MeshImporter::ImportStaticMesh - Failed to deserialize .nsmesh file: {}", cookedPath.string());
            }
        }
        else
        {
            NOX_CORE_INFO("Cooking GLTF from {} to {}", sourcePath.string(), cookedPath.string());
            
            std::vector<MeshData> meshDataList = ParseGltfToMeshData(sourcePath);
            if (meshDataList.empty()) 
            {
                NOX_CORE_ASSERT(false, "No Meshes found in source file: {}", sourcePath.string());
                return Ref<StaticMesh>(nullptr);
            }
            
            for (const auto& data : meshDataList)
            {
                uint32_t baseVertex = static_cast<uint32_t>(combinedData.Vertices.size());
                uint32_t meshletVertOffset = static_cast<uint32_t>(combinedData.MeshletVertices.size());
                uint32_t meshletTriOffset = static_cast<uint32_t>(combinedData.MeshletTriangles.size());

                combinedData.Vertices.insert(combinedData.Vertices.end(), data.Vertices.begin(), data.Vertices.end());
                combinedData.Bounds.insert(combinedData.Bounds.end(), data.Bounds.begin(), data.Bounds.end());
                combinedData.MeshletVertices.insert(combinedData.MeshletVertices.end(), data.MeshletVertices.begin(), data.MeshletVertices.end());
                combinedData.MeshletTriangles.insert(combinedData.MeshletTriangles.end(), data.MeshletTriangles.begin(), data.MeshletTriangles.end());

                for (auto draw : data.Draws)
                {
                    // Fix: globalVertexOffset should point to the absolute base vertex of this chunk in the combined buffer
                    draw.globalVertexOffset = baseVertex; 
                    draw.vertexOffset += meshletVertOffset;
                    draw.triangleOffset += meshletTriOffset;
                    combinedData.Draws.push_back(draw);
                }
            }
            
            if (!std::filesystem::exists(cookedPath.parent_path()))
                std::filesystem::create_directories(cookedPath.parent_path());
            
            MeshSerializer::SerializeStaticMesh(cookedPath, combinedData);
        }
        
        // Upload combined flattened geometry -> Single Handle
        MeshHandle singleHandle = Renderer::UploadMeshGeometry(combinedData);
        
        Renderer::UpdateMeshletBuffers();
        
        return CreateRef<StaticMesh>(singleHandle);
    }

    Ref<Mesh> MeshImporter::LoadMesh(const std::filesystem::path& path)
    {
        NOX_CORE_INFO("MeshImporter::LoadMesh loading raw mesh from {}", path.string());
        
        std::filesystem::path sourcePath = path;
        std::filesystem::path cookedPath = sourcePath;
        cookedPath.replace_extension(".nmesh");
        
        std::vector<MeshData> meshDataList;
        
        if (std::filesystem::exists(cookedPath))
        {
            bool success = MeshSerializer::DeserializeMesh(path, meshDataList);
            if (!success) NOX_CORE_ASSERT(false, "MeshImporter::LoadMesh - Failed to deserialize .nmesh file");
        }
        else
        {
            meshDataList = ParseGltfToMeshData(path);
            if (meshDataList.empty())
            {
                NOX_CORE_ASSERT("MeshImporter::LoadMesh - Failed to load or empty mesh at path: {}", path.string());
                return Ref<Mesh>(nullptr);
            }
            
            MeshSerializer::SerializeMesh(cookedPath, meshDataList);
        }
        
        Ref<Mesh> meshAsset = CreateRef<Mesh>();
        
        // Upload each sub-mesh independently -> Vector of Handles
        for (const auto& data : meshDataList)
        {
            MeshHandle subMeshHandle = Renderer::UploadMeshGeometry(data);
            meshAsset->m_SubMeshes.push_back(subMeshHandle);
        }
        
        Renderer::UpdateMeshletBuffers();
        
        return meshAsset;
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
    
    std::vector<MeshData> MeshImporter::ParseGltfToMeshData(const std::filesystem::path& path)
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
        std::cout << "mesh count: " << model.meshes_count << std::endl;
        for (uint32_t meshIndex = 0; meshIndex < model.meshes_count; meshIndex++)
        {
            const tg3_mesh& mesh = model.meshes[meshIndex];

            for (uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives_count; primitiveIndex++)
            {
                const tg3_primitive& primitive = mesh.primitives[primitiveIndex];
                MeshData primitiveData{};

                // Get vertex positions
                const tg3_accessor& posAccessor = model.accessors[FindAttribute(primitive, "POSITION")];
                const tg3_buffer_view& posBufferView = model.buffer_views[posAccessor.buffer_view];
                const tg3_buffer& posBuffer = model.buffers[posBufferView.buffer];

                // Get texture coordinates if available
                bool hasTexCoords = FindAttribute(primitive, "TEXCOORD_0") ? true : false;
                const tg3_accessor* texCoordAccessor = nullptr;
                const tg3_buffer_view* texCoordBufferView = nullptr;
                const tg3_buffer* texCoordBuffer = nullptr;

                if (hasTexCoords)
                {
                    texCoordAccessor = &model.accessors[FindAttribute(primitive, "TEXCOORD_0")];
                    texCoordBufferView = &model.buffer_views[texCoordAccessor->buffer_view];
                    texCoordBuffer = &model.buffers[texCoordBufferView->buffer];
                }
                
                size_t primitiveVertexCount = posAccessor.count;
                primitiveData.Vertices.reserve(primitiveVertexCount);

                for (size_t i = 0; i < primitiveVertexCount; i++)
                {
                    shaderio::Vertex vertex{};

                    const float* pos = reinterpret_cast<const float*>(&posBuffer.data.data[posBufferView.byte_offset + posAccessor.byte_offset + i * 12]);
                    // glTF uses a right-handed coordinate system with Y-up
                    // Vulkan uses a right-handed coordinate system with Y-down
                    // We need to flip the Y coordinate
                    // i dont need that look first line in load model
                    vertex.pos = {pos[0], pos[1], pos[2]};

                    if (hasTexCoords)
                    {
                        const float* texCoord = reinterpret_cast<const float*>(&texCoordBuffer->data.data[texCoordBufferView->byte_offset + texCoordAccessor->byte_offset + i * 8]);
                        vertex.texCoord = {texCoord[0], texCoord[1]};
                    }
                    else
                    {
                        vertex.texCoord = {0.0f, 0.0f};
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
                
                // Recommended limits for Vulkan mesh shaders
                const size_t maxVertices = 64;
                const size_t maxTriangles = 64; //124
                
                // Generate meshlets with meshoptimizer
                size_t maxMeshlets = meshopt_buildMeshletsBound(primitiveIndices.size(), maxVertices, maxTriangles);
                std::vector<meshopt_Meshlet> localMeshlets(maxMeshlets);
                std::vector<unsigned int> localMeshletVertices(primitiveIndices.size());
                std::vector<unsigned char> localMeshletTriangles(primitiveIndices.size());
                
                size_t meshletCount = meshopt_buildMeshlets(
                    localMeshlets.data(),
                    localMeshletVertices.data(),
                    localMeshletTriangles.data(),
                    primitiveIndices.data(),
                    primitiveIndices.size(),
                    &primitiveData.Vertices[0].pos.x,
                    primitiveVertexCount,
                    sizeof(shaderio::Vertex),
                    maxVertices,
                    maxTriangles,
                    0.0f
                );
                
                localMeshlets.resize(meshletCount);
                
                for (auto& meshlet : localMeshlets)
                {
                    meshopt_optimizeMeshlet(
                      &localMeshletVertices[meshlet.vertex_offset],
                      &localMeshletTriangles[meshlet.triangle_offset],
                      meshlet.triangle_count,
                      meshlet.vertex_count
                    );
                }
                
                const meshopt_Meshlet& last = localMeshlets.back();
                localMeshletVertices.resize(last.vertex_offset + last.vertex_count);
                localMeshletTriangles.resize(last.triangle_offset + (last.triangle_count * 3));
                
                uint32_t meshletVertexOffset = static_cast<uint32_t>(primitiveData.MeshletVertices.size());
                uint32_t meshletTrianglesOffset = static_cast<uint32_t>(primitiveData.MeshletTriangles.size());
                
                for (const auto& meshlet : localMeshlets)
                {
                    meshopt_Bounds bounds = meshopt_computeMeshletBounds(
                        &localMeshletVertices[meshlet.vertex_offset],
                        &localMeshletTriangles[meshlet.triangle_offset],
                        meshlet.triangle_count,
                        &primitiveData.Vertices[0].pos.x,
                        primitiveVertexCount,
                        sizeof(shaderio::Vertex)
                    );
                    
                    // Buffer 1: Tasl Shader Culling Data
                    shaderio::MeshletBounds b{};
                    b.center     = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
                    b.radius     = bounds.radius;
                    b.coneApex   = glm::vec3(bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]);
                    b.coneCutoff = bounds.cone_cutoff;
                    b.coneAxis   = glm::vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]);
                    primitiveData.Bounds.push_back(b);

                    // Buffer 2: Mesh Shader Drawing Data
                    shaderio::MeshletDraw d{};
                    d.vertexOffset       = meshletVertexOffset + meshlet.vertex_offset;
                    d.triangleOffset     = meshletTrianglesOffset + meshlet.triangle_offset;
                    d.vertexCount        = meshlet.vertex_count;
                    d.triangleCount      = meshlet.triangle_count;
                    d.globalVertexOffset = 0;
                    primitiveData.Draws.push_back(d);
                }
                
                primitiveData.MeshletVertices.insert(primitiveData.MeshletVertices.end(), localMeshletVertices.begin(), localMeshletVertices.end());
                primitiveData.MeshletTriangles.insert(primitiveData.MeshletTriangles.end(), localMeshletTriangles.begin(), localMeshletTriangles.end());
                
                result.push_back(std::move(primitiveData));
            }
        }

        tg3_model_free(&model);
        tg3_error_stack_free(&errors);
        
        return result;
    }
}
