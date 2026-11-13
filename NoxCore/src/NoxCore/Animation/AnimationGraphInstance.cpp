#include "AnimationGraphInstance.h"

#include <algorithm>
#include <functional>

#include "AnimationGraphNodes.h"
#include "NoxCore/Asset/AssetManager.h"

namespace Nox
{
    namespace
    {
        // Every asset handle a graph's nodes name in a property the node type marks as an asset (its PropertyAssetTypeHints of the
        // "AssetType::..." kind; a "Bone" hint is a name, not an asset), the state machines' states included.
        template<typename Visit>
        void ForEachGraphAsset(const NodeGraph& graph, const Visit& visit)
        {
            for (const GraphNode& node : graph.Nodes)
            {
                if (const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node.TypeName))
                {
                    for (const auto& [propertyName, typeHint] : type->PropertyAssetTypeHints)
                    {
                        if (std::string_view(typeHint).rfind("AssetType::", 0) != 0)
                            continue;
                        auto property = node.Properties.find(propertyName);
                        if (property == node.Properties.end())
                            continue;
                        if (const auto* handle = std::get_if<uint64_t>(&property->second); handle && *handle != 0)
                            visit(AssetHandle(*handle));
                    }
                }
                for (const NodeSubGraph& subGraph : node.SubGraphs)
                    ForEachGraphAsset(subGraph.Graph, visit);
            }
        }
    }

    void AnimationGraphInstance::CollectGraphAssets(const NodeGraph& graph, std::unordered_set<AssetHandle>& outAssets)
    {
        ForEachGraphAsset(graph, [&](AssetHandle handle) { outAssets.insert(handle); });
    }

    void AnimationGraphInstance::CollectMontageClips(std::unordered_set<AssetHandle>& outAssets) const
    {
        for (const Montage& montage : m_Montages)
            if (montage.Clip != 0)
                outAssets.insert(montage.Clip);
    }

    void AnimationGraphInstance::Sync(AssetHandle graphHandle, std::vector<AssetHandle>& missingAssets)
    {
        // Re-resolve when the graph handle changed, or it was set but never successfully resolved yet (asset
        // still loading last time we checked).
        bool needsResolve = (graphHandle != m_GraphHandle) || (graphHandle != 0 && !m_Graph);
        m_GraphHandle = graphHandle;

        if (needsResolve)
        {
            m_Graph = Ref<NodeGraphAsset>(graphHandle != 0 ? AssetManager::FindLoadedAsset<NodeGraphAsset>(graphHandle) : nullptr);
            if (graphHandle != 0 && !m_Graph)
            {
                missingAssets.push_back(graphHandle);
                return;
            }
        }
        if (!m_Graph)
            return;

        // Assets the graph's nodes reference (a Clip node's clip, also inside a state machine's states) aren't loaded by
        // anything else, e.g. after a scene reload: report the ones not resident yet so Scene::ApplySyncPoint requests them.
        ForEachGraphAsset(m_Graph->Graph, [&](AssetHandle handle)
        {
            if (AssetManager::IsAssetHandleValid(handle) && !AssetManager::IsAssetLoaded(handle))
                missingAssets.push_back(handle);
        });
        for (const Montage& montage : m_Montages)
        {
            if (AssetManager::IsAssetHandleValid(montage.Clip) && !AssetManager::IsAssetLoaded(montage.Clip))
                missingAssets.push_back(montage.Clip);
        }

        // The graph was recompiled since m_EvalContext was sized against it (the editor recompiles live after
        // every edit, adding/removing nodes changes the buffer sizes): start over against the new one. Node
        // playheads reset on a structural edit -- acceptable, and the only safe option once slots can move.
        if (!needsResolve && m_Graph->CompileVersion == m_CompiledVersion)
            return;
        m_CompiledVersion = m_Graph->CompileVersion;

        if (!m_Graph->Compiled.IsValid())
            return;

        m_EvalContext.Init(m_Graph->Compiled);

        // Bring Parameters in line with what the graph declares now: each declared parameter starts at this entity's override
        // (set in the Inspector, saved with the scene) or else at the graph's default -- so a default changed in the graph
        // reaches every entity that did not override it, as in Unreal. An override whose type no longer matches is dropped;
        // parameters the graph no longer declares are dropped.
        const auto& declared = m_Graph->Compiled.Parameters;
        auto isDeclared = [&](const std::string& name)
        {
            return std::any_of(declared.begin(), declared.end(), [&](const GraphParameter& p) { return p.Name == name; });
        };
        std::erase_if(Parameters, [&](const auto& entry) { return !isDeclared(entry.first); });
        std::erase_if(Overrides, [&](const auto& entry) { return !isDeclared(entry.first); });
        for (const GraphParameter& param : declared)
        {
            auto overridden = Overrides.find(param.Name);
            if (overridden != Overrides.end() && overridden->second.index() != param.DefaultValue.index())
            {
                Overrides.erase(overridden);
                overridden = Overrides.end();
            }
            Parameters[param.Name] = overridden != Overrides.end() ? overridden->second : param.DefaultValue;
        }
    }

    const NodeGraphValue* AnimationGraphInstance::DefaultOf(const std::string& name) const
    {
        if (!m_Graph)
            return nullptr;
        for (const GraphParameter& param : m_Graph->Compiled.Parameters)
            if (param.Name == name)
                return &param.DefaultValue;
        return nullptr;
    }

    void AnimationGraphInstance::SetFloat(const std::string& name, float value)
    {
        Parameters[name] = value;
    }

    void AnimationGraphInstance::SetBool(const std::string& name, bool value)
    {
        Parameters[name] = value;
    }

    float AnimationGraphInstance::GetFloat(const std::string& name) const
    {
        auto it = Parameters.find(name);
        return it != Parameters.end() ? NodeGraphValueToFloat(it->second) : 0.0f;
    }

    bool AnimationGraphInstance::GetBool(const std::string& name) const
    {
        return GetFloat(name) != 0.0f;
    }

    namespace
    {
        glm::mat4 RestGlobalMatrix(const Node* node); // (below)
    }

    const AnimPose* AnimationGraphInstance::Evaluate(float deltaTime, bool playing, const Skeleton& skeleton)
    {
        // Also bails if the graph was recompiled after this frame's Sync() -- m_EvalContext would still be sized
        // for the previous compile; the next Sync() re-Init()s it.
        if (!playing || !m_Graph || !m_Graph->Compiled.IsValid() || m_Graph->CompileVersion != m_CompiledVersion)
            return nullptr;

        m_FiredNotifies.clear();
        UpdateMontages(deltaTime);

        AnimGraphEvalData evalData;
        evalData.TargetSkeleton = &skeleton;
        evalData.DeltaTime = deltaTime;
        evalData.Parameters = &Parameters;
        evalData.FiredNotifies = &m_FiredNotifies;
        evalData.Montages = &m_MontageSlots;
        FootProbes.clear();
        evalData.IKTargets = &IKTargets;
        evalData.FootGround = &FootGround;
        evalData.FootProbes = &FootProbes;
        evalData.ModelUnitsPerMeter = ModelUnitsPerMeter;
        if (RootMotion)
        {
            // Where ExtractRootMotion will leave the root bone, for the IK nodes to measure the pose as it is drawn.
            if (const Node* root = FindRootMotionBone(skeleton))
            {
                evalData.InPlaceRootIndex = root->Index;
                evalData.InPlaceRootParent = RestGlobalMatrix(root->Parent);
                evalData.InPlaceRootRest = glm::vec3(evalData.InPlaceRootParent * glm::vec4(root->RestTranslation, 1.0f));
            }
        }
        m_EvalContext.UserData = &evalData;

        EvaluateGraph(m_EvalContext, m_Graph->Compiled);

        m_LastPose = m_EvalContext.GetInput<AnimPose>(m_Graph->Compiled.OutputNodeIndex, 0);
        if (RootMotion)
            ExtractRootMotion(skeleton);
        return &m_LastPose;
    }

    void AnimationGraphInstance::PlayMontage(AssetHandle clip, const std::string& slot, float rate, float blendIn, float blendOut, float startTime)
    {
        m_Montages.erase(std::remove_if(m_Montages.begin(), m_Montages.end(), [&](const Montage& m) { return m.Slot == slot; }), m_Montages.end());
        if (clip == 0)
            return;
        Montage montage;
        montage.Clip = clip;
        montage.Slot = slot.empty() ? std::string("DefaultSlot") : slot;
        montage.Rate = rate;
        montage.BlendIn = std::max(blendIn, 0.0f);
        montage.BlendOut = std::max(blendOut, 0.0f);
        montage.StartOffset = std::max(startTime, 0.0f);
        // (the record of the slot, for replication)
        auto record = std::find_if(m_MontageRecords.begin(), m_MontageRecords.end(), [&](const MontageRecord& r) { return r.Slot == montage.Slot; });
        if (record == m_MontageRecords.end())
            record = m_MontageRecords.insert(m_MontageRecords.end(), MontageRecord{ montage.Slot });
        *record = { montage.Slot, clip, ++m_MontageSerial, rate, montage.BlendIn, montage.BlendOut, false, 0.2f };
        m_Montages.push_back(std::move(montage));
    }

    void AnimationGraphInstance::StopMontage(const std::string& slot, float blendOut)
    {
        for (Montage& montage : m_Montages)
        {
            if (montage.Slot == slot && !montage.Stopping)
            {
                for (MontageRecord& record : m_MontageRecords)
                    if (record.Slot == slot && !record.Stopped)
                    {
                        record.Stopped = true;
                        record.StopBlendOut = std::max(blendOut, 0.0f);
                    }
                montage.Stopping = true;
                montage.StopFrom = montage.Weight;
                montage.StopElapsed = 0.0f;
                montage.StopDuration = std::max(blendOut, 0.0f);
            }
        }
    }

    bool AnimationGraphInstance::IsMontagePlaying(const std::string& slot) const
    {
        for (const Montage& montage : m_Montages)
        {
            if (montage.Slot == slot && !montage.Stopping)
                return true;
        }
        return false;
    }

    void AnimationGraphInstance::UpdateMontages(float deltaTime)
    {
        m_MontageSlots.clear();
        for (auto it = m_Montages.begin(); it != m_Montages.end();)
        {
            Montage& montage = *it;
            const AnimationSequence* clip = AssetManager::FindLoadedAsset<AnimationSequence>(montage.Clip);
            if (!clip || clip->Duration <= 0.0f)
            {
                // Not loaded yet (Sync requested it), or a clip that will never play.
                if (!AssetManager::IsAssetHandleValid(montage.Clip))
                    it = m_Montages.erase(it);
                else
                    ++it;
                continue;
            }
            if (!montage.Started)
            {
                montage.Started = true;
                const float skipped = std::min(montage.StartOffset, clip->Duration);
                montage.Time = montage.Rate >= 0.0f ? skipped : clip->Duration - skipped;
                montage.Elapsed = montage.Rate != 0.0f ? skipped / std::abs(montage.Rate) : 0.0f; // (started late: past its blend in)
            }

            const float previous = montage.Time;
            montage.Time = glm::clamp(montage.Time + deltaTime * montage.Rate, 0.0f, clip->Duration);
            montage.Elapsed += deltaTime;
            CollectCrossedNotifies(*clip, previous, montage.Time, false, m_FiredNotifies);

            const float remaining = montage.Rate >= 0.0f ? clip->Duration - montage.Time : montage.Time;
            float weight = montage.BlendIn > 0.0f ? std::min(montage.Elapsed / montage.BlendIn, 1.0f) : 1.0f;
            if (montage.BlendOut > 0.0f)
                weight = std::min(weight, remaining / montage.BlendOut);
            bool finished = remaining <= 0.0f || montage.Rate == 0.0f && montage.Stopping;
            if (montage.Stopping)
            {
                montage.StopElapsed += deltaTime;
                const float fade = montage.StopDuration > 0.0f ? 1.0f - montage.StopElapsed / montage.StopDuration : 0.0f;
                weight = std::min(weight, montage.StopFrom * std::max(fade, 0.0f));
                finished = finished || fade <= 0.0f;
            }
            montage.Weight = glm::clamp(weight, 0.0f, 1.0f);
            if (finished)
            {
                it = m_Montages.erase(it);
                continue;
            }
            m_MontageSlots[montage.Slot] = { clip, montage.Time, montage.Weight };
            ++it;
        }
    }

    namespace
    {
        glm::mat4 RestLocalMatrix(const Node& node)
        {
            if (node.HasRestMatrix)
                return node.RestMatrix;
            return glm::translate(glm::mat4(1.0f), node.RestTranslation) * glm::toMat4(node.RestRotation) * glm::scale(glm::mat4(1.0f), node.RestScale);
        }

        // Where a node's rest pose puts it in the skeleton's model space (its parents are not animated by root motion clips:
        // the armature / scene nodes above the root bone).
        glm::mat4 RestGlobalMatrix(const Node* node)
        {
            glm::mat4 global(1.0f);
            for (; node; node = node->Parent)
                global = RestLocalMatrix(*node) * global;
            return global;
        }
    }

    const Node* AnimationGraphInstance::FindRootMotionBone(const Skeleton& skeleton) const
    {
        if (!RootBone.empty())
        {
            for (const Node* node : skeleton.AllNodes)
            {
                if (node && node->Name == RootBone)
                    return node;
            }
        }
        if (!skeleton.Skins.empty() && skeleton.Skins[0] && !skeleton.Skins[0]->Joints.empty())
            return skeleton.Skins[0]->Joints[0];
        return nullptr;
    }

    void AnimationGraphInstance::ExtractRootMotion(const Skeleton& skeleton)
    {
        const Node* root = FindRootMotionBone(skeleton);
        if (!root || root->Index < 0 || root->Index >= static_cast<int32_t>(m_LastPose.LocalTransforms.size()))
            return;

        // The travel the clips worked out for this step (each from its own playhead, blended like the pose: AnimPose::RootMotion).
        m_RootMotion += m_LastPose.RootMotion;

        const glm::mat4 parent = RestGlobalMatrix(root->Parent);
        Animator::NodeTransform& transform = m_LastPose.LocalTransforms[root->Index];
        const glm::vec3 position = glm::vec3(parent * glm::vec4(transform.Translation, 1.0f));

        // In place: the root keeps its rest pose's horizontal position; its height and rotation stay animated.
        const glm::vec3 rest = glm::vec3(parent * glm::vec4(root->RestTranslation, 1.0f));
        transform.Translation = glm::vec3(glm::inverse(parent) * glm::vec4(rest.x, position.y, rest.z, 1.0f));
    }
}
