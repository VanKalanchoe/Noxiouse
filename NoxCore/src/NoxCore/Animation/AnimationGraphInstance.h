#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/NodeGraph/GraphEvalContext.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"

#include "AnimPose.h"
#include "AnimationGraphNodes.h"

namespace Nox
{
    class Skeleton;

    // Per-entity runtime state for AnimatorComponent's graph mode (Graph != 0): the GraphEvalContext (node
    // playheads, cached pin values) and the instance's parameter overrides, paired with the asset-owned
    // CompiledGraph the same way Animator (mutable) pairs with AnimationSequence (asset). Not shared between
    // entities, even ones pointing at the same graph asset.
    //
    // Deliberately mirrors Animator's own split between a self-contained mode (UpdateTransforms + GetFinalBone-
    // Transforms) and an ECS-node-driven mode (UpdateNodeAnimation, caller-supplied node array): Evaluate() only
    // returns the raw AnimPose, and AnimatorComponent's caller (Scene::UpdateAnimators) decides whether to apply
    // it to the skeleton's shared Node tree (self-contained) or to per-joint node entities (the production
    // skinning path for imported skeletal meshes), exactly like it already does for Animator.
    class AnimationGraphInstance
    {
    public:
        // Re-points at the graph asset if graphHandle changed since the last call (re-Init()s the
        // GraphEvalContext and reseeds Parameters from the graph's declared defaults); no-op otherwise. Only
        // looks at already-loaded assets (never blocks on disk I/O -- this runs inside a parallel scene
        // system); a handle that isn't resident yet is appended to missingAssets, same convention as
        // Scene::UpdateAnimators, for Scene::ApplySyncPoint to request.
        void Sync(AssetHandle graphHandle, std::vector<AssetHandle>& missingAssets);

        // Runs the compiled graph once against skeleton and returns the resulting pose, or nullptr if not
        // playing, no graph assigned, or the graph failed to compile. The returned pointer is valid until the
        // next Evaluate() call on this instance.
        const AnimPose* Evaluate(float deltaTime, bool playing, const Skeleton& skeleton);

        // Script access (Nox.AnimatorComponent). Set* stores whatever is given, even for a name the graph doesn't
        // declare (yet) -- a script may run before the graph has loaded; Sync() drops undeclared ones. Bool and
        // int parameters read back through GetFloat as 0/1 and the number.
        void SetFloat(const std::string& name, float value);
        void SetBool(const std::string& name, bool value);
        float GetFloat(const std::string& name) const;
        bool GetBool(const std::string& name) const;

        std::unordered_map<std::string, NodeGraphValue> Parameters; // instance overrides of the graph's declared parameters

        // Montages (UE's anim montages): a clip played once over the graph in the Slot node of that name, started by scripts
        // (attack, interact, reload). It blends in over blendIn seconds and out over the last blendOut seconds; a new one in
        // an occupied slot replaces the old. A clip not loaded yet starts once it is (Sync requests it).
        void PlayMontage(AssetHandle clip, const std::string& slot, float rate, float blendIn, float blendOut);
        void StopMontage(const std::string& slot, float blendOut);
        bool IsMontagePlaying(const std::string& slot) const;

        // The notifies the playheads crossed in the last Evaluate (clips, blend spaces' leading clips, montages).
        const std::vector<std::string>& GetFiredNotifies() const { return m_FiredNotifies; }

        // Root motion: Evaluate keeps the root bone in place horizontally and collects how far it moved (the skeleton's
        // model space, +Y up) for the caller to move the entity with. RootBone empty = the skin's first joint.
        bool RootMotion = false;
        std::string RootBone;
        glm::vec3 ConsumeRootMotion() { return std::exchange(m_RootMotion, glm::vec3(0.0f)); }

    private:
        void UpdateMontages(float deltaTime);
        void ExtractRootMotion(const Skeleton& skeleton);

        struct Montage
        {
            AssetHandle Clip = 0;
            std::string Slot;
            float Time = 0.0f;
            float Rate = 1.0f;
            float BlendIn = 0.2f;
            float BlendOut = 0.2f;
            float Elapsed = 0.0f;
            float Weight = 0.0f;
            bool Started = false;   // its clip has loaded and it began
            bool Stopping = false;
            float StopFrom = 1.0f;  // the weight when StopMontage was called
            float StopElapsed = 0.0f;
            float StopDuration = 0.2f;
        };
        std::vector<Montage> m_Montages;
        std::unordered_map<std::string, MontageSlotState> m_MontageSlots; // what the Slot nodes read, rebuilt each Evaluate
        std::vector<std::string> m_FiredNotifies;

        glm::vec3 m_RootMotion{ 0.0f };
        glm::vec3 m_LastRootPosition{ 0.0f };
        glm::vec3 m_LastRootStep{ 0.0f };
        float m_AverageRootStep = 0.0f;
        bool m_HasLastRoot = false;

        AssetHandle m_GraphHandle = 0;
        Ref<NodeGraphAsset> m_Graph;
        uint32_t m_CompiledVersion = 0; // NodeGraphAsset::CompileVersion that m_EvalContext was last Init()'d against
        GraphEvalContext m_EvalContext;
        AnimPose m_LastPose;
    };
}
