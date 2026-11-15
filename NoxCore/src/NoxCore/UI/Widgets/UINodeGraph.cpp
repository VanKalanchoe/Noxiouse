#include "UINodeGraph.h"

#include <algorithm>
#include <cmath>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        constexpr float kNodeWidth = 185.0f;
        constexpr float kHeaderHeight = 28.0f;
        constexpr float kPinRowHeight = 22.0f;
        constexpr float kPinRadius = 5.5f;
        constexpr float kNodeBottomPad = 8.0f;
    }

    UINodeGraph::UINodeGraph()
    {
        StyleClass = "NodeGraph";
        DrawBackground = true;
        ClipChildren = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
    }

    uint64_t UINodeGraph::AddNode(UINode node)
    {
        if (node.Id == 0)
            node.Id = m_NextId++;
        else
            m_NextId = std::max(m_NextId, node.Id + 1);

        for (auto& p : node.Inputs)
        {
            p.IsOutput = false;
            if (p.Id == 0) p.Id = m_NextId++;
            else m_NextId = std::max(m_NextId, p.Id + 1);
        }
        for (auto& p : node.Outputs)
        {
            p.IsOutput = true;
            if (p.Id == 0) p.Id = m_NextId++;
            else m_NextId = std::max(m_NextId, p.Id + 1);
        }

        const uint64_t id = node.Id;
        m_Nodes.push_back(std::move(node));
        return id;
    }

    uint64_t UINodeGraph::AddLink(uint64_t outputPinId, uint64_t inputPinId)
    {
        if (outputPinId == 0 || inputPinId == 0 || outputPinId == inputPinId)
            return 0;

        // Replace existing link on the input pin (single-input rule)
        m_Links.erase(
            std::remove_if(m_Links.begin(), m_Links.end(), [inputPinId](const UINodeLink& l)
            {
                return l.InputPinId == inputPinId;
            }),
            m_Links.end()
        );

        UINodeLink link{ m_NextId++, outputPinId, inputPinId };
        m_Links.push_back(link);
        if (OnLinkCreated)
            OnLinkCreated(link);
        return link.Id;
    }

    void UINodeGraph::Clear()
    {
        m_Nodes.clear();
        m_Links.clear();
        m_SelectedNodeId = 0;
        m_SelectedLinkId = 0;
        m_DraggingNode = false;
        m_DraggingPinId = 0;
    }

    UIVec2 UINodeGraph::GraphToScreen(const UIVec2& p) const
    {
        return GetBounds().Min + m_Pan + p * m_Zoom;
    }

    UIVec2 UINodeGraph::ScreenToGraph(const UIVec2& p) const
    {
        return (p - GetBounds().Min - m_Pan) / m_Zoom;
    }

    UIVec2 UINodeGraph::GetNodeSize(const UINode& node) const
    {
        const size_t rows = std::max(node.Inputs.size(), node.Outputs.size());
        const float h = kHeaderHeight + static_cast<float>(rows) * kPinRowHeight + kNodeBottomPad;
        return { kNodeWidth, h };
    }

    bool UINodeGraph::TryGetPinPos(uint64_t pinId, UIVec2& outScreenPos, UIColor& outColor, bool& outIsOutput) const
    {
        for (const auto& node : m_Nodes)
        {
            for (size_t i = 0; i < node.Inputs.size(); ++i)
            {
                if (node.Inputs[i].Id == pinId)
                {
                    const UIVec2 local{ 0.0f, kHeaderHeight + (static_cast<float>(i) + 0.5f) * kPinRowHeight + 2.0f };
                    outScreenPos = GraphToScreen(node.Position + local);
                    outColor = node.Inputs[i].Color;
                    outIsOutput = false;
                    return true;
                }
            }
            for (size_t i = 0; i < node.Outputs.size(); ++i)
            {
                if (node.Outputs[i].Id == pinId)
                {
                    const UIVec2 local{ kNodeWidth, kHeaderHeight + (static_cast<float>(i) + 0.5f) * kPinRowHeight + 2.0f };
                    outScreenPos = GraphToScreen(node.Position + local);
                    outColor = node.Outputs[i].Color;
                    outIsOutput = true;
                    return true;
                }
            }
        }
        return false;
    }

    uint64_t UINodeGraph::HitTestPin(const UIVec2& screenPos, bool* outIsOutput) const
    {
        const float hitR = (kPinRadius + 6.0f) * m_Zoom;
        for (auto it = m_Nodes.rbegin(); it != m_Nodes.rend(); ++it)
        {
            const UINode& node = *it;
            for (size_t i = 0; i < node.Inputs.size(); ++i)
            {
                const UIVec2 p = GraphToScreen(node.Position + UIVec2{ 0.0f, kHeaderHeight + (static_cast<float>(i) + 0.5f) * kPinRowHeight + 2.0f });
                if (glm::length(screenPos - p) <= hitR)
                {
                    if (outIsOutput) *outIsOutput = false;
                    return node.Inputs[i].Id;
                }
            }
            for (size_t i = 0; i < node.Outputs.size(); ++i)
            {
                const UIVec2 p = GraphToScreen(node.Position + UIVec2{ kNodeWidth, kHeaderHeight + (static_cast<float>(i) + 0.5f) * kPinRowHeight + 2.0f });
                if (glm::length(screenPos - p) <= hitR)
                {
                    if (outIsOutput) *outIsOutput = true;
                    return node.Outputs[i].Id;
                }
            }
        }
        return 0;
    }

    int UINodeGraph::HitTestNode(const UIVec2& screenPos) const
    {
        for (int i = static_cast<int>(m_Nodes.size()) - 1; i >= 0; --i)
        {
            const UIVec2 minS = GraphToScreen(m_Nodes[i].Position);
            const UIVec2 maxS = GraphToScreen(m_Nodes[i].Position + GetNodeSize(m_Nodes[i]));
            if (UIRect{ minS, maxS }.Contains(screenPos))
                return i;
        }
        return -1;
    }

    uint64_t UINodeGraph::HitTestLink(const UIVec2& screenPos) const
    {
        const float maxDistSq = 8.0f * 8.0f;
        for (auto it = m_Links.rbegin(); it != m_Links.rend(); ++it)
        {
            UIVec2 p0{}, p3{};
            UIColor c0{}, c3{};
            bool out0 = true, out3 = false;
            if (!TryGetPinPos(it->OutputPinId, p0, c0, out0) || !TryGetPinPos(it->InputPinId, p3, c3, out3))
                continue;

            const float tangent = std::max(45.0f * m_Zoom, std::abs(p3.x - p0.x) * 0.45f);
            const UIVec2 p1 = p0 + UIVec2(tangent, 0.0f);
            const UIVec2 p2 = p3 - UIVec2(tangent, 0.0f);

            UIVec2 prev = p0;
            constexpr int kSegments = 16;
            for (int s = 1; s <= kSegments; ++s)
            {
                const float t = static_cast<float>(s) / static_cast<float>(kSegments);
                const float u = 1.0f - t;
                const UIVec2 cur = (u * u * u) * p0 + (3.0f * u * u * t) * p1 + (3.0f * u * t * t) * p2 + (t * t * t) * p3;

                const UIVec2 ab = cur - prev;
                const float abLenSq = glm::dot(ab, ab);
                const float proj = abLenSq > 1e-5f ? std::clamp(glm::dot(screenPos - prev, ab) / abLenSq, 0.0f, 1.0f) : 0.0f;
                const UIVec2 closest = prev + ab * proj;
                const UIVec2 diff = screenPos - closest;
                if (glm::dot(diff, diff) <= maxDistSq)
                    return it->Id;

                prev = cur;
            }
        }
        return 0;
    }

    bool UINodeGraph::TryGetConnectedColor(uint64_t pinId, UIColor& outColor) const
    {
        for (const auto& link : m_Links)
        {
            if (link.OutputPinId == pinId || link.InputPinId == pinId)
            {
                UIVec2 dummyPos{};
                bool dummyOut = true;
                if (TryGetPinPos(link.OutputPinId, dummyPos, outColor, dummyOut))
                    return true;
            }
        }
        return false;
    }

    bool UINodeGraph::OnPointerDown(const UIPointerEvent& e)
    {
        if (!IsEnabled())
            return false;

        m_DragMouseScreen = e.Position;

        if (e.Button == 3)
        {
            // Right-click a pin or link to disconnect it; otherwise pan.
            if (const uint64_t pinId = HitTestPin(e.Position))
            {
                m_Links.erase(
                    std::remove_if(m_Links.begin(), m_Links.end(), [&](const UINodeLink& l)
                    {
                        if (l.OutputPinId == pinId || l.InputPinId == pinId)
                        {
                            if (OnLinkDeleted) OnLinkDeleted(l.Id);
                            if (m_SelectedLinkId == l.Id) m_SelectedLinkId = 0;
                            return true;
                        }
                        return false;
                    }),
                    m_Links.end()
                );
                return true;
            }
            if (const uint64_t linkId = HitTestLink(e.Position))
            {
                m_Links.erase(
                    std::remove_if(m_Links.begin(), m_Links.end(), [&](const UINodeLink& l) { return l.Id == linkId; }),
                    m_Links.end()
                );
                if (m_SelectedLinkId == linkId) m_SelectedLinkId = 0;
                if (OnLinkDeleted) OnLinkDeleted(linkId);
                return true;
            }
            m_Panning = true;
            return true;
        }

        if (e.Button == 2)
        {
            m_Panning = true;
            return true;
        }

        if (e.Button == 1)
        {
            bool isOut = false;
            if (const uint64_t pinId = HitTestPin(e.Position, &isOut))
            {
                m_SelectedLinkId = 0;
                m_DraggingPinId = pinId;
                m_DraggingPinIsOutput = isOut;
                return true;
            }

            if (const int idx = HitTestNode(e.Position); idx >= 0)
            {
                m_SelectedLinkId = 0;
                m_SelectedNodeId = m_Nodes[idx].Id;
                m_DraggingNode = true;
                if (OnNodeSelected)
                    OnNodeSelected(m_SelectedNodeId);
                return true;
            }

            if (const uint64_t linkId = HitTestLink(e.Position))
            {
                m_SelectedNodeId = 0;
                m_SelectedLinkId = linkId;
                return true;
            }

            m_SelectedNodeId = 0;
            m_SelectedLinkId = 0;
            m_Panning = true; // left-drag on empty canvas also pans smoothly
            return true;
        }

        return UIElement::OnPointerDown(e);
    }

    bool UINodeGraph::OnPointerMove(const UIPointerEvent& e)
    {
        m_DragMouseScreen = e.Position;

        if (m_Panning)
        {
            m_Pan += e.Delta;
            return true;
        }

        if (m_DraggingNode && m_SelectedNodeId != 0)
        {
            for (auto& node : m_Nodes)
            {
                if (node.Id == m_SelectedNodeId)
                {
                    node.Position += e.Delta / m_Zoom;
                    break;
                }
            }
            return true;
        }

        if (m_DraggingPinId != 0)
            return true;

        return UIElement::OnPointerMove(e);
    }

    bool UINodeGraph::OnPointerUp(const UIPointerEvent& e)
    {
        if (m_Panning && (e.Button == 1 || e.Button == 2 || e.Button == 3))
        {
            m_Panning = false;
            return true;
        }

        if (m_DraggingNode && e.Button == 1)
        {
            m_DraggingNode = false;
            return true;
        }

        if (m_DraggingPinId != 0 && e.Button == 1)
        {
            bool targetOut = false;
            const uint64_t targetPin = HitTestPin(e.Position, &targetOut);
            if (targetPin != 0 && targetPin != m_DraggingPinId && targetOut != m_DraggingPinIsOutput)
            {
                const uint64_t outPin = m_DraggingPinIsOutput ? m_DraggingPinId : targetPin;
                const uint64_t inPin = m_DraggingPinIsOutput ? targetPin : m_DraggingPinId;
                AddLink(outPin, inPin);
            }
            m_DraggingPinId = 0;
            return true;
        }

        return UIElement::OnPointerUp(e);
    }

    bool UINodeGraph::OnMouseWheel(const UIPointerEvent& e)
    {
        const UIVec2 before = ScreenToGraph(e.Position);
        const float factor = e.ScrollDelta.y > 0.0f ? 1.10f : 0.90f;
        m_Zoom = std::clamp(m_Zoom * factor, 0.35f, 2.0f);
        const UIVec2 afterScreen = GraphToScreen(before);
        m_Pan += (e.Position - afterScreen);
        return true;
    }

    bool UINodeGraph::OnKeyDown(const UIKeyEvent& e)
    {
        if (e.KeyCode == SDL_SCANCODE_DELETE || e.KeyCode == SDL_SCANCODE_BACKSPACE)
        {
            if (m_SelectedLinkId != 0)
            {
                const uint64_t delLink = m_SelectedLinkId;
                m_Links.erase(
                    std::remove_if(m_Links.begin(), m_Links.end(), [&](const UINodeLink& l) { return l.Id == delLink; }),
                    m_Links.end()
                );
                m_SelectedLinkId = 0;
                if (OnLinkDeleted) OnLinkDeleted(delLink);
                return true;
            }
            if (m_SelectedNodeId != 0)
            {
            const uint64_t delId = m_SelectedNodeId;
            std::vector<uint64_t> deadPins;
            for (const auto& node : m_Nodes)
            {
                if (node.Id == delId)
                {
                    for (const auto& p : node.Inputs) deadPins.push_back(p.Id);
                    for (const auto& p : node.Outputs) deadPins.push_back(p.Id);
                }
            }
            m_Links.erase(
                std::remove_if(m_Links.begin(), m_Links.end(), [&](const UINodeLink& l)
                {
                    return std::find(deadPins.begin(), deadPins.end(), l.OutputPinId) != deadPins.end() ||
                           std::find(deadPins.begin(), deadPins.end(), l.InputPinId) != deadPins.end();
                }),
                m_Links.end()
            );
            m_Nodes.erase(
                std::remove_if(m_Nodes.begin(), m_Nodes.end(), [&](const UINode& n) { return n.Id == delId; }),
                m_Nodes.end()
            );
            m_SelectedNodeId = 0;
            return true;
            }
        }
        return false;
    }

    UIVec2 UINodeGraph::ComputeDesiredSize()
    {
        return { 360.0f, 240.0f };
    }

    void UINodeGraph::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        const UIStyleBox& style = GetResolvedStyle();
        drawList.PushClipRect(b.Min, b.Max, style.CornerRadius.ToVec4());

        // Subtle background grid
        const float gridStep = 32.0f * m_Zoom;
        if (gridStep >= 10.0f)
        {
            const float startX = b.Min.x + std::fmod(m_Pan.x, gridStep);
            const float startY = b.Min.y + std::fmod(m_Pan.y, gridStep);
            const UIColor gridCol{ 1.0f, 1.0f, 1.0f, 0.035f };
            for (float x = startX; x < b.Max.x; x += gridStep)
                drawList.AddRoundedRect({ x, b.Min.y }, { x + 1.0f, b.Max.y }, gridCol);
            for (float y = startY; y < b.Max.y; y += gridStep)
                drawList.AddRoundedRect({ b.Min.x, y }, { b.Max.x, y + 1.0f }, gridCol);
        }

        // Links (cubic Beziers)
        const UIColor accent = theme.GetToken("Accent", { 0.24f, 0.56f, 0.96f, 1.0f });
        for (const auto& link : m_Links)
        {
            UIVec2 p0{}, p3{};
            UIColor c0{}, c3{};
            bool out0 = true, out3 = false;
            if (TryGetPinPos(link.OutputPinId, p0, c0, out0) && TryGetPinPos(link.InputPinId, p3, c3, out3))
            {
                const bool linkSelected = (link.Id == m_SelectedLinkId);
                const float tangent = std::max(45.0f * m_Zoom, std::abs(p3.x - p0.x) * 0.45f);
                const UIColor wireColor = linkSelected ? UIColor{ 1.0f, 1.0f, 1.0f, 0.98f } : c0;
                const float wireWidth = (linkSelected ? 3.8f : 2.5f) * m_Zoom;
                drawList.AddBezier(p0, p0 + UIVec2(tangent, 0.0f), p3 - UIVec2(tangent, 0.0f), p3, wireWidth, wireColor);
            }
        }

        // Active dragged wire preview
        if (m_DraggingPinId != 0)
        {
            UIVec2 pStart{};
            UIColor cStart{};
            bool isOut = true;
            if (TryGetPinPos(m_DraggingPinId, pStart, cStart, isOut))
            {
                const float dir = isOut ? 1.0f : -1.0f;
                const float tangent = std::max(45.0f * m_Zoom, std::abs(m_DragMouseScreen.x - pStart.x) * 0.45f);
                drawList.AddBezier(
                    pStart,
                    pStart + UIVec2(dir * tangent, 0.0f),
                    m_DragMouseScreen - UIVec2(dir * tangent, 0.0f),
                    m_DragMouseScreen,
                    2.5f * m_Zoom,
                    cStart
                );
            }
        }

        // Nodes
        const UIColor borderDefault = theme.GetToken("Border", { 0.22f, 0.25f, 0.33f, 0.9f });
        const float fontSize = 13.0f * m_Zoom;
        const float titleSize = 14.0f * m_Zoom;

        for (const auto& node : m_Nodes)
        {
            const UIVec2 nSize = GetNodeSize(node);
            const UIVec2 nMin = GraphToScreen(node.Position);
            const UIVec2 nMax = GraphToScreen(node.Position + nSize);
            const bool selected = (node.Id == m_SelectedNodeId);
            const float radius = 8.0f * m_Zoom;

            // Body + shadow
            drawList.AddRoundedRect(
                nMin, nMax,
                { 0.11f, 0.13f, 0.18f, 0.96f },
                glm::vec4(radius),
                selected ? 2.0f : 1.0f,
                selected ? accent : borderDefault,
                { 0.0f, 0.0f, 0.0f, 0.45f },
                { 0.0f, 4.0f * m_Zoom },
                12.0f * m_Zoom
            );

            // Header bar
            const UIVec2 hMax{ nMax.x, nMin.y + kHeaderHeight * m_Zoom };
            drawList.AddRoundedRect(
                nMin, hMax,
                node.HeaderColor,
                { radius, radius, 0.0f, 0.0f }
            );

            const UIVec2 tSize = UILabel::MeasureText(node.Title, nullptr, titleSize);
            drawList.AddText(
                node.Title, nullptr,
                { nMin.x + 10.0f * m_Zoom, nMin.y + (kHeaderHeight * m_Zoom - tSize.y) * 0.5f },
                titleSize,
                { 1.0f, 1.0f, 1.0f, 0.96f }
            );

            // Input & Output pins (solid connected-wire color when linked, hollow ring when unlinked)
            const float pr = kPinRadius * m_Zoom;
            auto drawPin = [&](const UIVec2& p, uint64_t pinId, const UIColor& baseColor)
            {
                UIColor connColor{};
                const bool connected = TryGetConnectedColor(pinId, connColor);
                const UIColor ringColor = connected ? connColor : baseColor;
                const UIColor fillColor = connected ? connColor : UIColor{ 0.08f, 0.10f, 0.14f, 1.0f };
                drawList.AddRoundedRect({ p.x - pr, p.y - pr }, { p.x + pr, p.y + pr }, fillColor, glm::vec4(pr), 1.6f * m_Zoom, ringColor);
            };

            for (size_t i = 0; i < node.Inputs.size(); ++i)
            {
                const UIVec2 p = GraphToScreen(node.Position + UIVec2{ 0.0f, kHeaderHeight + (static_cast<float>(i) + 0.5f) * kPinRowHeight + 2.0f });
                drawPin(p, node.Inputs[i].Id, node.Inputs[i].Color);
                const UIVec2 pSize = UILabel::MeasureText(node.Inputs[i].Name, nullptr, fontSize);
                drawList.AddText(node.Inputs[i].Name, nullptr, { p.x + pr + 6.0f * m_Zoom, p.y - pSize.y * 0.5f }, fontSize, style.TextColor);
            }
            for (size_t i = 0; i < node.Outputs.size(); ++i)
            {
                const UIVec2 p = GraphToScreen(node.Position + UIVec2{ kNodeWidth, kHeaderHeight + (static_cast<float>(i) + 0.5f) * kPinRowHeight + 2.0f });
                drawPin(p, node.Outputs[i].Id, node.Outputs[i].Color);
                const UIVec2 pSize = UILabel::MeasureText(node.Outputs[i].Name, nullptr, fontSize);
                drawList.AddText(node.Outputs[i].Name, nullptr, { p.x - pr - 6.0f * m_Zoom - pSize.x, p.y - pSize.y * 0.5f }, fontSize, style.TextColor);
            }
        }

        drawList.PopClipRect();
    }
}
