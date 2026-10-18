#include "Renderer2D.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Core/Log.h"
#include "MSDFData.h"

namespace Nox
{
    static bool m_reloadShader = false;

    Renderer2D::Renderer2D(bool isEditor, RendererContext context) : m_isEditor(isEditor), m_context(context)
    {
        NOX_CORE_INFO("Renderer2D Start");

        initRenderer2D();

        m_context.fileWatcher.watch(std::filesystem::path("assets/shaders/QuadMesh.slang"), [this](auto path) { m_reloadShader = true; });
        m_context.fileWatcher.watch(std::filesystem::path("assets/shaders/GridMesh.slang"), [this](auto path) { m_reloadShader = true; });
    }

    Renderer2D::~Renderer2D()
    {
        NOX_CORE_INFO("Renderer2D Shutdown");
    }

    void Renderer2D::initRenderer2D()
    {
        // Quad
        createMeshPipeline(false);
        createQuadStorageBuffers();
        
        // Circle
        createCircleMeshPipeline(false);
        createCircleStorageBuffers();
        
        // Text
        createTextMeshPipeline(false);
        createTextStorageBuffers();
        
        // Text
        createLineMeshPipeline(false);
        createLineStorageBuffers();
        createGridMeshPipeline(false);

        // Screen space
        createScreenUniformBuffers();
    }

    // needed for pipeline management you cant run this inside a commandbuffer that already started
    bool Renderer2D::BeginFrame()
    {
        if (m_reloadShader)
        {
            // maybe i can compile only the one thats i geuss with bool m_reload shader but every pipeline hs own bool
            m_reloadShader = false;
            createMeshPipeline(true);
            createCircleMeshPipeline(true);
            createTextMeshPipeline(true);
            createLineMeshPipeline(true);
            createGridMeshPipeline(true);
            return m_reloadShader;
        }

        return m_reloadShader;
    }
    
    namespace
    {
        constexpr size_t kMaxLines = 10000; // the line storage buffer holds this many, thick and thin together
        // The quad / circle / text storage buffers hold this many each (world and screen space together); anything past it
        // is dropped instead of written past the end of the mapped buffer.
        constexpr size_t kMaxQuads = 10000;
        constexpr size_t kMaxCircles = 10000;
        constexpr size_t kMaxGlyphs = 10000;

