#include "EditorV2Layer.h"

#include "NoxCore/Core/Application.h"
#include "NoxCore/Renderer/Font.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox
{
    EditorV2Layer::EditorV2Layer() : Layer("EditorV2Layer")
    {
        m_DefaultFont = Font::GetDefault();
    }

    void EditorV2Layer::OnUpdate(Timestep ts)
    {
        m_Elapsed += ts.GetSeconds();
    }

    void EditorV2Layer::OnRender()
    {
        Application& app = Application::Get();
        Renderer* renderer = app.GetRenderer();
        if (!renderer)
            return;

        UI::UIDrawList* draw = renderer->GetUIDrawList();
        if (!draw)
            return;

        int width = 1920;
        int height = 1080;
        app.getWindow()->getSizeInPixels(width, height);
        const glm::vec2 viewportSize(static_cast<float>(width), static_cast<float>(height));

        draw->Clear(viewportSize);

        // Background canvas
        draw->AddRoundedRect(
            { 0.0f, 0.0f }, viewportSize,
            { 0.070f, 0.078f, 0.102f, 1.0f }
        );

        // Top bar
        draw->AddRoundedRectGradient(
            { 12.0f, 12.0f }, { viewportSize.x - 12.0f, 56.0f },
            { 0.125f, 0.141f, 0.184f, 1.0f },
            { 0.102f, 0.114f, 0.149f, 1.0f },
            glm::vec4(8.0f),
            1.0f, { 0.22f, 0.25f, 0.33f, 0.9f },
            { 0.0f, 0.0f, 0.0f, 0.45f }, { 0.0f, 4.0f }, 10.0f
        );
        draw->AddText("NoxEditorV2  |  Phase 0: Bindless SDF UI Renderer", m_DefaultFont, { 28.0f, 24.0f }, 18.0f,
                      { 0.92f, 0.94f, 0.98f, 1.0f });

        // Left Hierarchy / Content card
        draw->AddRoundedRect(
            { 12.0f, 68.0f }, { 320.0f, viewportSize.y - 12.0f },
            { 0.102f, 0.114f, 0.149f, 0.96f },
            glm::vec4(10.0f),
            1.0f, { 0.22f, 0.25f, 0.33f, 0.85f },
            { 0.0f, 0.0f, 0.0f, 0.45f }, { 0.0f, 6.0f }, 14.0f
        );
        draw->AddText("Scene Hierarchy (Phase 2)", m_DefaultFont, { 28.0f, 84.0f }, 16.0f,
                      { 0.80f, 0.86f, 0.96f, 1.0f });

        // Center Node Graph preview card
        const glm::vec2 cardMin(334.0f, 68.0f);
        const glm::vec2 cardMax(viewportSize.x - 334.0f, viewportSize.y - 12.0f);
        draw->AddRoundedRect(
            cardMin, cardMax,
            { 0.082f, 0.092f, 0.122f, 1.0f },
            glm::vec4(10.0f),
            1.0f, { 0.22f, 0.25f, 0.33f, 0.85f }
        );

        draw->PushClipRect(cardMin + glm::vec2(2.0f), cardMax - glm::vec2(2.0f), glm::vec4(8.0f));

        // Sample Node A
        const glm::vec2 nodeAMin = cardMin + glm::vec2(80.0f, 120.0f);
        const glm::vec2 nodeAMax = nodeAMin + glm::vec2(220.0f, 120.0f);
        draw->AddRoundedRect(
            nodeAMin, nodeAMax,
            { 0.13f, 0.145f, 0.19f, 0.96f },
            glm::vec4(8.0f),
            1.5f, { 0.28f, 0.56f, 0.96f, 0.95f },
            { 0.0f, 0.0f, 0.0f, 0.55f }, { 0.0f, 8.0f }, 16.0f
        );
        draw->AddRoundedRectGradient(
            nodeAMin, { nodeAMax.x, nodeAMin.y + 30.0f },
            { 0.20f, 0.44f, 0.82f, 1.0f },
            { 0.15f, 0.34f, 0.66f, 1.0f },
            { 8.0f, 8.0f, 0.0f, 0.0f }
        );
        draw->AddText("Texture Sample", m_DefaultFont, nodeAMin + glm::vec2(12.0f, 7.0f), 14.0f,
                      { 0.97f, 0.98f, 1.0f, 1.0f });

        // Sample Node B
        const glm::vec2 nodeBMin = cardMin + glm::vec2(440.0f, 200.0f);
        const glm::vec2 nodeBMax = nodeBMin + glm::vec2(220.0f, 130.0f);
        draw->AddRoundedRect(
            nodeBMin, nodeBMax,
            { 0.13f, 0.145f, 0.19f, 0.96f },
            glm::vec4(8.0f),
            1.0f, { 0.30f, 0.34f, 0.44f, 0.9f },
            { 0.0f, 0.0f, 0.0f, 0.55f }, { 0.0f, 8.0f }, 16.0f
        );
        draw->AddRoundedRectGradient(
            nodeBMin, { nodeBMax.x, nodeBMin.y + 30.0f },
            { 0.18f, 0.60f, 0.44f, 1.0f },
            { 0.13f, 0.46f, 0.34f, 1.0f },
            { 8.0f, 8.0f, 0.0f, 0.0f }
        );
        draw->AddText("Material Output", m_DefaultFont, nodeBMin + glm::vec2(12.0f, 7.0f), 14.0f,
                      { 0.97f, 0.98f, 1.0f, 1.0f });

        // Smooth Cubic Bezier wire connecting Node A -> Node B
        const glm::vec2 wireStart(nodeAMax.x, nodeAMin.y + 68.0f);
        const glm::vec2 wireEnd(nodeBMin.x, nodeBMin.y + 68.0f);
        const float tangent = std::max(60.0f, std::abs(wireEnd.x - wireStart.x) * 0.5f);
        draw->AddBezier(
            wireStart,
            wireStart + glm::vec2(tangent, 0.0f),
            wireEnd - glm::vec2(tangent, 0.0f),
            wireEnd,
            3.0f,
            { 0.35f, 0.78f, 0.98f, 0.95f }
        );

        draw->PopClipRect();

        // Right Inspector card
        draw->AddRoundedRect(
            { viewportSize.x - 320.0f, 68.0f }, { viewportSize.x - 12.0f, viewportSize.y - 12.0f },
            { 0.102f, 0.114f, 0.149f, 0.96f },
            glm::vec4(10.0f),
            1.0f, { 0.22f, 0.25f, 0.33f, 0.85f },
            { 0.0f, 0.0f, 0.0f, 0.45f }, { 0.0f, 6.0f }, 14.0f
        );
        draw->AddText("Inspector (Phase 2)", m_DefaultFont, { viewportSize.x - 304.0f, 84.0f }, 16.0f,
                      { 0.80f, 0.86f, 0.96f, 1.0f });
    }
}
