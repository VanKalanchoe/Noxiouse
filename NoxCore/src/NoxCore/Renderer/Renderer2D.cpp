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
    }

    void Renderer2D::Update(uint32_t currentImage)
    {
        memcpy(m_data.quadStorageBuffersMapped[currentImage], m_data.quadDatas.data(), m_data.quadDatas.size() * sizeof(shaderio::QuadData));
        memcpy(m_data.circleStorageBuffersMapped[currentImage], m_data.circleDatas.data(), m_data.circleDatas.size() * sizeof(shaderio::CircleData));
        memcpy(m_data.textStorageBuffersMapped[currentImage], m_data.textDatas.data(), m_data.textDatas.size() * sizeof(shaderio::TextData));
        const size_t thickCount = std::min(m_data.lineDatas.size(), kMaxLines);
        const size_t thinCount = std::min(m_data.thinLineDatas.size(), kMaxLines - thickCount);
        auto* lines = static_cast<shaderio::LineData*>(m_data.lineStorageBuffersMapped[currentImage]);
        memcpy(lines, m_data.lineDatas.data(), thickCount * sizeof(shaderio::LineData));
        memcpy(lines + thickCount, m_data.thinLineDatas.data(), thinCount * sizeof(shaderio::LineData));
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
            quadReferences.numOfElements = m_data.quadDatas.size();
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
            circleReferences.numOfElements = m_data.circleDatas.size();
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
            circleReferences.numOfElements = m_data.textDatas.size();
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
        {
            const size_t thickCount = std::min(m_data.lineDatas.size(), kMaxLines);
            const size_t thinCount = std::min(m_data.thinLineDatas.size(), kMaxLines - thickCount);
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
    }

    void Renderer2D::EndFrame()
    {
        m_data.quadDatas.clear();
        m_data.circleDatas.clear();
        m_data.textDatas.clear();
        m_data.lineDatas.clear();
        m_data.thinLineDatas.clear();
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

            m_data.textDatas.emplace_back(instance);

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
    void Renderer2D::DrawGrid(const glm::vec3& cameraPosition, float cell)
    {
        constexpr int steps = 8;
        constexpr int gridSize = 200;
        constexpr float divisionLevelBias = -0.2f;
        constexpr float divisionLevelMin = 0.0f;
        constexpr float divisionLevelMax = 2.0f;

        const float cameraDistance = std::max(std::abs(cameraPosition.y), 1e-6f);
        const float divisionLevel = std::log(cameraDistance / cell) / std::log(static_cast<float>(steps)) + divisionLevelBias;
        const float clampedLevel = std::clamp(divisionLevel, divisionLevelMin, divisionLevelMax);
        const float flooredLevel = std::floor(clampedLevel);
        const float decimals = clampedLevel - flooredLevel;

        const float smallStep = cell * std::pow(static_cast<float>(steps), flooredLevel);
        const float largeStep = smallStep * steps;

        float fadeSize = cell * std::pow(static_cast<float>(steps), divisionLevel - 1.0f);
        fadeSize = std::clamp(fadeSize, cell * std::pow(static_cast<float>(steps), divisionLevelMin), cell * std::pow(static_cast<float>(steps), divisionLevelMax));

        m_Grid.centerX = largeStep * std::trunc(cameraPosition.x / largeStep);
        m_Grid.centerZ = largeStep * std::trunc(cameraPosition.z / largeStep);
        m_Grid.smallStep = smallStep;
        m_Grid.gridFadeSize = (gridSize - steps) * fadeSize;
        m_Grid.decimals = decimals;
        m_Grid.steps = steps;
        m_Grid.gridSize = gridSize;
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
