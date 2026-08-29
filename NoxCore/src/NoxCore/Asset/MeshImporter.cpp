#include "MeshImporter.h"

#include <string_view>
#include <vector>

#define TINYGLTF3_IMPLEMENTATION
#define TINYGLTF3_ENABLE_FS 
#include <glm/gtx/matrix_decompose.hpp>

#include "tiny_gltf_v3.h"

#include "meshoptimizer.h"
#include "NoxCore/Core/Log.h"

#include "NoxCore/Project/Project.h"
#include "NoxCore/Asset/MeshSerializer.h"

namespace Nox
{
    Ref<Mesh> MeshImporter::ImportMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
        std::filesystem::path sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;

        NOX_CORE_INFO("MeshImporter::ImportMesh loading mesh from {}", cookedPath.string());

        std::vector<MeshData> meshDataList;
        std::vector<MaterialData> materialDataList;

        if (std::filesystem::exists(cookedPath))
        {
            NOX_CORE_INFO("Loading cooked dynamic mesh from {}", cookedPath.string());
            bool success = MeshSerializer::DeserializeMesh(cookedPath, meshDataList, materialDataList);
            if (!success) NOX_CORE_ASSERT(false, "MeshImporter::ImportMesh - Failed to deserialize .nmesh file: {}", cookedPath.string());
        }
        else
        {
            // It doesn't exist, cook it from the source!
            NOX_CORE_INFO("Cooking GLTF from {} to {}", sourcePath.string(), cookedPath.string());

            Skeleton extractedSkeleton;
            std::vector<Ref<AnimationSequence>> extractedAnimations;

            meshDataList = ParseGltfToMeshData(sourcePath, materialDataList, extractedSkeleton, extractedAnimations);
            if (meshDataList.empty())
            {
                NOX_CORE_ASSERT(false, "MeshImporter::ImportMesh - Failed to load or empty mesh at source path: {}", sourcePath.string());
                return Ref<Mesh>(nullptr);
            }

            // Ensure the directory for the cooked path actually exists before saving!
            if (!std::filesystem::exists(cookedPath.parent_path()))
                std::filesystem::create_directories(cookedPath.parent_path());

            MeshSerializer::SerializeMesh(cookedPath, meshDataList, materialDataList);

            // Save skeleton if present
            if (!extractedSkeleton.AllNodes.empty())
            {
                std::filesystem::path skelPath = cookedPath;
                skelPath.replace_extension(".nskel");
                SkeletonSerializer::Serialize(skelPath, extractedSkeleton);
                NOX_CORE_INFO("[Importer] Extracted and cooked Skeleton ({} nodes) to {}", extractedSkeleton.AllNodes.size(), skelPath.string());
            }

            // Save animation clips if present
            for (size_t i = 0; i < extractedAnimations.size(); i++)
            {
                std::filesystem::path animPath = cookedPath.parent_path() / (cookedPath.stem().string() + "_" + extractedAnimations[i]->Name + ".nanim");
                AnimationSerializer::Serialize(animPath, *extractedAnimations[i]);
                NOX_CORE_INFO("[Importer] Extracted and cooked Animation Sequence '{}' to {}", extractedAnimations[i]->Name, animPath.string());
            }
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

        meshDataList.clear();
        materialDataList.clear();

        return meshAsset;
    }

