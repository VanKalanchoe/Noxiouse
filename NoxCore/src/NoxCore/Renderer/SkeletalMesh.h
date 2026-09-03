#pragma once

#include <vector>
#include <string>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Animation/Skeleton.h"
#include "NoxCore/Animation/AnimationSequence.h"
#include "Renderer.h"

namespace Nox
{
    class SkeletalMesh : public Asset
    {
    public:
        // GPU Submesh Handles and Material metadata
        std::vector<MeshHandle> m_SubMeshes;
        std::vector<std::string> m_SubmeshNames;
        std::vector<MaterialData> m_Materials;
        
        // Lights
        std::vector<LightNodeData> m_Lights;
        const std::vector<LightNodeData>& GetLights() const { return m_Lights; }

        // References to Skeletal & Animation Assets
        Ref<Skeleton> SkeletonAsset;
        std::vector<Ref<AnimationSequence>> Animations;

        // AssetHandles for lazy loading via AssetManager
        AssetHandle SkeletonHandle = 0;
        std::vector<AssetHandle> AnimationHandles;

        static AssetType GetStaticType() { return AssetType::SkeletalMesh; }
        virtual AssetType GetType() const override { return GetStaticType(); }
    };
}
