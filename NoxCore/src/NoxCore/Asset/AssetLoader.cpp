#include "AssetLoader.h"

#include <algorithm>
#include <deque>
#include <exception>

#include "AssetImporter.h"
#include "MeshImporter.h"
#include "TextureImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Tasks/JobSystem.h"

namespace Nox
{
    // One asset's load: advanced once per frame on the main thread, its file reads and decoding running as jobs.
    class AssetLoader::Load
    {
    public:
        Load(AssetHandle handle, const AssetMetadata& metadata) : m_Metadata(metadata), m_Start(std::chrono::steady_clock::now())
        {
            m_Result.Handle = handle;
        }
        virtual ~Load() = default;

        // Goes as far as it can this frame, taking staging bytes from the budget; true once the result is final.
        virtual bool Advance(Renderer& renderer, uint64_t& uploadBudget) = 0;
        // Blocks until no job of this load runs (they write memory the load owns).
        virtual void WaitForJobs() = 0;
        // After WaitForJobs, while the renderer exists: returns the GPU objects and geometry the load still holds.
        virtual void Release() = 0;

        LoadedAsset TakeResult()
        {
            m_Result.Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_Start).count();
            return std::move(m_Result);
        }

    protected:
        AssetMetadata m_Metadata;
        LoadedAsset m_Result;