    Ref<StaticMesh> MeshImporter::ImportStaticMesh(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::filesystem::path cookedPath = Project::GetActiveAssetDirectory() / metadata.FilePath;
        std::filesystem::path sourcePath = Project::GetActiveAssetDirectory() / metadata.SourceFilePath;

        NOX_CORE_INFO("MeshImporter::ImportStaticMesh loading static mesh from {}", cookedPath.string());

        std::vector<MeshData> meshDataList;
        std::vector<MaterialData> materialDataList;

        if (std::filesystem::exists(cookedPath))
        {
            NOX_CORE_INFO("Loading cooked static mesh from {}", cookedPath.string());
            bool success = MeshSerializer::DeserializeStaticMesh(cookedPath, meshDataList, materialDataList);
            if (!success)
            {
                NOX_CORE_ASSERT(false, "MeshImporter::ImportStaticMesh - Failed to deserialize .nsmesh file: {}", cookedPath.string());
            }
        }
        else
        {
            NOX_CORE_INFO("Cooking GLTF from {} to {}", sourcePath.string(), cookedPath.string());

            // Dummy parameters for unused skeleton/animations in static mesh import
            Skeleton dummySkeleton;
            std::vector<Ref<AnimationSequence>> dummyAnimations;

            meshDataList = ParseGltfToMeshData(sourcePath, materialDataList, dummySkeleton, dummyAnimations);
            if (meshDataList.empty())
            {
                NOX_CORE_ASSERT(false, "No Meshes found in source file: {}", sourcePath.string());
                return Ref<StaticMesh>(nullptr);
            }

            if (!std::filesystem::exists(cookedPath.parent_path()))
                std::filesystem::create_directories(cookedPath.parent_path());

            MeshSerializer::SerializeStaticMesh(cookedPath, meshDataList, materialDataList);
        }

        Ref<StaticMesh> staticMeshAsset = CreateRef<StaticMesh>();
        for (const auto& data : meshDataList)
        {
            MeshHandle subMeshHandle = Renderer::UploadMesh(data);
            staticMeshAsset->m_SubMeshes.push_back(subMeshHandle);
            staticMeshAsset->m_SubmeshNames.push_back(data.Name);
        }
        staticMeshAsset->m_Materials = std::move(materialDataList);


        meshDataList.clear();
        materialDataList.clear();

        return staticMeshAsset;
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
            newNode->RestRotation = glm::quat(static_cast<float>(tg3Node.rotation[3]), static_cast<float>(tg3Node.rotation[0]), static_cast<float>(tg3Node.rotation[1]), static_cast<float>(tg3Node.rotation[2]));
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
        modelPath.parent_path() / "textures";

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

    std::vector<MeshData> MeshImporter::ParseGltfToMeshData
    (
        const std::filesystem::path& path,
        std::vector<MaterialData>& outMaterials,
        Skeleton& outSkeleton,
        std::vector<Ref<AnimationSequence>>& outAnimations
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

        for (uint32_t meshIndex = 0; meshIndex < model.meshes_count; meshIndex++)
        {
            const tg3_mesh& mesh = model.meshes[meshIndex];

            // Naming
            std::string meshName;
            if (mesh.name.data && mesh.name.len > 0)
            {
                meshName = std::string(mesh.name.data, mesh.name.len);
            }

            if (meshName.empty())
            {
                for (uint32_t nodeIndex = 0; nodeIndex < model.nodes_count; nodeIndex++)
                {
                    const auto& node = model.nodes[nodeIndex];
                    // Check if this node references our mesh index and has a valid name string
                    if (node.mesh == meshIndex && node.name.data && node.name.len > 0)
                    {
                        meshName = std::string(node.name.data, node.name.len);
                        NOX_CORE_INFO("[Importer] Found node name for mesh {}: '{}'", meshIndex, meshName);
                        break; // Found the matching node name!
                    }
                }
                if (meshName.empty())
                {
                    NOX_CORE_WARN("[Importer] Could not find any name for mesh {}! Node mesh index matching failed.", meshIndex);
                }
            }

            // Mesh
            for (uint32_t primitiveIndex = 0; primitiveIndex < mesh.primitives_count; primitiveIndex++)
            {
                const tg3_primitive& primitive = mesh.primitives[primitiveIndex];
                MeshData primitiveData{};

                if (!meshName.empty())
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

                // Get texture coordinates if available
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
                    b.center = glm::vec3(bounds.center[0], bounds.center[1], bounds.center[2]);
                    b.radius = bounds.radius;
                    b.coneApex = glm::vec3(bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]);
                    b.coneCutoff = bounds.cone_cutoff;
                    b.coneAxis = glm::vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]);
                    primitiveData.Bounds.push_back(b);

                    // Buffer 2: Mesh Shader Drawing Data
                    shaderio::MeshletDraw d{};
                    d.vertexOffset = meshletVertexOffset + meshlet.vertex_offset;
                    d.triangleOffset = meshletTrianglesOffset + meshlet.triangle_offset;
                    d.vertexCount = meshlet.vertex_count;
                    d.triangleCount = meshlet.triangle_count;
                    d.globalVertexOffset = 0;
                    primitiveData.Draws.push_back(d);
                }

                primitiveData.MeshletVertices.insert(primitiveData.MeshletVertices.end(), localMeshletVertices.begin(), localMeshletVertices.end());
                primitiveData.MeshletTriangles.insert(primitiveData.MeshletTriangles.end(), localMeshletTriangles.begin(), localMeshletTriangles.end());

                MaterialData materialData{};

                // Materials
                if (primitive.material >= 0 && primitive.material < (int32_t)model.materials_count)
                {
                    const auto& gltfMaterial = model.materials[primitive.material];

                    // Base Color Factor
                    materialData.AlbedoColor = glm::vec4(
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[0]),
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[1]),
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[2]),
                        static_cast<float>(gltfMaterial.pbr_metallic_roughness.base_color_factor[3])
                    );

                    // Base Color Texture URI
                    int32_t albedoTexIndex = gltfMaterial.pbr_metallic_roughness.base_color_texture.index;
                    if (albedoTexIndex >= 0 && albedoTexIndex < (int32_t)model.textures_count)
                    {
                        int32_t imageIndex = model.textures[albedoTexIndex].source;
                        if (imageIndex >= 0 && imageIndex < (int32_t)model.images_count)
                        {
                            const auto& image = model.images[imageIndex];

                            std::filesystem::path texturePath =
                                ExtractGltfImage(
                                    model,
                                    image,
                                    imageIndex,
                                    path
                                );

                            if (!texturePath.empty())
                            {
                                materialData.AlbedoTexturePath =
                                    texturePath.string();
                            }
                        }
                    }
                }
                outMaterials.push_back(materialData);
                result.push_back(std::move(primitiveData));
            }
        }

        tg3_model_free(&model);
        tg3_error_stack_free(&errors);

        return result;
    }
}
