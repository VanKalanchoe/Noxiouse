#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    struct BoneInfo
    {
        std::string Name;
        int32_t ParentIndex = -1;
        glm::mat4 InverseBindMatrix = glm::mat4(1.0f);
        glm::mat4 LocalRestTransform = glm::mat4(1.0f);
    };

    class Skeleton : public Asset
    {
    public:
        std::vector<BoneInfo> Bones;
        std::unordered_map<std::string, int32_t> BoneNameToIndexMap;

        int32_t FindBoneIndex(const std::string& name) const
        {
            auto it = BoneNameToIndexMap.find(name);
            if (it != BoneNameToIndexMap.end())
                return it->second;
            return -1;
        }

        static AssetType GetStaticType() { return AssetType::Skeleton; }
        virtual AssetType GetType() const override { return GetStaticType(); }
    };
}