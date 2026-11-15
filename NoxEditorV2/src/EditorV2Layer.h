#pragma once

#include <entt/entt.hpp>

#include "NoxCore/Core/Layer.h"
#include "NoxCore/Core/Ref.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/UI/Core/UIContext.h"

namespace Nox
{
    class Scene;

    namespace UI
    {
        class UIGizmo;
        class UILabel;
        class UIPropertyGrid;
        class UITreeView;
        class UIViewportSlot;
    }

    class EditorV2Layer : public Layer
    {
    public:
        EditorV2Layer();
        ~EditorV2Layer() override;

        void OnEvent(Event& event) override;
        void OnUpdate(Timestep ts) override;
        void OnRender() override;

    private:
        void BuildUI();
        void InitBasicScene();
        void RefreshHierarchy();
        void RefreshInspector();

        UI::UIContext m_UI;
        Ref<UI::UILabel> m_StatusLabel;
        Ref<UI::UITreeView> m_HierarchyTree;
        Ref<UI::UIPropertyGrid> m_Inspector;
        Ref<UI::UIViewportSlot> m_ViewportSlot;
        Ref<UI::UIGizmo> m_Gizmo;

        Ref<Scene> m_ActiveScene;
        EditorCamera m_EditorCamera{ 45.0f, 1.778f, 0.1f, 1000.0f };
        entt::entity m_SelectedEntity = entt::null;

        float m_LastDeltaSeconds = 0.016f;
        int m_ClickCount = 0;
        UI::UIVec2 m_LastMousePos{ 0.0f, 0.0f };
    };
}
