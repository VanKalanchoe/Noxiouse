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
        return other1.pos == other2.pos  && other1.texCoord == other2.texCoord;
    }
}

#include "NRI/NRI.h"
#include "NoxCore/Utils/NOXWatcher.h"
#include "EditorCamera.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Asset/TextureImporter.h"

constexpr int MAX_FRAMES_IN_FLIGHT = 2; // currently in swapchainvk and devicevk headers seperate combine them in the future

namespace Nox
{
    struct RendererContext
    {
        NRI::Device& device;
        NRI::ShaderCompiler& shaderCompiler;
        Utils::NOXWatcher& fileWatcher;
        Ref<Texture2D>& whiteTexture;
        
        std::function<std::unique_ptr<NRI::CommandBuffer>()> beginSingleTimeCommands;
        std::function<void(std::unique_ptr<NRI::CommandBuffer>&&)> endSingleTimeCommands;
    };
    
    const std::vector<shaderio::Vertex> m_vertices =
    {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}},
    };

    const std::vector<uint32_t> m_indices = 
    {
        0, 1, 2, 2, 3, 0
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
        void Update(uint32_t currentImage);
        void Flush(NRI::CommandBuffer& commandBuffer, const NRI::Buffer& currentUniformBuffer, uint32_t frameIndex) const;
        
        void BeginScene(const EditorCamera& camera);
        void EndScene();

    private:
        void initRenderer2D();
        void createMeshPipeline(bool forceCompile);
        void createQuadVertexBuffer();
        void createQuadIndexBuffer();
        void createQuadUniformBuffers();

    private:
        struct Data
        {
            std::unique_ptr<NRI::Buffer> vertexBuffer = nullptr;
            std::unique_ptr<NRI::Buffer> indexBuffer = nullptr;
            
            std::vector<shaderio::QuadData> quadDatas;
            std::vector<std::unique_ptr<NRI::Buffer>> quadUniformBuffers;
            std::vector<void*> quadUniformBuffersMapped;
        };
        
    private:
        bool m_isEditor = false;
        RendererContext m_context;
        Data m_data;
        
        std::unique_ptr<NRI::Pipeline> m_MeshQuadPipeline = nullptr;
    };   
}
