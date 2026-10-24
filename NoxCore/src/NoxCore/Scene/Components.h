#pragma once
#include "WorldPartition.h"
#include <entt/entt.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <optional>
#include "string"
#include <unordered_map>

#include "SceneCamera.h"

#include "NoxCore/Core/UUID.h"
#include "NoxCore/Core/WorldUnits.h"

#include "box2d/box2d.h"
#include "NoxCore/Animation/Animator.h"
#include "NoxCore/Animation/AnimationGraphInstance.h"

#include "NoxCore/Renderer/Font.h"

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Scripting/ScriptTypes.h"
#include "NoxCore/Renderer/DataTypes.h"

namespace Nox
{
    struct IDComponent
    {
        UUID ID;
        IDComponent() = default;
        IDComponent(const IDComponent&) = default;
        IDComponent(const UUID& uuid) : ID(uuid) {}
    };

    struct TagComponent
    {
        std::string Tag;
        TagComponent() = default;
        TagComponent(const TagComponent&) = default;
        TagComponent(const std::string& tag) : Tag(tag) {}
    };
    
    struct TransformComponent
    {
        glm::vec3 Translation = {0.0f, 0.0f, 0.0f};
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale = {1.0f, 1.0f, 1.0f};

        TransformComponent() = default;
        TransformComponent(const TransformComponent&) = default;
        TransformComponent(const glm::vec3& translation) : Translation(translation) {}

        glm::mat4 GetTransform() const
        {
            glm::mat4 rotation = glm::toMat4(glm::quat(Rotation));

            return glm::translate(glm::mat4(1.0f), Translation)
                * rotation
                * glm::scale(glm::mat4(1.0f), Scale);
        }
    };
    
    // Cached World Matrix for Scene Graph
    struct WorldTransformComponent
    {
        glm::mat4 WorldMatrix{ 1.0f };

        WorldTransformComponent() = default;
        WorldTransformComponent(const WorldTransformComponent&) = default;
        WorldTransformComponent(const glm::mat4& world) : WorldMatrix(world) {}

        operator const glm::mat4&() const { return WorldMatrix; }
    };

    // Scene Graph Hierarchy Links (UUID-based for safety)
    struct RelationshipComponent
    {
        UUID Parent = 0;
        std::vector<UUID> Children;

        RelationshipComponent() = default;
        RelationshipComponent(const RelationshipComponent&) = default;
    };

    // The outliner folder an entity is filed under, like Unreal's: a path ("Bistro/Props") that only groups rows in the
    // hierarchy panel. Not an entity, no transform, no parenting. Empty / absent = the top level.
    struct FolderComponent
    {
        std::string Path;

        FolderComponent() = default;
        FolderComponent(const FolderComponent&) = default;
        FolderComponent(const std::string& path) : Path(path) {}
    };

    // Dirty Transform Tracking
    // Present on every entity (added at creation). Set instead of added/removed, so marking and clearing are plain
    // writes that tasks can do without structural registry changes.
    struct DirtyTransformComponent
    {
        bool isDirty = true;
    };
    
    struct MeshComponent
    {
        AssetHandle Mesh = 0;
        uint32_t SubmeshIndex = 0;
        uint32_t SubmeshCount = 1;
        
        MeshComponent() = default;
        MeshComponent(const MeshComponent&) = default;
        MeshComponent(AssetHandle mesh) : Mesh(mesh) {};
    };
    
    struct MaterialComponent
    {
        // Per-slot overrides of the mesh's materials (indexed by submesh, UE's OverrideMaterials). A zero handle, or no
        // entry, uses the mesh primitive's own .nmat; without overrides the list is empty and nothing is serialized.
        std::vector<AssetHandle> MaterialAssets;

        MaterialComponent() = default;
        MaterialComponent(const MaterialComponent&) = default;
    };
    
    // What a model instance changed on one of its nodes; everything else comes from the model's cooked node data.
    struct ModelNodeOverride
    {
        uint32_t NodeIndex = 0;
        std::optional<TransformComponent> Transform;
        std::optional<std::string> Name;
        std::vector<AssetHandle> MaterialAssets; // MaterialComponent overrides, empty for none
    };

    // Root of an imported model placed in a scene (UE's Packed Level Actor / Unity's model prefab instance): its node
    // entities are spawned from the model's cooked node data once the model is loaded, and only this component is saved
    // for them -- removed nodes and per-node overrides (§5.11.5).
    struct ModelInstanceComponent
    {
        AssetHandle Model = 0;
        std::vector<uint32_t> RemovedNodes;       // glTF node indices deleted from this instance
        std::vector<ModelNodeOverride> Overrides; // to apply at spawn; the node entities hold them once spawned
        bool AtFileLayout = false;                // a per-mesh asset: spawn the file's instances of it, each where the file had it
        bool Spawned = false;                     // runtime only

        ModelInstanceComponent() = default;
        ModelInstanceComponent(const ModelInstanceComponent&) = default;
    };

