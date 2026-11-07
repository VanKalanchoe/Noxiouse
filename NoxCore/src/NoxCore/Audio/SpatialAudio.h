#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <phonon.h>

#include "NoxCore/Asset/Asset.h"
#include "AudioTypes.h"

namespace Nox
{
    // Triangles for the acoustic scene, in their own space (a mesh asset's: world units), each with its acoustic material.
    struct AcousticMesh
    {
        std::vector<glm::vec3> Vertices;
        std::vector<uint32_t> Indices;
        std::vector<AcousticMaterial> TriangleMaterials;
    };

    // A mesh asset's acoustic triangles, read and simplified on a worker (AcousticMeshReader.cpp): poll IsReady, then Get once.
    struct AcousticMeshRead
    {
        std::function<bool()> IsReady;
        std::function<std::shared_ptr<AcousticMesh>()> Get; // null: the mesh could not be read
    };
    AcousticMeshRead StartAcousticMeshRead(AssetHandle mesh, uint32_t firstSubmesh, uint32_t submeshCount, std::vector<AcousticMaterial> materials);

    // Spatial audio through Steam Audio (docs/Audio_Architecture_Plan_2026.md §6, phase A4; Valve's library, as Unity's and Unreal's
    // Steam Audio plugins use it): the context and HRTF the mixer's per-voice effects run with, the acoustic scene built from the
    // level's static meshes, and the simulator that traces occlusion / transmission (and air absorption) for each 3D voice.
    //
    // The acoustic scene: one Steam Audio sub-scene per mesh asset and set of acoustic materials -- its triangles read from the cooked
    // mesh and simplified on a worker --, placed by an instanced mesh per entity (moving one costs a transform). Positions are meters
    // around an origin that follows the listener (large worlds: Steam Audio works in floats).
    //
    // Game thread only, except Context() / Hrtf() (thread-safe objects the mixer thread's effects use).
    class SpatialAudio
    {
    public:
        // After the output device opened (its rate; the mixer's block size). False when Steam Audio could not start: voices are
        // then panned by the engine itself and nothing is occluded.
        static bool Init(uint32_t sampleRate, uint32_t frameSize);
        static void Shutdown();
        static bool IsAvailable();

        static IPLContext Context();
        static IPLHRTF Hrtf();

        // ---- The acoustic scene (the running scene's static meshes) ----------------------------------------------------------
        struct Instance
        {
            AssetHandle Mesh = 0;
            uint32_t FirstSubmesh = 0;
            uint32_t SubmeshCount = 1;               // UINT32_MAX: all
            std::vector<AcousticMaterial> Materials; // per submesh of the range (resolved: never Auto)
            glm::dmat4 World{ 1.0 };                 // world units
        };
        // By a key of the caller's (an entity's id): added, or updated (a new transform moves it; other changes rebuild it).
        static void SetInstance(uint64_t key, const Instance& instance);
        static void RemoveInstance(uint64_t key);
        static void ClearInstances();
        // Triangles made by code (tests, procedural geometry), placed like a mesh asset's.
        static void SetMeshInstance(uint64_t key, std::shared_ptr<const AcousticMesh> mesh, const glm::dmat4& world);
        // Once a frame before the voices are simulated: finished meshes go in, the origin follows the listener, changes committed.
        static void UpdateScene(const glm::dvec3& listener);
        struct SceneStats
        {
            uint32_t Instances = 0;
            uint32_t Meshes = 0;      // built sub-scenes
            uint32_t Building = 0;    // meshes still being read / simplified
            uint64_t Triangles = 0;   // placed (instances x their mesh's)
        };
        static SceneStats GetSceneStats();
        // A material's acoustic material was changed: the scene resolves its meshes' materials again.
        static void MaterialsChanged();
        static uint32_t GetMaterialVersion();

        // ---- Simulation (the engine, for its 3D voices) ------------------------------------------------------------------------
        static IPLSource CreateSource();
        static void ReleaseSource(IPLSource& source);
        struct SourceInputs
        {
            glm::dvec3 Position{ 0.0 }; // world units
            bool Occlusion = true;
            bool AirAbsorption = true;
            float Radius = 0.5f;        // meters
        };
        struct SourceOutputs
        {
            float Occlusion = 1.0f;     // 1: nothing in the way
            float Transmission[3] = { 1.0f, 1.0f, 1.0f };
            float AirAbsorption[3] = { 1.0f, 1.0f, 1.0f };
        };
        static void SetListener(const glm::dvec3& position, const glm::quat& orientation);
        static void SetSourceInputs(IPLSource source, const SourceInputs& inputs);
        // Traces every source's occlusion and transmission (a few rays each, against the committed scene).
        static void RunDirect();
        static SourceOutputs GetSourceOutputs(IPLSource source);

        // ---- Reflections (A4b): rays bounced off the scene give each nearby voice its early reflections and reverb tail ---------
        // Traced on Steam Audio's own reflection thread against a second copy of the acoustic scene (changed only between runs, so
        // occlusion above never waits for it), for a budget of the voices nearest the listener: each gets the reflections of one of
        // a pool of reflection sources. Each voice's response (ambisonics, order 1) is convolved through Steam Audio's reflection
        // mixer, decoded once for all.
        struct ReflectionSettings
        {
            bool Enabled = true;
            int32_t Rays = 4096;         // rays from the listener each run
            int32_t Bounces = 4;
            float Duration = 1.0f;       // seconds of response simulated and convolved (up to MaxReflectionDuration)
            int32_t MaxSources = 4;      // voices with reflections at once (up to MaxReflectionSources): ~0.15-0.3 ms of mixing each
            float Interval = 0.1f;       // seconds between runs at most often
        };
        static constexpr int32_t MaxReflectionSources = 16;
        static constexpr float MaxReflectionDuration = 1.5f;
        static constexpr int32_t ReflectionOrder = 1;
        static constexpr int32_t ReflectionChannels = 4; // (order + 1)^2
        static void SetReflectionSettings(const ReflectionSettings& settings);
        static ReflectionSettings GetReflectionSettings();
        // The reflection thread is not running (a new run can start; its last results can be read).
        static bool ReflectionsIdle();
        // True once after each finished run: GetReflectionParams then gives its results.
        static bool TakeReflectionResults();
        // Starts a run (when idle) for the pool slots given a position (world units); the others are left out.
        static void StartReflections(const std::vector<std::pair<int32_t, glm::dvec3>>& sources);
        // The effect parameters a pool slot's last run produced (false: none yet).
        static bool GetReflectionParams(int32_t slot, IPLReflectionEffectParams& out);
        static float GetLastReflectionMilliseconds();

        // Meters around the origin (what Steam Audio sees) from world units.
        static IPLVector3 ToAcoustic(const glm::dvec3& world);
    };

    // A material's acoustic material when it says Auto: from how it looks (glass, metal, the rest Generic).
    AcousticMaterial ResolveAcousticMaterial(AcousticMaterial chosen, float metallic, float transmission, bool blended);
}
