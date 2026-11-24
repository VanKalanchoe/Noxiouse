#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "NoxCore/Core/Ref.h"
#include "NoxCore/Core/UUID.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include <NoxUI/Core/UIElement.h>

namespace NoxUI
{
        class UILabel;
        class UIScrollView;
        class UISearchBar;
        class UIStackBox;
        class UITextInput;
    }

namespace Nox
{
    class EditorSelection;
    class EntityActiveSwitch;


    // Native Nox::UI entity properties / component inspector for NoxEditor.
    class InspectorPanel
    {
    public:
        InspectorPanel();
        ~InspectorPanel();

        const Ref<NoxUI::UIElement>& BuildUI();
        const Ref<NoxUI::UIElement>& GetRoot() const { return m_Root; }

        void SetContext(const Ref<Scene>& context);
        // The editor's selection: the inspector shows its primary (and applies transform deltas to the rest).
        void SetSelection(const EditorSelection* selection) { m_SelectionSource = selection; m_SeenSelectionVersion = 0; }
        void Update();
        void MarkDirty() { m_Dirty = true; }

        void SetSceneMutatedCallback(std::function<void()> cb) { m_OnSceneMutated = std::move(cb); }
        void SetTagChangedCallback(std::function<void()> cb) { m_OnTagChanged = std::move(cb); }
        void SetSelectEntityCallback(std::function<void(Entity)> cb) { m_OnSelectEntity = std::move(cb); }
        void SetOpenAssetCallback(std::function<void(AssetHandle)> cb) { m_OnOpenAsset = std::move(cb); }

    private:
        uint64_t ComputeEntitySignature() const;
        void RebuildContent();
        void SyncLiveFields();
        void ShowAddComponentMenu(const NoxUI::UIRect& triggerBounds);
        void NotifyMutated(bool markDirty = false);
        void FollowSelection();

        Ref<Scene> m_Context;
        const EditorSelection* m_SelectionSource = nullptr;
        uint64_t m_SeenSelectionVersion = 0;
        Entity m_Primary;                 // (resolved from the selection at its last change)
        std::vector<Entity> m_Selection;
        UUID m_LastEntityID = 0;
        size_t m_LastSelectionCount = 0;
        uint64_t m_LastSignature = 0;
        WorldUnits::DisplayUnit m_LastDisplayUnit = WorldUnits::DisplayUnit::Auto;
        std::string m_Filter;
        bool m_Dirty = true;
        bool m_MaterialShowAll = false;
        bool m_AllAnimClips = false;
        UUID m_PendingSplit = 0;

        Ref<NoxUI::UIElement> m_Root;
        Ref<NoxUI::UIStackBox> m_HeaderBox;
        Ref<EntityActiveSwitch> m_ActiveSwitch;
        Ref<NoxUI::UITextInput> m_NameInput;
        Ref<NoxUI::UILabel> m_SubheaderLabel;
        Ref<NoxUI::UILabel> m_SelectionBadge;
        Ref<NoxUI::UISearchBar> m_FilterBar;
        Ref<NoxUI::UIScrollView> m_ScrollView;
        Ref<NoxUI::UIStackBox> m_EmptyState;

        std::unordered_map<std::string, bool> m_ExpandedCards;

        std::function<void()> m_OnSceneMutated;
        std::function<void()> m_OnTagChanged;
        std::function<void(Entity)> m_OnSelectEntity;
        std::function<void(AssetHandle)> m_OnOpenAsset;
    };
}