    // A node entity spawned from a model instance (never saved: it is spawned again on load).
    struct ModelNodeComponent
    {
        UUID Instance = 0; // the instance root
        uint32_t NodeIndex = 0;

        ModelNodeComponent() = default;
        ModelNodeComponent(const ModelNodeComponent&) = default;
    };

    // One field of one component of one entity of a prefab instance that differs from the prefab (docs/Prefab_Architecture_Plan_2026.md
    // P5). Found by comparing the entity's serialized components with the prefab's, so it works for every component; applied by
    // patching the prefab's text before the entities are built.
    struct PrefabPropertyOverride
    {
        uint64_t Entity = 0;   // the entity's id in the prefab file
        std::string Component; // the component's YAML key, e.g. "RigidBody3DComponent"
        std::string Field;     // a field of it, e.g. "AngularDamping"
        std::string Value;     // its value as YAML text, e.g. "0.9" or "[1, 0, 0]"

        bool SameField(const PrefabPropertyOverride& other) const
        {
            return Entity == other.Entity && Component == other.Component && Field == other.Field;
        }
    };

    // A change to the STRUCTURE of a prefab instance (P6a): an entity of the prefab that was deleted on this instance, a component of
    // one of its entities that was removed, or one that was added. (Added entities are ordinary scene entities below the spawned
    // ones and save with the scene.) Found by comparing the entity's serialized component keys with the prefab's.
    struct PrefabStructureChange
    {
        enum class ChangeKind : uint8_t { RemovedEntity, RemovedComponent, AddedComponent };

        ChangeKind Kind = ChangeKind::RemovedEntity;
        uint64_t Entity = 0;   // the entity's id in the prefab file
        std::string Component; // the component's YAML key (unused for a removed entity)
        std::string Value;     // an added component: its fields as YAML text

        bool SameChange(const PrefabStructureChange& other) const
        {
            return Kind == other.Kind && Entity == other.Entity && Component == other.Component;
        }
    };

    // The differences of a prefab instance that sits INSIDE another prefab's instance (nested prefabs, P6b). The inner instance's own
    // entities are spawned, not saved, so the outer instance saves what differs from what the prefab file says about the inner one.
    struct PrefabNestedChange
    {
        uint64_t Entity = 0; // the inner instance's root: its id in the outer prefab's file
        std::vector<PrefabPropertyOverride> Overrides;
        std::vector<PrefabStructureChange> Structure;
        std::vector<PrefabNestedChange> Nested; // instances inside the inner one
    };

    // Root of a prefab instance placed in a scene (Stage C, docs/Prefab_Architecture_Plan_2026.md): the entities of the
    // prefab (.nprefab) are spawned from the asset once it is loaded (PrefabInstance::SpawnPending), and only this component,
    // the root's own name/transform/parent/folder are saved for them.
    struct PrefabInstanceComponent
    {
        AssetHandle Prefab = 0;
        bool Spawned = false; // runtime only
        // A freshly placed instance takes the rotation and scale of the prefab's root when it spawns (its position is where it
        // was placed); saved instances keep their own. Runtime only, cleared by the spawn.
        bool InitTransform = false;
        // What differs from the prefab: applied when the instance spawns. Once it is spawned the entities hold the values and this
        // is only what was captured last (PrefabInstance::CollectOverrides works it out from the entities).
        std::vector<PrefabPropertyOverride> Overrides;
        std::vector<PrefabStructureChange> Structure; // deleted entities, removed and added components (same rule as Overrides)
        // The name a spawned entity had when it was deleted on this instance (prefab file id -> name): shown for the deletion, since the
        // entity itself is gone. Filled by Scene::DestroyEntity and from `Structure` when the instance spawns.
        std::unordered_map<uint64_t, std::string> RemovedNames;
        std::vector<PrefabNestedChange> Nested; // prefab instances inside this one that differ from the prefab's own text of them

        PrefabInstanceComponent() = default;
        PrefabInstanceComponent(const PrefabInstanceComponent&) = default;
    };

    // An entity spawned from a prefab instance (never saved: it is spawned again on load).
    struct PrefabNodeComponent
    {
        UUID Instance = 0;    // the instance root
        uint64_t LocalId = 0; // the entity's id in the prefab file

        PrefabNodeComponent() = default;
        PrefabNodeComponent(const PrefabNodeComponent&) = default;
    };

    // Following KHR_Punctual
    struct DirectionalLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float AngularDiameter = 0.5357f; // In degrees (UE5 default: 0.5357 deg = astronomical Sun diameter)
        uint32_t ShadowSamples = 1;      // 1 for fast stochastic + DLSS/NRD, 2-8 for high quality

