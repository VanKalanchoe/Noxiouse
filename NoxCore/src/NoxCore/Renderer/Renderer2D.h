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
    #include "NoxCore/Renderer/shaderIO.h"
    
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
        
        // actual Render Path
        bool BeginFrame();
        void Update(uint32_t currentImage);
        void Flush(NRI::CommandBuffer& commandBuffer, const NRI::Buffer& currentUniformBuffer, uint32_t frameIndex);
        void EndFrame();
        
        // Render Data
        void BeginScene(const Camera& camera, const glm::mat4& transform);
        void BeginScene(const EditorCamera& camera);
        void EndScene();
        
        void DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
        void DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f), int entityID = -1);
        void DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID);

        void DrawCircle(const glm::mat4& transform, const glm::vec4& color, float thickness = 1.0f, float fade = 0.005f, int entityID = -1);
        
        struct TextParams
        {
            glm::vec4 Color{ 1.0f };
            float Kerning = 0.0f;
            float LineSpacing = 0.0f;
        };
        
        void DrawString(const std::string& string, Ref<Font> font, const glm::mat4& transform, const TextParams& textParams, int entityID = -1);
        void DrawString(const std::string& string, const glm::mat4& transform, const TextComponent& component, int entityID = -1);
        
        void DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, int entityID = -1);
        void DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, int entityID = -1);
        void DrawRect(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);
        
    private:
        void initRenderer2D();
        
        // Quad
        void createMeshPipeline(bool forceCompile);
        void createQuadUniformBuffers();
        
        // Circle
        void createCircleMeshPipeline(bool forceCompile);
        void createCircleUniformBuffers();
        
        // Text
        void createTextMeshPipeline(bool forceCompile);
        void createTextUniformBuffers();
        
        // Line
        void createLineMeshPipeline(bool forceCompile);
        void createLineUniformBuffers();

    private:
        struct Data
        {
            // todo:: combine all buffers into with offsets idk how mcuh improvement that is
            // Quad
            std::vector<shaderio::QuadData> quadDatas;
            std::vector<std::unique_ptr<NRI::Buffer>> quadUniformBuffers;
            std::vector<void*> quadUniformBuffersMapped;
            
            // Circle
            std::vector<shaderio::CircleData> circleDatas;
            std::vector<std::unique_ptr<NRI::Buffer>> circleUniformBuffers;
            std::vector<void*> circleUniformBuffersMapped;
            
            // Text
            Ref<Texture2D> FontAtlasTexture;
            std::vector<shaderio::TextData> textDatas;
            std::vector<std::unique_ptr<NRI::Buffer>> textUniformBuffers;
            std::vector<void*> textUniformBuffersMapped;
            
            // Line
            std::vector<shaderio::LineData> lineDatas;
            std::vector<std::unique_ptr<NRI::Buffer>> lineUniformBuffers;
            std::vector<void*> lineUniformBuffersMapped;
        };
        
    private:
        bool m_isEditor = false;
        RendererContext m_context;
        Data m_data;
        
        std::unique_ptr<NRI::Pipeline> m_QuadMeshPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_CircleMeshPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_TextMeshPipeline = nullptr;
        std::unique_ptr<NRI::Pipeline> m_LineMeshPipeline = nullptr;
    };   
}
