#pragma once

#include <string>
#include <vector>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include "NoxCore/Asset/Asset.h"

namespace Nox
{
    struct Node 
    {
        int32_t Index = -1;
        std::string Name;
        Node* Parent = nullptr;
        std::vector<Node*> Children;

        // Rest pose (parsed from glTF)
        glm::vec3 RestTranslation{0.0f};
        glm::quat RestRotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 RestScale{1.0f};
        glm::mat4 RestMatrix{1.0f};
        bool HasRestMatrix = false;

        // Current animated TRS components
        glm::vec3 Translation{0.0f};
        glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 Scale{1.0f};
        glm::mat4 Matrix{1.0f};
        bool HasMatrix = false;

        // Computed every frame
        glm::mat4 LocalMatrix{1.0f};
        glm::mat4 GlobalMatrix{1.0f};
    };

    struct Skin 
    {
        std::string Name;
        std::vector<Node*> Joints; // Direct pointers to nodes in the tree
        std::vector<glm::mat4> InverseBindMatrices;
    };

    class Skeleton : public Asset
    {
    public:
        std::vector<Node*> AllNodes;  // Fast lookup by glTF node index
        std::vector<Node*> RootNodes; // For traversing the tree downward
        std::vector<Skin*> Skins;     // Maps the vertices to the exact nodes

        ~Skeleton() 
        {
            for (Node* node : AllNodes) delete node;
            for (Skin* skin : Skins) delete skin;
        }

        static AssetType GetStaticType() { return AssetType::Skeleton; }
        virtual AssetType GetType() const override { return GetStaticType(); }
    };
}