        DirectionalLightComponent() = default;
        DirectionalLightComponent(const DirectionalLightComponent&) = default;
    };

    struct PointLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 5.0f;
        float Range = WorldUnits::FromMeters(10.0f);
        float Radius = WorldUnits::FromMeters(0.05f); // light source radius (UE5 default: 0.05 m = 5 cm bulb)
        uint32_t ShadowSamples = 1;

        PointLightComponent() = default;
        PointLightComponent(const PointLightComponent&) = default;
    };

    struct SpotLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 10.0f;
        float Range = WorldUnits::FromMeters(15.0f);
        float InnerAngle = 20.0f; // degrees
        float OuterAngle = 35.0f; // degrees
        float Radius = WorldUnits::FromMeters(0.05f); // light source radius (UE5 default: 0.05 m = 5 cm bulb)
        uint32_t ShadowSamples = 1;

        SpotLightComponent() = default;
        SpotLightComponent(const SpotLightComponent&) = default;
    };

    // A rectangle that emits light from its front (the entity's -Z, like a spot light's direction): soft area light and soft ray traced
    // shadows, a light panel, a window, a TV. Width / Height are world units along the entity's X / Y (scaled with it); Intensity is the
    // brightness of its surface (radiance): a 1 x 0.5 m panel of Intensity 10 lights a wall 2 m away about as a default point light.
    struct RectLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 10.0f;
        float Width = WorldUnits::FromMeters(1.0f);
        float Height = WorldUnits::FromMeters(0.5f);
        float Range = 0.0f; // world units; 0 = unlimited

        RectLightComponent() = default;
        RectLightComponent(const RectLightComponent&) = default;
    };

    // A disc that emits light from its front (the entity's -Z): a round area light -- a ceiling downlight, a porthole, a lamp
    // shade's opening. Radius in world units in the entity's X / Y plane (scaled with it); Intensity is its surface's radiance,
    // like the rect light's.
    struct DiscLightComponent
    {
        glm::vec3 Color{ 1.0f, 1.0f, 1.0f };
        float Intensity = 10.0f;
        float Radius = WorldUnits::FromMeters(0.15f);
        float Range = 0.0f; // world units; 0 = unlimited

        DiscLightComponent() = default;
        DiscLightComponent(const DiscLightComponent&) = default;
    };

    // Image-based environment lighting. The renderer will preprocess TexturePath into its
    // diffuse irradiance/specular IBL maps; this is deliberately separate from punctual
    // directional/point/spot lights.
    enum class EnvironmentSource : uint8_t
    {
        Texture = 0,   // an HDR panorama or DDS cubemap (TexturePath)
        ProceduralSky, // Earth's atmosphere lit by the scene's first directional light (the sun)
    };

    // The procedural sky's own sun while the scene has no Directional Light: the sky draws it and it lights the scene, so
    // choosing Procedural Sky alone gives daylight. ~54 degrees up; intensity 10 gives a daylight sky at exposure 1.
    inline glm::vec3 DefaultSkySunDirection() { return glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f)); }
    inline constexpr float DefaultSkySunIntensity = 10.0f;
    // A light's rotation whose -Z (the way it shines) points down from that sun.
    inline glm::quat DefaultSkySunRotation() { return glm::quatLookAt(-DefaultSkySunDirection(), glm::vec3(0.0f, 1.0f, 0.0f)); }

    struct EnvironmentLightComponent
    {
        EnvironmentSource Source = EnvironmentSource::Texture;
        std::string TexturePath;
        glm::vec3 RadianceScale{ 1.0f, 1.0f, 1.0f };
        float Rotation = 0.0f; // radians around the world Y axis
        bool Enabled = true;

        // Procedural sky (Source == ProceduralSky); scales multiply Earth's atmosphere.
        float RayleighScale = 1.0f;
        float MieScale = 1.0f;
        float OzoneScale = 1.0f;
        float MieAnisotropy = 0.8f;
        glm::vec3 GroundAlbedo{ 0.3f, 0.3f, 0.3f };
        float AltitudeKm = 0.0f;
        float AerialPerspectiveScale = 1.0f; // haze on the scene: multiplies view distances (UE's "distance scale")
        // Volumetric clouds (procedural sky): a cumulus layer, raymarched into the sky.
        bool Clouds = true;
        float CloudCoverage = 0.35f;      // 0 = clear, 1 = overcast
        float CloudDensity = 1.0f;
        float CloudBottomKm = 1.5f;
        float CloudThicknessKm = 2.0f;
        float CloudScaleKm = 8.0f;        // larger = bigger clouds
        glm::vec2 CloudOffsetKm{ 0.0f, 0.0f }; // moves the clouds
        glm::vec2 CloudWind{ 0.0f, 0.0f };     // m/s along x and z: the clouds drift (the sky re-renders over 8 frames while they do)

        // The sun (and moon) light the scene as seen through the air: yellow-white at noon, red at sunset, gone below the horizon.
        bool SunThroughAtmosphere = true;
        // Night (procedural sky): a moon lights the scene and the sky once the sun is down, and the stars come out.
        bool Moon = true;
        float MoonIntensity = 0.004f;           // a full moon's light, in the sun's units (sun 10 = daylight); the real moon is ~1/400000
                                                // of the sun: this is ~100x that so moonlit scenes read with auto exposure, yet stay a
                                                // night (lamps outshine it). Was 0.03: a grey day once auto exposure made it up.
        glm::vec3 MoonColor{ 0.75f, 0.85f, 1.0f };
        float MoonAngularDiameter = 0.53f;      // degrees (the real moon's)
        bool MoonOppositeSun = true;            // always opposite the sun: a full moon; else at MoonElevation / MoonAzimuth (degrees,
        float MoonElevation = 40.0f;            // azimuth from +Z towards +X), its phase from where the sun is
        float MoonAzimuth = 200.0f;
        bool Stars = true;
        float StarIntensity = 1.0f;

        // Exponential height fog (UE's Exponential Height Fog), with either source: thick low down and at the horizon.
        bool HeightFog = false;
        float FogDensity = 0.5f;          // extinction per km at FogHeight
        float FogHeightFalloff = 5.0f;    // per km: density halves every ln(2) / falloff km upwards (5: ~140 m)
        float FogHeight = 0.0f;           // world units
        float FogStartDistance = 0.0f;    // world units
        float FogMaxOpacity = 1.0f;
        glm::vec3 FogColor{ 1.0f, 1.0f, 1.0f }; // tint of the environment's light (FogFromEnvironment) or the fog's radiance
        bool FogFromEnvironment = true;
        float FogSunScattering = 0.05f;   // glow towards the sun: the sun's illuminance times this
        float FogSunExponent = 8.0f;      // how tight that glow is

        EnvironmentLightComponent() = default;
        EnvironmentLightComponent(const EnvironmentLightComponent&) = default;
    };
    
    // Holds runtime animation state (tracks current time, playing animation, bone matrices)
    struct AnimatorComponent
    {
        Animator Animator;
        AssetHandle Animation = 0;
        AssetHandle Skeleton = 0;
        // Entity for every source glTF node, indexed by glTF node index. Stored as UUIDs, not entt
        // handles, so the mapping survives scene save/load and the Play-mode scene copy.
        //   Animation set -> the clip drives these node entities' transforms (glTF node animation).
        //   Skeleton set  -> this entity's skin reads its joints' world transforms from these
        //                    entities (glTF skinning: inverse(meshWorld) * jointWorld * inverseBind).
        // Empty means the legacy self-contained skeleton evaluation.
        std::vector<UUID> NodeEntities;
        bool Playing = true;

        // Drives the skeleton through a compiled NodeGraph (.nanimgraph) instead of Animator's single clip --
        // see docs/Animation_Graph_Architecture_Plan_2026.md. A mode on this component rather than a separate
        // component type (matching UE5's AnimationSingleNode/AnimationBlueprint split on USkeletalMeshComponent):
        // it needs the exact same Skeleton/NodeEntities/Playing/SkinMatrices machinery Animator already has,
        // including the joint-entity-driven skinning path real imported skeletal meshes use, so duplicating all
        // of that into a second component both bloats the ECS and silently drops that path. 0 = single-clip mode.
        AssetHandle Graph = 0;
        AnimationGraphInstance GraphInstance; // runtime only: node playheads, parameter overrides, last evaluated pose

        // Graph mode: the root bone's horizontal movement moves this entity instead of the skeleton (walk / run clips that
        // move forward). RootBone empty = the skin's first joint.
        bool RootMotion = false;
        std::string RootBone;
        // Runtime only: the notifies the last animation update crossed, handed to the scripts as OnAnimationEvent(name).
        std::vector<std::string> FiredNotifies;

        // Runtime only: skinning matrices rebuilt from the joint entities each frame.
        std::vector<glm::mat4> SkinMatrices;
        // Runtime only: final bone matrices for Graph mode's self-contained path (Skeleton set, NodeEntities
        // empty) -- Animator's own GetFinalBoneTransforms() serves the equivalent single-clip case.
        std::vector<glm::mat4> GraphFinalBoneTransforms;
        // Runtime only: Graph mode's self-contained path, every node's model-space matrix by node index (bone sockets read it).
        std::vector<glm::mat4> GraphGlobalMatrices;

        AnimatorComponent() = default;
        AnimatorComponent(const AnimatorComponent&) = default;
        AnimatorComponent(const Ref<AnimationSequence>& animation)
            : Animator(animation) {}
    };

    // A bone socket (UE's sockets, Unity's attaching to a bone): every frame this entity is placed on a bone of the nearest animator
    // above it -- a sword in a hand, a hat on a head -- with an offset in the bone's space. Works for every animator mode (single
    // clip, graph, a model's joint entities).
    struct BoneSocketComponent
    {
        std::string Bone;                        // the skeleton node's name
        glm::vec3 Offset{ 0.0f };                // in the bone's space (world units)
        glm::vec3 RotationOffset{ 0.0f };        // radians
        glm::vec3 Scale{ 1.0f };

        // Runtime only: the node index the name was found at, for the skeleton it was looked up in.
        int32_t CachedIndex = -1;
        AssetHandle CachedSkeleton = 0;
    };

    // Plays a sequencer timeline (Animation/Timeline.h) on this entity and the entities below it: its tracks name their targets
    // by path from here. Plays in Play and Simulate (PlayOnStart, or scripts: Play / Stop); the Timeline window previews it while
    // editing.
    struct TimelinePlayerComponent
    {
        AssetHandle Timeline = 0;
        bool PlayOnStart = true;
        float Speed = 1.0f;

        // Runtime only.
        float Time = 0.0f;
        bool Playing = false;
        std::vector<std::string> FiredEvents; // the event keys the last update passed, handed to the scripts as OnAnimationEvent(name)

        TimelinePlayerComponent() = default;
        TimelinePlayerComponent(const TimelinePlayerComponent&) = default;
    };

    // A particle emitter (docs/Hybrid_Renderer_Plan_2026-10.md §6, V1 / V2): particles spawned around the entity and simulated on
    // the CPU (Scene/ParticleSystem.h: gravity, drag, spin), drawn as camera-facing quads -- a texture (a flipbook sheet of
    // columns x rows frames) or soft round dots -- added (fire, sparks) or alpha blended (smoke, dust), optionally lit by the
    // scene's lights and environment, faded where they meet geometry. Runs in Edit, Simulate and Play.
    // Lengths are in meters (scaled by the project's world unit), angles in degrees.
    enum class ParticleShape : uint8_t
    {
        Point,
        Sphere, // inside a sphere of ShapeSize.x radius
        Box,    // inside a box of ShapeSize half extents
        Cone    // from a disc of ShapeSize.x radius, directions within ConeAngle of the emitter's +Y
    };

    enum class ParticleBlend : uint8_t
    {
        Additive, // light added (fire, sparks, magic): order-free
        Alpha     // covering (smoke, dust): sorted back to front
    };

    // What an emitter is doing (runtime only, never saved).
    struct ParticleSimulation
    {
        struct Particle
        {
            glm::vec3 Position{ 0.0f }; // world units: world space, or the emitter's space (LocalSpace)
            float Age = 0.0f;
            glm::vec3 Velocity{ 0.0f }; // world units per second
            float Lifetime = 1.0f;
            float Rotation = 0.0f;      // radians
            float Spin = 0.0f;          // radians per second
            float SizeScale = 1.0f;     // random per particle (SizeRandom)
            float Variation = 0.0f;     // random per particle in [0, 1): the flipbook's start frame, the color pick
        };
        std::vector<Particle> Particles;
        float Time = 0.0f;              // into the current loop / the one shot
        float SpawnCarry = 0.0f;        // a fraction of a particle owed by the spawn rate
        uint32_t Random = 0x9E3779B9u;
        bool BurstDone = false;
        uint32_t PendingBurst = 0;      // Emit(count) from scripts, spawned on the next update
        bool Started = false;
    };

    // A key of a particle's color / size over its life (Time 0 at birth, 1 at death), linearly interpolated between keys.
    struct ParticleColorKey
    {
        float Time = 0.0f;
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f }; // linear color and opacity
    };
    struct ParticleSizeKey
    {
        float Time = 0.0f;
        float Size = 0.2f; // meters
    };

    struct ParticleEmitterComponent
    {
        bool Emitting = true;           // spawning (particles already out live on either way)
        bool Looping = true;            // spawn on after Duration, a new burst each loop; else a one-shot of Duration seconds
        float Duration = 5.0f;          // seconds
        float SpawnRate = 30.0f;        // particles per second
        uint32_t Burst = 0;             // spawned at once when a loop starts
        uint32_t MaxParticles = 1000;

        ParticleShape Shape = ParticleShape::Point;
        glm::vec3 ShapeSize{ 0.25f, 0.25f, 0.25f };
        float ConeAngle = 25.0f;

        float LifetimeMin = 1.0f;       // seconds
        float LifetimeMax = 2.0f;
        glm::vec3 Velocity{ 0.0f, 1.0f, 0.0f };        // m/s in the emitter's space (a cone shoots along its directions at this length)
        glm::vec3 VelocityRandom{ 0.3f, 0.3f, 0.3f };  // +/- per axis
        float Gravity = 0.0f;           // multiples of Earth's (1 falls, -0.2 rises like hot smoke)
        float Drag = 0.0f;              // per second
        float SpinMin = 0.0f;           // degrees per second
        float SpinMax = 0.0f;

        float SizeStart = 0.2f;         // meters, over each particle's life
        float SizeEnd = 0.4f;
        float SizeRandom = 0.0f;        // 0..1: each particle's size varies by up to this fraction
        glm::vec4 ColorStart{ 1.0f, 1.0f, 1.0f, 1.0f }; // linear color and opacity over life
        glm::vec4 ColorEnd{ 1.0f, 1.0f, 1.0f, 0.0f };
        float Intensity = 1.0f;         // brightness multiplier (HDR: above 1 glows with bloom)
        // Curves over each particle's life: with two keys or more they replace Color Start / End and Size Start / End (sorted by
        // time when evaluated).
        std::vector<ParticleColorKey> ColorOverLife;
        std::vector<ParticleSizeKey> SizeOverLife;

        AssetHandle Texture = 0;        // none: soft round dots
        uint32_t FlipbookColumns = 1;
        uint32_t FlipbookRows = 1;
        float FlipbookFPS = 0.0f;       // 0: the sheet's frames spread over each particle's life
        ParticleBlend Blend = ParticleBlend::Additive;
        bool Lit = false;               // shaded by the scene's lights (the sun's shadow too) and environment, else emissive
        bool SoftParticles = true;      // fade out where they meet geometry instead of cutting into it
        float SoftDistance = 0.3f;      // meters
        bool LocalSpace = false;        // particles move with the entity

        ParticleSimulation Simulation;

        ParticleEmitterComponent() = default;
        ParticleEmitterComponent(const ParticleEmitterComponent&) = default;
    };

    struct SpriteRendererComponent
    {
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        /*std::string TextureName;*/
        //Ref<Texture2D> Texture;
        AssetHandle Texture = 0;
        float TilingFactor = 1.0f;

        SpriteRendererComponent() = default;
        SpriteRendererComponent(const SpriteRendererComponent&) = default;
        SpriteRendererComponent(const glm::vec4& color) : Color(color) {}
    };
    
    struct CircleRendererComponent
    {
        glm::vec4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
        float Thickness = 0.5f;
        float Fade = 0.005f;

        CircleRendererComponent() = default;
        CircleRendererComponent(const CircleRendererComponent&) = default;
    };
    
    struct CameraComponent
    {
        SceneCamera Camera;
        bool Primary = true; // todo think about moving to scene
        bool FixedAspectRatio = false;
        bool AutoExposure = false;
        float ExposureCompensation = 0.0f;
        float AutoExposureMinEV = -16.0f;
        float AutoExposureMaxEV = 16.0f;

        CameraComponent() = default;
        CameraComponent(const CameraComponent&) = default;
    };
    
    struct ScriptComponent
    {
        struct EntityReference
        {
            UUID Entity = 0;
            UUID ModelInstance = 0;
            uint32_t ModelNodeIndex = 0;

            bool IsModelNode() const { return ModelInstance != 0; }
        };

        std::vector<std::string> ClassNames;
        std::unordered_map<std::string, std::unordered_map<std::string, EntityReference>> EntityReferences;
        std::unordered_map<std::string, std::unordered_map<std::string, ScriptValue>> FieldOverrides;
        
        ScriptComponent() = default;
        ScriptComponent(const ScriptComponent&) = default;
    };
    
    /*// Forward declaration
    class ScriptableEntity;
    struct NativeScriptComponent
    {
        ScriptableEntity* Instance = nullptr;
        
        ScriptableEntity*(*InstantiateScript)();
        void(*DestroyScript)(NativeScriptComponent*);

        template<typename T>
        void Bind()
        {
            InstantiateScript = []() { return static_cast<ScriptableEntity*>(new T()); };
            DestroyScript = [](NativeScriptComponent* nsc) { delete nsc->Instance; nsc->Instance = nullptr; };
        }
    };*/
    
    // Physics

    struct RigidBody2DComponent
    {
        enum class BodyType { Static = 0, Dynamic, Kinematic };
        BodyType Type = BodyType::Static;
        bool FixedRotation = false;

        // Storage for runtime
        b2BodyId RuntimeBody = b2_nullBodyId;

        RigidBody2DComponent() = default;
        RigidBody2DComponent(const RigidBody2DComponent&) = default;
    };

    struct BoxCollider2DComponent
    {
        glm::vec2 Offset = { 0.0f, 0.0f };
        glm::vec2 Size = { 0.5f, 0.5f };//1x1m scale object
        
        // The surface material (Box2D's b2SurfaceMaterial; Inspector presets: Default, Ice, Rubber, Wood, Metal, Bouncy, Conveyor).
        float Density = 1.0f;              // kg per square meter
        float Friction = 0.5f;             // Coulomb friction, usually 0..1
        float Restitution = 0.0f;          // bounce, 0..1
        float RestitutionThreshold = 0.5f; // m/s: slower impacts do not bounce (one value for the world: the smallest collider's)
        float RollingResistance = 0.0f;    // slows rolling circles, 0..1
        float TangentSpeed = 0.0f;         // m/s along the surface: a conveyor belt
        
        // Storage for runtime
        void* RuntimeFixture = nullptr;

        BoxCollider2DComponent() = default;
        BoxCollider2DComponent(const BoxCollider2DComponent&) = default;
    };

    struct CircleCollider2DComponent
    {
        glm::vec2 Offset = { 0.0f, 0.0f };
        float Radius = 0.5f;// idk 0.5 doesnt work for me maybe changed with box2d v3 ?1x1m scale object
        
        // The surface material (Box2D's b2SurfaceMaterial; Inspector presets: Default, Ice, Rubber, Wood, Metal, Bouncy, Conveyor).
        float Density = 1.0f;              // kg per square meter
        float Friction = 0.5f;             // Coulomb friction, usually 0..1
        float Restitution = 0.0f;          // bounce, 0..1
        float RestitutionThreshold = 0.5f; // m/s: slower impacts do not bounce (one value for the world: the smallest collider's)
        float RollingResistance = 0.0f;    // slows rolling circles, 0..1
        float TangentSpeed = 0.0f;         // m/s along the surface: a conveyor belt
        
        // Storage for runtime
        void* RuntimeFixture = nullptr;

        CircleCollider2DComponent() = default;
        CircleCollider2DComponent(const CircleCollider2DComponent&) = default;
    };

    // 3D Physics

    struct RigidBody3DComponent
    {
        enum class BodyType { Static = 0, Dynamic, Kinematic };
        enum class MotionQuality { Discrete = 0, LinearCast };

        BodyType Type = BodyType::Dynamic;
        MotionQuality Quality = MotionQuality::Discrete;

        float Mass = 1.0f;
        float LinearDamping = 0.05f;
        float AngularDamping = 0.05f;
        float GravityFactor = 1.0f;

        bool AllowSleeping = true;
        bool IsSensor = false;
        uint16_t Layer = 1; // Default to MOVING

        // Storage for runtime (invalid in editor mode)
        uint32_t RuntimeBodyID = 0xFFFFFFFF; // JPH::BodyID::cInvalidBodyID

        RigidBody3DComponent() = default;
        RigidBody3DComponent(const RigidBody3DComponent&) = default;
    };

    struct BoxCollider3DComponent
    {
        glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
        glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

        float Friction = 0.5f;
        float Restitution = 0.0f;

        BoxCollider3DComponent() = default;
        BoxCollider3DComponent(const BoxCollider3DComponent&) = default;
    };

    struct SphereCollider3DComponent
    {
        float Radius = 0.5f;
        glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

        float Friction = 0.5f;
        float Restitution = 0.0f;

        SphereCollider3DComponent() = default;
        SphereCollider3DComponent(const SphereCollider3DComponent&) = default;
    };

    struct CapsuleCollider3DComponent
    {
        float HalfHeight = 0.5f;
        float Radius = 0.5f;
        glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };

        float Friction = 0.5f;
        float Restitution = 0.0f;

        CapsuleCollider3DComponent() = default;
        CapsuleCollider3DComponent(const CapsuleCollider3DComponent&) = default;
    };

    // A kinematic capsule character (Jolt CharacterVirtual): slides along walls, climbs steps and slopes, sticks to
    // the floor, rides moving platforms. It owns the entity translation (entity origin = feet); rotation is left to
    // scripts. Scripts drive it through the input fields, the physics step fills the runtime state.
    struct CharacterController3DComponent
    {
        // Defaults follow Unreal's third-person character (its centimeters as meters): capsule 34 cm x 176 cm, 45 cm steps,
        // 44.8 degree slopes, gravity x1.75, air control 0.35, 20.5 m/s^2 acceleration (2048 cm/s^2) and 20 m/s^2 braking.
        // Lengths go through WorldUnits so this stays the same real-world size whatever the project's world unit is (docs/Units_And_World_Tools_Plan_2026.md, U6).
        float Radius = WorldUnits::FromMeters(0.34f);
        float Height = WorldUnits::FromMeters(1.08f); // length of the straight part between the two caps; total height = Height + 2 * Radius
        float StepHeight = WorldUnits::FromMeters(0.45f);
        float MaxSlopeDegrees = 44.8f;
        float GravityScale = 1.75f;
        float AirControl = 0.35f;          // fraction of the ground acceleration available in the air
        float MaxAcceleration = WorldUnits::FromMeters(20.48f); // m/s^2 towards the wanted horizontal velocity: movement ramps up instead of snapping
        float BrakingDeceleration = WorldUnits::FromMeters(20.0f); // m/s^2 when no movement is wanted

        // Input, set by scripts each frame
        glm::vec3 MoveVelocity = { 0.0f, 0.0f, 0.0f }; // desired horizontal world velocity (world units per second)
        float JumpSpeed = 0.0f;                        // upward speed to apply on the next fixed step, then cleared
        // Written by the entity's animator: the root motion of what it plays, as a world velocity. Moves the character on top
        // of MoveVelocity, horizontally (gravity and the floor keep the height), through the same collision as any movement.
        glm::vec3 RootMotionVelocity = { 0.0f, 0.0f, 0.0f };

        // Runtime state, written by the physics step
        bool IsGrounded = false;
        glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };

        CharacterController3DComponent() = default;
        CharacterController3DComponent(const CharacterController3DComponent&) = default;
    };

    struct TextComponent
    {
        std::string TextString;
        AssetHandle Font = 0; // a Font asset (.ttf / .otf); 0 = the default font, which also stands in until it has loaded
        glm::vec4 Color{ 1.0f };
        float Kerning = 0.0f;
        float LineSpacing = 0.0f;
        // An outline around the glyphs (thickness 0..1 of the font's distance range; 0 = none) and a rectangle behind the text
        // (alpha 0 = none), its margin in em.
        glm::vec4 OutlineColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        float OutlineThickness = 0.0f;
        glm::vec4 BackgroundColor{ 0.0f };
        float BackgroundPadding = 0.1f;
    };
    
    // The level (a scene asset) a streamed-in entity belongs to (Scene::LoadLevel): what Scene::UnloadLevel removes. Runtime only:
    // never saved, never copied.
    struct LevelComponent
    {
        AssetHandle Level = 0;
    };

    // A Level Streaming Volume (Unreal's): while the streaming source -- the primary camera in Play, the editor camera in Simulate --
    // is inside this box, its levels (scene assets) are loaded into the running scene (Scene::LoadLevel); once it has been outside
    // every volume naming a level for UnloadDelay seconds, that level is unloaded. The box is HalfExtents in the entity's space,
    // scaled by its transform (as BoxCollider3DComponent). A level a volume names is left to its volumes.
    struct LevelStreamingVolumeComponent
    {
        std::vector<AssetHandle> Levels;
        glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
        float UnloadDelay = 2.0f; // seconds outside before unloading: walking along the edge does not load and unload every frame
    };

    // How a root entity is kept in a world-partitioned scene (WorldPartition.h). Without it the engine decides (Auto): an entity tree
    // with something in the world (a mesh, a local light, a body ...) goes into a grid cell and streams with it; sky, sun, cameras,
    // the player and logic stay in the scene file, always loaded. Only a root's choice counts: children go with their root.
    struct WorldPartitionComponent
    {
        enum class LoadingMode : uint8_t
        {
            Auto,
            Spatial,      // in a cell, streamed by distance (Unreal's "Is Spatially Loaded")
            AlwaysLoaded  // in the scene file
        };
        LoadingMode Mode = LoadingMode::AlwaysLoaded;
    };

    // A streaming source besides the camera (Unreal's World Partition Streaming Source component): while Enabled, the world's cells
    // within the loading range of this entity are loaded in Play and Simulate (a vehicle, a remote player, a cutscene target).
    struct WorldStreamingSourceComponent
    {
        // Unreal's EStreamingSourceTargetState (§5.13.3). Activated: the cells in its range are in the world (drawn, simulated).
        // Loaded: only loaded -- built and their assets streamed in, not in the world, ready to activate without a hitch (ahead of
        // a fast vehicle, at a teleport target).
        enum class TargetState : uint8_t { Activated = 0, Loaded };
        bool Enabled = true;
        TargetState Target = TargetState::Activated;
        float LoadingRangeScale = 1.0f; // times the grid's loading range (Unreal's FStreamingSourceShape::LoadingRangeScale)
    };

    // An HLOD (§5.13.2): the merged, simplified, baked stand-in of one world cell, drawn while that cell is not loaded and drawn. Made by
    // Build HLODs and kept in the HLOD grid's cells; never saved into the scene or a world cell, never edited.
    struct HLODComponent
    {
        WorldCellCoord Source;  // the world cell it stands for
        bool Hidden = false;    // runtime: its cell is in (not drawn, not traced)
    };

    template<typename... Component>
    struct ComponentGroup
    {
    };

    using AllComponents = 
        ComponentGroup<TransformComponent, WorldTransformComponent, RelationshipComponent, DirtyTransformComponent, FolderComponent,
        MeshComponent, MaterialComponent, ModelInstanceComponent, ModelNodeComponent, PrefabInstanceComponent, PrefabNodeComponent, DirectionalLightComponent, PointLightComponent, SpotLightComponent, RectLightComponent, DiscLightComponent, EnvironmentLightComponent, AnimatorComponent,
        TimelinePlayerComponent, ParticleEmitterComponent, SpriteRendererComponent,
            CircleRendererComponent, CameraComponent, ScriptComponent,
            /*NativeScriptComponent,*/ RigidBody2DComponent, BoxCollider2DComponent,
            CircleCollider2DComponent, RigidBody3DComponent, BoxCollider3DComponent,
            SphereCollider3DComponent, CapsuleCollider3DComponent, CharacterController3DComponent, TextComponent, BoneSocketComponent, LevelStreamingVolumeComponent,
            WorldPartitionComponent, WorldStreamingSourceComponent, HLODComponent>;
}
