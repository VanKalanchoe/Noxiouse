#include "SpatialAudio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Profiling/Profiler.h"

namespace Nox
{
    namespace
    {
        constexpr double kRebaseMeters = 1000.0;  // the origin moves to the listener once it is this far (floats keep ~0.1 mm there)
        constexpr int32_t kOcclusionSamples = 16; // volumetric occlusion: rays to points over the source's sphere
        constexpr int32_t kTransmissionRays = 3;  // up to this many surfaces passed through count for transmission
        constexpr int32_t kMaxRays = 16384;
        constexpr int32_t kDiffuseSamples = 32;

        struct SubScene
        {
            IPLScene Scene = nullptr;
            IPLStaticMesh Mesh = nullptr;
            AcousticMeshRead Read;
            std::shared_ptr<const AcousticMesh> Data; // code-made meshes: given, not read
            bool Reading = false;
            bool Failed = false;
            uint64_t Triangles = 0;
        };

        // One placement, in both scenes: the occlusion scene (changed at once, traced on the game thread) and the reflection scene
        // (brought up to date only between reflection runs, traced on the reflection thread). Both use the same sub-scene.
        struct PlacedInstance
        {
            std::string MeshKey;
            glm::dmat4 World{ 1.0 };
            IPLInstancedMesh Direct = nullptr;
            IPLInstancedMesh Reflect = nullptr;
            bool ReflectMoved = false; // its transform changed since the reflection scene last took it
            bool Removed = false;      // gone: out of the reflection scene at the next chance, then forgotten
        };

        struct State
        {
            IPLContext Context = nullptr;
            IPLHRTF Hrtf = nullptr;
            IPLScene Scene = nullptr;            // occlusion
            IPLSimulator Simulator = nullptr;
            IPLScene ReflectScene = nullptr;     // reflections
            IPLSimulator ReflectSimulator = nullptr;
            uint32_t SampleRate = 48000;
            uint32_t FrameSize = 256;
            glm::dvec3 Origin{ 0.0 };
            glm::dvec3 ListenerPosition{ 0.0 };
            glm::quat ListenerOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
            std::unordered_map<std::string, SubScene> Meshes;
            std::unordered_map<uint64_t, PlacedInstance> Instances;
            bool SceneDirty = false;
            bool SimulatorDirty = false;
            bool ReflectDirty = false;

            // Reflections: the pool of reflection sources, the thread that traces them.
            std::array<IPLSource, SpatialAudio::MaxReflectionSources> Pool{};
            std::array<bool, SpatialAudio::MaxReflectionSources> PoolAdded{};
            std::array<bool, SpatialAudio::MaxReflectionSources> PoolHasResults{};
            SpatialAudio::ReflectionSettings Settings;
            std::thread ReflectThread;
            std::mutex ReflectMutex;
            std::condition_variable ReflectWake;
            bool ReflectRequested = false;    // (under ReflectMutex)
            bool ReflectQuit = false;         // (under ReflectMutex)
            std::atomic<bool> ReflectRunning{ false };
            std::atomic<bool> ReflectFinished{ false };
            std::atomic<float> ReflectMilliseconds{ 0.0f };
            std::vector<IPLInstancedMesh> RetiredReflect; // placements replaced: out of the reflection scene at the next sync
        };
        State* s_State = nullptr;
        uint32_t s_MaterialVersion = 1;

        void IPLCALL LogCallback(IPLLogLevel level, const char* message)
        {
            std::string text(message ? message : "");
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            if (level == IPL_LOGLEVEL_ERROR)
                NOX_CORE_ERROR("Steam Audio: {}", text);
            else if (level == IPL_LOGLEVEL_WARNING)
                NOX_CORE_WARN("Steam Audio: {}", text);
        }

        std::string MeshKey(const SpatialAudio::Instance& instance)
        {
            std::string key = "asset:" + std::to_string(static_cast<uint64_t>(instance.Mesh)) + ':' + std::to_string(instance.FirstSubmesh) + ':' +
                              std::to_string(instance.SubmeshCount) + ':';
            for (AcousticMaterial material : instance.Materials)
                key += static_cast<char>('a' + static_cast<int>(material));
            return key;
        }

