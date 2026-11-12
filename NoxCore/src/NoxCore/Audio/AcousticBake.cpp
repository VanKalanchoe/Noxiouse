#include "AcousticBake.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <tuple>
#include <unordered_map>

#include <phonon.h>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Profiling/Profiler.h"

namespace Nox
{
    namespace
    {
        // A world-units matrix as Steam Audio's (row-major, meters from `origin`).
        IPLMatrix4x4 ToSteam(const glm::dmat4& world, const glm::dvec3& origin)
        {
            const double metersPerUnit = static_cast<double>(WorldUnits::ToMeters(1.0f));
            glm::dmat4 relative = world;
            relative[3] -= glm::dvec4(origin, 0.0);
            const glm::dmat4 transform = glm::dmat4(glm::dmat3(metersPerUnit)) * relative;
            IPLMatrix4x4 out{};
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    out.elements[row][column] = static_cast<float>(transform[column][row]);
            out.elements[3][0] = out.elements[3][1] = out.elements[3][2] = 0.0f;
            out.elements[3][3] = 1.0f;
            return out;
        }

        struct ProgressTarget
        {
            std::atomic<float>* Progress;
            float Base;
            float Span;
        };
        void IPLCALL OnProgress(IPLfloat32 fraction, void* userData)
        {
            const auto* target = static_cast<const ProgressTarget*>(userData);
            target->Progress->store(target->Base + target->Span * std::clamp(static_cast<float>(fraction), 0.0f, 1.0f), std::memory_order_relaxed);
        }
    }

    AcousticBaker::~AcousticBaker()
    {
        Cancel();
        if (m_Thread.joinable())
            m_Thread.join();
    }

