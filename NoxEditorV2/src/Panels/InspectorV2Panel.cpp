#include "InspectorV2Panel.h"
#include "InspectorWidgetsV2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <type_traits>

#include <glm/gtc/constants.hpp>

#include "NoxCore/Animation/AnimationSequence.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/Renderer/Mesh.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/EntityBounds.h"
#include "NoxCore/Scene/MaterialVariants.h"
#include "NoxCore/Scene/ModelInstance.h"
#include "NoxCore/Scene/Prefab.h"
#include "NoxCore/Scene/PrefabInstance.h"
#include "NoxCore/Scripting/ScriptEngine.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIMenu.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UISearchBar.h"
#include "NoxCore/UI/Widgets/UISeparator.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"
#include "NoxCore/UI/Widgets/UIVectorEdit.h"

namespace Nox
{
    namespace
    {
        using CardBuilder = InspectorCardBuilder;

        bool MatchesFilter(const std::string& filterLower, const std::string& haystack)
        {
            if (filterLower.empty())
                return true;
            return ToLowerStr(haystack).find(filterLower) != std::string::npos;
        }

        uint64_t MixHash(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        }

        uint64_t HashText(std::string_view text)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (unsigned char c : text)
            {
                h ^= c;
                h *= 0x100000001b3ull;
            }
            return h;
        }