        // World units -> meters around the origin, as a row-major matrix (Steam Audio's IPLMatrix4x4: elements[row][column]).
        IPLMatrix4x4 AcousticTransform(const State& state, const glm::dmat4& world)
        {
            const double metersPerUnit = static_cast<double>(WorldUnits::ToMeters(1.0f));
            glm::dmat4 relative = world;
            relative[3] -= glm::dvec4(state.Origin, 0.0);
            const glm::dmat4 transform = glm::dmat4(glm::dmat3(metersPerUnit)) * relative;
            IPLMatrix4x4 out{};
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column)
                    out.elements[row][column] = static_cast<float>(transform[column][row]);
            out.elements[3][0] = out.elements[3][1] = out.elements[3][2] = 0.0f;
            out.elements[3][3] = 1.0f;
            return out;
        }

        IPLCoordinateSpace3 ListenerSpace(const State& state, IPLVector3 origin)
        {
            const glm::quat& q = state.ListenerOrientation;
            const glm::vec3 right = q * glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 up = q * glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 ahead = q * glm::vec3(0.0f, 0.0f, -1.0f);
            IPLCoordinateSpace3 space{};
            space.right = { right.x, right.y, right.z };
            space.up = { up.x, up.y, up.z };
            space.ahead = { ahead.x, ahead.y, ahead.z };
            space.origin = origin;
            return space;
        }

        bool BuildSubScene(State& state, SubScene& sub, const AcousticMesh& mesh)
        {
            const size_t triangles = mesh.Indices.size() / 3;
            if (triangles == 0 || mesh.Vertices.empty())
                return false;
            IPLSceneSettings sceneSettings{};
            sceneSettings.type = IPL_SCENETYPE_DEFAULT;
            if (iplSceneCreate(state.Context, &sceneSettings, &sub.Scene) != IPL_STATUS_SUCCESS)
                return false;
            std::vector<IPLVector3> vertices(mesh.Vertices.size());
            for (size_t i = 0; i < vertices.size(); ++i)
                vertices[i] = { mesh.Vertices[i].x, mesh.Vertices[i].y, mesh.Vertices[i].z };
            std::vector<IPLTriangle> indices(triangles);
            for (size_t t = 0; t < triangles; ++t)
                for (int corner = 0; corner < 3; ++corner)
                    indices[t].indices[corner] = static_cast<IPLint32>(mesh.Indices[t * 3 + corner]);
            // The acoustic materials it uses, each once.
            std::vector<AcousticMaterial> used;
            std::vector<IPLint32> materialIndices(triangles, 0);
            for (size_t t = 0; t < triangles; ++t)
            {
                const AcousticMaterial material = t < mesh.TriangleMaterials.size() ? mesh.TriangleMaterials[t] : AcousticMaterial::Generic;
                auto found = std::find(used.begin(), used.end(), material);
                if (found == used.end())
                    found = used.insert(used.end(), material);
                materialIndices[t] = static_cast<IPLint32>(found - used.begin());
            }
            std::vector<IPLMaterial> materials(used.size());
            for (size_t m = 0; m < used.size(); ++m)
            {
                const AcousticMaterialProperties& properties = GetAcousticMaterialProperties(used[m]);
                std::memcpy(materials[m].absorption, properties.Absorption, sizeof(materials[m].absorption));
                materials[m].scattering = properties.Scattering;
                std::memcpy(materials[m].transmission, properties.Transmission, sizeof(materials[m].transmission));
            }
            IPLStaticMeshSettings meshSettings{};
            meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
            meshSettings.numTriangles = static_cast<IPLint32>(triangles);
            meshSettings.numMaterials = static_cast<IPLint32>(materials.size());
            meshSettings.vertices = vertices.data();
            meshSettings.triangles = indices.data();
            meshSettings.materialIndices = materialIndices.data();
            meshSettings.materials = materials.data();
            if (iplStaticMeshCreate(sub.Scene, &meshSettings, &sub.Mesh) != IPL_STATUS_SUCCESS)
            {
                iplSceneRelease(&sub.Scene);
                return false;
            }
            iplStaticMeshAdd(sub.Mesh, sub.Scene);
            iplSceneCommit(sub.Scene);
            sub.Triangles = triangles;
            return true;
        }

