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
    }

    Renderer2D::~Renderer2D()
    {
        NOX_CORE_INFO("Renderer2D Shutdown");
    }
    
    void Renderer2D::initRenderer2D()
    {
        createMeshPipeline(false);
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
        NRI::PipelineDesc desc{};
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
        m_MeshQuadPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }
    
    void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID)
    {
        shaderio::QuadData instance;
        instance.modelMatrix = transform;
        instance.color = color;
        instance.materialIndex = m_context.whiteTexture->GetDescriptorIndexSlot();
        instance.EntityID = entityID;

        m_data.quadDatas.emplace_back(instance);
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor, int entityID)
    {
        shaderio::QuadData instance;
        instance.modelMatrix = transform;
        instance.color = tintColor;
        instance.materialIndex = texture->GetDescriptorIndexSlot();
        instance.EntityID = entityID;

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
    
    // Runs inside a Begin rendering
    void Renderer2D::Flush(NRI::CommandBuffer& commandBuffer, const NRI::Buffer& currentUniformBuffer) const
    {
        // MeshQuad
        if (!m_data.quadDatas.empty())
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_MeshQuadPipeline);
            
            shaderio::PushConstantQuad quadReferences{};
            quadReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            commandBuffer.pushData(&quadReferences, sizeof(shaderio::PushConstantQuad));
            
            commandBuffer.drawMeshTasks(m_data.quadDatas.size(), 1, 1);    
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