    private:
        std::chrono::steady_clock::time_point m_Start;
    };

    namespace
    {
        // Staging bytes handed to loads per frame: one frame's copies stay small next to the transfer queue's bandwidth,
        // and the host memory held by staging stays bounded.
        constexpr uint64_t UploadBytesPerFrame = 64ull * 1024 * 1024;

        // A frame whose budget is untouched takes any single upload, so one larger than the budget still goes through.
        bool fitsBudget(uint64_t budget, uint64_t bytes)
        {
            return bytes <= budget || budget == UploadBytesPerFrame;
        }

        void takeBudget(uint64_t& budget, uint64_t bytes)
        {
            budget -= std::min(budget, bytes);
        }

        std::optional<XXH128_hash_t> hashSource(const std::filesystem::path& assetDirectory, const AssetMetadata& metadata)
        {
            if (metadata.SourceFilePath.empty())
                return std::nullopt;
            const std::filesystem::path path = assetDirectory / metadata.SourceFilePath;
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                return std::nullopt;
            return Utility::calcul_hash_streaming(path.string());
        }

        // A finished job's result, or empty when it threw (logged).
        template <typename T>
        std::optional<T> takeJobResult(TaskFuture<T>& future, const AssetMetadata& metadata)
        {
            try
            {
                return future.Get();
            }
            catch (const std::exception& exception)
            {
                NOX_CORE_ERROR("AssetLoader: loading {} failed: {}", metadata.FilePath.generic_string(), exception.what());
                return std::nullopt;
            }
        }

        // Staging a submesh takes (the upload's layout adds only alignment).
        uint64_t uploadBytes(const MeshData& data)
        {
            uint64_t triangles = 0;
            for (const shaderio::MeshletDraw& draw : data.Draws)
                triangles += draw.triangleCount;
            return sizeof(shaderio::Vertex) * data.Vertices.size() +
                   (sizeof(shaderio::MeshletDraw) + sizeof(shaderio::MeshletBounds)) * data.Draws.size() +
                   sizeof(uint32_t) * data.MeshletVertices.size() + data.MeshletTriangles.size() + sizeof(uint32_t) * 3 * triangles;
        }

        // Cooked .ntex: header read, texel read straight into staging, copy, publish.
        class TextureLoad final : public AssetLoader::Load
        {
        public:
            TextureLoad(AssetHandle handle, const AssetMetadata& metadata, const std::filesystem::path& assetDirectory) : Load(handle, metadata)
            {
                m_Header = JobSystem::Get().AsyncIO("Read Texture Header", [assetDirectory, metadata](const CancellationToken&)
                {
                    Header header;
                    header.Cooked = TextureImporter::ReadCookedTextureHeader(assetDirectory, metadata);
                    header.SourceHash = hashSource(assetDirectory, metadata);
                    return header;
                });
            }

            bool Advance(Renderer& renderer, uint64_t& uploadBudget) override
            {
                switch (m_Stage)
                {
                case Stage::Header:
                {
                    if (!m_Header.IsReady())
                        return false;
                    std::optional<Header> header = takeJobResult(m_Header, m_Metadata);
                    if (!header)
                        return true;
                    m_Result.SourceHash = header->SourceHash;
                    if (!header->Cooked)
                    {
                        m_Result.NeedsImport = true;
                        return true;
                    }
                    m_Cooked = std::move(*header->Cooked);
                    m_Stage = Stage::Staging;
                    [[fallthrough]];
                }
                case Stage::Staging:
                {
                    if (!fitsBudget(uploadBudget, m_Cooked.DataSize))
                        return false;
                    m_Upload = renderer.BeginTextureUpload(m_Cooked.Texture, m_Cooked.DataSize, false);
                    if (!m_Upload)
                        return false;
                    takeBudget(uploadBudget, m_Cooked.DataSize);

                    // From the file straight into staging: the texels are never copied on the CPU.
                    m_Read = JobSystem::Get().AsyncIO("Read Texture Data", [cooked = m_Cooked, destination = m_Upload->Staging.data](const CancellationToken&)
                    {
                        return TextureImporter::ReadCookedTextureData(cooked, destination);
                    });
                    m_Stage = Stage::Reading;
                    return false;
                }
                case Stage::Reading:
                {
                    if (!m_Read.IsReady())
                        return false;
                    const std::optional<bool> read = takeJobResult(m_Read, m_Metadata);
                    if (!read || !*read)
                    {
                        NOX_CORE_ERROR("AssetLoader: could not read {}", m_Metadata.FilePath.generic_string());
                        renderer.AbandonUpload(m_Upload->Staging);
                        m_Upload.reset();
                        return true;
                    }
                    m_CompleteValue = renderer.EndTextureUpload(*m_Upload, false);
                    m_Stage = Stage::Copying;
                    return false;
                }
                case Stage::Copying:
                {
                    if (renderer.GetCompletedUploadValue() < m_CompleteValue)
                        return false;
                    renderer.PublishTexture(*m_Upload->Texture);
                    m_Result.Loaded = Ref<Asset>(m_Upload->Texture);
                    m_Upload.reset();
                    return true;
                }
                }
                return true;
            }

            void WaitForJobs() override
            {
                m_Header.Wait();
                m_Read.Wait();
            }

            void Release() override
            {
                // The transfer queue may still write the image.
                if (m_Upload)
                    Renderer::DeferAssetRelease(Ref<Asset>(m_Upload->Texture));
                m_Upload.reset();
            }

        private:
            enum class Stage
            {
                Header,  // reading the header
                Staging, // waiting for staging room
                Reading, // reading the texels into staging
                Copying  // on the transfer queue
            };

            struct Header
            {
                std::optional<CookedTextureHeader> Cooked;
                std::optional<XXH128_hash_t> SourceHash;
            };

            Stage m_Stage = Stage::Header;
            TaskFuture<Header> m_Header;
            CookedTextureHeader m_Cooked;
            std::optional<TextureUpload> m_Upload;
            TaskFuture<bool> m_Read;
            uint64_t m_CompleteValue = 0;
        };

        // Cooked .nmesh / .nsmesh: read and parsed on an IO worker, then the submeshes in batches as staging allows:
        // ranges and staging on the main thread, written by a job, copied, published in order.
        class MeshLoad final : public AssetLoader::Load
        {
        public:
            MeshLoad(AssetHandle handle, const AssetMetadata& metadata, const std::filesystem::path& assetDirectory) : Load(handle, metadata)
            {
                m_Parse = JobSystem::Get().AsyncIO("Read Mesh", [assetDirectory, metadata](const CancellationToken&)
                {
                    Parsed parsed;
                    parsed.Mesh = MeshImporter::ReadCookedMesh(assetDirectory, metadata);
                    parsed.SourceHash = hashSource(assetDirectory, metadata);
                    return parsed;
                });
            }

            bool Advance(Renderer& renderer, uint64_t& uploadBudget) override
            {
                if (!m_Mesh)
                {
                    if (!m_Parse.IsReady())
                        return false;
                    std::optional<Parsed> parsed = takeJobResult(m_Parse, m_Metadata);
                    if (!parsed)
                        return true;
                    m_Result.SourceHash = parsed->SourceHash;
                    if (!parsed->Mesh)
                    {
                        m_Result.NeedsImport = true;
                        return true;
                    }
                    m_Cooked = std::move(*parsed->Mesh);
                    for (size_t index = 0; index < m_Cooked.Submeshes.size(); ++index)
                        m_Opaque.push_back(index < m_Cooked.Materials.size() ? m_Cooked.Materials[index].Mode == AlphaMode::Opaque : true);
                    m_Mesh = MeshImporter::CreateMeshAsset(m_Metadata.Type, m_Cooked);
                }

                admit(renderer, uploadBudget);

                for (const std::unique_ptr<Batch>& batch : m_Batches)
                {
                    if (batch->Recorded || !batch->Write.IsReady())
                        continue;
                    batch->Write.Get();
                    for (const MeshUpload& upload : batch->Uploads)
                        batch->CompleteValue = std::max(batch->CompleteValue, renderer.EndMeshUpload(upload, false));
                    batch->Recorded = true;
                }

                while (!m_Batches.empty() && m_Batches.front()->Recorded && renderer.GetCompletedUploadValue() >= m_Batches.front()->CompleteValue)
                {
                    const Batch& batch = *m_Batches.front();
                    for (size_t index = 0; index < batch.Uploads.size(); ++index)
                        MeshImporter::SetSubMesh(*m_Mesh, batch.First + index, renderer.PublishMesh(batch.Uploads[index]));
                    m_Batches.pop_front();
                }

                if (m_NextSubmesh < m_Cooked.Submeshes.size() || !m_Batches.empty())
                    return false;
                m_Result.Loaded = std::move(m_Mesh);
                return true;
            }

            void WaitForJobs() override
            {
                m_Parse.Wait();
                for (const std::unique_ptr<Batch>& batch : m_Batches)
                    batch->Write.Wait();
            }

            void Release() override
            {
                // Unpublished ranges go back through the deferred release queue; the published ones leave with the mesh.
                for (const std::unique_ptr<Batch>& batch : m_Batches)
                {
                    for (const MeshUpload& upload : batch->Uploads)
                        Renderer::UnloadMesh(upload.Handle);
                }
                m_Batches.clear();
                if (m_Mesh)
                    Renderer::DeferAssetRelease(std::move(m_Mesh));
            }

        private:
            struct Parsed
            {
                std::optional<CookedMesh> Mesh;
                std::optional<XXH128_hash_t> SourceHash;
            };

            // The submeshes admitted in one frame, written by one job.
            struct Batch
            {
                size_t First = 0;
                std::vector<MeshUpload> Uploads;
                TaskFuture<bool> Write;
                uint64_t CompleteValue = 0;
                bool Recorded = false;
            };

            void admit(Renderer& renderer, uint64_t& uploadBudget)
            {
                auto batch = std::make_unique<Batch>();
                batch->First = m_NextSubmesh;
                while (m_NextSubmesh < m_Cooked.Submeshes.size())
                {
                    const MeshData& data = m_Cooked.Submeshes[m_NextSubmesh];
                    const uint64_t bytes = uploadBytes(data);
                    if (!fitsBudget(uploadBudget, bytes))
                        break;
                    std::optional<MeshUpload> upload = renderer.BeginMeshUpload(data, m_Opaque[m_NextSubmesh], false);
                    if (!upload)
                        break;
                    takeBudget(uploadBudget, bytes);
                    batch->Uploads.push_back(std::move(*upload));
                    ++m_NextSubmesh;
                }
                if (batch->Uploads.empty())
                    return;

                // The job owns the batch's submeshes until it is done; each CPU copy is dropped once it is in staging.
                Batch* written = batch.get();
                MeshData* submeshes = m_Cooked.Submeshes.data();
                written->Write = JobSystem::Get().Async("Write Mesh Geometry", [written, submeshes](const CancellationToken&)
                {
                    for (size_t index = 0; index < written->Uploads.size(); ++index)
                    {
                        MeshData& data = submeshes[written->First + index];
                        Renderer::WriteMeshUpload(data, written->Uploads[index]);
                        data = MeshData{};
                    }
                    return true;
                });
                m_Batches.push_back(std::move(batch));
            }

            TaskFuture<Parsed> m_Parse;
            CookedMesh m_Cooked;
            std::vector<bool> m_Opaque;
            Ref<Asset> m_Mesh;
            size_t m_NextSubmesh = 0;
            std::deque<std::unique_ptr<Batch>> m_Batches;
        };

        // CPU-only assets (materials, skeletons, animations): the whole import runs as a job.
        class ImportLoad final : public AssetLoader::Load
        {
        public:
            ImportLoad(AssetHandle handle, const AssetMetadata& metadata, const std::filesystem::path& assetDirectory) : Load(handle, metadata)
            {
                m_Import = JobSystem::Get().Async("Import Asset", [handle, metadata, assetDirectory](const CancellationToken&)
                {
                    Imported imported;
                    imported.Loaded = AssetImporter::ImportAsset(handle, metadata);
                    imported.SourceHash = hashSource(assetDirectory, metadata);
                    return imported;
                });
            }

            bool Advance(Renderer&, uint64_t&) override
            {
                if (!m_Import.IsReady())
                    return false;
                if (std::optional<Imported> imported = takeJobResult(m_Import, m_Metadata))
                {
                    m_Result.Loaded = std::move(imported->Loaded);
                    m_Result.SourceHash = imported->SourceHash;
                }
                return true;
            }

            void WaitForJobs() override
            {
                m_Import.Wait();
            }

            void Release() override
            {
            }

        private:
            struct Imported
            {
                Ref<Asset> Loaded;
                std::optional<XXH128_hash_t> SourceHash;
            };

            TaskFuture<Imported> m_Import;
        };
    }

    // Out of line: the loads are only complete here.
    AssetLoader::AssetLoader() = default;

    AssetLoader::~AssetLoader()
    {
        // Jobs write memory the loads own.
        for (const std::unique_ptr<Load>& load : m_Loads)
            load->WaitForJobs();
    }

    bool AssetLoader::IsStreamable(AssetType type)
    {
        switch (type)
        {
            case AssetType::Texture2D:
            case AssetType::Mesh:
            case AssetType::StaticMesh:
            case AssetType::MeshSource:
            case AssetType::Material:
            case AssetType::Skeleton:
            case AssetType::AnimationSequence:
                return true;
            default:
                return false;
        }
    }

    void AssetLoader::Begin(AssetHandle handle, const AssetMetadata& metadata)
    {
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        switch (metadata.Type)
        {
            case AssetType::Texture2D:
                m_Loads.push_back(std::make_unique<TextureLoad>(handle, metadata, assetDirectory));
                break;
            case AssetType::Mesh:
            case AssetType::StaticMesh:
            case AssetType::MeshSource:
                m_Loads.push_back(std::make_unique<MeshLoad>(handle, metadata, assetDirectory));
                break;
            default:
                m_Loads.push_back(std::make_unique<ImportLoad>(handle, metadata, assetDirectory));
                break;
        }
        m_Loading.insert(handle);
    }

    void AssetLoader::Update(std::vector<LoadedAsset>& outFinished)
    {
        if (m_Loads.empty())
            return;

        NOX_PROFILE_SCOPE("Asset Loads");
        Renderer& renderer = *Application::Get().GetRenderer();
        uint64_t uploadBudget = UploadBytesPerFrame;
        std::erase_if(m_Loads, [&](const std::unique_ptr<Load>& load)
        {
            if (!load->Advance(renderer, uploadBudget))
                return false;
            outFinished.push_back(load->TakeResult());
            m_Loading.erase(outFinished.back().Handle);
            return true;
        });
    }

    void AssetLoader::Shutdown()
    {
        for (const std::unique_ptr<Load>& load : m_Loads)
        {
            load->WaitForJobs();
            load->Release();
        }
        m_Loads.clear();
        m_Loading.clear();
    }
}
