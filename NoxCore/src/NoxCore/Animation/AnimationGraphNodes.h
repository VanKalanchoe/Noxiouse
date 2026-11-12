#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "AnimPose.h"
#include "NoxCore/NodeGraph/NodeGraph.h"
#include "NoxCore/NodeGraph/NodeGraphTypes.h"

namespace Nox
{
    class Skeleton;
    class AnimationSequence;

    // A montage playing in a slot, as Slot nodes see it (AnimationGraphInstance runs the montages).
    struct MontageSlotState
    {
        const AnimationSequence* Clip = nullptr;
        float Time = 0.0f;   // seconds into the clip
        float Weight = 0.0f; // 0..1, blending in and out
    };

    // NodeGraph::Domain / NodeTypeDesc::Domain every animation-graph node type registers under.
    inline constexpr const char* kAnimationGraphDomain = "AnimationGraph";

    // Passed as GraphEvalContext::UserData for one evaluation of an animation graph (AnimationGraphInstance
    // owns and fills this in). Node Evaluate callbacks only read it.
    // IK (Backlog C6). A target a script set (AnimatorComponent::IKTargets), in the skeleton's model space.
    struct AnimIKTarget
    {
        glm::vec3 Position{ 0.0f };
        float Weight = 1.0f;
    };
    // What a foot's ray found under it last frame (model space), and what a Foot Placement node asks to be traced this frame.
    struct AnimFootGround
    {
        bool Hit = false;
        glm::vec3 Point{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
    };
    struct AnimFootProbe
    {
        std::string Bone;
        glm::vec3 Position{ 0.0f }; // the animated foot (model space)
        float Up = 0.5f;            // meters above it the ray starts
        float Down = 0.75f;         // meters below it the ray ends
    };

    struct AnimGraphEvalData
    {
        const Skeleton* TargetSkeleton = nullptr;
        float DeltaTime = 0.0f;
        // The graph instance's runtime parameter blackboard (graph defaults, overridden per-instance -- by the
        // inspector today, by scripts once Step 5 wires that up), looked up by name from nodes that reference a
        // parameter (e.g. Blend2D's blend factor). Never null during a real evaluation.
        const std::unordered_map<std::string, NodeGraphValue>* Parameters = nullptr;

        NodeGraphValue GetParameter(const std::string& name, const NodeGraphValue& fallback) const;

        // Notifies the playheads crossed this evaluation (null: nobody listens).
        std::vector<AnimationNotify>* FiredNotifies = nullptr;
        // Montages by slot name (null or empty: none playing).
        const std::unordered_map<std::string, MontageSlotState>* Montages = nullptr;

        // IK: the scripts' targets by name, the ground under each foot bone last frame, the rays asked for this frame (all model
        // space), and the skeleton's model units per meter (node properties are in meters).
        const std::unordered_map<std::string, AnimIKTarget>* IKTargets = nullptr;
        const std::unordered_map<std::string, AnimFootGround>* FootGround = nullptr;
        std::vector<AnimFootProbe>* FootProbes = nullptr;
        float ModelUnitsPerMeter = 1.0f;
        // Root motion: the root bone ends up in place after the evaluation (AnimationGraphInstance::ExtractRootMotion); IK works
        // on the pose as it will be drawn, so it puts the root there too when it measures bone positions. -1: no root motion.
        int32_t InPlaceRootIndex = -1;
        glm::mat4 InPlaceRootParent{ 1.0f }; // the root's parents' rest global matrix
        glm::vec3 InPlaceRootRest{ 0.0f };   // the root's rest position (model space)
    };

    // Registers "Clip", "Blend2D", "BlendSpace1D", "BlendSpace2D", "LayeredBlend", "ApplyAdditive", "Slot", "GetParameter",
    // "StateMachine" and "Output" under kAnimationGraphDomain
    // into NodeTypeRegistry. Call exactly
    // once at startup (NodeTypeRegistry has no de-duplication, so calling it twice duplicates every entry).
    void RegisterAnimationGraphNodeTypes();
    // "TwoBoneIK", "LookAt", "FootPlacement" (Category "IK"): called by RegisterAnimationGraphNodeTypes.
    void RegisterAnimationIKNodeTypes();

    // A brand-new animation graph as the editor's "create" action starts it: just the Output node, already the
    // graph's output. Needs RegisterAnimationGraphNodeTypes() to have run only for the graph to *compile*, not to
    // build it.
    NodeGraph CreateEmptyAnimationGraph();
}