        // Output pixel (x right, y down, origin top left) -> clip space for the 2D pass's flipped viewport {0, h, w, -h}, where
        // NDC y = +1 is the top row. Depth is reverse-Z 1.0 (the near plane); the screen-space draws also turn depth testing off.
        glm::mat4 ScreenProjection(const glm::vec2& outputSize)
        {
            const glm::vec2 size = glm::max(outputSize, glm::vec2(1.0f));
            return glm::mat4(glm::vec4(2.0f / size.x, 0.0f, 0.0f, 0.0f),
                             glm::vec4(0.0f, -2.0f / size.y, 0.0f, 0.0f),
                             glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
                             glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    void Renderer2D::Update(uint32_t currentImage, const shaderio::UniformBufferObject& frameUniforms)
    {
        const size_t worldQuadCount = std::min(m_data.quadDatas.size(), kMaxQuads);
        const size_t screenQuadCount = std::min(m_data.screenQuadDatas.size(), kMaxQuads - worldQuadCount);
        auto* quads = static_cast<shaderio::QuadData*>(m_data.quadStorageBuffersMapped[currentImage]);
        memcpy(quads, m_data.quadDatas.data(), worldQuadCount * sizeof(shaderio::QuadData));
        memcpy(quads + worldQuadCount, m_data.screenQuadDatas.data(), screenQuadCount * sizeof(shaderio::QuadData));

        memcpy(m_data.circleStorageBuffersMapped[currentImage], m_data.circleDatas.data(),
               std::min(m_data.circleDatas.size(), kMaxCircles) * sizeof(shaderio::CircleData));

        const size_t worldGlyphCount = std::min(m_data.textDatas.size(), kMaxGlyphs);
        const size_t screenGlyphCount = std::min(m_data.screenTextDatas.size(), kMaxGlyphs - worldGlyphCount);
        auto* glyphs = static_cast<shaderio::TextData*>(m_data.textStorageBuffersMapped[currentImage]);
        memcpy(glyphs, m_data.textDatas.data(), worldGlyphCount * sizeof(shaderio::TextData));
        memcpy(glyphs + worldGlyphCount, m_data.screenTextDatas.data(), screenGlyphCount * sizeof(shaderio::TextData));

        if (!m_data.screenQuadDatas.empty() || !m_data.screenTextDatas.empty())
        {
            // Everything else the 2D shaders read (the bindless heap offset) stays the frame's own.
            shaderio::UniformBufferObject screenUniforms = frameUniforms;
            screenUniforms.view = glm::mat4(1.0f);
            screenUniforms.proj = ScreenProjection(m_ScreenSize);
            screenUniforms.nonJitteredProj = screenUniforms.proj;
            memcpy(m_data.screenUniformBuffersMapped[currentImage], &screenUniforms, sizeof(shaderio::UniformBufferObject));
        }

        const size_t thickCount = std::min(m_data.lineDatas.size(), kMaxLines);
        const size_t thinCount = std::min(m_data.thinLineDatas.size(), kMaxLines - thickCount);
        const size_t xrayCount = std::min(m_data.xrayLineDatas.size(), kMaxLines - thickCount - thinCount);
        auto* lines = static_cast<shaderio::LineData*>(m_data.lineStorageBuffersMapped[currentImage]);
        memcpy(lines, m_data.lineDatas.data(), thickCount * sizeof(shaderio::LineData));
        memcpy(lines + thickCount, m_data.thinLineDatas.data(), thinCount * sizeof(shaderio::LineData));
        memcpy(lines + thickCount + thinCount, m_data.xrayLineDatas.data(), xrayCount * sizeof(shaderio::LineData));
    }

    // Runs inside a Begin rendering
    void Renderer2D::Flush(NRI::CommandBuffer& commandBuffer, const NRI::Buffer& currentUniformBuffer, uint32_t frameIndex)
    {
        // QuadMesh
        if (!m_data.quadDatas.empty())
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_QuadMeshPipeline);
            
            shaderio::PushConstantQuad quadReferences;
            quadReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            quadReferences.quadDataReference = m_data.quadStorageBuffers[frameIndex]->getDeviceAddress();
            quadReferences.numOfElements = std::min(m_data.quadDatas.size(), kMaxQuads);
            commandBuffer.pushData(&quadReferences, sizeof(shaderio::PushConstantQuad));
            
            commandBuffer.drawMeshTasks(1, 1, 1);
        }
        
        // CircleMesh
        if (!m_data.circleDatas.empty())
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_CircleMeshPipeline);
            
            shaderio::PushConstantCircle circleReferences;
            circleReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            circleReferences.circleDataReference = m_data.circleStorageBuffers[frameIndex]->getDeviceAddress();
            circleReferences.numOfElements = std::min(m_data.circleDatas.size(), kMaxCircles);
            commandBuffer.pushData(&circleReferences, sizeof(shaderio::PushConstantCircle));

            commandBuffer.drawMeshTasks(1, 1, 1);
        }
        
