#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    struct UINodePin
    {
        uint64_t Id = 0;
        std::string Name;
        bool IsOutput = false;
        UIColor Color{ 0.35f, 0.72f, 0.98f, 1.0f };
    };

    struct UINode
    {
        uint64_t Id = 0;
        std::string Title;
        UIColor HeaderColor{ 0.22f, 0.46f, 0.80f, 1.0f };
        UIVec2 Position{ 0.0f, 0.0f }; // graph-space top-left
        std::vector<UINodePin> Inputs;
        std::vector<UINodePin> Outputs;
    };

    struct UINodeLink
    {
        uint64_t Id = 0;
        uint64_t OutputPinId = 0;
        uint64_t InputPinId = 0;
    };

    // Pan/zoom retained node-graph canvas using SDF rounded rects and cubic Beziers.
    class UINodeGraph : public UIElement
    {
    public:
        UINodeGraph();

        uint64_t AddNode(UINode node);
        uint64_t AddLink(uint64_t outputPinId, uint64_t inputPinId);
        void Clear();

        std::vector<UINode>& GetNodes() { return m_Nodes; }
        std::vector<UINodeLink>& GetLinks() { return m_Links; }

        std::function<void(const UINodeLink&)> OnLinkCreated;
        std::function<void(uint64_t linkId)> OnLinkDeleted;
        std::function<void(uint64_t nodeId)> OnNodeSelected;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;
        bool OnMouseWheel(const UIPointerEvent& e) override;
        bool OnKeyDown(const UIKeyEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        UIVec2 GraphToScreen(const UIVec2& p) const;
        UIVec2 ScreenToGraph(const UIVec2& p) const;
        UIVec2 GetNodeSize(const UINode& node) const;
        bool TryGetPinPos(uint64_t pinId, UIVec2& outScreenPos, UIColor& outColor, bool& outIsOutput) const;
        uint64_t HitTestPin(const UIVec2& screenPos, bool* outIsOutput = nullptr) const;
        int HitTestNode(const UIVec2& screenPos) const;
        uint64_t HitTestLink(const UIVec2& screenPos) const;
        bool TryGetConnectedColor(uint64_t pinId, UIColor& outColor) const;

        std::vector<UINode> m_Nodes;
        std::vector<UINodeLink> m_Links;
        uint64_t m_NextId = 1;

        UIVec2 m_Pan{ 40.0f, 40.0f };
        float m_Zoom = 1.0f;

        uint64_t m_SelectedNodeId = 0;
        uint64_t m_SelectedLinkId = 0;
        bool m_Panning = false;
        bool m_DraggingNode = false;
        uint64_t m_DraggingPinId = 0;
        bool m_DraggingPinIsOutput = false;
        UIVec2 m_DragMouseScreen{ 0.0f, 0.0f };
    };
}
