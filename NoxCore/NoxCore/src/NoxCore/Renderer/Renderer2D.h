#pragma once
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

namespace shaderio // Shader IO namespace -- shared layout between C++ and shaders
{
    using namespace glm; // GLSL-style types without the glm:: prefix inside the namespace
#include "shaderIO.h"
    
    inline bool operator==(const shaderio::Vertex& other1, const shaderio::Vertex& other2)
    {
        return other1.pos == other2.pos && other1.color == other2.color && other1.texCoord == other2.texCoord;
    }
}

#include "NRI/NRI.h"
#include "NoxCore/Utils/NOXWatcher.h"
#include "EditorCamera.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Asset/TextureImporter.h"

namespace Nox
{
    struct RendererContext
    {
        NRI::Device& device;
        NRI::ShaderCompiler& shaderCompiler;
        Utils::NOXWatcher& fileWatcher;
        Ref<Texture2D>& whiteTexture;
        /*NRI::DescriptorHeap& resourceHeap;
        NRI::DescriptorHeap& samplerHeap;*/
    };
    
    class Renderer2D
    {
    public:
        Renderer2D(bool isEditor, RendererContext ctx);
        ~Renderer2D();
        
        void DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
        void DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);
        void DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID);
        
        bool drawFrame();
        
        void Flush(NRI::CommandBuffer& cmd, const NRI::Buffer& currentUniformBuffer) const;
        
        void BeginScene(const EditorCamera& camera);
        void EndScene();

    private:
        void initRenderer2D();
        void createMeshPipeline(bool forceCompile);
        
    private:
        struct Data
        {
            std::vector<shaderio::QuadData> quadDatas;
            std::unique_ptr<NRI::Buffer> quadBuffer;
        };
        
    private:
        bool m_isEditor = false;
        RendererContext m_context;
        Data m_data;
        
        std::unique_ptr<NRI::Pipeline> m_MeshQuadPipeline = nullptr;
    };   
}