        // TextMesh
        if (!m_data.textDatas.empty())
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_TextMeshPipeline);
            
            shaderio::PushConstantText circleReferences;
            circleReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            circleReferences.textDataReference = m_data.textStorageBuffers[frameIndex]->getDeviceAddress();
            circleReferences.numOfElements = std::min(m_data.textDatas.size(), kMaxGlyphs);
            commandBuffer.pushData(&circleReferences, sizeof(shaderio::PushConstantText));

            commandBuffer.drawMeshTasks(1, 1, 1);
        }
        
        // Grid (Godot's editor grid, see GridMesh.slang): real one pixel lines, one mesh shader group each
        if (m_GridDraw)
        {
            commandBuffer.setLineWidth(1.0f);
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_GridMeshPipeline);

            shaderio::PushConstantGrid gridReferences = m_Grid;
            gridReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            commandBuffer.pushData(&gridReferences, sizeof(shaderio::PushConstantGrid));

            commandBuffer.drawMeshTasks(1, 1, 1);
        }

        // LineMesh
        if (!m_data.lineDatas.empty())
        {
            commandBuffer.setLineWidth(5.0f);
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_LineMeshPipeline);

            shaderio::PushConstantLine circleReferences;
            circleReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
            circleReferences.lineDataReference = m_data.lineStorageBuffers[frameIndex]->getDeviceAddress();
            circleReferences.numOfElements = std::min(m_data.lineDatas.size(), kMaxLines);
            commandBuffer.pushData(&circleReferences, sizeof(shaderio::PushConstantLine));
            
            commandBuffer.drawMeshTasks(1, 1, 1);
        }

        // Thin lines: the same pipeline drawn one pixel wide, from the same buffer behind the thick lines (an address offset).
        size_t thinCount = 0;
        {
            const size_t thickCount = std::min(m_data.lineDatas.size(), kMaxLines);
            thinCount = std::min(m_data.thinLineDatas.size(), kMaxLines - thickCount);
            if (thinCount > 0)
            {
                commandBuffer.setLineWidth(1.0f);
                commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_LineMeshPipeline);

                shaderio::PushConstantLine thinReferences;
                thinReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
                thinReferences.lineDataReference = m_data.lineStorageBuffers[frameIndex]->getDeviceAddress() + thickCount * sizeof(shaderio::LineData);
                thinReferences.numOfElements = thinCount;
                commandBuffer.pushData(&thinReferences, sizeof(shaderio::PushConstantLine));

                commandBuffer.drawMeshTasks(1, 1, 1);
            }
        }

        // X-ray lines: depth testing off for this batch only, so a collider wireframe stays visible embedded inside its mesh
        // instead of mostly hidden behind it; restored afterwards since this is not necessarily the pass's last draw.
        {
            const size_t thickCount = std::min(m_data.lineDatas.size(), kMaxLines);
            const size_t xrayCount = std::min(m_data.xrayLineDatas.size(), kMaxLines - thickCount - thinCount);
            if (xrayCount > 0)
            {
                commandBuffer.setLineWidth(1.5f);
                commandBuffer.setDepthTestEnable(false);
                commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_LineMeshPipeline);

                shaderio::PushConstantLine xrayReferences;
                xrayReferences.matrixReference = currentUniformBuffer.getDeviceAddress();
                xrayReferences.lineDataReference = m_data.lineStorageBuffers[frameIndex]->getDeviceAddress() + (thickCount + thinCount) * sizeof(shaderio::LineData);
                xrayReferences.numOfElements = xrayCount;
                commandBuffer.pushData(&xrayReferences, sizeof(shaderio::PushConstantLine));

                commandBuffer.drawMeshTasks(1, 1, 1);
                commandBuffer.setDepthTestEnable(true);
            }
        }

        // Screen space last, so it lands on top of the world-space 2D content too: panels before their text, same
        // pipelines, the pixel-projection uniforms instead of the camera's, depth testing off (nothing may hide it).
        const NRI::Buffer& screenUniforms = *m_data.screenUniformBuffers[frameIndex];
        const size_t worldQuadCount = std::min(m_data.quadDatas.size(), kMaxQuads);
        const size_t screenQuadCount = std::min(m_data.screenQuadDatas.size(), kMaxQuads - worldQuadCount);
        const size_t worldGlyphCount = std::min(m_data.textDatas.size(), kMaxGlyphs);
        const size_t screenGlyphCount = std::min(m_data.screenTextDatas.size(), kMaxGlyphs - worldGlyphCount);
        if (screenQuadCount == 0 && screenGlyphCount == 0)
            return;
        commandBuffer.setDepthTestEnable(false);

        if (screenQuadCount > 0)
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_QuadMeshPipeline);

            shaderio::PushConstantQuad screenQuadReferences;
            screenQuadReferences.matrixReference = screenUniforms.getDeviceAddress();
            screenQuadReferences.quadDataReference = m_data.quadStorageBuffers[frameIndex]->getDeviceAddress() + worldQuadCount * sizeof(shaderio::QuadData);
            screenQuadReferences.numOfElements = screenQuadCount;
            commandBuffer.pushData(&screenQuadReferences, sizeof(shaderio::PushConstantQuad));

            commandBuffer.drawMeshTasks(1, 1, 1);
        }

        if (screenGlyphCount > 0)
        {
            commandBuffer.bindPipeline(NRI::PipelineBindPoint::Graphics, *m_TextMeshPipeline);

            shaderio::PushConstantText screenTextReferences;
            screenTextReferences.matrixReference = screenUniforms.getDeviceAddress();
            screenTextReferences.textDataReference = m_data.textStorageBuffers[frameIndex]->getDeviceAddress() + worldGlyphCount * sizeof(shaderio::TextData);
            screenTextReferences.numOfElements = screenGlyphCount;
            commandBuffer.pushData(&screenTextReferences, sizeof(shaderio::PushConstantText));

            commandBuffer.drawMeshTasks(1, 1, 1);
        }
        commandBuffer.setDepthTestEnable(true); // restored like the x-ray lines do: not necessarily the pass's last draw
    }

    void Renderer2D::BeginScreenSpace(const glm::vec2& outputSize)
    {
        m_ScreenSpace = true;
        m_ScreenSize = outputSize;
    }

    void Renderer2D::EndScreenSpace()
    {
        m_ScreenSpace = false;
    }

    void Renderer2D::createScreenUniformBuffers()
    {
        const uint64_t bufferSize = sizeof(shaderio::UniformBufferObject);
        m_data.screenUniformBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_data.screenUniformBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_context.device.createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Uniform
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_data.screenUniformBuffers.emplace_back(std::move(uboBuffer));
            m_data.screenUniformBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer2D::EndFrame()
    {
        m_data.screenQuadDatas.clear();
        m_data.screenTextDatas.clear();
        m_ScreenSpace = false;
        m_data.quadDatas.clear();
        m_data.circleDatas.clear();
        m_data.textDatas.clear();
        m_data.lineDatas.clear();
        m_data.thinLineDatas.clear();
        m_data.xrayLineDatas.clear();
        m_GridDraw = false;
    }

    // Runs outside of any rendering call whenever you want
    void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform)
    {
    }

    // Runs outside of any rendering call whenever you want
    void Renderer2D::BeginScene(const EditorCamera& camera)
    {

    }

    // Runs outside of any rendering call whenever you want
    void Renderer2D::EndScene()
    {
    }

    void Renderer2D::createMeshPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats =
        {
            NRI::ImageFormat::Surface,
            NRI::ImageFormat::R32SINT
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/QuadMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/QuadMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/QuadMesh.slang"
        });
        m_QuadMeshPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }

    void Renderer2D::createQuadStorageBuffers()
    {
        uint64_t bufferSize = 10000 * sizeof(shaderio::QuadData);
        // Reserve memory in vectors to prevent reallocation overhead
        m_data.quadStorageBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_data.quadStorageBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_context.device.createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_data.quadStorageBuffers.emplace_back(std::move(uboBuffer));
            m_data.quadStorageBuffersMapped.emplace_back(mappedMemory);
        }
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const glm::vec4& color, int entityID)
    {
        shaderio::QuadData instance;
        instance.modelMatrix = transform;
        instance.color = color;
        instance.materialIndex = m_context.whiteTexture->GetDescriptorIndexSlot();
        instance.entityID = entityID;

        (m_ScreenSpace ? m_data.screenQuadDatas : m_data.quadDatas).emplace_back(instance);
    }

    void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor, int entityID)
    {
        shaderio::QuadData instance;
        instance.modelMatrix = transform;
        instance.color = tintColor;
        instance.materialIndex = texture->GetDescriptorIndexSlot();
        instance.entityID = entityID;

        (m_ScreenSpace ? m_data.screenQuadDatas : m_data.quadDatas).emplace_back(instance);
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
    
    void Renderer2D::createCircleMeshPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats =
        {
            NRI::ImageFormat::Surface,
            NRI::ImageFormat::R32SINT
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/CircleMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/CircleMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/CircleMesh.slang"
        });
        m_CircleMeshPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }
    
    void Renderer2D::createCircleStorageBuffers()
    {
        uint64_t bufferSize = 10000 * sizeof(shaderio::CircleData);
        // Reserve memory in vectors to prevent reallocation overhead
        m_data.circleStorageBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_data.circleStorageBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_context.device.createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_data.circleStorageBuffers.emplace_back(std::move(uboBuffer));
            m_data.circleStorageBuffersMapped.emplace_back(mappedMemory);
        }
    }
    
    void Renderer2D::DrawCircle(const glm::mat4& transform, const glm::vec4& color, float thickness /*= 1.0f*/, float fade /*= 0.005f*/, int entityID /*= -1*/)
    {
        shaderio::CircleData instance;
        instance.worldPosition = transform;
        instance.color = color;
        instance.thickness = thickness;
        instance.fade = fade;
        instance.entityID = entityID;
        
        m_data.circleDatas.emplace_back(instance);
    }
    
     void Renderer2D::createTextMeshPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats =
        {
            NRI::ImageFormat::Surface,
            NRI::ImageFormat::R32SINT
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/TextMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/TextMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/TextMesh.slang"
        });
        m_TextMeshPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }
    
    void Renderer2D::createTextStorageBuffers()
    {
        uint64_t bufferSize = 10000 * sizeof(shaderio::TextData);
        // Reserve memory in vectors to prevent reallocation overhead
        m_data.textStorageBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_data.textStorageBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_context.device.createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_data.textStorageBuffers.emplace_back(std::move(uboBuffer));
            m_data.textStorageBuffersMapped.emplace_back(mappedMemory);
        }
    }
    
    void Renderer2D::DrawString(const std::string& string, Ref<Font> font, const glm::mat4& transform, const TextParams& textParams, int entityID)
    {
        const auto& fontGeometry = font->GetMSDFData()->FontGeometry;
        const auto& metrics = fontGeometry.getMetrics();
        Ref<Texture2D> fontAtlas = font->GetAtlasTexture();

        m_data.FontAtlasTexture = fontAtlas;

        double x = 0.0;
        double fsScale = 1.0 / (metrics.ascenderY - metrics.descenderY);
        double y = 0.0;

        const float spaceGlyphAdvance = fontGeometry.getGlyph(' ')->getAdvance();

        for (size_t i = 0; i < string.size(); i++)
        {
            char character = string[i];
            if (character == '\r')
                continue;

            if (character == '\n')
            {
                x = 0;
                y -= fsScale * metrics.lineHeight * textParams.LineSpacing;
                continue;
            }

            if (character == ' ')
            {
                float advance = spaceGlyphAdvance;
                if (i < string.size() - 1)
                {
                    char nextCharacter = string[i + 1];
                    double dAdvance;
                    fontGeometry.getAdvance(dAdvance, character, nextCharacter);
                    advance = (float)dAdvance;
                }

                x += fsScale * advance + textParams.Kerning;
                continue;
            }

            if (character == '\t')
            {
                // NOTE(Yan): is this right?
                x += 4.0f * (fsScale * spaceGlyphAdvance + textParams.Kerning);
                continue;
            }

            auto glyph = fontGeometry.getGlyph(character);
            if (!glyph)
                glyph = fontGeometry.getGlyph('?');
            if (!glyph)
                return;

            double al, ab, ar, at;
            glyph->getQuadAtlasBounds(al, ab, ar, at);
            glm::vec2 texCoordMin((float)al, (float)ab);
            glm::vec2 texCoordMax((float)ar, (float)at);

            double pl, pb, pr, pt;
            glyph->getQuadPlaneBounds(pl, pb, pr, pt);
            glm::vec2 quadMin((float)pl, (float)pb);
            glm::vec2 quadMax((float)pr, (float)pt);

            quadMin *= fsScale, quadMax *= fsScale;
            quadMin += glm::vec2(x, y);
            quadMax += glm::vec2(x, y);

            float texelWidth = 1.0f / fontAtlas->GetWidth();
            float texelHeight = 1.0f / fontAtlas->GetHeight();
            texCoordMin *= glm::vec2(texelWidth, texelHeight);
            texCoordMax *= glm::vec2(texelWidth, texelHeight);

            shaderio::TextData instance;
            instance.quadMin = quadMin;
            instance.quadMax = quadMax;
            instance.transform = transform; // store transform per draw
            instance.texMin = texCoordMin;
            instance.texMax = texCoordMax;
            instance.color = textParams.Color;
            instance.materialIndex = fontAtlas->GetDescriptorIndexSlot();
            instance.entityID = entityID;

            (m_ScreenSpace ? m_data.screenTextDatas : m_data.textDatas).emplace_back(instance);

            if (i < string.size() - 1)
            {
                double advance = glyph->getAdvance();
                char nextCharacter = string[i + 1];
                fontGeometry.getAdvance(advance, character, nextCharacter);

                x += fsScale * advance + textParams.Kerning;
            }
        }
    }

    void Renderer2D::DrawString(const std::string& string, const glm::mat4& transform, const TextComponent& component, int entityID)
    {
        DrawString(string, component.FontAsset, transform, {component.Color, component.Kerning, component.LineSpacing}, entityID);
    }
    
    void Renderer2D::createLineMeshPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats =
        {
            NRI::ImageFormat::Surface,
            NRI::ImageFormat::R32SINT
        };
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Task,
            .entryPoint = "taskMain",
            .sourcePath = "assets/shaders/LineMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Mesh,
            .entryPoint = "meshMain",
            .sourcePath = "assets/shaders/LineMesh.slang"
        });
        desc.shaders.push_back({
            .stage = NRI::ShaderStage::Fragment,
            .entryPoint = "fragMain",
            .sourcePath = "assets/shaders/LineMesh.slang"
        });
        m_LineMeshPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }
    
    void Renderer2D::createGridMeshPipeline(bool forceCompile)
    {
        NRI::PipelineDesc desc{};
        desc.forceCompile = forceCompile;
        desc.colorFormats =
        {
            NRI::ImageFormat::Surface,
            NRI::ImageFormat::R32SINT
        };
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Task, .entryPoint = "taskMain", .sourcePath = "assets/shaders/GridMesh.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Mesh, .entryPoint = "meshMain", .sourcePath = "assets/shaders/GridMesh.slang" });
        desc.shaders.push_back({ .stage = NRI::ShaderStage::Fragment, .entryPoint = "fragMain", .sourcePath = "assets/shaders/GridMesh.slang" });
        m_GridMeshPipeline = m_context.device.createPipeline(desc, m_context.shaderCompiler);
    }

    void Renderer2D::createLineStorageBuffers()
    {
        uint64_t bufferSize = 10000 * sizeof(shaderio::LineData);
        // Reserve memory in vectors to prevent reallocation overhead
        m_data.lineStorageBuffers.reserve(MAX_FRAMES_IN_FLIGHT);
        m_data.lineStorageBuffersMapped.reserve(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            std::unique_ptr<NRI::Buffer> uboBuffer = m_context.device.createBuffer(NRI::BufferDesc
                {
                    .size = bufferSize,
                    .usage = NRI::BufferUsage::Storage
                });

            void* mappedMemory = uboBuffer->map(0, bufferSize);

            m_data.lineStorageBuffers.emplace_back(std::move(uboBuffer));
            m_data.lineStorageBuffersMapped.emplace_back(mappedMemory);
        }
    }
    
    void Renderer2D::DrawLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, int entityID)
    {
        shaderio::LineData instance;
        instance.p0 = p0;
        instance.p1 = p1;
        instance.color = color;
        instance.entityID = entityID;

        m_data.lineDatas.emplace_back(instance);
    }

    // Godot's Node3DEditor::_init_grid with Godot's defaults (8 steps, levels 0..2, bias -0.2), the numbers it works out on the CPU.
    // `cell` is the smallest step (Godot's 1 m): the cell size stays the same while you zoom, it only steps up 8x when the camera is
    // far enough that the cells would be too small.
    void Renderer2D::DrawGrid(const glm::vec3& focus, float cell, int planeAxis, float cameraDistance)
    {
        constexpr int steps = 8;
        constexpr int gridSize = 200;
        constexpr float divisionLevelBias = -0.2f;
        constexpr float divisionLevelMin = 0.0f;
        constexpr float divisionLevelMax = 2.0f;

        const int axisA = (planeAxis + 1) % 3;
        const int axisB = (planeAxis + 2) % 3;

        const float distance = std::max(std::abs(cameraDistance), 1e-6f);
        const float divisionLevel = std::log(distance / cell) / std::log(static_cast<float>(steps)) + divisionLevelBias;
        const float clampedLevel = std::clamp(divisionLevel, divisionLevelMin, divisionLevelMax);
        const float flooredLevel = std::floor(clampedLevel);
        const float decimals = clampedLevel - flooredLevel;

        const float smallStep = cell * std::pow(static_cast<float>(steps), flooredLevel);
        const float largeStep = smallStep * steps;

        float fadeSize = cell * std::pow(static_cast<float>(steps), divisionLevel - 1.0f);
        fadeSize = std::clamp(fadeSize, cell * std::pow(static_cast<float>(steps), divisionLevelMin), cell * std::pow(static_cast<float>(steps), divisionLevelMax));

        m_Grid.centerA = largeStep * std::trunc(focus[axisA] / largeStep);
        m_Grid.centerB = largeStep * std::trunc(focus[axisB] / largeStep);
        m_Grid.smallStep = smallStep;
        m_Grid.gridFadeSize = (gridSize - steps) * fadeSize;
        m_Grid.decimals = decimals;
        m_Grid.steps = steps;
        m_Grid.gridSize = gridSize;
        m_Grid.planeAxis = planeAxis;
        m_GridDraw = true;
    }

    void Renderer2D::DrawThinLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color)
    {
        shaderio::LineData instance;
        instance.p0 = p0;
        instance.p1 = p1;
        instance.color = color;
        instance.entityID = -1;

        m_data.thinLineDatas.emplace_back(instance);
    }

    void Renderer2D::DrawXRayLine(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color)
    {
        shaderio::LineData instance;
        instance.p0 = p0;
        instance.p1 = p1;
        instance.color = color;
        instance.entityID = -1;

        m_data.xrayLineDatas.emplace_back(instance);
    }

    void Renderer2D::DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, int entityID)
    {
        glm::vec3 p0 = glm::vec3(position.x - size.x * 0.5f, position.y - size.y * 0.5f, position.z);
        glm::vec3 p1 = glm::vec3(position.x + size.x * 0.5f, position.y - size.y * 0.5f, position.z);
        glm::vec3 p2 = glm::vec3(position.x + size.x * 0.5f, position.y + size.y * 0.5f, position.z);
        glm::vec3 p3 = glm::vec3(position.x - size.x * 0.5f, position.y + size.y * 0.5f, position.z);

        DrawLine(p0, p1, color, entityID);
        DrawLine(p1, p2, color, entityID);
        DrawLine(p2, p3, color, entityID);
        DrawLine(p3, p0, color, entityID);
    }

    void Renderer2D::DrawRect(const glm::mat4& transform, const glm::vec4& color, int entityID)
    {
        glm::vec3 p0 = transform * glm::vec4(-0.5f, -0.5f, 0.0f, 1.0f);
        glm::vec3 p1 = transform * glm::vec4(0.5f, -0.5f, 0.0f, 1.0f);
        glm::vec3 p2 = transform * glm::vec4(0.5f, 0.5f, 0.0f, 1.0f);
        glm::vec3 p3 = transform * glm::vec4(-0.5f, 0.5f, 0.0f, 1.0f);

        DrawLine(p0, p1, color, entityID);
        DrawLine(p1, p2, color, entityID);
        DrawLine(p2, p3, color, entityID);
        DrawLine(p3, p0, color, entityID);
    }
}