        template <typename Collider>
        bool FitColliderToMesh(Entity entity, Collider& collider)
        {
            glm::vec3 lowest, highest;
            if (!ComputeLocalMeshBounds(entity, lowest, highest))
                return false;
            const glm::vec3 half = glm::max((highest - lowest) * 0.5f, glm::vec3(WorldUnits::FromMeters(0.001f)));
            const glm::vec3 center = (highest + lowest) * 0.5f;
            glm::vec3 scale(1.0f);
            if (entity.HasComponent<WorldTransformComponent>())
            {
                const glm::dmat4& world = entity.GetComponent<WorldTransformComponent>().WorldMatrix;
                scale = glm::vec3(glm::length(glm::dvec3(world[0])), glm::length(glm::dvec3(world[1])), glm::length(glm::dvec3(world[2])));
            }
            const float flat = std::max(half.x, half.z);
            if constexpr (std::is_same_v<Collider, BoxCollider3DComponent>)
                collider.HalfExtents = half;
            else if constexpr (std::is_same_v<Collider, SphereCollider3DComponent>)
                collider.Radius = std::max({ half.x, half.y, half.z });
            else if constexpr (std::is_same_v<Collider, CapsuleCollider3DComponent>)
            {
                collider.Radius = flat;
                collider.BottomRadius = 0.0f;
                collider.HalfHeight = std::max(half.y - flat, WorldUnits::FromMeters(0.001f));
            }
            else if constexpr (std::is_same_v<Collider, CylinderCollider3DComponent>)
            {
                collider.Radius = flat;
                collider.BottomRadius = 0.0f;
                collider.HalfHeight = half.y;
            }
            collider.Offset = center * scale;
            return true;
        }
    }

    InspectorV2Panel::InspectorV2Panel() = default;
    InspectorV2Panel::~InspectorV2Panel() = default;

    const Ref<UI::UIElement>& InspectorV2Panel::BuildUI()
    {
        m_ActiveSwitch = Ref<EntityActiveSwitch>(new EntityActiveSwitch(true, [this](bool active)
        {
            if (!m_Primary || !m_Context)
                return;
            if (m_Context->IsEntityLocked(m_Primary))
            {
                m_ActiveSwitch->Checked = !m_Primary.HasComponent<InactiveComponent>();
                return;
            }
            if (active && m_Primary.HasComponent<InactiveComponent>())
                m_Primary.RemoveComponent<InactiveComponent>();
            else if (!active && !m_Primary.HasComponent<InactiveComponent>())
                m_Primary.AddComponent<InactiveComponent>();
            NotifyMutated(true);
        }));
        m_ActiveSwitch->Tooltip = "Entity Active state (Inactive disables rendering, physics, animation, and scripts).";
        m_ActiveSwitch->VAlign = UI::UIAlignment::Center;

        m_NameInput = UI::MakeUI<UI::UITextInput>("", "Entity Name");
        m_NameInput->WidthMode = UI::UISizeMode::Fill;
        m_NameInput->FlexWeight = 1.0f;
        m_NameInput->VAlign = UI::UIAlignment::Center;
        m_NameInput->OnChanged = [this](const std::string& name)
        {
            if (!m_Primary || !m_Context || m_Context->IsEntityLocked(m_Primary) || !m_Primary.HasComponent<TagComponent>())
                return;
            m_Primary.GetComponent<TagComponent>().Tag = name;
            if (m_OnTagChanged)
                m_OnTagChanged();
            NotifyMutated(false);
        };

        auto addCompBtn = UI::Button("+ Add").Tooltip("Add a component to this entity");
        addCompBtn->StyleVariant = "primary";
        addCompBtn->Padding = UI::UIThickness::Symmetric(10.0f, 5.0f);
        addCompBtn->WidthMode = UI::UISizeMode::Auto;
        addCompBtn->HeightMode = UI::UISizeMode::Auto;
        addCompBtn->VAlign = UI::UIAlignment::Center;
        UI::UIButton* addCompPtr = addCompBtn.Get().get();
        addCompBtn->OnClick = [this, addCompPtr]()
        {
            ShowAddComponentMenu(addCompPtr->GetBounds());
        };

        auto nameRow = UI::HStack(8.0f, { m_ActiveSwitch, m_NameInput, addCompBtn }).FillWidth();
        nameRow->HeightMode = UI::UISizeMode::Auto;
        nameRow->VAlign = UI::UIAlignment::Center;

        m_SubheaderLabel = UI::Label("");
        m_SubheaderLabel->FontSizeOverride = 13.5f;
        m_SubheaderLabel->UseColorOverride = true;
        m_SubheaderLabel->ColorOverride = { 0.58f, 0.64f, 0.74f, 1.0f };

        m_SelectionBadge = UI::Label("");
        m_SelectionBadge->FontSizeOverride = 13.5f;
        m_SelectionBadge->UseColorOverride = true;
        m_SelectionBadge->ColorOverride = { 0.45f, 0.72f, 0.98f, 1.0f };
        m_SelectionBadge->Visible = false;

        m_FilterBar = UI::MakeUI<UI::UISearchBar>("Filter components & properties...");
        m_FilterBar->WidthMode = UI::UISizeMode::Fill;
        m_FilterBar->OnSearchChanged = [this](const std::string& q)
        {
            m_Filter = q;
            m_Dirty = true;
        };

        m_HeaderBox = UI::VStack(5.0f, { nameRow, m_SubheaderLabel, m_SelectionBadge, m_FilterBar }).FillWidth();
        m_HeaderBox->HeightMode = UI::UISizeMode::Auto;
        m_HeaderBox->Visible = false;

        m_ScrollView = UI::MakeUI<UI::UIScrollView>();
        m_ScrollView->WidthMode = UI::UISizeMode::Fill;
        m_ScrollView->HeightMode = UI::UISizeMode::Fill;
        m_ScrollView->ChildSpacing = 8.0f;

        auto emptyTitle = UI::Label("No Entity Selected");
        emptyTitle->FontSizeOverride = 17.0f;
        emptyTitle->HAlign = UI::UIAlignment::Center;
        emptyTitle->UseColorOverride = true;
        emptyTitle->ColorOverride = { 0.78f, 0.82f, 0.90f, 1.0f };

        auto emptyHint = UI::Label("Select an entity in Scene Hierarchy or the Viewport.");
        emptyHint->FontSizeOverride = 14.0f;
        emptyHint->HAlign = UI::UIAlignment::Center;
        emptyHint->UseColorOverride = true;
        emptyHint->ColorOverride = { 0.54f, 0.59f, 0.68f, 1.0f };

        m_EmptyState = UI::VStack(4.0f, { emptyTitle, emptyHint }).Fill();
        m_EmptyState->VAlign = UI::UIAlignment::Center;
        m_EmptyState->HAlign = UI::UIAlignment::Center;

        m_Root = UI::VStack(8.0f, { m_HeaderBox, m_ScrollView, m_EmptyState })
            .Padding(8.0f)
            .Fill();

        m_Dirty = true;
        RebuildContent();
        return m_Root;
    }

    void InspectorV2Panel::SetContext(const Ref<Scene>& context)
    {
        if (m_Context == context)
            return;
        m_Context = context;
        m_Primary = {};
        m_Selection.clear();
        m_LastEntityID = 0;
        m_LastSignature = 0;
        m_Dirty = true;
    }

    void InspectorV2Panel::SetSelection(Entity primary, const std::vector<Entity>& selection)
    {
        const UUID nextID = primary ? primary.GetUUID() : UUID(0);
        if (nextID != m_LastEntityID || selection.size() != m_LastSelectionCount)
        {
            m_Primary = primary;
            m_Selection = selection;
            m_Dirty = true;
        }
        else
        {
            m_Primary = primary;
            m_Selection = selection;
        }
    }

    void InspectorV2Panel::NotifyMutated(bool markDirty)
    {
        if (markDirty)
            m_Dirty = true;
        if (m_OnSceneMutated)
            m_OnSceneMutated();
    }

    uint64_t InspectorV2Panel::ComputeEntitySignature() const
    {
        if (!m_Primary || !m_Context)
            return 0;

        Entity primary = m_Primary;
        uint64_t h = 0x517cc1b727220a95ull;
        h = MixHash(h, primary.GetUUID());
        h = MixHash(h, m_Selection.size());
        h = MixHash(h, static_cast<uint64_t>(WorldUnits::GetDisplayUnit()));
        h = MixHash(h, m_Context->IsEntityLocked(primary) ? 1ull : 0ull);
        h = MixHash(h, m_Context->IsRunning() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<InactiveComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<TransformComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<ModelInstanceComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<PrefabInstanceComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<PrefabNodeComponent>() ? 1ull : 0ull);
        if (primary.HasComponent<MeshComponent>())
        {
            const auto& mc = primary.GetComponent<MeshComponent>();
            h = MixHash(h, mc.Mesh);
            h = MixHash(h, mc.SubmeshIndex);
            h = MixHash(h, mc.SubmeshCount);
        }
        if (primary.HasComponent<MaterialComponent>())
        {
            const auto& mat = primary.GetComponent<MaterialComponent>();
            h = MixHash(h, 100ull + mat.MaterialAssets.size());
            h = MixHash(h, mat.Parameters.size() * 31ull + mat.GraphParameters.size() * 7ull);
        }
        h = MixHash(h, primary.HasComponent<DirectionalLightComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<PointLightComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<SpotLightComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<RectLightComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<DiscLightComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<ReflectionProbeComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<IrradianceVolumeComponent>() ? 1ull : 0ull);
        if (primary.HasComponent<EnvironmentLightComponent>())
            h = MixHash(h, 200ull + static_cast<uint64_t>(primary.GetComponent<EnvironmentLightComponent>().Source)
                           + (primary.GetComponent<EnvironmentLightComponent>().HeightFog ? 10ull : 0ull));
        if (primary.HasComponent<AnimatorComponent>())
        {
            const auto& anim = primary.GetComponent<AnimatorComponent>();
            h = MixHash(h, 150ull + static_cast<uint64_t>(anim.Animation));
            h = MixHash(h, static_cast<uint64_t>(anim.Graph));
            h = MixHash(h, anim.RootMotion ? 1ull : 0ull);
            h = MixHash(h, anim.GraphInstance.Parameters.size());
        }
        h = MixHash(h, primary.HasComponent<BoneSocketComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<TimelinePlayerComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<AudioSourceComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<ImpactSoundComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<AudioListenerComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<AudioVolumeComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<PostProcessVolumeComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<AcousticProbeVolumeComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<ParticleEmitterComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<SpriteRendererComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<CircleRendererComponent>() ? 1ull : 0ull);
        if (primary.HasComponent<CameraComponent>())
            h = MixHash(h, 300ull + static_cast<uint64_t>(primary.GetComponent<CameraComponent>().Camera.GetProjectionType())
                           + (primary.GetComponent<CameraComponent>().AutoExposure ? 10ull : 0ull));
        if (primary.HasComponent<ScriptComponent>())
        {
            const auto& sc = primary.GetComponent<ScriptComponent>();
            h = MixHash(h, 400ull + sc.ClassNames.size());
            for (const std::string& cn : sc.ClassNames)
                h = MixHash(h, HashText(cn));
        }
        h = MixHash(h, primary.HasComponent<RigidBody2DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<BoxCollider2DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<CircleCollider2DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<RigidBody3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<BoxCollider3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<SphereCollider3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<CapsuleCollider3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<CylinderCollider3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<PlaneCollider3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<MeshCollider3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<CharacterController3DComponent>() ? 1ull : 0ull);
        h = MixHash(h, primary.HasComponent<TextComponent>() ? 1ull : 0ull);
        if (primary.HasComponent<MetadataComponent>())
            h = MixHash(h, 500ull + primary.GetComponent<MetadataComponent>().Values.size());
        return h;
    }

    void InspectorV2Panel::Update()
    {
        if (m_PendingSplit != 0 && m_Context)
        {
            if (Entity root = m_Context->GetEntityByUUID(m_PendingSplit))
            {
                std::string error;
                if (m_Context->SplitIntoSeparateEntities(root, error) == 0 && !error.empty())
                    NOX_CORE_WARN("Split Into Separate Entities: {}", error);
                m_Primary = {};
                m_Selection.clear();
                if (m_OnSelectEntity)
                    m_OnSelectEntity({});
                if (m_OnTagChanged)
                    m_OnTagChanged();
                m_Dirty = true;
                NotifyMutated(false);
            }
            m_PendingSplit = 0;
        }

        if (!m_ScrollView)
            return;

        if (m_Primary && !m_Primary)
            m_Primary = {};

        const uint64_t sig = ComputeEntitySignature();
        const WorldUnits::DisplayUnit unit = WorldUnits::GetDisplayUnit();
        if (m_Dirty || sig != m_LastSignature || unit != m_LastDisplayUnit)
        {
            m_LastSignature = sig;
            m_LastDisplayUnit = unit;
            m_LastEntityID = m_Primary ? m_Primary.GetUUID() : UUID(0);
            m_LastSelectionCount = m_Selection.size();
            m_Dirty = false;
            RebuildContent();
        }
        else
        {
            SyncLiveFields();
        }
    }

    void InspectorV2Panel::SyncLiveFields()
    {
        if (!m_Primary)
            return;

        if (m_ActiveSwitch && !UI::HasState(m_ActiveSwitch->GetState(), UI::UIState::Pressed))
            m_ActiveSwitch->Checked = !m_Primary.HasComponent<InactiveComponent>();

        if (m_NameInput && !UI::HasState(m_NameInput->GetState(), UI::UIState::Focused) && m_Primary.HasComponent<TagComponent>())
        {
            const std::string& tag = m_Primary.GetComponent<TagComponent>().Tag;
            if (m_NameInput->Text != tag)
                m_NameInput->Text = tag;
        }

        for (const auto& fn : m_SyncCallbacks)
            fn();
    }

    void InspectorV2Panel::RebuildContent()
    {
        if (!m_ScrollView || !m_HeaderBox || !m_EmptyState)
            return;

        m_SyncCallbacks.clear();
        m_ScrollView->ClearChildren();

        if (!m_Primary || !m_Context)
        {
            m_HeaderBox->Visible = false;
            m_ScrollView->Visible = false;
            m_EmptyState->Visible = true;
            return;
        }

        m_HeaderBox->Visible = true;
        m_ScrollView->Visible = true;
        m_EmptyState->Visible = false;

        if (m_ActiveSwitch)
            m_ActiveSwitch->Checked = !m_Primary.HasComponent<InactiveComponent>();
        if (m_NameInput && m_Primary.HasComponent<TagComponent>())
            m_NameInput->Text = m_Primary.GetComponent<TagComponent>().Tag;

        if (m_SubheaderLabel)
        {
            const bool inactive = m_Primary.HasComponent<InactiveComponent>();
            const WorldBounds bounds = ComputeWorldBounds(*m_Context, m_Primary);
            std::string subText;
            if (bounds.Valid)
            {
                const glm::vec3 size = bounds.Size();
                subText = "Size: " + WorldUnits::Format(size.x) + " x " + WorldUnits::Format(size.y) + " x " + WorldUnits::Format(size.z);
            }
            else
            {
                char idBuf[48];
                std::snprintf(idBuf, sizeof(idBuf), "UUID: %llu", static_cast<unsigned long long>(m_Primary.GetUUID()));
                subText = idBuf;
            }
            if (inactive)
                subText += "   •   Inactive";
            m_SubheaderLabel->Text = subText;
            m_SubheaderLabel->ColorOverride = inactive
                ? UI::UIColor{ 0.86f, 0.56f, 0.42f, 1.0f }
                : UI::UIColor{ 0.58f, 0.64f, 0.74f, 1.0f };
            m_SubheaderLabel->Visible = true;
        }

        if (m_SelectionBadge)
        {
            if (m_Selection.size() > 1)
            {
                m_SelectionBadge->Text = std::to_string(m_Selection.size()) + " entities selected (Transform edits apply deltas to all)";
                m_SelectionBadge->Visible = true;
            }
            else
            {
                m_SelectionBadge->Visible = false;
            }
        }

        UI::UIContext* ctx = UI::UIContext::GetCurrent();
        const std::string filterLower = ToLowerStr(m_Filter);
        Entity entity = m_Primary;

        auto makeCard = [&](const std::string& title, bool removable,
                            std::function<void()> onReset,
                            std::function<void()> onRemove,
                            const std::function<void(CardBuilder&)>& populate)
        {
            if (!MatchesFilter(filterLower, title))
                return;

            if (!m_ExpandedCards.contains(title))
                m_ExpandedCards[title] = true;
            bool expanded = m_ExpandedCards[title];

            auto res = CreateCollapsibleCard(
                title,
                expanded,
                [this, title](bool next) { m_ExpandedCards[title] = next; },
                removable,
                onReset ? [this, onReset]() { onReset(); NotifyMutated(true); } : std::function<void()>{},
                (removable && onRemove) ? [this, onRemove]() { onRemove(); NotifyMutated(true); } : std::function<void()>{});

            CardBuilder builder{
                res.Body,
                m_SyncCallbacks,
                [this](bool rebuild) { NotifyMutated(rebuild); },
                ctx
            };
            populate(builder);

            m_ScrollView->AddChild(res.Card);
        };

        // Prefab Instance banner card
        if (entity.HasComponent<PrefabInstanceComponent>())
        {
            makeCard("Prefab Instance", false, {}, {}, [this, entity](CardBuilder& c) mutable
            {
                const AssetHandle prefabHandle = entity.GetComponent<PrefabInstanceComponent>().Prefab;
                c.AddReadonly("Asset", AssetDisplayName(prefabHandle));
                auto actions = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
                actions->WidthMode = UI::UISizeMode::Fill;
                if (m_OnOpenAsset && AssetManager::IsAssetHandleValid(prefabHandle))
                {
                    auto openBtn = UI::Button("Open", [this, prefabHandle]() { m_OnOpenAsset(prefabHandle); });
                    openBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                    actions->AddChild(openBtn);
                }
                auto unpackBtn = UI::Button("Unpack", [this, entity]()
                {
                    PrefabInstance::Unpack(*m_Context, entity);
                    NotifyMutated(true);
                }).Tooltip("Turn this instance into ordinary entities (nested instances stay instances).");
                unpackBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                actions->AddChild(unpackBtn);

                const auto overrides = PrefabInstance::ListOverrides(*m_Context, entity);
                if (!overrides.empty())
                {
                    auto revertBtn = UI::Button("Revert All (" + std::to_string(overrides.size()) + ")", [this, entity]()
                    {
                        PrefabInstance::RevertOverride(*m_Context, entity, nullptr);
                        NotifyMutated(true);
                    });
                    revertBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                    actions->AddChild(revertBtn);
                }
                c.AddRow("Actions", actions);
            });
        }

        if (entity.HasComponent<PrefabNodeComponent>())
        {
            Entity instance = m_Context->GetEntityByUUID(entity.GetComponent<PrefabNodeComponent>().Instance);
            makeCard("Prefab Node", false, {}, {}, [this, instance](CardBuilder& c) mutable
            {
                c.AddReadonly("Instance", instance ? instance.GetName() : "(missing)");
                if (instance && m_OnSelectEntity)
                {
                    auto selBtn = UI::Button("Select Instance Root", [this, instance]() { m_OnSelectEntity(instance); });
                    selBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                    c.AddRow("Navigate", selBtn);
                }
            });
        }

        // Transform Component
        if (entity.HasComponent<TransformComponent>())
        {
            makeCard("Transform", false,
                     [entity]() mutable
                     {
                         auto& tc = entity.GetComponent<TransformComponent>();
                         tc.Translation = glm::dvec3(0.0);
                         tc.Rotation = glm::vec3(0.0f);
                         tc.Scale = glm::vec3(1.0f);
                         entity.MarkTransformDirty();
                     },
                     {},
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& tc = entity.GetComponent<TransformComponent>();

                         const double largest = std::max({ std::abs(tc.Translation.x), std::abs(tc.Translation.y), std::abs(tc.Translation.z) });
                         const LengthUnitInfo unit = GetLengthUnitInfo(static_cast<float>(largest));
                         const float posSpeed = unit.Meters ? 0.05f : 5.0f;

                         auto posEdit = UI::MakeUI<UI::UIVectorEdit>(
                             3,
                             glm::vec4(glm::vec3(tc.Translation / static_cast<double>(unit.Factor)), 0.0f),
                             posSpeed,
                             [this, entity, factor = unit.Factor](const glm::vec4& v) mutable
                             {
                                 auto& comp = entity.GetComponent<TransformComponent>();
                                 const glm::dvec3 prev = comp.Translation;
                                 comp.Translation = glm::dvec3(v.x, v.y, v.z) * static_cast<double>(factor);
                                 const glm::dvec3 delta = comp.Translation - prev;
                                 entity.MarkTransformDirty();
                                 for (Entity other : m_Selection)
                                 {
                                     if (!other || other == entity || !other.HasComponent<TransformComponent>())
                                         continue;
                                     if (other.HasComponent<RelationshipComponent>())
                                     {
                                         const UUID pID = other.GetComponent<RelationshipComponent>().Parent;
                                         if (pID != 0 && std::find(m_Selection.begin(), m_Selection.end(), m_Context->GetEntityByUUID(pID)) != m_Selection.end())
                                             continue;
                                     }
                                     other.GetComponent<TransformComponent>().Translation += delta;
                                     other.MarkTransformDirty();
                                 }
                                 NotifyMutated(false);
                             });
                         c.AddRow(std::string("Position (") + unit.Suffix + ")", posEdit);
                         c.Syncs.push_back([posEdit, entity, factor = unit.Factor]() mutable
                         {
                             if (!entity || posEdit->IsEditing())
                                 return;
                             const glm::vec3 d(entity.GetComponent<TransformComponent>().Translation / static_cast<double>(factor));
                             posEdit->SetValue(glm::vec4(d, 0.0f), false);
                         });

                         const glm::vec3 rotDeg = glm::degrees(tc.Rotation);
                         auto rotEdit = UI::MakeUI<UI::UIVectorEdit>(
                             3,
                             glm::vec4(rotDeg, 0.0f),
                             0.5f,
                             [this, entity](const glm::vec4& v) mutable
                             {
                                 auto& comp = entity.GetComponent<TransformComponent>();
                                 const glm::vec3 prev = comp.Rotation;
                                 comp.Rotation = glm::radians(glm::vec3(v.x, v.y, v.z));
                                 const glm::vec3 delta = comp.Rotation - prev;
                                 entity.MarkTransformDirty();
                                 for (Entity other : m_Selection)
                                 {
                                     if (!other || other == entity || !other.HasComponent<TransformComponent>())
                                         continue;
                                     other.GetComponent<TransformComponent>().Rotation += delta;
                                     other.MarkTransformDirty();
                                 }
                                 NotifyMutated(false);
                             });
                         c.AddRow("Rotation (deg)", rotEdit);
                         c.Syncs.push_back([rotEdit, entity]() mutable
                         {
                             if (!entity || rotEdit->IsEditing())
                                 return;
                             const glm::vec3 deg = glm::degrees(entity.GetComponent<TransformComponent>().Rotation);
                             rotEdit->SetValue(glm::vec4(deg, 0.0f), false);
                         });

                         auto scaleEdit = UI::MakeUI<UI::UIVectorEdit>(
                             3,
                             glm::vec4(tc.Scale, 0.0f),
                             0.02f,
                             [this, entity](const glm::vec4& v) mutable
                             {
                                 auto& comp = entity.GetComponent<TransformComponent>();
                                 const glm::vec3 prev = comp.Scale;
                                 comp.Scale = glm::vec3(v.x, v.y, v.z);
                                 const glm::vec3 delta = comp.Scale - prev;
                                 entity.MarkTransformDirty();
                                 for (Entity other : m_Selection)
                                 {
                                     if (!other || other == entity || !other.HasComponent<TransformComponent>())
                                         continue;
                                     other.GetComponent<TransformComponent>().Scale += delta;
                                     other.MarkTransformDirty();
                                 }
                                 NotifyMutated(false);
                             });
                         c.AddRow("Scale", scaleEdit);
                         c.Syncs.push_back([scaleEdit, entity]() mutable
                         {
                             if (!entity || scaleEdit->IsEditing())
                                 return;
                             scaleEdit->SetValue(glm::vec4(entity.GetComponent<TransformComponent>().Scale, 0.0f), false);
                         });

                         const WorldBounds bounds = ComputeWorldBounds(*m_Context, entity);
                         if (bounds.Valid)
                         {
                             const glm::vec3 size = bounds.Size();
                             c.AddReadonly("World Size",
                                 WorldUnits::Format(size.x) + " x " + WorldUnits::Format(size.y) + " x " + WorldUnits::Format(size.z));
                         }
                     });
        }

        // Model Instance
        if (entity.HasComponent<ModelInstanceComponent>())
        {
            makeCard("Model Instance", true, {},
                     [entity]() mutable { entity.RemoveComponent<ModelInstanceComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<ModelInstanceComponent>();
                         c.AddReadonly("Model", AssetDisplayName(comp.Model));
                         c.AddReadonly("Status", comp.Spawned ? "Spawned" : "Loading...");
                         if (!comp.RemovedNodes.empty())
                         {
                             auto restoreBtn = UI::Button("Restore Removed Nodes (" + std::to_string(comp.RemovedNodes.size()) + ")",
                                 [this, entity]() { ModelInstance::Respawn(*m_Context, entity); NotifyMutated(true); });
                             restoreBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                             c.AddRow("Nodes", restoreBtn);
                         }
                         auto splitBtn = UI::Button("Split Into Separate Entities", [this, entity]()
                         {
                             m_PendingSplit = Entity(entity).GetUUID();
                         });
                         splitBtn->SetEnabled(comp.Spawned && !m_Context->IsRunning());
                         splitBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("Convert", splitBtn);
                     });
        }

        // Mesh Component
        if (entity.HasComponent<MeshComponent>())
        {
            makeCard("Mesh", true,
                     [entity]() mutable
                     {
                         auto& comp = entity.GetComponent<MeshComponent>();
                         comp.SubmeshIndex = 0;
                         comp.SubmeshCount = 1;
                         comp.MorphWeights.clear();
                         entity.PatchComponent<MeshComponent>();
                     },
                     [entity]() mutable { entity.RemoveComponent<MeshComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<MeshComponent>();
                         c.AddAssetSlot("Mesh Asset", comp.Mesh,
                             { AssetType::MeshSource, AssetType::StaticMesh, AssetType::Mesh },
                             [entity](AssetHandle h) mutable
                             {
                                 auto& mc = entity.GetComponent<MeshComponent>();
                                 mc.Mesh = h;
                                 mc.SubmeshIndex = 0;
                                 entity.PatchComponent<MeshComponent>();
                             });
                         if (comp.Mesh != 0)
                         {
                             auto patch = [entity]() mutable { entity.PatchComponent<MeshComponent>(); };
                             c.AddUInt32("Submesh Index", &comp.SubmeshIndex, 0, 0, patch);
                             c.AddUInt32("Submesh Count", &comp.SubmeshCount, 0, 0, patch);

                             const auto targets = GetMeshMorphTargets(comp.Mesh);
                             for (const auto& [name, defaultWeight] : targets)
                             {
                                 const auto found = comp.MorphWeights.find(name);
                                 float weight = found != comp.MorphWeights.end() ? found->second : defaultWeight;
                                 auto slider = CreateSliderField(weight, 0.0f, 1.0f, [entity, name](float v) mutable
                                 {
                                     entity.GetComponent<MeshComponent>().MorphWeights[name] = v;
                                 });
                                 c.AddRow("Morph: " + name, slider);
                             }
                             if (!targets.empty() && !comp.MorphWeights.empty())
                             {
                                 auto resetMorphBtn = UI::Button("Reset Morphs to Defaults", [this, entity]() mutable
                                 {
                                     entity.GetComponent<MeshComponent>().MorphWeights.clear();
                                     NotifyMutated(true);
                                 });
                                 resetMorphBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                                 c.AddRow("Morphs", resetMorphBtn);
                             }
                         }
                     });
        }

        // Material Component
        if (entity.HasComponent<MaterialComponent>())
        {
            makeCard("Material", true,
                     [entity]() mutable
                     {
                         entity.GetComponent<MaterialComponent>().MaterialAssets.clear();
                         entity.GetComponent<MaterialComponent>().Parameters.clear();
                         entity.GetComponent<MaterialComponent>().GraphParameters.clear();
                         entity.PatchComponent<MaterialComponent>();
                     },
                     [entity]() mutable { entity.RemoveComponent<MaterialComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<MaterialComponent>();
                         auto setOverride = [this, entity](size_t slot, AssetHandle material) mutable
                         {
                             auto& mc = entity.GetComponent<MaterialComponent>();
                             if (mc.MaterialAssets.size() <= slot)
                                 mc.MaterialAssets.resize(slot + 1, AssetHandle(0));
                             mc.MaterialAssets[slot] = material;
                             while (!mc.MaterialAssets.empty() && mc.MaterialAssets.back() == 0)
                                 mc.MaterialAssets.pop_back();
                             entity.PatchComponent<MaterialComponent>();
                             NotifyMutated(true);
                         };

                         const std::vector<std::string> variants = MaterialVariants::List(*m_Context, entity);
                         if (!variants.empty())
                         {
                             const auto current = MaterialVariants::Current(*m_Context, entity);
                             std::vector<std::string> varItems = { "Default" };
                             int selected = current && current->empty() ? 0 : -1;
                             for (size_t i = 0; i < variants.size(); ++i)
                             {
                                 varItems.push_back(variants[i]);
                                 if (current && *current == variants[i])
                                     selected = static_cast<int>(i) + 1;
                             }
                             c.AddEnum("Material Variant", varItems, std::max(selected, 0), [this, entity, variants](int idx)
                             {
                                 MaterialVariants::Select(*m_Context, entity, idx == 0 ? "" : variants[static_cast<size_t>(idx - 1)]);
                             });
                         }

                         const std::vector<AssetHandle>* meshMaterials = nullptr;
                         size_t firstSlot = 0;
                         size_t slotEnd = comp.MaterialAssets.size();
                         if (entity.HasComponent<MeshComponent>())
                         {
                             const auto& mc = entity.GetComponent<MeshComponent>();
                             const AssetType meshType = AssetManager::GetAssetType(mc.Mesh);
                             if (meshType == AssetType::StaticMesh)
                             {
                                 if (const StaticMesh* sm = AssetManager::FindLoadedAsset<StaticMesh>(mc.Mesh))
                                     meshMaterials = &sm->GetMaterialAssets();
                             }
                             else if (meshType == AssetType::Mesh || meshType == AssetType::MeshSource)
                             {
                                 if (const Mesh* m = AssetManager::FindLoadedAsset<Mesh>(mc.Mesh))
                                     meshMaterials = &m->GetMaterialAssets();
                             }

                             if (meshMaterials)
                             {
                                 slotEnd = meshMaterials->size();
                                 if (!m_MaterialShowAll)
                                 {
                                     firstSlot = std::min<size_t>(mc.SubmeshIndex, slotEnd);
                                     if (mc.SubmeshCount != UINT32_MAX)
                                         slotEnd = std::min<size_t>(firstSlot + std::max(mc.SubmeshCount, 1u), slotEnd);
                                 }
                             }
                         }

                         if (meshMaterials)
                         {
                             auto rangeRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
                             rangeRow->WidthMode = UI::UISizeMode::Fill;
                             rangeRow->VAlign = UI::UIAlignment::Center;
                             auto rangeLbl = UI::Label(slotEnd > firstSlot
                                 ? ("Slots " + std::to_string(firstSlot) + "-" + std::to_string(slotEnd - 1) + " of " + std::to_string(meshMaterials->size()))
                                 : ("No mesh slots (" + std::to_string(meshMaterials->size()) + " total)"));
                             rangeLbl->FontSizeOverride = 14.0f;
                             rangeLbl->UseColorOverride = true;
                             rangeLbl->ColorOverride = { 0.62f, 0.68f, 0.78f, 1.0f };
                             rangeLbl->WidthMode = UI::UISizeMode::Fill;
                             rangeLbl->FlexWeight = 1.0f;
                             rangeRow->AddChild(rangeLbl);

                             auto showAllBtn = UI::Button(m_MaterialShowAll ? "Entity Slots" : "Show All", [this]()
                             {
                                 m_MaterialShowAll = !m_MaterialShowAll;
                                 m_Dirty = true;
                             });
                             showAllBtn->Padding = UI::UIThickness::Symmetric(8.0f, 3.0f);
                             rangeRow->AddChild(showAllBtn);
                             c.Body->AddChild(rangeRow);
                         }
                         else if (slotEnd == 0)
                         {
                             slotEnd = 1;
                         }

                         slotEnd = std::min<size_t>(slotEnd, firstSlot + 64);

                         for (size_t slot = firstSlot; slot < slotEnd; ++slot)
                         {
                             const AssetHandle overrideHandle = slot < comp.MaterialAssets.size() ? comp.MaterialAssets[slot] : AssetHandle(0);
                             AssetHandle handle = overrideHandle;
                             if (handle == 0 && meshMaterials && slot < meshMaterials->size())
                                 handle = (*meshMaterials)[slot];
                             const bool overridden = (meshMaterials != nullptr && overrideHandle != 0);
                             const bool validMat = (handle != 0 && AssetManager::IsAssetHandleValid(handle) &&
                                                    AssetManager::GetAssetType(handle) == AssetType::Material);

                             std::string matLabel = validMat ? AssetDisplayName(handle) : (meshMaterials ? "Default Mesh Material" : "None");
                             if (overridden)
                                 matLabel += " *";

                             // One row per slot:  [>] [ the material's name ] [Edit] [...]
                             //   [>]    its properties right here, under the row (Godot's and Unity's way: the resource opens in
                             //          place) -- for a quick change without leaving the entity;
                             //   name   picks another material;
                             //   Edit   the Material Editor panel (Unreal's way: the asset in its own editor) -- for real work on it;
                             //   [...]  Make Unique, Reset to the mesh's material.
                             Ref<Material> slotMaterial = validMat ? AssetManager::GetAsset<Material>(handle) : nullptr;
                             const std::string expandKey = "MatInline_" + std::to_string(slot) + "_" + std::to_string(static_cast<uint64_t>(handle));
                             const bool inlineOpen = slotMaterial && m_ExpandedCards.contains(expandKey) && m_ExpandedCards[expandKey];

                             auto slotRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                             slotRow->WidthMode = UI::UISizeMode::Fill;
                             slotRow->VAlign = UI::UIAlignment::Center;

                             // The material's properties under the row, set in a little so they read as belonging to it. Built the
                             // first time they are shown; opening and closing then only shows and hides them (a rebuild of the
                             // whole Inspector for it lost the focus and the work).
                             Ref<UI::UIStackBox> inlineBody;
                             std::function<void()> fillInline;
                             if (slotMaterial)
                             {
                                 inlineBody = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 6.0f);
                                 inlineBody->WidthMode = UI::UISizeMode::Fill;
                                 inlineBody->StyleClass = "Card";
                                 inlineBody->DrawBackground = true;
                                 inlineBody->Padding = UI::UIThickness::All(8.0f);
                                 inlineBody->Visible = inlineOpen;
                                 fillInline = [this, handle, slotMaterial, body = inlineBody, syncs = &c.Syncs, mutate = c.OnMutate, uiContext = c.Context]()
                                 {
                                     if (!body->GetChildren().empty())
                                         return;
                                     InspectorCardBuilder inlineBuilder{ body, *syncs, mutate, uiContext };
                                     PopulateMaterialPropertiesUI(handle, slotMaterial->GetData(), inlineBuilder, m_ExpandedCards,
                                         { m_OnOpenAsset, [this]() { m_Dirty = true; } });
                                 };
                                 slotRow->AddChild(CreateExpandToggle(inlineOpen,
                                     [this, expandKey, body = inlineBody, fillInline](bool open)
                                     {
                                         m_ExpandedCards[expandKey] = open;
                                         if (open)
                                             fillInline();
                                         body->Visible = open;
                                     },
                                     "Show or hide this material's properties here"));
                             }

                             auto pickBtn = UI::Button(matLabel).Tooltip(overridden
                                 ? "Material overridden on this entity. Click to change."
                                 : "Click to assign or override material asset.");
                             pickBtn->WidthMode = UI::UISizeMode::Fill;
                             pickBtn->FlexWeight = 1.0f;
                             pickBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                             pickBtn->TextElide = UI::UITextElide::Middle; // a long .nmat name keeps its start and its end
                             UI::UIButton* pickPtr = pickBtn.Get().get();
                             const char* emptyText = meshMaterials ? "Use Mesh Material" : "None";
                             pickBtn->OnClick = [pickPtr, handle, overrideHandle, emptyText, slot, setOverride]() mutable
                             {
                                 UI::UIContext* curCtx = UI::UIContext::GetCurrent();
                                 if (!curCtx)
                                     return;
                                 ShowSearchableAssetPicker(
                                     *curCtx,
                                     pickPtr->GetBounds(),
                                     emptyText,
                                     overrideHandle != 0 ? overrideHandle : handle,
                                     CollectAssetPickerOptions({ AssetType::Material }),
                                     [slot, setOverride](AssetHandle picked) mutable { setOverride(slot, picked); },
                                     false);
                             };
                             slotRow->AddChild(pickBtn);

                             if (validMat && m_OnOpenAsset)
                             {
                                 auto editBtn = UI::Button("Edit", [this, handle]() { m_OnOpenAsset(handle); })
                                     .Tooltip("Open in the Material Editor");
                                 editBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                                 slotRow->AddChild(editBtn);
                             }

                             // An independent copy of the material's asset, assigned to this slot.
                             std::function<void()> makeUnique;
                             if (slotMaterial)
                             {
                                 makeUnique = [handle, mat = slotMaterial, slot, setOverride]() mutable
                                 {
                                     if (!Project::GetActive() || !Project::GetActive()->GetEditorAssetManager())
                                         return;
                                     auto mgr = Project::GetActive()->GetEditorAssetManager();
                                     const auto& metadata = mgr->GetMetadata(handle);
                                     std::filesystem::path uniquePath = metadata.FilePath.parent_path() /
                                         (metadata.FilePath.stem().string() + "_Instance_" +
                                          std::to_string(static_cast<uint64_t>(AssetHandle())) + ".nmat");
                                     if (MaterialSerializer::Serialize(Project::GetActiveAssetDirectory() / uniquePath, mat->GetData()))
                                     {
                                         mgr->ImportAsset(uniquePath, uniquePath, AssetType::Material);
                                         for (const auto& [uniqueHandle, uniqueMeta] : mgr->GetAssetRegistry())
                                         {
                                             if (uniqueMeta.Type == AssetType::Material && uniqueMeta.FilePath == uniquePath)
                                             {
                                                 setOverride(slot, uniqueHandle);
                                                 break;
                                             }
                                         }
                                     }
                                 };
                             }
                             const bool canReset = overridden || (!meshMaterials && overrideHandle != 0);
                             const bool hasMeshMaterials = meshMaterials != nullptr;
                             if (makeUnique || canReset)
                             {
                                 slotRow->AddChild(CreateMoreButton(
                                     [makeUnique, canReset, hasMeshMaterials, slot, setOverride](const UI::UIRect& bounds) mutable
                                     {
                                         UI::UIContext* curCtx = UI::UIContext::GetCurrent();
                                         if (!curCtx)
                                             return;
                                         std::vector<UI::UIMenuItem> items;
                                         if (makeUnique)
                                         {
                                             UI::UIMenuItem item;
                                             item.Label = "Make Unique";
                                             item.Tooltip = "Create an independent copy of this material asset and assign it to this slot";
                                             item.OnTrigger = makeUnique;
                                             items.push_back(std::move(item));
                                         }
                                         if (canReset)
                                         {
                                             UI::UIMenuItem item;
                                             item.Label = hasMeshMaterials ? "Reset to Mesh Material" : "Clear Material";
                                             item.OnTrigger = [slot, setOverride]() mutable { setOverride(slot, AssetHandle(0)); };
                                             items.push_back(std::move(item));
                                         }
                                         UI::UIMenuPopup::ShowPopupMenu(*curCtx, bounds, std::move(items), true);
                                     },
                                     "Material options: Make Unique, Reset"));
                             }

                             c.AddRow("Slot " + std::to_string(slot), slotRow);

                             if (inlineBody)
                             {
                                 if (inlineOpen)
                                     fillInline();
                                 c.Body->AddChild(inlineBody);
                             }
                         }

                         // Dynamic Material Instance Parameters (Built-in & Graph parameters per entity/slot)
                         c.AddSubheader("Dynamic Parameters", "Per-entity parameter overrides over slot materials.");
                         for (size_t i = 0; i < comp.Parameters.size(); ++i)
                         {
                             MaterialComponent::Parameter& param = comp.Parameters[i];
                             const std::string pname = MaterialParameters::Name(param.Name);
                             if (MaterialParameters::ComponentCount(param.Name) == 3)
                                 c.AddColor3(pname, reinterpret_cast<glm::vec3*>(&param.Value));
                             else
                                 c.AddFloat(pname, &param.Value.x, 0.01f, 0.0f,
                                     param.Name == MaterialParameter::EmissiveStrength ? 1000.0f : 1.0f);

                             auto metaRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                             metaRow->WidthMode = UI::UISizeMode::Fill;
                             int32_t slotNum = param.Slot == UINT32_MAX ? -1 : static_cast<int32_t>(param.Slot);
                             auto slotDrag = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(slotNum), 0.1f, [this, &comp, i](float v)
                             {
                                 if (i < comp.Parameters.size())
                                 {
                                     int iv = static_cast<int>(std::round(v));
                                     comp.Parameters[i].Slot = iv < 0 ? UINT32_MAX : static_cast<uint32_t>(iv);
                                     NotifyMutated(false);
                                 }
                             });
                             slotDrag->Format = "{:.0f}";
                             slotDrag->Min = -1.0f;
                             slotDrag->Max = 1024.0f;
                             slotDrag->WidthMode = UI::UISizeMode::Fill;
                             slotDrag->FlexWeight = 1.0f;
                             slotDrag->Tooltip = "Target slot (-1 = all slots)";
                             metaRow->AddChild(slotDrag);

                             auto rmBtn = UI::Button("x", [this, entity, i]() mutable
                             {
                                 auto& mc = entity.GetComponent<MaterialComponent>();
                                 if (i < mc.Parameters.size())
                                     mc.Parameters.erase(mc.Parameters.begin() + static_cast<ptrdiff_t>(i));
                                 entity.PatchComponent<MaterialComponent>();
                                 NotifyMutated(true);
                             });
                             rmBtn->WidthMode = UI::UISizeMode::Fixed;
                             rmBtn->HeightMode = UI::UISizeMode::Fixed;
                             rmBtn->FixedSize = { 24.0f, 24.0f };
                             rmBtn->Padding = UI::UIThickness::All(0.0f);
                             metaRow->AddChild(rmBtn);
                             c.AddRow("  Target Slot (-1=All)", metaRow);
                         }

                         for (size_t i = 0; i < comp.GraphParameters.size(); ++i)
                         {
                             MaterialComponent::GraphParameter& gp = comp.GraphParameters[i];
                             if (gp.IsTexture)
                             {
                                 auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
                                 const AssetHandle th = (assets && !gp.Texture.empty()) ? assets->FindHandleByPath(gp.Texture, AssetType::Texture2D) : AssetHandle(0);
                                 c.AddAssetSlot(gp.Name, th, { AssetType::Texture2D }, [assets, &gp](AssetHandle h)
                                 {
                                     gp.Texture = (h != 0 && assets) ? assets->GetMetadata(h).FilePath.generic_string() : std::string();
                                 });
                             }
                             else
                             {
                                 c.AddFloat(gp.Name, &gp.Value.x, 0.01f);
                             }

                             auto rmBtn = UI::Button("Remove " + gp.Name, [this, entity, i]() mutable
                             {
                                 auto& mc = entity.GetComponent<MaterialComponent>();
                                 if (i < mc.GraphParameters.size())
                                     mc.GraphParameters.erase(mc.GraphParameters.begin() + static_cast<ptrdiff_t>(i));
                                 entity.PatchComponent<MaterialComponent>();
                                 NotifyMutated(true);
                             });
                             rmBtn->Padding = UI::UIThickness::Symmetric(8.0f, 3.0f);
                             c.AddRow("", rmBtn);
                         }

                         auto addParamBtn = UI::Button("+ Add Parameter");
                         addParamBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         UI::UIButton* addParamPtr = addParamBtn.Get().get();
                         addParamBtn->OnClick = [this, addParamPtr, entity]() mutable
                         {
                             UI::UIContext* curCtx = UI::UIContext::GetCurrent();
                             if (!curCtx)
                                 return;
                             std::vector<UI::UIMenuItem> items;
                             for (uint8_t idx = 0; idx < static_cast<uint8_t>(MaterialParameter::Count); ++idx)
                             {
                                 const MaterialParameter kind = static_cast<MaterialParameter>(idx);
                                 items.push_back({ MaterialParameters::Name(kind), "", [this, entity, kind]() mutable
                                 {
                                     MaterialComponent::Parameter p;
                                     p.Name = kind;
                                     p.Value = kind == MaterialParameter::BaseColor ? glm::vec4(1.0f)
                                             : kind == MaterialParameter::EmissiveStrength ? glm::vec4(1.0f)
                                             : kind == MaterialParameter::Roughness ? glm::vec4(0.5f) : glm::vec4(0.0f);
                                     entity.GetComponent<MaterialComponent>().Parameters.push_back(p);
                                     entity.PatchComponent<MaterialComponent>();
                                     NotifyMutated(true);
                                 } });
                             }
                             UI::UIMenuPopup::ShowPopupMenu(*curCtx, addParamPtr->GetBounds(), std::move(items), false);
                         };
                         c.AddRow("Parameters", addParamBtn);
                     });
        }

        // Lights & Environment
        if (entity.HasComponent<DirectionalLightComponent>())
        {
            makeCard("Directional Light", true,
                     [entity]() mutable { entity.GetComponent<DirectionalLightComponent>() = DirectionalLightComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<DirectionalLightComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<DirectionalLightComponent>();
                         c.AddColor3("Color", &comp.Color);
                         c.AddFloat("Intensity", &comp.Intensity, 0.1f, 0.0f, 100.0f);
                         c.AddFloat("Angular Diameter", &comp.AngularDiameter, 0.05f, 0.0f, 20.0f, "{:.2f}");
                         c.AddUInt32("Shadow Samples", &comp.ShadowSamples, 1, 16);
                     });
        }

        if (entity.HasComponent<PointLightComponent>())
        {
            makeCard("Point Light", true,
                     [entity]() mutable { entity.GetComponent<PointLightComponent>() = PointLightComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<PointLightComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<PointLightComponent>();
                         c.AddColor3("Color", &comp.Color);
                         c.AddFloat("Intensity", &comp.Intensity, 0.5f, 0.0f, 1000.0f);
                         c.AddLength("Range", &comp.Range);
                         c.AddLength("Source Radius", &comp.Radius);
                         c.AddUInt32("Shadow Samples", &comp.ShadowSamples, 1, 16);
                     });
        }

        if (entity.HasComponent<SpotLightComponent>())
        {
            makeCard("Spot Light", true,
                     [entity]() mutable { entity.GetComponent<SpotLightComponent>() = SpotLightComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<SpotLightComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<SpotLightComponent>();
                         c.AddColor3("Color", &comp.Color);
                         c.AddFloat("Intensity", &comp.Intensity, 0.5f, 0.0f, 1000.0f);
                         c.AddLength("Range", &comp.Range);
                         c.AddFloat("Inner Angle", &comp.InnerAngle, 0.5f, 0.0f, 89.0f, "{:.1f}", [&comp]()
                         {
                             comp.OuterAngle = std::max(comp.OuterAngle, comp.InnerAngle);
                         });
                         c.AddFloat("Outer Angle", &comp.OuterAngle, 0.5f, 0.0f, 89.0f, "{:.1f}", [&comp]()
                         {
                             comp.InnerAngle = std::min(comp.InnerAngle, comp.OuterAngle);
                         });
                         c.AddLength("Source Radius", &comp.Radius);
                         c.AddUInt32("Shadow Samples", &comp.ShadowSamples, 1, 16);
                     });
        }

        if (entity.HasComponent<RectLightComponent>())
        {
            makeCard("Rect Light", true,
                     [entity]() mutable { entity.GetComponent<RectLightComponent>() = RectLightComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<RectLightComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<RectLightComponent>();
                         c.AddColor3("Color", &comp.Color);
                         c.AddFloat("Intensity", &comp.Intensity, 0.25f, 0.0f, 10000.0f);
                         c.AddLength("Width", &comp.Width);
                         c.AddLength("Height", &comp.Height);
                         c.AddLength("Range", &comp.Range);
                     });
        }

        if (entity.HasComponent<DiscLightComponent>())
        {
            makeCard("Disc Light", true,
                     [entity]() mutable { entity.GetComponent<DiscLightComponent>() = DiscLightComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<DiscLightComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<DiscLightComponent>();
                         c.AddColor3("Color", &comp.Color);
                         c.AddFloat("Intensity", &comp.Intensity, 0.25f, 0.0f, 10000.0f);
                         c.AddLength("Radius", &comp.Radius);
                         c.AddLength("Range", &comp.Range);
                     });
        }

        if (entity.HasComponent<ReflectionProbeComponent>())
        {
            makeCard("Reflection Probe", true,
                     [entity]() mutable { entity.GetComponent<ReflectionProbeComponent>() = ReflectionProbeComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<ReflectionProbeComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<ReflectionProbeComponent>();
                         c.AddLengthVec3("Extents", &comp.Extents);
                         c.AddLength("Blend Distance", &comp.BlendDistance);
                         c.AddFloat("Intensity", &comp.Intensity, 0.02f, 0.0f, 16.0f);
                         static constexpr uint32_t kRes[] = { 64, 128, 256, 512 };
                         int resIdx = 1;
                         for (int i = 0; i < 4; ++i)
                             if (kRes[i] == comp.Resolution) resIdx = i;
                         c.AddEnum("Resolution", { "64", "128", "256", "512" }, resIdx, [&comp](int idx)
                         {
                             comp.Resolution = kRes[std::clamp(idx, 0, 3)];
                         });
                         auto recapBtn = UI::Button("Recapture", [&comp]() { ++comp.Revision; });
                         recapBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("Capture", recapBtn);
                     });
        }

        if (entity.HasComponent<IrradianceVolumeComponent>())
        {
            makeCard("Irradiance Volume", true,
                     [entity]() mutable { entity.GetComponent<IrradianceVolumeComponent>() = IrradianceVolumeComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<IrradianceVolumeComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<IrradianceVolumeComponent>();
                         c.AddLengthVec3("Extents", &comp.Extents);
                         c.AddLength("Probe Spacing", &comp.ProbeSpacing);
                         c.AddFloat("Intensity", &comp.Intensity, 0.02f, 0.0f, 16.0f);
                         auto recapBtn = UI::Button("Recapture", [&comp]() { ++comp.Revision; });
                         recapBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("Capture", recapBtn);
                     });
        }

        if (entity.HasComponent<EnvironmentLightComponent>())
        {
            makeCard("Environment Light", true,
                     [entity]() mutable { entity.GetComponent<EnvironmentLightComponent>() = EnvironmentLightComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<EnvironmentLightComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<EnvironmentLightComponent>();
                         c.AddBool("Enabled", &comp.Enabled);
                         c.AddVec3("Radiance Scale", &comp.RadianceScale, 0.02f);
                         c.AddEnum("Source", { "Texture", "Procedural Sky" }, static_cast<int>(comp.Source), [&comp](int idx)
                         {
                             comp.Source = static_cast<EnvironmentSource>(idx);
                         });
                         if (comp.Source == EnvironmentSource::Texture)
                         {
                             c.AddString("Texture Path", &comp.TexturePath, "assets/env.hdr");
                             c.AddFloat("Rotation", &comp.Rotation, 0.02f);
                         }
                         else
                         {
                             c.AddFloat("Rayleigh Scale", &comp.RayleighScale, 0.02f, 0.0f, 10.0f);
                             c.AddFloat("Mie Scale", &comp.MieScale, 0.02f, 0.0f, 10.0f);
                             c.AddFloat("Ozone Scale", &comp.OzoneScale, 0.02f, 0.0f, 10.0f);
                             c.AddSlider("Mie Anisotropy", &comp.MieAnisotropy, 0.0f, 0.99f);
                             c.AddColor3("Ground Albedo", &comp.GroundAlbedo);
                             c.AddBool("Volumetric Clouds", &comp.Clouds);
                             if (comp.Clouds)
                             {
                                 c.AddSlider("Cloud Coverage", &comp.CloudCoverage, 0.0f, 1.0f);
                                 c.AddFloat("Cloud Density", &comp.CloudDensity, 0.02f, 0.0f, 10.0f);
                             }
                             c.AddBool("Moon", &comp.Moon);
                             c.AddBool("Stars", &comp.Stars);
                         }
                         c.AddBool("Height Fog", &comp.HeightFog);
                         if (comp.HeightFog)
                         {
                             c.AddFloat("Fog Density", &comp.FogDensity, 0.02f, 0.0f, 1000.0f);
                             c.AddFloat("Height Falloff", &comp.FogHeightFalloff, 0.05f, 0.0f, 1000.0f);
                             c.AddLength("Fog Height", &comp.FogHeight);
                             c.AddLength("Start Distance", &comp.FogStartDistance);
                             c.AddSlider("Max Opacity", &comp.FogMaxOpacity, 0.0f, 1.0f);
                             c.AddBool("Color From Sky", &comp.FogFromEnvironment);
                             c.AddColor3("Fog Color", &comp.FogColor);
                             c.AddFloat("Sun Glow", &comp.FogSunScattering, 0.002f, 0.0f, 10.0f);
                         }
                     });
        }

        // Camera
        if (entity.HasComponent<CameraComponent>())
        {
            makeCard("Camera", true,
                     [entity]() mutable { entity.GetComponent<CameraComponent>() = CameraComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<CameraComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<CameraComponent>();
                         auto& cam = comp.Camera;
                         c.AddBool("Primary", &comp.Primary);
                         c.AddBool("Auto Exposure", &comp.AutoExposure);
                         if (!comp.AutoExposure)
                             c.AddFloat("Manual EV100", &comp.ManualEV100, 0.1f, -10.0f, 20.0f, "{:.1f}");
                         c.AddFloat("Exposure Comp", &comp.ExposureCompensation, 0.05f, -12.0f, 12.0f);
                         if (comp.AutoExposure)
                         {
                             c.AddFloat("Min EV", &comp.AutoExposureMinEV, 0.1f, -16.0f, 16.0f);
                             c.AddFloat("Max EV", &comp.AutoExposureMaxEV, 0.1f, -16.0f, 16.0f);
                         }
                         c.AddEnum("Projection", { "Perspective", "Orthographic" }, static_cast<int>(cam.GetProjectionType()),
                             [&cam](int idx) { cam.SetProjectionType(static_cast<SceneCamera::ProjectionType>(idx)); });
                         if (cam.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
                         {
                             float fovDeg = glm::degrees(cam.GetPerspectiveVerticalFOV());
                             auto fovDrag = UI::MakeUI<UI::UIDragScalar>(fovDeg, 0.5f, [this, &cam](float v)
                             {
                                 cam.SetPerspectiveVerticalFOV(glm::radians(std::clamp(v, 5.0f, 175.0f)));
                                 NotifyMutated(false);
                             });
                             fovDrag->WidthMode = UI::UISizeMode::Fill;
                             fovDrag->Min = 5.0f;
                             fovDrag->Max = 175.0f;
                             c.AddRow("Vertical FOV", fovDrag);

                             const float nearFactor = GetLengthUnitInfo(cam.GetPerspectiveNearClip()).Factor;
                             const char* nearSuffix = GetLengthUnitInfo(cam.GetPerspectiveNearClip()).Suffix;
                             auto nearDrag = UI::MakeUI<UI::UIDragScalar>(cam.GetPerspectiveNearClip() / nearFactor, 0.01f, [this, &cam, nearFactor](float d)
                             {
                                 cam.SetPerspectiveNearClip(std::max(0.001f, d * nearFactor));
                                 NotifyMutated(false);
                             });
                             nearDrag->WidthMode = UI::UISizeMode::Fill;
                             c.AddRow(std::string("Near Clip (") + nearSuffix + ")", nearDrag);

                             const float farFactor = GetLengthUnitInfo(cam.GetPerspectiveFarClip()).Factor;
                             const char* farSuffix = GetLengthUnitInfo(cam.GetPerspectiveFarClip()).Suffix;
                             auto farDrag = UI::MakeUI<UI::UIDragScalar>(cam.GetPerspectiveFarClip() / farFactor, 1.0f, [this, &cam, farFactor](float d)
                             {
                                 cam.SetPerspectiveFarClip(std::max(1.0f, d * farFactor));
                                 NotifyMutated(false);
                             });
                             farDrag->WidthMode = UI::UISizeMode::Fill;
                             c.AddRow(std::string("Far Clip (") + farSuffix + ")", farDrag);
                         }
                         else
                         {
                             float orthoSize = cam.GetOrthographicSize();
                             auto sizeDrag = UI::MakeUI<UI::UIDragScalar>(orthoSize, 0.1f, [this, &cam](float v)
                             {
                                 cam.SetOrthographicSize(std::max(0.01f, v));
                                 NotifyMutated(false);
                             });
                             sizeDrag->WidthMode = UI::UISizeMode::Fill;
                             c.AddRow("Ortho Size", sizeDrag);
                             c.AddBool("Fixed Aspect Ratio", &comp.FixedAspectRatio);
                         }
                     });
        }

        // Script Component
        if (entity.HasComponent<ScriptComponent>())
        {
            makeCard("Script", true,
                     [entity]() mutable { entity.GetComponent<ScriptComponent>() = ScriptComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<ScriptComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<ScriptComponent>();
                         for (size_t idx = 0; idx < comp.ClassNames.size(); ++idx)
                         {
                             auto classRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                             classRow->WidthMode = UI::UISizeMode::Fill;
                             auto classInput = UI::MakeUI<UI::UITextInput>(comp.ClassNames[idx], "Namespace.ClassName");
                             classInput->WidthMode = UI::UISizeMode::Fill;
                             classInput->FlexWeight = 1.0f;
                             classInput->OnChanged = [this, &comp, idx](const std::string& v)
                             {
                                 if (idx < comp.ClassNames.size())
                                     comp.ClassNames[idx] = v;
                                 NotifyMutated(true);
                             };
                             classRow->AddChild(classInput);

                             auto removeBtn = UI::Button("x", [this, &comp, idx]()
                             {
                                 if (idx < comp.ClassNames.size())
                                 {
                                     comp.EntityReferences.erase(comp.ClassNames[idx]);
                                     comp.FieldOverrides.erase(comp.ClassNames[idx]);
                                     comp.ClassNames.erase(comp.ClassNames.begin() + static_cast<ptrdiff_t>(idx));
                                 }
                                 NotifyMutated(true);
                             }).Tooltip("Remove script class");
                             removeBtn->WidthMode = UI::UISizeMode::Fixed;
                             removeBtn->HeightMode = UI::UISizeMode::Fixed;
                             removeBtn->FixedSize = { 24.0f, 24.0f };
                             removeBtn->Padding = UI::UIThickness::All(0.0f);
                             classRow->AddChild(removeBtn);
                             c.AddRow("Class " + std::to_string(idx + 1), classRow);

                             const std::string& className = comp.ClassNames[idx];
                             if (!className.empty() && !ScriptEngine::EntityClassExists(className))
                             {
                                 auto errLabel = UI::Label("Class not found in loaded script assembly");
                                 errLabel->FontSizeOverride = 13.0f;
                                 errLabel->UseColorOverride = true;
                                 errLabel->ColorOverride = { 0.90f, 0.34f, 0.38f, 1.0f };
                                 c.Body->AddChild(errLabel);
                                 continue;
                             }

                             if (ScriptEngine::EntityClassExists(className))
                             {
                                 for (const ScriptFieldInfo& field : ScriptEngine::GetExposedFields(className))
                                 {
                                     const std::string& fieldName = field.Name;
                                     if (field.Type == ScriptFieldType::Entity)
                                     {
                                         auto& ref = comp.EntityReferences[className][fieldName];
                                         Entity targetEnt;
                                         if (ref.IsModelNode())
                                             targetEnt = m_Context->GetEntityByUUID(ModelInstance::NodeUUID(ref.ModelInstance, ref.ModelNodeIndex));
                                         else if (ref.Entity != 0)
                                             targetEnt = m_Context->GetEntityByUUID(ref.Entity);

                                         std::vector<std::string> entNames = { "None" };
                                         std::vector<Entity> candidates = { Entity{} };
                                         int selectedIdx = 0;
                                         for (auto handle : m_Context->GetAllEntitiesWith<TagComponent>())
                                         {
                                             Entity cand(handle, m_Context.get());
                                             if (field.DisallowSelf && cand == entity)
                                                 continue;
                                             if (cand == targetEnt)
                                                 selectedIdx = static_cast<int>(candidates.size());
                                             entNames.push_back(cand.GetName());
                                             candidates.push_back(cand);
                                         }
                                         c.AddEnum(fieldName, std::move(entNames), selectedIdx, [&ref, candidates](int choice)
                                         {
                                             ref = {};
                                             if (choice > 0 && choice < static_cast<int>(candidates.size()))
                                             {
                                                 Entity picked = candidates[static_cast<size_t>(choice)];
                                                 if (picked.HasComponent<ModelNodeComponent>())
                                                 {
                                                     const auto& node = picked.GetComponent<ModelNodeComponent>();
                                                     ref.ModelInstance = node.Instance;
                                                     ref.ModelNodeIndex = node.NodeIndex;
                                                 }
                                                 else if (picked)
                                                 {
                                                     ref.Entity = picked.GetUUID();
                                                 }
                                             }
                                         }, field.Tooltip);
                                         continue;
                                     }

                                     auto& overrides = comp.FieldOverrides[className];
                                     ScriptValue currentVal = overrides.contains(fieldName) ? overrides[fieldName] : field.DefaultValue;
                                     const bool ranged = field.Max > field.Min;
                                     switch (field.Type)
                                     {
                                     case ScriptFieldType::Bool:
                                     {
                                         bool bv = std::get<bool>(currentVal);
                                         auto cb = UI::CheckBox("", bv, [this, &overrides, fieldName](bool next)
                                         {
                                             overrides[fieldName] = next;
                                             NotifyMutated(false);
                                         });
                                         cb->SetEnabled(!field.ReadOnly);
                                         c.AddRow(fieldName, cb, field.Tooltip);
                                         break;
                                     }
                                     case ScriptFieldType::Int:
                                     {
                                         int32_t iv = std::get<int32_t>(currentVal);
                                         auto drag = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(iv), 0.25f, [this, &overrides, fieldName, ranged, field](float next)
                                         {
                                             int32_t res = static_cast<int32_t>(std::round(next));
                                             if (ranged)
                                                 res = std::clamp(res, static_cast<int32_t>(field.Min), static_cast<int32_t>(field.Max));
                                             overrides[fieldName] = res;
                                             NotifyMutated(false);
                                         });
                                         drag->WidthMode = UI::UISizeMode::Fill;
                                         drag->Format = "{:.0f}";
                                         drag->SetEnabled(!field.ReadOnly);
                                         c.AddRow(fieldName, drag, field.Tooltip);
                                         break;
                                     }
                                     case ScriptFieldType::UInt:
                                     {
                                         uint32_t uv = std::get<uint32_t>(currentVal);
                                         auto drag = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(uv), 0.25f, [this, &overrides, fieldName, ranged, field](float next)
                                         {
                                             uint32_t res = static_cast<uint32_t>(std::max(0.0f, std::round(next)));
                                             if (ranged)
                                                 res = std::clamp(res, static_cast<uint32_t>(std::max(0.0f, field.Min)), static_cast<uint32_t>(std::max(0.0f, field.Max)));
                                             overrides[fieldName] = res;
                                             NotifyMutated(false);
                                         });
                                         drag->WidthMode = UI::UISizeMode::Fill;
                                         drag->Format = "{:.0f}";
                                         drag->SetEnabled(!field.ReadOnly);
                                         c.AddRow(fieldName, drag, field.Tooltip);
                                         break;
                                     }
                                     case ScriptFieldType::Float:
                                     {
                                         float factor = 1.0f;
                                         const bool hasUnit = !field.Unit.empty() && ScriptUnitFactor(field.Unit, factor);
                                         const float display = hasUnit ? std::get<float>(currentVal) / factor : std::get<float>(currentVal);
                                         auto drag = UI::MakeUI<UI::UIDragScalar>(display, 0.1f, [this, &overrides, fieldName, ranged, field, factor, hasUnit](float next)
                                         {
                                             float v = ranged ? std::clamp(next, field.Min, field.Max) : next;
                                             overrides[fieldName] = hasUnit ? (v * factor) : v;
                                             NotifyMutated(false);
                                         });
                                         drag->WidthMode = UI::UISizeMode::Fill;
                                         drag->SetEnabled(!field.ReadOnly);
                                         c.AddRow(hasUnit ? (fieldName + " (" + field.Unit + ")") : fieldName, drag, field.Tooltip);
                                         break;
                                     }
                                     case ScriptFieldType::String:
                                     {
                                         std::string sv = std::get<std::string>(currentVal);
                                         auto input = UI::MakeUI<UI::UITextInput>(sv);
                                         input->WidthMode = UI::UISizeMode::Fill;
                                         input->ReadOnly = field.ReadOnly;
                                         input->OnChanged = [this, &overrides, fieldName](const std::string& next)
                                         {
                                             overrides[fieldName] = next;
                                             NotifyMutated(false);
                                         };
                                         c.AddRow(fieldName, input, field.Tooltip);
                                         break;
                                     }
                                     case ScriptFieldType::Vector3:
                                     {
                                         const glm::vec3 vv = std::get<glm::vec3>(currentVal);
                                         auto vec = UI::MakeUI<UI::UIVectorEdit>(3, glm::vec4(vv, 0.0f), 0.1f, [this, &overrides, fieldName](const glm::vec4& next)
                                         {
                                             overrides[fieldName] = glm::vec3(next.x, next.y, next.z);
                                             NotifyMutated(false);
                                         });
                                         c.AddRow(fieldName, vec, field.Tooltip);
                                         break;
                                     }
                                     default:
                                         break;
                                     }
                                 }
                             }
                         }

                         auto addScriptBtn = UI::Button("+ Add Script Class", [this, &comp]()
                         {
                             comp.ClassNames.emplace_back();
                             NotifyMutated(true);
                         });
                         addScriptBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("", addScriptBtn);
                     });
        }

        // Physics 3D
        if (entity.HasComponent<RigidBody3DComponent>())
        {
            makeCard("Rigidbody 3D", true,
                     [entity]() mutable { entity.GetComponent<RigidBody3DComponent>() = RigidBody3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<RigidBody3DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<RigidBody3DComponent>();
                         c.AddEnum("Body Type", { "Static", "Dynamic", "Kinematic" }, static_cast<int>(comp.Type),
                             [&comp](int i) { comp.Type = static_cast<RigidBody3DComponent::BodyType>(i); });
                         c.AddEnum("Motion Quality", { "Discrete", "LinearCast" }, static_cast<int>(comp.Quality),
                             [&comp](int i) { comp.Quality = static_cast<RigidBody3DComponent::MotionQuality>(i); });
                         c.AddFloat("Mass (kg)", &comp.Mass, 0.1f, 0.0f, 10000.0f);
                         c.AddFloat("Linear Damping", &comp.LinearDamping, 0.01f, 0.0f, 10.0f);
                         c.AddFloat("Angular Damping", &comp.AngularDamping, 0.01f, 0.0f, 10.0f);
                         c.AddFloat("Gravity Factor", &comp.GravityFactor, 0.05f, -10.0f, 10.0f);
                         c.AddBool("Allow Sleeping", &comp.AllowSleeping);
                         c.AddBool("Is Sensor", &comp.IsSensor);
                     });
        }

        if (entity.HasComponent<BoxCollider3DComponent>())
        {
            makeCard("Box Collider 3D", true,
                     [entity]() mutable { entity.GetComponent<BoxCollider3DComponent>() = BoxCollider3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<BoxCollider3DComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<BoxCollider3DComponent>();
                         c.AddLengthVec3("Half Extents", &comp.HalfExtents);
                         c.AddLengthVec3("Offset", &comp.Offset);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                         glm::vec3 lo, hi;
                         auto fitBtn = UI::Button("Fit to Mesh", [this, entity, &comp]() mutable
                         {
                             if (FitColliderToMesh(entity, comp))
                                 NotifyMutated(true);
                         });
                         fitBtn->SetEnabled(ComputeLocalMeshBounds(entity, lo, hi));
                         fitBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("Fit", fitBtn);
                     });
        }

        if (entity.HasComponent<SphereCollider3DComponent>())
        {
            makeCard("Sphere Collider 3D", true,
                     [entity]() mutable { entity.GetComponent<SphereCollider3DComponent>() = SphereCollider3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<SphereCollider3DComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<SphereCollider3DComponent>();
                         c.AddLength("Radius", &comp.Radius);
                         c.AddLengthVec3("Offset", &comp.Offset);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                         c.AddFloat("Rolling Resist", &comp.RollingResistance, 0.005f, 0.0f, 1.0f);
                         glm::vec3 lo, hi;
                         auto fitBtn = UI::Button("Fit to Mesh", [this, entity, &comp]() mutable
                         {
                             if (FitColliderToMesh(entity, comp))
                                 NotifyMutated(true);
                         });
                         fitBtn->SetEnabled(ComputeLocalMeshBounds(entity, lo, hi));
                         fitBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("Fit", fitBtn);
                     });
        }

        if (entity.HasComponent<CapsuleCollider3DComponent>())
        {
            makeCard("Capsule Collider 3D", true,
                     [entity]() mutable { entity.GetComponent<CapsuleCollider3DComponent>() = CapsuleCollider3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<CapsuleCollider3DComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<CapsuleCollider3DComponent>();
                         c.AddLength("Half Height", &comp.HalfHeight);
                         c.AddLength("Radius", &comp.Radius);
                         c.AddLength("Bottom Radius", &comp.BottomRadius);
                         c.AddLengthVec3("Offset", &comp.Offset);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                         glm::vec3 lo, hi;
                         auto fitBtn = UI::Button("Fit to Mesh", [this, entity, &comp]() mutable
                         {
                             if (FitColliderToMesh(entity, comp))
                                 NotifyMutated(true);
                         });
                         fitBtn->SetEnabled(ComputeLocalMeshBounds(entity, lo, hi));
                         fitBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("Fit", fitBtn);
                     });
        }

        if (entity.HasComponent<CylinderCollider3DComponent>())
        {
            makeCard("Cylinder Collider 3D", true,
                     [entity]() mutable { entity.GetComponent<CylinderCollider3DComponent>() = CylinderCollider3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<CylinderCollider3DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<CylinderCollider3DComponent>();
                         c.AddLength("Half Height", &comp.HalfHeight);
                         c.AddLength("Radius", &comp.Radius);
                         c.AddLength("Bottom Radius", &comp.BottomRadius);
                         c.AddLengthVec3("Offset", &comp.Offset);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                     });
        }

        if (entity.HasComponent<PlaneCollider3DComponent>())
        {
            makeCard("Plane Collider 3D", true,
                     [entity]() mutable { entity.GetComponent<PlaneCollider3DComponent>() = PlaneCollider3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<PlaneCollider3DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<PlaneCollider3DComponent>();
                         c.AddLength("Half Size X", &comp.HalfSize.x);
                         c.AddLength("Half Size Z", &comp.HalfSize.y);
                         c.AddLengthVec3("Offset", &comp.Offset);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                     });
        }

        if (entity.HasComponent<MeshCollider3DComponent>())
        {
            makeCard("Mesh Collider 3D", true,
                     [entity]() mutable { entity.GetComponent<MeshCollider3DComponent>() = MeshCollider3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<MeshCollider3DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<MeshCollider3DComponent>();
                         c.AddAssetSlot("Mesh Asset", comp.Mesh,
                             { AssetType::MeshSource, AssetType::StaticMesh, AssetType::Mesh },
                             [&comp](AssetHandle h) { comp.Mesh = h; comp.SubmeshIndex = 0; });
                         c.AddBool("Convex Hull", &comp.Convex);
                         c.AddUInt32("Submesh Index", &comp.SubmeshIndex);
                         c.AddUInt32("Submesh Count", &comp.SubmeshCount);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                     });
        }

        if (entity.HasComponent<CharacterController3DComponent>())
        {
            makeCard("Character Controller 3D", true,
                     [entity]() mutable { entity.GetComponent<CharacterController3DComponent>() = CharacterController3DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<CharacterController3DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<CharacterController3DComponent>();
                         c.AddLength("Radius", &comp.Radius);
                         c.AddLength("Height", &comp.Height);
                         c.AddLength("Step Height", &comp.StepHeight);
                         c.AddFloat("Max Slope (deg)", &comp.MaxSlopeDegrees, 0.25f, 0.0f, 89.0f, "{:.1f}");
                         c.AddFloat("Gravity Scale", &comp.GravityScale, 0.05f, 0.0f, 10.0f);
                         c.AddSlider("Air Control", &comp.AirControl, 0.0f, 1.0f);
                         c.AddLength("Max Accel", &comp.MaxAcceleration, 0.0f, 0.0f, "/s^2");
                         c.AddLength("Braking Decel", &comp.BrakingDeceleration, 0.0f, 0.0f, "/s^2");
                         c.AddReadonly("Grounded", comp.IsGrounded ? "Yes" : "No");
                     });
        }

        // Physics 2D & Sprites
        if (entity.HasComponent<RigidBody2DComponent>())
        {
            makeCard("Rigidbody 2D", true,
                     [entity]() mutable { entity.GetComponent<RigidBody2DComponent>() = RigidBody2DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<RigidBody2DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<RigidBody2DComponent>();
                         c.AddEnum("Body Type", { "Static", "Dynamic", "Kinematic" }, static_cast<int>(comp.Type),
                             [&comp](int i) { comp.Type = static_cast<RigidBody2DComponent::BodyType>(i); });
                         c.AddBool("Fixed Rotation", &comp.FixedRotation);
                     });
        }

        if (entity.HasComponent<BoxCollider2DComponent>())
        {
            makeCard("Box Collider 2D", true,
                     [entity]() mutable { entity.GetComponent<BoxCollider2DComponent>() = BoxCollider2DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<BoxCollider2DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<BoxCollider2DComponent>();
                         c.AddVec2("Offset", &comp.Offset, 0.05f);
                         c.AddVec2("Half Size", &comp.Size, 0.05f);
                         c.AddFloat("Density", &comp.Density, 0.05f, 0.0f, 100.0f);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                     });
        }

        if (entity.HasComponent<CircleCollider2DComponent>())
        {
            makeCard("Circle Collider 2D", true,
                     [entity]() mutable { entity.GetComponent<CircleCollider2DComponent>() = CircleCollider2DComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<CircleCollider2DComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<CircleCollider2DComponent>();
                         c.AddVec2("Offset", &comp.Offset, 0.05f);
                         c.AddLength("Radius", &comp.Radius);
                         c.AddFloat("Density", &comp.Density, 0.05f, 0.0f, 100.0f);
                         c.AddSlider("Friction", &comp.Friction, 0.0f, 1.0f);
                         c.AddSlider("Restitution", &comp.Restitution, 0.0f, 1.0f);
                     });
        }

        if (entity.HasComponent<SpriteRendererComponent>())
        {
            makeCard("Sprite Renderer", true,
                     [entity]() mutable { entity.GetComponent<SpriteRendererComponent>() = SpriteRendererComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<SpriteRendererComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<SpriteRendererComponent>();
                         c.AddColor4("Color", &comp.Color);
                         c.AddAssetSlot("Texture", comp.Texture, { AssetType::Texture2D }, [&comp](AssetHandle h) { comp.Texture = h; });
                         c.AddFloat("Tiling", &comp.TilingFactor, 0.05f, 0.01f, 100.0f);
                     });
        }

        if (entity.HasComponent<CircleRendererComponent>())
        {
            makeCard("Circle Renderer", true,
                     [entity]() mutable { entity.GetComponent<CircleRendererComponent>() = CircleRendererComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<CircleRendererComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<CircleRendererComponent>();
                         c.AddColor4("Color", &comp.Color);
                         c.AddSlider("Thickness", &comp.Thickness, 0.0f, 1.0f);
                         c.AddFloat("Fade", &comp.Fade, 0.001f, 0.0f, 1.0f);
                     });
        }

        // Animation & Timeline
        if (entity.HasComponent<AnimatorComponent>())
        {
            makeCard("Animator", true, {},
                     [entity]() mutable { entity.RemoveComponent<AnimatorComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<AnimatorComponent>();
                         Ref<AnimationSequence> currentAnim = comp.Animator.GetCurrentAnimation();
                         if (!currentAnim && comp.Animation != 0)
                         {
                             if (Ref<AnimationSequence> loaded = AssetManager::GetAsset<AnimationSequence>(comp.Animation))
                             {
                                 loaded->Handle = comp.Animation;
                                 comp.Animator.PlayAnimation(loaded);
                                 if (!comp.Playing)
                                     comp.Animator.Pause();
                                 currentAnim = comp.Animator.GetCurrentAnimation();
                             }
                         }

                         // Media Controls
                         auto mediaRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
                         mediaRow->WidthMode = UI::UISizeMode::Fill;
                         const bool isPlaying = comp.Animator.IsPlaying();
                         auto playPauseBtn = UI::Button(isPlaying ? "Pause" : "Play", [this, entity]() mutable
                         {
                             auto& ac = entity.GetComponent<AnimatorComponent>();
                             if (ac.Animator.IsPlaying())
                             {
                                 ac.Animator.Pause();
                                 ac.Playing = false;
                             }
                             else
                             {
                                 Ref<AnimationSequence> anim = ac.Animator.GetCurrentAnimation();
                                 if (!ac.Animator.IsLooping() && anim && ac.Animator.GetCurrentAnimationTime() >= anim->Duration)
                                     ac.Animator.SetCurrentTime(0.0f);
                                 ac.Animator.Resume();
                                 ac.Playing = true;
                             }
                             NotifyMutated(true);
                         });
                         playPauseBtn->WidthMode = UI::UISizeMode::Fill;
                         playPauseBtn->FlexWeight = 1.0f;
                         playPauseBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         if (isPlaying)
                             playPauseBtn->StyleVariant = "primary";
                         mediaRow->AddChild(playPauseBtn);

                         auto stopBtn = UI::Button("Stop", [this, entity]() mutable
                         {
                             auto& ac = entity.GetComponent<AnimatorComponent>();
                             ac.Animator.Stop();
                             ac.Playing = false;
                             NotifyMutated(true);
                         });
                         stopBtn->WidthMode = UI::UISizeMode::Fill;
                         stopBtn->FlexWeight = 1.0f;
                         stopBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         mediaRow->AddChild(stopBtn);
                         c.AddRow("Playback", mediaRow);

                         bool looping = comp.Animator.IsLooping();
                         auto loopCb = UI::CheckBox("", looping, [this, entity](bool next) mutable
                         {
                             entity.GetComponent<AnimatorComponent>().Animator.SetLooping(next);
                             NotifyMutated(false);
                         });
                         c.AddRow("Looping", loopCb);
                         c.Syncs.push_back([loopCb, entity]() mutable
                         {
                             if (entity && entity.HasComponent<AnimatorComponent>() && !UI::HasState(loopCb->GetState(), UI::UIState::Pressed))
                                 loopCb->Checked = entity.GetComponent<AnimatorComponent>().Animator.IsLooping();
                         });

                         float speed = comp.Animator.GetPlaybackSpeed();
                         auto speedDrag = UI::MakeUI<UI::UIDragScalar>(speed, 0.05f, [this, entity](float v) mutable
                         {
                             entity.GetComponent<AnimatorComponent>().Animator.SetPlaybackSpeed(std::clamp(v, 0.0f, 10.0f));
                             NotifyMutated(false);
                         });
                         speedDrag->WidthMode = UI::UISizeMode::Fill;
                         speedDrag->Min = 0.0f;
                         speedDrag->Max = 10.0f;
                         c.AddRow("Speed", speedDrag);
                         c.Syncs.push_back([speedDrag, entity]() mutable
                         {
                             if (entity && entity.HasComponent<AnimatorComponent>() &&
                                 !speedDrag->IsInteracting())
                                 speedDrag->Value = entity.GetComponent<AnimatorComponent>().Animator.GetPlaybackSpeed();
                         });

                         // Single-Clip Animation Selector (skeleton-filtered by default)
                         auto allClipsCb = UI::CheckBox("Show clips from all skeletons", m_AllAnimClips, [this](bool next)
                         {
                             m_AllAnimClips = next;
                             m_Dirty = true;
                         });
                         if (comp.Skeleton != 0)
                             c.AddRow("Clip Filter", allClipsCb);

                         std::string clipLabel = currentAnim ? (currentAnim->Name.empty() ? AssetDisplayName(currentAnim->Handle) : currentAnim->Name)
                                                             : AssetDisplayName(comp.Animation);
                         auto clipRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                         clipRow->WidthMode = UI::UISizeMode::Fill;
                         auto clipBtn = UI::Button(clipLabel);
                         clipBtn->WidthMode = UI::UISizeMode::Fill;
                         clipBtn->FlexWeight = 1.0f;
                         clipBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         UI::UIButton* clipPtr = clipBtn.Get().get();
                         clipBtn->OnClick = [this, clipPtr, entity]() mutable
                         {
                             UI::UIContext* curCtx = UI::UIContext::GetCurrent();
                             auto mgr = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
                             if (!curCtx || !mgr)
                                 return;
                             const auto& ac = entity.GetComponent<AnimatorComponent>();
                             const auto& registry = mgr->GetAssetRegistry();
                             const AssetMetadata* skelMeta = (ac.Skeleton != 0 && registry.contains(ac.Skeleton)) ? &registry.at(ac.Skeleton) : nullptr;
                             std::vector<AssetPickerOption> opts;
                             for (const auto& [h, meta] : registry)
                             {
                                 if (meta.Type != AssetType::AnimationSequence)
                                     continue;
                                 if (!m_AllAnimClips && skelMeta)
                                 {
                                     const bool sameSource = !skelMeta->SourceFilePath.empty() && meta.SourceFilePath == skelMeta->SourceFilePath;
                                     const bool sameDir = meta.FilePath.parent_path() == skelMeta->FilePath.parent_path();
                                     if (!sameSource && !sameDir)
                                         continue;
                                 }
                                 opts.push_back({ meta.FilePath.stem().string(), meta.FilePath.parent_path().generic_string(), h });
                             }
                             std::sort(opts.begin(), opts.end(), [](const AssetPickerOption& a, const AssetPickerOption& b)
                             {
                                 return ToLowerStr(a.Label) < ToLowerStr(b.Label);
                             });
                             ShowSearchableAssetPicker(*curCtx, clipPtr->GetBounds(), "None", ac.Animation, std::move(opts),
                                 [this, entity](AssetHandle picked) mutable
                                 {
                                     auto& animComp = entity.GetComponent<AnimatorComponent>();
                                     if (picked == 0)
                                     {
                                         animComp.Animation = 0;
                                         animComp.Playing = false;
                                         animComp.Animator.Stop();
                                         animComp.Animator.PlayAnimation(nullptr);
                                     }
                                     else if (Ref<AnimationSequence> anim = AssetManager::GetAsset<AnimationSequence>(picked))
                                     {
                                         anim->Handle = picked;
                                         animComp.Animation = picked;
                                         animComp.Playing = true;
                                         animComp.Animator.PlayAnimation(anim);
                                     }
                                     NotifyMutated(true);
                                 });
                         };
                         clipRow->AddChild(clipBtn);
                         if (comp.Animation != 0)
                         {
                             auto clrClip = UI::Button("x", [this, entity]() mutable
                             {
                                 auto& ac = entity.GetComponent<AnimatorComponent>();
                                 ac.Animation = 0;
                                 ac.Playing = false;
                                 ac.Animator.Stop();
                                 ac.Animator.PlayAnimation(nullptr);
                                 NotifyMutated(true);
                             });
                             clrClip->WidthMode = UI::UISizeMode::Fixed;
                             clrClip->HeightMode = UI::UISizeMode::Fixed;
                             clrClip->FixedSize = { 24.0f, 24.0f };
                             clrClip->Padding = UI::UIThickness::All(0.0f);
                             clipRow->AddChild(clrClip);
                         }
                         c.AddRow("Animation Clip", clipRow);

                         const float maxDuration = currentAnim ? std::max(0.01f, currentAnim->Duration) : 100.0f;
                         auto timeSlider = UI::MakeUI<UI::UISlider>(comp.Animator.GetCurrentAnimationTime(), 0.0f, maxDuration,
                             [entity](float t) mutable { entity.GetComponent<AnimatorComponent>().Animator.SetCurrentTime(t); });
                         timeSlider->WidthMode = UI::UISizeMode::Fill;
                         c.AddRow("Time (Ticks)", timeSlider);
                         c.Syncs.push_back([timeSlider, entity]() mutable
                         {
                             if (entity && entity.HasComponent<AnimatorComponent>() && !UI::HasState(timeSlider->GetState(), UI::UIState::Pressed))
                                 timeSlider->Value = entity.GetComponent<AnimatorComponent>().Animator.GetCurrentAnimationTime();
                         });

                         // Animation Graph Mode
                         c.AddSubheader("Animation Graph");
                         auto graphRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                         graphRow->WidthMode = UI::UISizeMode::Fill;
                         auto graphBtn = UI::Button(AssetDisplayName(comp.Graph));
                         graphBtn->WidthMode = UI::UISizeMode::Fill;
                         graphBtn->FlexWeight = 1.0f;
                         graphBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         UI::UIButton* graphPtr = graphBtn.Get().get();
                         graphBtn->OnClick = [this, graphPtr, entity]() mutable
                         {
                             UI::UIContext* curCtx = UI::UIContext::GetCurrent();
                             if (!curCtx)
                                 return;
                             ShowSearchableAssetPicker(*curCtx, graphPtr->GetBounds(), "None",
                                 entity.GetComponent<AnimatorComponent>().Graph,
                                 CollectAssetPickerOptions({ AssetType::AnimationGraph }),
                                 [this, entity](AssetHandle picked) mutable
                                 {
                                     entity.GetComponent<AnimatorComponent>().Graph = picked;
                                     NotifyMutated(true);
                                 });
                         };
                         graphRow->AddChild(graphBtn);
                         if (comp.Graph != 0 && m_OnOpenAsset)
                         {
                             auto openGr = UI::Button("Open", [this, gh = comp.Graph]() { m_OnOpenAsset(gh); });
                             openGr->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                             graphRow->AddChild(openGr);
                         }
                         if (comp.Graph != 0)
                         {
                             auto clrGr = UI::Button("x", [this, entity]() mutable
                             {
                                 entity.GetComponent<AnimatorComponent>().Graph = 0;
                                 NotifyMutated(true);
                             });
                             clrGr->WidthMode = UI::UISizeMode::Fixed;
                             clrGr->HeightMode = UI::UISizeMode::Fixed;
                             clrGr->FixedSize = { 24.0f, 24.0f };
                             clrGr->Padding = UI::UIThickness::All(0.0f);
                             graphRow->AddChild(clrGr);
                         }
                         c.AddRow("Graph", graphRow);

                         if (comp.Graph != 0)
                         {
                             c.AddBool("Root Motion", &comp.RootMotion);
                             if (comp.RootMotion)
                                 c.AddString("Root Bone", &comp.RootBone, "(first joint)");

                             AnimationGraphInstance& graphInst = comp.GraphInstance;
                             if (!graphInst.Parameters.empty())
                                 c.AddSubheader("Graph Parameters");
                             for (auto& [pName, pVal] : graphInst.Parameters)
                             {
                                 auto pRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                                 pRow->WidthMode = UI::UISizeMode::Fill;
                                 pRow->VAlign = UI::UIAlignment::Center;
                                 if (float* f = std::get_if<float>(&pVal))
                                 {
                                     auto d = UI::MakeUI<UI::UIDragScalar>(*f, 0.01f, [this, &graphInst, pName, f](float v)
                                     {
                                         *f = v;
                                         graphInst.Overrides[pName] = *f;
                                         NotifyMutated(false);
                                     });
                                     d->WidthMode = UI::UISizeMode::Fill;
                                     d->FlexWeight = 1.0f;
                                     pRow->AddChild(d);
                                 }
                                 else if (bool* b = std::get_if<bool>(&pVal))
                                 {
                                     auto cb = UI::CheckBox("", *b, [this, &graphInst, pName, b](bool v)
                                     {
                                         *b = v;
                                         graphInst.Overrides[pName] = *b;
                                         NotifyMutated(false);
                                     });
                                     cb->FlexWeight = 1.0f;
                                     pRow->AddChild(cb);
                                 }
                                 else if (int32_t* iv = std::get_if<int32_t>(&pVal))
                                 {
                                     auto d = UI::MakeUI<UI::UIDragScalar>(static_cast<float>(*iv), 0.25f, [this, &graphInst, pName, iv](float v)
                                     {
                                         *iv = static_cast<int32_t>(std::round(v));
                                         graphInst.Overrides[pName] = *iv;
                                         NotifyMutated(false);
                                     });
                                     d->Format = "{:.0f}";
                                     d->WidthMode = UI::UISizeMode::Fill;
                                     d->FlexWeight = 1.0f;
                                     pRow->AddChild(d);
                                 }
                                 if (graphInst.Overrides.contains(pName))
                                 {
                                     auto rst = UI::Button("Reset", [this, &graphInst, pName]()
                                     {
                                         graphInst.Overrides.erase(pName);
                                         if (const NodeGraphValue* fb = graphInst.DefaultOf(pName))
                                             graphInst.Parameters[pName] = *fb;
                                         NotifyMutated(true);
                                     });
                                     rst->Padding = UI::UIThickness::Symmetric(6.0f, 3.0f);
                                     pRow->AddChild(rst);
                                 }
                                 c.AddRow(pName, pRow);
                             }
                         }
                     });
        }

        if (entity.HasComponent<BoneSocketComponent>())
        {
            makeCard("Bone Socket", true,
                     [entity]() mutable { entity.GetComponent<BoneSocketComponent>() = BoneSocketComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<BoneSocketComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<BoneSocketComponent>();
                         c.AddString("Bone Name", &comp.Bone, "hand_r");
                         c.AddLengthVec3("Offset", &comp.Offset);
                         c.AddVec3("Rotation (rad)", &comp.RotationOffset, 0.02f);
                         c.AddVec3("Scale", &comp.Scale, 0.02f);
                     });
        }

        if (entity.HasComponent<TimelinePlayerComponent>())
        {
            makeCard("Timeline Player", true,
                     [entity]() mutable { entity.GetComponent<TimelinePlayerComponent>() = TimelinePlayerComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<TimelinePlayerComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<TimelinePlayerComponent>();
                         c.AddAssetSlot("Timeline", comp.Timeline, { AssetType::Timeline }, [&comp](AssetHandle h) { comp.Timeline = h; });
                         if (comp.Timeline != 0 && m_OnOpenAsset)
                         {
                             auto openTl = UI::Button("Open Timeline", [this, h = comp.Timeline]() { m_OnOpenAsset(h); });
                             openTl->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                             c.AddRow("", openTl);
                         }
                         c.AddBool("Play On Start", &comp.PlayOnStart);
                         c.AddFloat("Playback Speed", &comp.Speed, 0.05f, -10.0f, 10.0f);
                     });
        }

        // Audio
        if (entity.HasComponent<AudioSourceComponent>())
        {
            makeCard("Audio Source", true,
                     [entity]() mutable { entity.GetComponent<AudioSourceComponent>() = AudioSourceComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<AudioSourceComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<AudioSourceComponent>();
                         c.AddAssetSlot("Sound", comp.Sound, { AssetType::AudioClip, AssetType::AudioGraph },
                             [&comp](AssetHandle h) { comp.Sound = h; });
                         const bool isGraph = comp.Sound != 0 && AssetManager::GetAssetType(comp.Sound) == AssetType::AudioGraph;
                         if (isGraph && m_OnOpenAsset)
                         {
                             auto openAg = UI::Button("Open Audio Graph", [this, h = comp.Sound]() { m_OnOpenAsset(h); });
                             openAg->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                             c.AddRow("", openAg);
                         }
                         c.AddSlider("Volume", &comp.Volume, 0.0f, 2.0f);
                         c.AddFloat("Pitch", &comp.Pitch, 0.01f, 0.1f, 4.0f);
                         if (!isGraph)
                             c.AddBool("Loop", &comp.Loop);
                         c.AddBool("Play On Start", &comp.PlayOnStart);
                         c.AddBool("Spatial (3D)", &comp.Spatial);
                     });
        }

        if (entity.HasComponent<ImpactSoundComponent>())
        {
            makeCard("Impact Sound", true,
                     [entity]() mutable { entity.GetComponent<ImpactSoundComponent>() = ImpactSoundComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<ImpactSoundComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<ImpactSoundComponent>();
                         c.AddAssetSlot("Sound", comp.Sound, { AssetType::AudioClip, AssetType::AudioGraph },
                             [&comp](AssetHandle h) { comp.Sound = h; });
                         c.AddSlider("Volume", &comp.Volume, 0.0f, 2.0f);
                         c.AddFloat("Min Speed (m/s)", &comp.MinSpeed, 0.05f, 0.0f, 50.0f);
                         c.AddFloat("Max Speed (m/s)", &comp.MaxSpeed, 0.1f, 0.1f, 100.0f);
                         c.AddFloat("Cooldown (s)", &comp.Cooldown, 0.01f, 0.0f, 5.0f);
                     });
        }

        if (entity.HasComponent<AudioListenerComponent>())
        {
            makeCard("Audio Listener", true, {},
                     [entity]() mutable { entity.RemoveComponent<AudioListenerComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<AudioListenerComponent>();
                         c.AddBool("Listening", &comp.Listening);
                     });
        }

        if (entity.HasComponent<AudioVolumeComponent>())
        {
            makeCard("Audio Volume", true,
                     [entity]() mutable { entity.GetComponent<AudioVolumeComponent>() = AudioVolumeComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<AudioVolumeComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<AudioVolumeComponent>();
                         c.AddLengthVec3("Half Extents", &comp.HalfExtents);
                         c.AddInt32("Priority", &comp.Priority, -100, 100);
                         c.AddSlider("Reverb Send", &comp.ReverbSend, 0.0f, 1.0f);
                         c.AddSlider("Interior Volume", &comp.InteriorVolume, 0.0f, 1.0f);
                         c.AddSlider("Exterior Volume", &comp.ExteriorVolume, 0.0f, 1.0f);
                         c.AddFloat("Fade Time (s)", &comp.FadeTime, 0.05f, 0.0f, 10.0f);
                     });
        }

        // Volumes / Particles / Text / Metadata
        if (entity.HasComponent<PostProcessVolumeComponent>())
        {
            makeCard("Post Process Volume", true,
                     [entity]() mutable { entity.GetComponent<PostProcessVolumeComponent>() = PostProcessVolumeComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<PostProcessVolumeComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<PostProcessVolumeComponent>();
                         c.AddBool("Enabled", &comp.Enabled);
                         c.AddBool("Unbound (Global)", &comp.Unbound);
                         if (!comp.Unbound)
                             c.AddLengthVec3("Half Extents", &comp.HalfExtents);
                         c.AddInt32("Priority", &comp.Priority, -100, 100);
                     });
        }

        if (entity.HasComponent<ParticleEmitterComponent>())
        {
            makeCard("Particle Emitter", true,
                     [entity]() mutable { entity.GetComponent<ParticleEmitterComponent>() = ParticleEmitterComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<ParticleEmitterComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<ParticleEmitterComponent>();
                         c.AddBool("Emitting", &comp.Emitting);
                         c.AddBool("Looping", &comp.Looping);
                         c.AddFloat("Duration (s)", &comp.Duration, 0.1f, 0.05f, 600.0f);
                         c.AddFloat("Spawn Rate", &comp.SpawnRate, 0.5f, 0.0f, 5000.0f);
                         c.AddUInt32("Burst", &comp.Burst, 0, 10000);
                         c.AddUInt32("Max Particles", &comp.MaxParticles, 1, 50000);
                         c.AddEnum("Shape", { "Point", "Sphere", "Box", "Cone" }, static_cast<int>(comp.Shape),
                             [&comp](int i) { comp.Shape = static_cast<ParticleShape>(i); });
                         c.AddAssetSlot("Texture", comp.Texture, { AssetType::Texture2D }, [&comp](AssetHandle h) { comp.Texture = h; }, "Soft Dot");
                     });
        }

        if (entity.HasComponent<TextComponent>())
        {
            makeCard("Text Renderer", true,
                     [entity]() mutable { entity.GetComponent<TextComponent>() = TextComponent{}; },
                     [entity]() mutable { entity.RemoveComponent<TextComponent>(); },
                     [entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<TextComponent>();
                         c.AddString("Text", &comp.TextString, "Hello World");
                         c.AddColor4("Color", &comp.Color);
                         c.AddFloat("Kerning", &comp.Kerning, 0.01f);
                         c.AddFloat("Line Spacing", &comp.LineSpacing, 0.01f);
                         c.AddSlider("Outline", &comp.OutlineThickness, 0.0f, 1.0f);
                         c.AddColor4("Outline Color", &comp.OutlineColor);
                     });
        }

        if (entity.HasComponent<MetadataComponent>())
        {
            makeCard("Metadata", true, {},
                     [entity]() mutable { entity.RemoveComponent<MetadataComponent>(); },
                     [this, entity](CardBuilder& c) mutable
                     {
                         auto& comp = entity.GetComponent<MetadataComponent>();
                         for (size_t i = 0; i < comp.Values.size(); ++i)
                         {
                             auto kvRow = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 4.0f);
                             kvRow->WidthMode = UI::UISizeMode::Fill;
                             auto keyIn = UI::MakeUI<UI::UITextInput>(comp.Values[i].first, "Key");
                             keyIn->WidthMode = UI::UISizeMode::Fill;
                             keyIn->FlexWeight = 0.45f;
                             keyIn->OnChanged = [this, &comp, i](const std::string& k) { comp.Values[i].first = k; NotifyMutated(false); };
                             auto valIn = UI::MakeUI<UI::UITextInput>(comp.Values[i].second, "Value");
                             valIn->WidthMode = UI::UISizeMode::Fill;
                             valIn->FlexWeight = 0.55f;
                             valIn->OnChanged = [this, &comp, i](const std::string& v) { comp.Values[i].second = v; NotifyMutated(false); };
                             auto delBtn = UI::Button("x", [this, &comp, i]()
                             {
                                 if (i < comp.Values.size())
                                     comp.Values.erase(comp.Values.begin() + static_cast<ptrdiff_t>(i));
                                 NotifyMutated(true);
                             });
                             delBtn->WidthMode = UI::UISizeMode::Fixed;
                             delBtn->HeightMode = UI::UISizeMode::Fixed;
                             delBtn->FixedSize = { 24.0f, 24.0f };
                             delBtn->Padding = UI::UIThickness::All(0.0f);
                             kvRow->AddChild(keyIn);
                             kvRow->AddChild(valIn);
                             kvRow->AddChild(delBtn);
                             c.Body->AddChild(kvRow);
                         }
                         auto addBtn = UI::Button("+ Add Key/Value", [this, &comp]()
                         {
                             comp.Values.emplace_back("Key", "Value");
                             NotifyMutated(true);
                         });
                         addBtn->Padding = UI::UIThickness::Symmetric(8.0f, 4.0f);
                         c.AddRow("", addBtn);
                     });
        }
    }

    void InspectorV2Panel::ShowAddComponentMenu(const UI::UIRect& triggerBounds)
    {
        UI::UIContext* ctx = UI::UIContext::GetCurrent();
        if (!ctx || !m_Primary)
            return;

        Entity entity = m_Primary;
        auto addEntry = [&]<typename T>(const char* label, std::function<void(T&)> onInit = {}) -> UI::UIMenuItem
        {
            const bool alreadyHas = entity.HasComponent<T>();
            return {
                label, "",
                [this, entity, onInit]() mutable
                {
                    if (!entity || entity.HasComponent<T>())
                        return;
                    T& added = entity.AddComponent<T>();
                    if (onInit)
                        onInit(added);
                    NotifyMutated(true);
                },
                false, !alreadyHas, alreadyHas
            };
        };

        std::vector<UI::UIMenuItem> rendering = {
            addEntry.template operator()<MeshComponent>("Mesh"),
            addEntry.template operator()<MaterialComponent>("Material"),
            addEntry.template operator()<CameraComponent>("Camera"),
            addEntry.template operator()<SpriteRendererComponent>("Sprite Renderer"),
            addEntry.template operator()<CircleRendererComponent>("Circle Renderer"),
            addEntry.template operator()<ParticleEmitterComponent>("Particle Emitter"),
            addEntry.template operator()<TextComponent>("Text Renderer"),
            addEntry.template operator()<PostProcessVolumeComponent>("Post Process Volume")
        };

        std::vector<UI::UIMenuItem> lighting = {
            addEntry.template operator()<DirectionalLightComponent>("Directional Light"),
            addEntry.template operator()<PointLightComponent>("Point Light"),
            addEntry.template operator()<SpotLightComponent>("Spot Light"),
            addEntry.template operator()<RectLightComponent>("Rect Light"),
            addEntry.template operator()<DiscLightComponent>("Disc Light"),
            addEntry.template operator()<ReflectionProbeComponent>("Reflection Probe"),
            addEntry.template operator()<IrradianceVolumeComponent>("Irradiance Volume"),
            addEntry.template operator()<EnvironmentLightComponent>("Environment Light")
        };

        std::vector<UI::UIMenuItem> physics3D = {
            addEntry.template operator()<RigidBody3DComponent>("Rigidbody 3D"),
            addEntry.template operator()<BoxCollider3DComponent>("Box Collider 3D", [entity](BoxCollider3DComponent& c) mutable { FitColliderToMesh(entity, c); }),
            addEntry.template operator()<SphereCollider3DComponent>("Sphere Collider 3D", [entity](SphereCollider3DComponent& c) mutable { FitColliderToMesh(entity, c); }),
            addEntry.template operator()<CapsuleCollider3DComponent>("Capsule Collider 3D", [entity](CapsuleCollider3DComponent& c) mutable { FitColliderToMesh(entity, c); }),
            addEntry.template operator()<CylinderCollider3DComponent>("Cylinder Collider 3D", [entity](CylinderCollider3DComponent& c) mutable { FitColliderToMesh(entity, c); }),
            addEntry.template operator()<PlaneCollider3DComponent>("Plane Collider 3D"),
            addEntry.template operator()<MeshCollider3DComponent>("Mesh Collider 3D", [entity](MeshCollider3DComponent& c) mutable
            {
                if (entity.HasComponent<MeshComponent>())
                {
                    const auto& mc = entity.GetComponent<MeshComponent>();
                    c.Mesh = mc.Mesh;
                    c.SubmeshIndex = mc.SubmeshIndex;
                    c.SubmeshCount = mc.SubmeshCount;
                }
            }),
            addEntry.template operator()<CharacterController3DComponent>("Character Controller 3D")
        };

        std::vector<UI::UIMenuItem> physics2D = {
            addEntry.template operator()<RigidBody2DComponent>("Rigidbody 2D"),
            addEntry.template operator()<BoxCollider2DComponent>("Box Collider 2D"),
            addEntry.template operator()<CircleCollider2DComponent>("Circle Collider 2D")
        };

        std::vector<UI::UIMenuItem> audio = {
            addEntry.template operator()<AudioSourceComponent>("Audio Source"),
            addEntry.template operator()<ImpactSoundComponent>("Impact Sound"),
            addEntry.template operator()<AudioListenerComponent>("Audio Listener"),
            addEntry.template operator()<AudioVolumeComponent>("Audio Volume")
        };

        std::vector<UI::UIMenuItem> animation = {
            addEntry.template operator()<AnimatorComponent>("Animator"),
            addEntry.template operator()<BoneSocketComponent>("Bone Socket"),
            addEntry.template operator()<TimelinePlayerComponent>("Timeline Player")
        };

        std::vector<UI::UIMenuItem> rootItems = {
            addEntry.template operator()<ScriptComponent>("Script"),
            { "Rendering", "", {}, false, true, false, false, "", std::move(rendering) },
            { "Lighting", "", {}, false, true, false, false, "", std::move(lighting) },
            { "Physics 3D", "", {}, false, true, false, false, "", std::move(physics3D) },
            { "Physics 2D", "", {}, false, true, false, false, "", std::move(physics2D) },
            { "Audio", "", {}, false, true, false, false, "", std::move(audio) },
            { "Animation", "", {}, false, true, false, false, "", std::move(animation) },
            addEntry.template operator()<MetadataComponent>("Metadata")
        };

        UI::UIMenuPopup::ShowPopupMenu(*ctx, triggerBounds, std::move(rootItems), true);
    }
}