        void ReleaseSubScene(SubScene& sub)
        {
            if (sub.Mesh)
                iplStaticMeshRelease(&sub.Mesh);
            if (sub.Scene)
                iplSceneRelease(&sub.Scene);
        }

        IPLInstancedMesh CreateInstanced(const State& state, IPLScene scene, IPLScene subScene, const glm::dmat4& world)
        {
            IPLInstancedMeshSettings settings{};
            settings.subScene = subScene;
            settings.transform = AcousticTransform(state, world);
            IPLInstancedMesh mesh = nullptr;
            if (iplInstancedMeshCreate(scene, &settings, &mesh) != IPL_STATUS_SUCCESS)
                return nullptr;
            iplInstancedMeshAdd(mesh, scene);
            return mesh;
        }

        void RemoveInstanced(IPLScene scene, IPLInstancedMesh& mesh)
        {
            if (!mesh)
                return;
            iplInstancedMeshRemove(mesh, scene);
            iplInstancedMeshRelease(&mesh);
        }

        // The occlusion scene's part of an instance: at once.
        void PlaceDirect(State& state, PlacedInstance& instance)
        {
            if (instance.Direct || instance.Removed)
                return;
            const auto found = state.Meshes.find(instance.MeshKey);
            if (found == state.Meshes.end() || !found->second.Scene)
                return;
            instance.Direct = CreateInstanced(state, state.Scene, found->second.Scene, instance.World);
            state.SceneDirty = true;
            state.ReflectDirty = true;
        }

        void PlaceInstance(State& state, uint64_t key, const std::string& meshKey, const glm::dmat4& world)
        {
            auto [entry, added] = state.Instances.try_emplace(key);
            PlacedInstance& instance = entry->second;
            if (!added && instance.MeshKey == meshKey && !instance.Removed)
            {
                if (instance.World == world)
                    return;
                instance.World = world; // moved: a new transform
                if (instance.Direct)
                {
                    iplInstancedMeshUpdateTransform(instance.Direct, state.Scene, AcousticTransform(state, world));
                    state.SceneDirty = true;
                }
                instance.ReflectMoved = true;
                state.ReflectDirty = true;
                return;
            }
            if (!added && instance.MeshKey != meshKey)
            {
                // Another mesh: the old placement goes (from the reflection scene when it is next brought up to date).
                RemoveInstanced(state.Scene, instance.Direct);
                state.SceneDirty = true;
                if (instance.Reflect)
                {
                    state.RetiredReflect.push_back(instance.Reflect);
                    instance.Reflect = nullptr;
                }
            }
            PlacedInstance& placed = state.Instances[key];
            placed.MeshKey = meshKey;
            placed.World = world;
            placed.Removed = false;
            placed.ReflectMoved = true;
            state.ReflectDirty = true;
            PlaceDirect(state, placed);
        }

        // Brings the reflection scene up to date (only while the reflection thread is idle) and commits it.
        void SyncReflectScene(State& state)
        {
            if (!state.ReflectDirty)
                return;
            for (IPLInstancedMesh& mesh : state.RetiredReflect)
                RemoveInstanced(state.ReflectScene, mesh);
            state.RetiredReflect.clear();
            for (auto it = state.Instances.begin(); it != state.Instances.end();)
            {
                PlacedInstance& instance = it->second;
                if (instance.Removed)
                {
                    RemoveInstanced(state.ReflectScene, instance.Reflect);
                    if (!instance.Direct)
                    {
                        it = state.Instances.erase(it);
                        continue;
                    }
                }
                else if (!instance.Reflect)
                {
                    const auto found = state.Meshes.find(instance.MeshKey);
                    if (found != state.Meshes.end() && found->second.Scene)
                        instance.Reflect = CreateInstanced(state, state.ReflectScene, found->second.Scene, instance.World);
                    instance.ReflectMoved = false;
                }
                else if (instance.ReflectMoved)
                {
                    iplInstancedMeshUpdateTransform(instance.Reflect, state.ReflectScene, AcousticTransform(state, instance.World));
                    instance.ReflectMoved = false;
                }
                ++it;
            }
            iplSceneCommit(state.ReflectScene);
            iplSimulatorCommit(state.ReflectSimulator);
            state.ReflectDirty = false;
        }