    void AcousticBaker::Start(const std::vector<SpatialAudio::Instance>& instances,
                              std::vector<std::pair<std::shared_ptr<const AcousticMesh>, glm::dmat4>> meshes,
                              std::vector<AcousticBakeVolume> volumes)
    {
        if (m_Thread.joinable())
            m_Thread.join();
        m_Results.clear();
        m_Reads.clear();
        m_Instances = instances;
        m_Meshes = std::move(meshes);
        m_Volumes = std::move(volumes);
        m_Cancelled.store(false);
        m_Progress.store(0.0f);
        // Each mesh asset (with its materials) read once, as the acoustic scene reads it.
        std::unordered_map<std::string, size_t> readOf;
        std::vector<size_t> instanceRead(m_Instances.size());
        for (size_t i = 0; i < m_Instances.size(); ++i)
        {
            const SpatialAudio::Instance& instance = m_Instances[i];
            std::string key = std::to_string(static_cast<uint64_t>(instance.Mesh)) + ':' + std::to_string(instance.FirstSubmesh) + ':' + std::to_string(instance.SubmeshCount) + ':';
            for (AcousticMaterial material : instance.Materials)
                key += static_cast<char>('a' + static_cast<int>(material));
            auto [found, added] = readOf.try_emplace(key, m_Reads.size());
            if (added)
                m_Reads.push_back(StartAcousticMeshRead(instance.Mesh, instance.FirstSubmesh, instance.SubmeshCount, instance.Materials));
            instanceRead[i] = found->second;
        }
        m_Running.store(true, std::memory_order_release);
        m_Thread = std::thread([this, instanceRead]()
        {
            Profiler::Get().SetThreadName("Acoustics Bake");
            // The reads finish on JobSystem workers; their meshes join the code-made ones.
            SetStatus("Reading meshes");
            std::vector<std::shared_ptr<const AcousticMesh>> read(m_Reads.size());
            for (size_t r = 0; r < m_Reads.size(); ++r)
            {
                if (!m_Reads[r].IsReady)
                    continue;
                while (!m_Reads[r].IsReady())
                {
                    if (m_Cancelled.load())
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (m_Cancelled.load())
                    break;
                read[r] = m_Reads[r].Get();
            }
            for (size_t i = 0; i < m_Instances.size() && !m_Cancelled.load(); ++i)
                if (read[instanceRead[i]])
                    m_Meshes.emplace_back(read[instanceRead[i]], m_Instances[i].World);
            if (!m_Cancelled.load())
                Run();
            m_Reads.clear();
            m_Running.store(false, std::memory_order_release);
        });
    }

    void AcousticBaker::Run()
    {
        IPLContext context = SpatialAudio::Context();
        if (!context)
        {
            for (const AcousticBakeVolume& volume : m_Volumes)
                m_Results.push_back({ volume.Key, nullptr, "Steam Audio is not running" });
            return;
        }
        const double metersPerUnit = static_cast<double>(WorldUnits::ToMeters(1.0f));
        const int32_t threads = static_cast<int32_t>(std::max(1u, std::thread::hardware_concurrency() / 2));
        // Volumes by their frame (the grid point nearest their center): one scene each.
        std::map<std::tuple<double, double, double>, std::vector<size_t>> byOrigin;
        for (size_t v = 0; v < m_Volumes.size(); ++v)
        {
            const glm::dvec3 origin = SpatialAudio::GridOrigin(glm::dvec3(m_Volumes[v].Box[3]));
            byOrigin[{ origin.x, origin.y, origin.z }].push_back(v);
        }
        size_t done = 0;
        const float perVolume = 1.0f / static_cast<float>(std::max<size_t>(m_Volumes.size(), 1));
        for (const auto& [key, volumeIndices] : byOrigin)
        {
            if (m_Cancelled.load())
                break;
            const glm::dvec3 origin{ std::get<0>(key), std::get<1>(key), std::get<2>(key) };
            SetStatus("Building the scene");
            // Every triangle in meters from the origin, one static mesh (a bake traces it many times: no instancing).
            std::vector<IPLVector3> vertices;
            std::vector<IPLTriangle> triangles;
            std::vector<IPLint32> materialIndices;
            std::vector<AcousticMaterial> used;
            for (const auto& [mesh, world] : m_Meshes)
            {
                const IPLint32 base = static_cast<IPLint32>(vertices.size());
                for (const glm::vec3& v : mesh->Vertices)
                {
                    const glm::dvec3 p = (glm::dvec3(world * glm::dvec4(glm::dvec3(v), 1.0)) - origin) * metersPerUnit;
                    vertices.push_back({ static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) });
                }
                const size_t count = mesh->Indices.size() / 3;
                for (size_t t = 0; t < count; ++t)
                {
                    triangles.push_back({ { base + static_cast<IPLint32>(mesh->Indices[t * 3]), base + static_cast<IPLint32>(mesh->Indices[t * 3 + 1]),
                                            base + static_cast<IPLint32>(mesh->Indices[t * 3 + 2]) } });
                    const AcousticMaterial material = t < mesh->TriangleMaterials.size() ? mesh->TriangleMaterials[t] : AcousticMaterial::Generic;
                    auto found = std::find(used.begin(), used.end(), material);
                    if (found == used.end())
                        found = used.insert(used.end(), material);
                    materialIndices.push_back(static_cast<IPLint32>(found - used.begin()));
                }
            }
            if (triangles.empty())
            {
                for (size_t v : volumeIndices)
                    m_Results.push_back({ m_Volumes[v].Key, nullptr, "no static meshes to bake against (the level's geometry makes the acoustics)" });
                continue;
            }
            std::vector<IPLMaterial> materials(used.size());
            for (size_t m = 0; m < used.size(); ++m)
            {
                const AcousticMaterialProperties& properties = GetAcousticMaterialProperties(used[m]);
                std::copy(std::begin(properties.Absorption), std::end(properties.Absorption), materials[m].absorption);
                materials[m].scattering = properties.Scattering;
                std::copy(std::begin(properties.Transmission), std::end(properties.Transmission), materials[m].transmission);
            }
            IPLSceneSettings sceneSettings{};
            sceneSettings.type = IPL_SCENETYPE_DEFAULT;
            IPLScene scene = nullptr;
            IPLStaticMesh staticMesh = nullptr;
            IPLStaticMeshSettings meshSettings{};
            meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
            meshSettings.numTriangles = static_cast<IPLint32>(triangles.size());
            meshSettings.numMaterials = static_cast<IPLint32>(materials.size());
            meshSettings.vertices = vertices.data();
            meshSettings.triangles = triangles.data();
            meshSettings.materialIndices = materialIndices.data();
            meshSettings.materials = materials.data();
            if (iplSceneCreate(context, &sceneSettings, &scene) != IPL_STATUS_SUCCESS || iplStaticMeshCreate(scene, &meshSettings, &staticMesh) != IPL_STATUS_SUCCESS)
            {
                if (scene)
                    iplSceneRelease(&scene);
                for (size_t v : volumeIndices)
                    m_Results.push_back({ m_Volumes[v].Key, nullptr, "Steam Audio could not build the scene" });
                continue;
            }
            iplStaticMeshAdd(staticMesh, scene);
            iplSceneCommit(scene);

            for (size_t v : volumeIndices)
            {
                if (m_Cancelled.load())
                    break;
                const AcousticBakeVolume& volume = m_Volumes[v];
                const AcousticBakeSettings& settings = volume.Settings;
                const float base = static_cast<float>(done) * perVolume;
                // Probes: uniform over the floors inside the box, Height above them, Spacing apart.
                SetStatus(volume.Name + ": placing probes");
                IPLProbeArray probes = nullptr;
                iplProbeArrayCreate(context, &probes);
                IPLProbeGenerationParams generation{};
                generation.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
                generation.spacing = std::max(settings.Spacing, 0.1f);
                generation.height = std::max(settings.Height, 0.0f);
                generation.transform = ToSteam(volume.Box, origin);
                iplProbeArrayGenerateProbes(probes, scene, &generation);
                const int32_t probeCount = iplProbeArrayGetNumProbes(probes);
                if (probeCount <= 0)
                {
                    iplProbeArrayRelease(&probes);
                    m_Results.push_back({ volume.Key, nullptr, "no floor inside the volume: probes go Height above the geometry below them" });
                    ++done;
                    continue;
                }
                std::vector<glm::vec3> positions(static_cast<size_t>(probeCount));
                for (int32_t p = 0; p < probeCount; ++p)
                {
                    const IPLVector3 center = iplProbeArrayGetProbe(probes, p).center;
                    positions[static_cast<size_t>(p)] = glm::vec3(glm::dvec3(center.x, center.y, center.z) / metersPerUnit);
                }
                IPLProbeBatch batch = nullptr;
                iplProbeBatchCreate(context, &batch);
                iplProbeBatchAddProbeArray(batch, probes);
                iplProbeBatchCommit(batch);
                iplProbeArrayRelease(&probes);

                const float reverbShare = settings.Reverb ? (settings.Pathing ? 0.85f : 1.0f) : 0.0f;
                if (settings.Reverb && !m_Cancelled.load())
                {
                    SetStatus(volume.Name + ": baking reverb (" + std::to_string(probeCount) + " probes)");
                    IPLReflectionsBakeParams params{};
                    params.scene = scene;
                    params.probeBatch = batch;
                    params.sceneType = IPL_SCENETYPE_DEFAULT;
                    params.identifier = { IPL_BAKEDDATATYPE_REFLECTIONS, IPL_BAKEDDATAVARIATION_REVERB };
                    params.bakeFlags = IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION;
                    params.numRays = std::max(settings.Rays, 256);
                    params.numDiffuseSamples = 1024;
                    params.numBounces = std::clamp(settings.Bounces, 1, 64);
                    params.simulatedDuration = std::clamp(settings.Duration, 0.1f, SpatialAudio::MaxReflectionDuration);
                    params.savedDuration = params.simulatedDuration;
                    params.order = SpatialAudio::ReflectionOrder;
                    params.numThreads = threads;
                    params.irradianceMinDistance = 1.0f;
                    params.bakeBatchSize = 1;
                    ProgressTarget target{ &m_Progress, base, perVolume * reverbShare };
                    iplReflectionsBakerBake(context, &params, OnProgress, &target);
                }
                if (settings.Pathing && !m_Cancelled.load())
                {
                    SetStatus(volume.Name + ": baking paths (" + std::to_string(probeCount) + " probes)");
                    IPLPathBakeParams params{};
                    params.scene = scene;
                    params.probeBatch = batch;
                    params.identifier = { IPL_BAKEDDATATYPE_PATHING, IPL_BAKEDDATAVARIATION_DYNAMIC };
                    params.numSamples = std::max(settings.VisSamples, 1);
                    params.radius = std::max(settings.VisRadius, 0.0f);
                    params.threshold = std::clamp(settings.VisThreshold, 0.0f, 1.0f);
                    params.visRange = std::max(settings.VisRange, 0.0f);
                    params.pathRange = std::max(settings.PathRange, 0.0f);
                    params.numThreads = threads;
                    ProgressTarget target{ &m_Progress, base + perVolume * reverbShare, perVolume * (1.0f - reverbShare) };
                    iplPathBakerBake(context, &params, OnProgress, &target);
                }
                if (m_Cancelled.load())
                {
                    iplProbeBatchRelease(&batch);
                    break;
                }
                // Saved: Steam Audio's serialized probe batch, with the frame it is in.
                IPLSerializedObjectSettings serializedSettings{};
                IPLSerializedObject serialized = nullptr;
                iplSerializedObjectCreate(context, &serializedSettings, &serialized);
                iplProbeBatchSave(batch, serialized);
                Ref<AcousticData> data = CreateRef<AcousticData>();
                const IPLbyte* bytes = iplSerializedObjectGetData(serialized);
                data->ProbeBatch.assign(bytes, bytes + iplSerializedObjectGetSize(serialized));
                data->Origin = origin;
                data->Volume = volume.Box;
                data->ProbeCount = static_cast<uint32_t>(probeCount);
                data->ProbePositions = std::move(positions);
                data->Settings = settings;
                data->BakedAt = static_cast<uint64_t>(std::time(nullptr));
                iplSerializedObjectRelease(&serialized);
                iplProbeBatchRelease(&batch);
                m_Results.push_back({ volume.Key, data, {} });
                ++done;
                m_Progress.store(static_cast<float>(done) * perVolume, std::memory_order_relaxed);
            }
            iplStaticMeshRelease(&staticMesh);
            iplSceneRelease(&scene);
        }
        SetStatus(m_Cancelled.load() ? "Cancelled" : "Done");
    }

    std::string AcousticBaker::GetStatus() const
    {
        std::scoped_lock lock(m_StatusMutex);
        return m_Status;
    }

    void AcousticBaker::SetStatus(std::string status)
    {
        std::scoped_lock lock(m_StatusMutex);
        m_Status = std::move(status);
    }

    void AcousticBaker::Cancel()
    {
        if (!m_Running.load())
            return;
        m_Cancelled.store(true);
        if (IPLContext context = SpatialAudio::Context())
        {
            iplReflectionsBakerCancelBake(context);
            iplPathBakerCancelBake(context);
        }
    }

    std::vector<AcousticBaker::Result> AcousticBaker::TakeResults()
    {
        if (m_Running.load())
            return {};
        if (m_Thread.joinable())
            m_Thread.join();
        return std::move(m_Results);
    }
}
