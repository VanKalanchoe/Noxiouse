#include "AnimationGraphInstance.h"

#include <algorithm>
#include <functional>

#include "AnimationGraphNodes.h"
#include "NoxCore/Asset/AssetManager.h"

namespace Nox
{
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
        std::function<void(const NodeGraph&)> collectMissing = [&](const NodeGraph& graph)
        {
            for (const GraphNode& node : graph.Nodes)
            {
                if (const NodeTypeDesc* type = NodeTypeRegistry::Find(graph.Domain, node.TypeName))
                {
                    for (const auto& [propertyName, typeHint] : type->PropertyAssetTypeHints)
                    {
                        auto property = node.Properties.find(propertyName);
                        if (property == node.Properties.end())
                            continue;
                        const auto* handle = std::get_if<uint64_t>(&property->second);
                        if (handle && *handle != 0 && AssetManager::IsAssetHandleValid(AssetHandle(*handle)) &&
                            !AssetManager::IsAssetLoaded(AssetHandle(*handle)))
                            missingAssets.push_back(AssetHandle(*handle));
                    }
                }
                for (const NodeSubGraph& subGraph : node.SubGraphs)
                    collectMissing(subGraph.Graph);
            }
        };
        collectMissing(m_Graph->Graph);

        // The graph was recompiled since m_EvalContext was sized against it (the editor recompiles live after
        // every edit, adding/removing nodes changes the buffer sizes): start over against the new one. Node
        // playheads reset on a structural edit -- acceptable, and the only safe option once slots can move.
        if (!needsResolve && m_Graph->CompileVersion == m_CompiledVersion)
            return;
        m_CompiledVersion = m_Graph->CompileVersion;

        if (!m_Graph->Compiled.IsValid())
            return;

        m_EvalContext.Init(m_Graph->Compiled);

        // Bring Parameters in line with what the graph declares now: a value set for a parameter that still
        // exists (by the inspector, a script, or loaded from the scene) is kept, unless its type changed in the
        // editor; new parameters start at their default; ones the graph no longer declares are dropped.
        const auto& declared = m_Graph->Compiled.Parameters;
        for (auto it = Parameters.begin(); it != Parameters.end();)
        {
            auto match = std::find_if(declared.begin(), declared.end(),
                [&](const GraphParameter& p) { return p.Name == it->first; });
            it = (match == declared.end()) ? Parameters.erase(it) : std::next(it);
        }
        for (const GraphParameter& param : declared)
        {
            auto existing = Parameters.find(param.Name);
            if (existing == Parameters.end() || existing->second.index() != param.DefaultValue.index())
                Parameters[param.Name] = param.DefaultValue;
        }
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

    const AnimPose* AnimationGraphInstance::Evaluate(float deltaTime, bool playing, const Skeleton& skeleton)
    {
        // Also bails if the graph was recompiled after this frame's Sync() -- m_EvalContext would still be sized
        // for the previous compile; the next Sync() re-Init()s it.
        if (!playing || !m_Graph || !m_Graph->Compiled.IsValid() || m_Graph->CompileVersion != m_CompiledVersion)
            return nullptr;

        AnimGraphEvalData evalData;
        evalData.TargetSkeleton = &skeleton;
        evalData.DeltaTime = deltaTime;
        evalData.Parameters = &Parameters;
        m_EvalContext.UserData = &evalData;

        EvaluateGraph(m_EvalContext, m_Graph->Compiled);

        m_LastPose = m_EvalContext.GetInput<AnimPose>(m_Graph->Compiled.OutputNodeIndex, 0);
        return &m_LastPose;
    }
}
