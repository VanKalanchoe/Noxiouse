#include "Renderer2D.h"

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    static bool m_reloadShader = false;

    Renderer2D::Renderer2D(bool isEditor, RendererContext context) : m_isEditor(isEditor), m_context(context)
    {
        NOX_CORE_INFO("Renderer2D Start");

        initRenderer2D();

        m_context.fileWatcher.watch(std::filesystem::path("../../shaders/MeshQuad.slang"), [this]() { m_reloadShader = true; });
        m_context.fileWatcher.watch(std::filesystem::path("../../shaders/quad.slang"), [this]() { m_reloadShader = true; });
    }

    Renderer2D::~Renderer2D()
    {
        NOX_CORE_INFO("Renderer2D Shutdown");
    }

    void Renderer2D::initRenderer2D()
    {
        createMeshPipeline(false);


        createQuadVertexBuffer();
        createQuadIndexBuffer();
        createQuadUniformBuffers();
    }

    // needed for pipeline management you cant run this inside a commandbuffer that already started
    bool Renderer2D::drawFrame()
    {
        if (m_reloadShader)
        {
            m_reloadShader = false;
            createMeshPipeline(true);
            return m_reloadShader;
        }

        return m_reloadShader;
    }

    void Renderer2D::createMeshPipeline(bool forceCompile)
    {
        /*NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "../../shaders/MeshQuad.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "../../shaders/MeshQuad.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "../../shaders/MeshQuad.slang"
        });
        m_MeshQuadPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);*/
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Vertex,
            .entryPoint = "vertMain",
            .sourcePath = "../../shaders/quad.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "../../shaders/quad.slang"
        });
        m_MeshQuadPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }

    void Renderer2D::createQuadVertexBuffer()
    {
        uint64_t bufferSize = sizeof(m_vertices[0]) * m_vertices.size();

        std::unique_ptr<NRI::Buffer> stagingBuffer = m_context.device.createBuffer(NRI::BufferDesc{
            .size = bufferSize,
            .usage = NRI::BufferUsage::Staging
        });

        void* mappedMemory = stagingBuffer->map(0, bufferSize);
        memcpy(mappedMemory, m_vertices.data(), bufferSize);
        stagingBuffer->unmap();

        m_data.vertexBuffer = m_context.device.createBuffer(NRI::BufferDesc
            {
                .size = bufferSize,
                .usage = NRI::BufferUsage::Storage
            });

        std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = m_context.beginSingleTimeCommands();
        m_data.vertexBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_vertices.data());
        m_context.endSingleTimeCommands(std::move(commandCopyBuffer));
    }

    void Renderer2D::createQuadIndexBuffer()
    {
        uint64_t bufferSize = sizeof(m_indices[0]) * m_indices.size();

        std::unique_ptr<NRI::Buffer> stagingBuffer = m_context.device.createBuffer(NRI::BufferDesc{
            .size = bufferSize,
            .usage = NRI::BufferUsage::Staging
        });

        void* mappedMemory = stagingBuffer->map(0, bufferSize);
        memcpy(mappedMemory, m_indices.data(), bufferSize);
        stagingBuffer->unmap();

        m_data.indexBuffer = m_context.device.createBuffer(NRI::BufferDesc
            {
                .size = bufferSize,
                .usage = NRI::BufferUsage::Index
            });

        std::unique_ptr<NRI::CommandBuffer> commandCopyBuffer = m_context.beginSingleTimeCommands();
        m_data.indexBuffer->uploadData(*commandCopyBuffer, *stagingBuffer, m_indices.data());
        m_context.endSingleTimeCommands(std::move(commandCopyBuffer));
    }

    void Renderer2D::createQuadUniformBuffers()
    {
        uint64_t bufferSize = 10000 * sizeof(shaderio::QuadData);
        // Reserve memory in vectors to prevent reallocation overhead
        m_data.quadUniformBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_data.quadUniformBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_context.device.createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_data.quadUniformBuffers.emplace_back(std::move(uboBuffer));
            m_data.quadUniformBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID)
    {
        shaderio::QuadData instance;
        instance.modelMatrix = transform;
        instance.color = color;
        instance.materialIndex = m_context.whiteTexture->GetDescriptorIndexSlot();
        instance.entityID = entityID;

        m_data.quadDatas.emplace_back(instance);
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor, int entityID)
    {
        shaderio::QuadData instance;
        instance.modelMatrix = transform;
        instance.color = tintColor;
        instance.materialIndex = texture->GetDescriptorIndexSlot();
        instance.entityID = entityID;

        m_data.quadDatas.emplace_back(instance);
    }

    void Renderer2D::DrawSprite(const glm::mat4& transform, SpriteRendererComponent& src, int entityID)
    {
        if (src.Texture)
        {
            Ref<Texture2D> texture = AssetManager::GetAsset<Texture2D>(src.Texture);
            DrawQuad(transform, texture, src.TilingFactor, src.Color, entityID);
        }
        else
        {
            DrawQuad(transform, src.Color, entityID);
        }
    }

    void Renderer2D::Update(uint32_t currentImage)
    {
        memcpy(m_data.quadUniformBuffersMapped[currentImage], m_data.quadDatas.data(), m_data.quadDatas.size() * sizeof(shaderio::QuadData));
    }

    // Runs inside a Begin rendering
    void Renderer2D::Flush(NRI::CommandBuffer& commandBuffer, const NRI::Buffer& currentUniformBuffer, uint32_t frameIndex) const
    {
        // MeshQuad
        if (!m_data.quadDatas.empty())
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_MeshQuadPipeline);

            commandBuffer.bindIndexBuffer(*m_data.indexBuffer, 0);
            
            shaderio::PushConstantQuad quadReferences{};
            quadReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            quadReferences.vertexReference = m_data.vertexBuffer->getDeviceAddress();
            quadReferences.quadDataReference = m_data.quadUniformBuffers[frameIndex]->getDeviceAddress();
            quadReferences.numOfElements = m_data.quadDatas.size();
            commandBuffer.pushData(&quadReferences, sizeof(shaderio::PushConstantQuad));
            
            commandBuffer.drawIndexed(m_indices.size(), m_data.quadDatas.size(), 0, 0, 0);
         
            /*commandBuffer.drawMeshTasks(m_data.quadDatas.size(), 1, 1);*/
        }
    }

    // Runs outside of any rendering call whenever you want
    void Renderer2D::BeginScene(const EditorCamera& camera)
    {
        m_data.quadDatas.clear();
    }

    // Runs outside of any rendering call whenever you want
    void Renderer2D::EndScene()
    {
    }
}