        void ReflectionThread(State* state)
        {
            Profiler::Get().SetThreadName("Audio Reflections");
            for (;;)
            {
                {
                    std::unique_lock lock(state->ReflectMutex);
                    state->ReflectWake.wait(lock, [state] { return state->ReflectRequested || state->ReflectQuit; });
                    if (state->ReflectQuit)
                        return;
                    state->ReflectRequested = false;
                }
                const auto begin = std::chrono::steady_clock::now();
                {
                    NOX_PROFILE_SCOPE("Steam Audio Reflections");
                    iplSimulatorRunReflections(state->ReflectSimulator);
                }
                state->ReflectMilliseconds.store(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count(), std::memory_order_relaxed);
                state->ReflectFinished.store(true, std::memory_order_release);
                state->ReflectRunning.store(false, std::memory_order_release);
            }
        }

        bool Idle(const State& state) { return !state.ReflectRunning.load(std::memory_order_acquire); }
    }

    AcousticMaterial ResolveAcousticMaterial(AcousticMaterial chosen, float metallic, float transmission, bool blended)
    {
        if (chosen != AcousticMaterial::Auto)
            return chosen;
        if (transmission > 0.5f || blended)
            return AcousticMaterial::Glass;
        if (metallic > 0.5f)
            return AcousticMaterial::Metal;
        return AcousticMaterial::Generic;
    }

    bool SpatialAudio::Init(uint32_t sampleRate, uint32_t frameSize)
    {
        if (s_State)
            return true;
        auto state = std::make_unique<State>();
        state->SampleRate = sampleRate;
        state->FrameSize = frameSize;

        IPLContextSettings contextSettings{};
        contextSettings.version = STEAMAUDIO_VERSION;
        contextSettings.logCallback = LogCallback;
        contextSettings.simdLevel = IPL_SIMDLEVEL_AVX2; // (the highest it uses; lower on older CPUs)
        if (iplContextCreate(&contextSettings, &state->Context) != IPL_STATUS_SUCCESS)
        {
            NOX_CORE_ERROR("Steam Audio: the context could not be created -- 3D sounds are panned without HRTF or occlusion");
            return false;
        }
        IPLAudioSettings audioSettings{ static_cast<IPLint32>(sampleRate), static_cast<IPLint32>(frameSize) };
        IPLHRTFSettings hrtfSettings{};
        hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
        hrtfSettings.volume = 1.0f;
        hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
        if (iplHRTFCreate(state->Context, &audioSettings, &hrtfSettings, &state->Hrtf) != IPL_STATUS_SUCCESS)
        {
            NOX_CORE_ERROR("Steam Audio: the HRTF could not be loaded");
            iplContextRelease(&state->Context);
            return false;
        }
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = IPL_SCENETYPE_DEFAULT;
        IPLSimulationSettings direct{};
        direct.flags = IPL_SIMULATIONFLAGS_DIRECT;
        direct.sceneType = IPL_SCENETYPE_DEFAULT;
        direct.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        direct.maxNumOcclusionSamples = kOcclusionSamples;
        direct.maxNumSources = 128;
        direct.numThreads = 1;
        direct.samplingRate = static_cast<IPLint32>(sampleRate);
        direct.frameSize = static_cast<IPLint32>(frameSize);
        IPLSimulationSettings reflect = direct;
        reflect.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        reflect.maxNumRays = kMaxRays;
        reflect.numDiffuseSamples = kDiffuseSamples;
        reflect.maxDuration = MaxReflectionDuration;
        reflect.maxOrder = ReflectionOrder;
        reflect.maxNumSources = MaxReflectionSources;
        reflect.numThreads = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 4));
        const bool created = iplSceneCreate(state->Context, &sceneSettings, &state->Scene) == IPL_STATUS_SUCCESS &&
                             iplSimulatorCreate(state->Context, &direct, &state->Simulator) == IPL_STATUS_SUCCESS &&
                             iplSceneCreate(state->Context, &sceneSettings, &state->ReflectScene) == IPL_STATUS_SUCCESS &&
                             iplSimulatorCreate(state->Context, &reflect, &state->ReflectSimulator) == IPL_STATUS_SUCCESS;
        if (!created)
        {
            NOX_CORE_ERROR("Steam Audio: the acoustic scene / simulators could not be created");
            if (state->ReflectSimulator) iplSimulatorRelease(&state->ReflectSimulator);
            if (state->ReflectScene) iplSceneRelease(&state->ReflectScene);
            if (state->Simulator) iplSimulatorRelease(&state->Simulator);
            if (state->Scene) iplSceneRelease(&state->Scene);
            iplHRTFRelease(&state->Hrtf);
            iplContextRelease(&state->Context);
            return false;
        }
        for (IPLScene scene : { state->Scene, state->ReflectScene })
            iplSceneCommit(scene);
        iplSimulatorSetScene(state->Simulator, state->Scene);
        iplSimulatorCommit(state->Simulator);
        iplSimulatorSetScene(state->ReflectSimulator, state->ReflectScene);
        IPLSourceSettings poolSettings{};
        poolSettings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        for (IPLSource& source : state->Pool)
            iplSourceCreate(state->ReflectSimulator, &poolSettings, &source);
        iplSimulatorCommit(state->ReflectSimulator);
        state->ReflectThread = std::thread(ReflectionThread, state.get());
        s_State = state.release();
        NOX_CORE_INFO("Steam Audio {}.{}.{}: HRTF, occlusion, transmission and reflections on", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
        return true;
    }

    void SpatialAudio::Shutdown()
    {
        if (!s_State)
            return;
        State& state = *s_State;
        {
            std::scoped_lock lock(state.ReflectMutex);
            state.ReflectQuit = true;
        }
        state.ReflectWake.notify_all();
        state.ReflectThread.join();
        ClearInstances();
        for (auto& [key, sub] : state.Meshes)
            ReleaseSubScene(sub); // (a read still running owns its own data: it finishes into nothing)
        state.Meshes.clear();
        for (size_t i = 0; i < state.Pool.size(); ++i)
        {
            if (state.PoolAdded[i])
                iplSourceRemove(state.Pool[i], state.ReflectSimulator);
            iplSourceRelease(&state.Pool[i]);
        }
        iplSimulatorRelease(&state.ReflectSimulator);
        iplSceneRelease(&state.ReflectScene);
        iplSimulatorRelease(&state.Simulator);
        iplSceneRelease(&state.Scene);
        iplHRTFRelease(&state.Hrtf);
        iplContextRelease(&state.Context);
        delete s_State;
        s_State = nullptr;
    }

    bool SpatialAudio::IsAvailable() { return s_State != nullptr; }
    IPLContext SpatialAudio::Context() { return s_State ? s_State->Context : nullptr; }
    IPLHRTF SpatialAudio::Hrtf() { return s_State ? s_State->Hrtf : nullptr; }

    void SpatialAudio::SetInstance(uint64_t key, const Instance& instance)
    {
        if (!s_State)
            return;
        State& state = *s_State;
        const std::string meshKey = MeshKey(instance);
        auto [mesh, added] = state.Meshes.try_emplace(meshKey);
        if (added)
        {
            mesh->second.Read = StartAcousticMeshRead(instance.Mesh, instance.FirstSubmesh, instance.SubmeshCount, instance.Materials);
            mesh->second.Reading = static_cast<bool>(mesh->second.Read.IsReady);
            mesh->second.Failed = !mesh->second.Reading;
        }
        PlaceInstance(state, key, meshKey, instance.World);
    }

    void SpatialAudio::SetMeshInstance(uint64_t key, std::shared_ptr<const AcousticMesh> mesh, const glm::dmat4& world)
    {
        if (!s_State || !mesh)
            return;
        State& state = *s_State;
        const std::string meshKey = "code:" + std::to_string(reinterpret_cast<uintptr_t>(mesh.get()));
        auto [entry, added] = state.Meshes.try_emplace(meshKey);
        if (added)
        {
            entry->second.Data = mesh;
            entry->second.Failed = !BuildSubScene(state, entry->second, *mesh);
        }
        PlaceInstance(state, key, meshKey, world);
    }

    void SpatialAudio::RemoveInstance(uint64_t key)
    {
        if (!s_State)
            return;
        State& state = *s_State;
        const auto found = state.Instances.find(key);
        if (found == state.Instances.end() || found->second.Removed)
            return;
        RemoveInstanced(state.Scene, found->second.Direct);
        state.SceneDirty = true;
        found->second.Removed = true; // (out of the reflection scene at the next sync)
        state.ReflectDirty = true;
    }

    void SpatialAudio::ClearInstances()
    {
        if (!s_State)
            return;
        State& state = *s_State;
        // The reflection thread finishes its run first (it traces the reflection scene).
        while (!Idle(state))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        for (auto& [key, instance] : state.Instances)
        {
            RemoveInstanced(state.Scene, instance.Direct);
            RemoveInstanced(state.ReflectScene, instance.Reflect);
        }
        state.Instances.clear();
        for (IPLInstancedMesh& mesh : state.RetiredReflect)
            RemoveInstanced(state.ReflectScene, mesh);
        state.RetiredReflect.clear();
        // The built meshes go too (a reimported mesh is read again next time); reads still running finish first.
        for (auto it = state.Meshes.begin(); it != state.Meshes.end();)
        {
            if (it->second.Reading)
            {
                ++it;
                continue;
            }
            ReleaseSubScene(it->second);
            it = state.Meshes.erase(it);
        }
        iplSceneCommit(state.Scene);
        iplSimulatorCommit(state.Simulator);
        iplSceneCommit(state.ReflectScene);
        iplSimulatorCommit(state.ReflectSimulator);
        state.PoolHasResults.fill(false);
        state.SceneDirty = false;
        state.ReflectDirty = false;
    }

    void SpatialAudio::UpdateScene(const glm::dvec3& listener)
    {
        if (!s_State)
            return;
        State& state = *s_State;
        // Meshes read since: built, and the instances waiting for them placed.
        bool built = false;
        for (auto& [key, sub] : state.Meshes)
        {
            if (!sub.Reading || !sub.Read.IsReady())
                continue;
            sub.Reading = false;
            const std::shared_ptr<AcousticMesh> mesh = sub.Read.Get();
            sub.Read = {};
            sub.Failed = !mesh || !BuildSubScene(state, sub, *mesh);
            built |= !sub.Failed;
        }
        if (built)
            for (auto& [key, instance] : state.Instances)
                PlaceDirect(state, instance);

        // The origin follows the listener (every instance's transform then moves with it) -- between reflection runs only.
        const double metersPerUnit = static_cast<double>(WorldUnits::ToMeters(1.0f));
        if (glm::length(listener - state.Origin) * metersPerUnit > kRebaseMeters && Idle(state))
        {
            state.Origin = listener;
            for (auto& [key, instance] : state.Instances)
            {
                if (instance.Direct)
                    iplInstancedMeshUpdateTransform(instance.Direct, state.Scene, AcousticTransform(state, instance.World));
                instance.ReflectMoved = true;
            }
            state.SceneDirty = true;
            state.ReflectDirty = true;
        }
        if (state.SceneDirty)
        {
            iplSceneCommit(state.Scene);
            state.SceneDirty = false;
            state.SimulatorDirty = true;
        }
        if (Idle(state))
            SyncReflectScene(state);
    }

    SpatialAudio::SceneStats SpatialAudio::GetSceneStats()
    {
        SceneStats stats;
        if (!s_State)
            return stats;
        for (const auto& [key, sub] : s_State->Meshes)
        {
            stats.Meshes += sub.Scene ? 1u : 0u;
            stats.Building += sub.Reading ? 1u : 0u;
        }
        for (const auto& [key, instance] : s_State->Instances)
        {
            if (!instance.Direct)
                continue;
            ++stats.Instances;
            if (const auto found = s_State->Meshes.find(instance.MeshKey); found != s_State->Meshes.end())
                stats.Triangles += found->second.Triangles;
        }
        return stats;
    }

    void SpatialAudio::MaterialsChanged() { ++s_MaterialVersion; }
    uint32_t SpatialAudio::GetMaterialVersion() { return s_MaterialVersion; }

    IPLSource SpatialAudio::CreateSource()
    {
        if (!s_State)
            return nullptr;
        IPLSourceSettings settings{};
        settings.flags = IPL_SIMULATIONFLAGS_DIRECT;
        IPLSource source = nullptr;
        if (iplSourceCreate(s_State->Simulator, &settings, &source) != IPL_STATUS_SUCCESS)
            return nullptr;
        iplSourceAdd(source, s_State->Simulator);
        s_State->SimulatorDirty = true;
        return source;
    }

    void SpatialAudio::ReleaseSource(IPLSource& source)
    {
        if (!s_State || !source)
            return;
        iplSourceRemove(source, s_State->Simulator);
        iplSourceRelease(&source);
        s_State->SimulatorDirty = true;
    }

    IPLVector3 SpatialAudio::ToAcoustic(const glm::dvec3& world)
    {
        const glm::dvec3 relative = (world - (s_State ? s_State->Origin : glm::dvec3(0.0))) * static_cast<double>(WorldUnits::ToMeters(1.0f));
        return { static_cast<float>(relative.x), static_cast<float>(relative.y), static_cast<float>(relative.z) };
    }

    void SpatialAudio::SetListener(const glm::dvec3& position, const glm::quat& orientation)
    {
        if (!s_State)
            return;
        s_State->ListenerPosition = position;
        s_State->ListenerOrientation = orientation;
    }

    void SpatialAudio::SetSourceInputs(IPLSource source, const SourceInputs& inputs)
    {
        if (!s_State || !source)
            return;
        IPLSimulationInputs simulation{};
        simulation.flags = IPL_SIMULATIONFLAGS_DIRECT;
        simulation.directFlags = static_cast<IPLDirectSimulationFlags>(
            (inputs.Occlusion ? (IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION) : 0) |
            (inputs.AirAbsorption ? IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION : 0));
        simulation.source.origin = ToAcoustic(inputs.Position);
        simulation.source.right = { 1.0f, 0.0f, 0.0f };
        simulation.source.up = { 0.0f, 1.0f, 0.0f };
        simulation.source.ahead = { 0.0f, 0.0f, -1.0f };
        simulation.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        simulation.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
        simulation.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
        simulation.occlusionRadius = std::max(inputs.Radius, 0.01f);
        simulation.numOcclusionSamples = kOcclusionSamples;
        simulation.numTransmissionRays = kTransmissionRays;
        iplSourceSetInputs(source, IPL_SIMULATIONFLAGS_DIRECT, &simulation);
    }

    void SpatialAudio::RunDirect()
    {
        if (!s_State)
            return;
        State& state = *s_State;
        if (state.SimulatorDirty)
        {
            iplSimulatorCommit(state.Simulator);
            state.SimulatorDirty = false;
        }
        IPLSimulationSharedInputs shared{};
        shared.listener = ListenerSpace(state, ToAcoustic(state.ListenerPosition));
        iplSimulatorSetSharedInputs(state.Simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
        iplSimulatorRunDirect(state.Simulator);
    }

    SpatialAudio::SourceOutputs SpatialAudio::GetSourceOutputs(IPLSource source)
    {
        SourceOutputs result;
        if (!s_State || !source)
            return result;
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);
        result.Occlusion = outputs.direct.occlusion;
        for (int band = 0; band < 3; ++band)
        {
            result.Transmission[band] = outputs.direct.transmission[band];
            result.AirAbsorption[band] = outputs.direct.airAbsorption[band];
        }
        return result;
    }

    void SpatialAudio::SetReflectionSettings(const ReflectionSettings& settings)
    {
        if (!s_State)
            return;
        ReflectionSettings& stored = s_State->Settings;
        stored = settings;
        stored.Rays = std::clamp(stored.Rays, 256, kMaxRays);
        stored.Bounces = std::clamp(stored.Bounces, 1, 32);
        stored.Duration = std::clamp(stored.Duration, 0.1f, MaxReflectionDuration);
        stored.MaxSources = std::clamp(stored.MaxSources, 0, MaxReflectionSources);
        stored.Interval = std::max(stored.Interval, 0.0f);
    }

    SpatialAudio::ReflectionSettings SpatialAudio::GetReflectionSettings()
    {
        return s_State ? s_State->Settings : ReflectionSettings{};
    }

    bool SpatialAudio::ReflectionsIdle() { return s_State && Idle(*s_State); }

    bool SpatialAudio::TakeReflectionResults()
    {
        return s_State && Idle(*s_State) && s_State->ReflectFinished.exchange(false, std::memory_order_acq_rel);
    }

    void SpatialAudio::StartReflections(const std::vector<std::pair<int32_t, glm::dvec3>>& sources)
    {
        if (!s_State || !Idle(*s_State))
            return;
        State& state = *s_State;
        SyncReflectScene(state);
        // The pool slots in this run in the simulator, the others out.
        std::array<bool, MaxReflectionSources> wanted{};
        for (const auto& [slot, position] : sources)
            if (slot >= 0 && slot < MaxReflectionSources)
                wanted[slot] = true;
        bool changed = false;
        for (int32_t slot = 0; slot < MaxReflectionSources; ++slot)
        {
            if (wanted[slot] == state.PoolAdded[slot])
                continue;
            if (wanted[slot])
                iplSourceAdd(state.Pool[slot], state.ReflectSimulator);
            else
            {
                iplSourceRemove(state.Pool[slot], state.ReflectSimulator);
                state.PoolHasResults[slot] = false;
            }
            state.PoolAdded[slot] = wanted[slot];
            changed = true;
        }
        if (changed)
            iplSimulatorCommit(state.ReflectSimulator);
        if (sources.empty())
            return;
        for (const auto& [slot, position] : sources)
        {
            if (slot < 0 || slot >= MaxReflectionSources)
                continue;
            IPLSimulationInputs inputs{};
            inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            inputs.source.origin = ToAcoustic(position);
            inputs.source.right = { 1.0f, 0.0f, 0.0f };
            inputs.source.up = { 0.0f, 1.0f, 0.0f };
            inputs.source.ahead = { 0.0f, 0.0f, -1.0f };
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
            inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
            inputs.reverbScale[0] = inputs.reverbScale[1] = inputs.reverbScale[2] = 1.0f;
            inputs.baked = IPL_FALSE;
            iplSourceSetInputs(state.Pool[slot], IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
        }
        IPLSimulationSharedInputs shared{};
        shared.listener = ListenerSpace(state, ToAcoustic(state.ListenerPosition));
        shared.numRays = state.Settings.Rays;
        shared.numBounces = state.Settings.Bounces;
        shared.duration = state.Settings.Duration;
        shared.order = ReflectionOrder;
        shared.irradianceMinDistance = 1.0f;
        iplSimulatorSetSharedInputs(state.ReflectSimulator, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);
        for (const auto& [slot, position] : sources)
            if (slot >= 0 && slot < MaxReflectionSources)
                state.PoolHasResults[slot] = true; // (results once this run finishes)
        state.ReflectRunning.store(true, std::memory_order_release);
        {
            std::scoped_lock lock(state.ReflectMutex);
            state.ReflectRequested = true;
        }
        state.ReflectWake.notify_one();
    }

    bool SpatialAudio::GetReflectionParams(int32_t slot, IPLReflectionEffectParams& out)
    {
        if (!s_State || slot < 0 || slot >= MaxReflectionSources || !s_State->PoolHasResults[slot] || !s_State->PoolAdded[slot])
            return false;
        IPLSimulationOutputs outputs{};
        iplSourceGetOutputs(s_State->Pool[slot], IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
        out = outputs.reflections;
        out.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        out.numChannels = ReflectionChannels;
        if (out.irSize <= 0)
            out.irSize = static_cast<IPLint32>(s_State->Settings.Duration * static_cast<float>(s_State->SampleRate));
        return out.ir != nullptr;
    }

    float SpatialAudio::GetLastReflectionMilliseconds()
    {
        return s_State ? s_State->ReflectMilliseconds.load(std::memory_order_relaxed) : 0.0f;
    }
}
