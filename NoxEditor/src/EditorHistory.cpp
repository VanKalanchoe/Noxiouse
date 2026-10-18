#include "EditorHistory.h"

#include <unordered_set>
#include <utility>

#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Scene/Components.h"
#include "NoxCore/Scene/Entity.h"
#include "NoxCore/Scene/Scene.h"
#include "NoxCore/Scene/SceneSerializer.h"

namespace Nox
{
    namespace
    {
        // A Bistro-sized level is about 1 MB of scene text per state; small scenes are a few KB.
        constexpr size_t MaxStates = 100;
        constexpr size_t MaxBytes = 512ull * 1024ull * 1024ull;
        // Frames without a change (and without loads running) after which a step counts as settled.
        constexpr uint32_t SettleFrames = 30;

        uint64_t Mix(uint64_t x)
        {
            // splitmix64's finalizer
            x ^= x >> 30;
            x *= 0xbf58476d1ce4e5b9ull;
            x ^= x >> 27;
            x *= 0x94d049bb133111ebull;
            x ^= x >> 31;
            return x;
        }

        uint64_t HashBytes(const void* data, size_t size)
        {
            // FNV-1a
            uint64_t hash = 0xcbf29ce484222325ull;
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 0x100000001b3ull;
            }
            return hash;
        }

        // Spawned from a model / prefab asset: not saved, so not part of a state either (the instance root holds what was changed).
        bool IsSpawnedNode(Scene& scene, entt::registry& registry, entt::entity handle)
        {
            if (const auto* node = registry.try_get<ModelNodeComponent>(handle))
            {
                Entity root = scene.GetEntityByUUID(node->Instance);
                if (root && root.HasComponent<ModelInstanceComponent>())
                    return true;
            }
            if (const auto* node = registry.try_get<PrefabNodeComponent>(handle))
            {
                Entity root = scene.GetEntityByUUID(node->Instance);
                if (root && root.HasComponent<PrefabInstanceComponent>())
                    return true;
            }
            return false;
        }

        // Entities whose transforms a playing animator moves every frame (animation runs in Edit mode too, e.g. the fans of an
        // imported level): their transforms are not a change the user made, so neither the fingerprint nor the watched entity's
        // text looks at them.
        std::unordered_set<UUID> AnimatedEntities(entt::registry& registry)
        {
            std::unordered_set<UUID> animated;
            for (auto handle : registry.view<AnimatorComponent>())
            {
                const AnimatorComponent& animator = registry.get<AnimatorComponent>(handle);
                if (animator.Playing && (animator.Animation != 0 || animator.Graph != 0))
                    animated.insert(animator.NodeEntities.begin(), animator.NodeEntities.end());
            }
            return animated;
        }

        const std::string& EmptyName()
        {
            static const std::string empty;
            return empty;
        }
    }

    void EditorHistory::Update(const Ref<Scene>& scene, const FrameInfo& frame)
    {
        if (!scene)
            return;
        if (scene != m_Scene)
        {
            Bind(scene);
            Watch(frame.Watched);
            return;
        }

        // The user started something new: whatever the last step still produced on its own is part of it no longer, and a drag
        // in progress becomes one step when it is released.
        if (frame.InteractionActive && !frame.InteractionEnded)
        {
            m_Settling = false;
            return;
        }

        if (frame.InteractionEnded || m_Settling)
        {
            size_t entityCount = 0;
            const uint64_t fingerprint = Fingerprint(*scene, entityCount);
            // The watched entity is the one from before this frame's selection change: an edit committed by clicking another
            // entity (a text field losing focus) still belongs to the entity it was typed into.
            const bool watchedChanged = frame.InteractionEnded && m_Watched != 0 && EntityText(*scene, m_Watched) != m_WatchedText;

            if (fingerprint != m_Fingerprint || watchedChanged)
            {
                std::string text = SceneText(scene);
                if (text != m_States[m_Index].Text)
                {
                    if (frame.InteractionEnded)
                    {
                        std::string name;
                        if (entityCount > m_EntityCount)
                            name = entityCount - m_EntityCount == 1 ? "Create Entity" : "Create " + std::to_string(entityCount - m_EntityCount) + " Entities";
                        else if (entityCount < m_EntityCount)
                            name = m_EntityCount - entityCount == 1 ? "Delete Entity" : "Delete " + std::to_string(m_EntityCount - entityCount) + " Entities";
                        else if (frame.GizmoEnded)
                            name = "Transform";
                        else if (Entity watched = scene->GetEntityByUUID(m_Watched); watchedChanged && watched)
                            name = "Edit " + watched.GetName();
                        else
                            name = "Edit Scene";
                        Commit(std::move(text), std::move(name));
                    }
                    else
                    {
                        Amend(std::move(text));
                    }
                    m_Settling = true;
                    m_StableFrames = 0;
                }
                m_Fingerprint = fingerprint;
                m_EntityCount = entityCount;
                m_Watched = 0; // re-read below
            }
            else if (m_Settling && !frame.AssetsLoading && ++m_StableFrames >= SettleFrames)
            {
                m_Settling = false;
            }
        }

        if (frame.Watched != m_Watched)
            Watch(frame.Watched);
    }

    void EditorHistory::Clear()
    {
        m_Scene = nullptr;
        m_States.clear();
        m_Index = 0;
        m_TotalBytes = 0;
        m_Fingerprint = 0;
        m_EntityCount = 0;
        m_Watched = 0;
        m_WatchedText.clear();
        m_Settling = false;
        m_StableFrames = 0;
    }

    const std::string& EditorHistory::GetUndoName() const
    {
        return CanUndo() ? m_States[m_Index].Name : EmptyName();
    }

    const std::string& EditorHistory::GetRedoName() const
    {
        return CanRedo() ? m_States[m_Index + 1].Name : EmptyName();
    }

    std::string EditorHistory::StepBack()
    {
        if (!CanUndo())
            return {};
        --m_Index;
        return m_States[m_Index].Text;
    }

    std::string EditorHistory::StepForward()
    {
        if (!CanRedo())
            return {};
        ++m_Index;
        return m_States[m_Index].Text;
    }

    void EditorHistory::OnSceneRestored(const Ref<Scene>& scene)
    {
        m_Scene = scene;
        m_Fingerprint = Fingerprint(*scene, m_EntityCount);
        m_Watched = 0;
        m_WatchedText.clear();
        // The restored text spawns its model and prefab instances again; that must not count as a change.
        m_Settling = true;
        m_StableFrames = 0;
    }

    void EditorHistory::Bind(const Ref<Scene>& scene)
    {
        Clear();
        m_Scene = scene;
        m_States.push_back({ SceneText(scene), std::string() });
        m_TotalBytes = m_States.back().Text.size();
        m_Fingerprint = Fingerprint(*scene, m_EntityCount);
        m_Settling = true;
    }

    void EditorHistory::Commit(std::string text, std::string name)
    {
        // A change after undoing drops the states that could have been redone.
        for (size_t i = m_Index + 1; i < m_States.size(); ++i)
            m_TotalBytes -= m_States[i].Text.size();
        m_States.resize(m_Index + 1);

        m_TotalBytes += text.size();
        m_States.push_back({ std::move(text), std::move(name) });
        m_Index = m_States.size() - 1;
        Trim();
    }

    void EditorHistory::Amend(std::string text)
    {
        m_TotalBytes -= m_States[m_Index].Text.size();
        m_TotalBytes += text.size();
        m_States[m_Index].Text = std::move(text);
        Trim();
    }

    void EditorHistory::Trim()
    {
        while ((m_States.size() > MaxStates || m_TotalBytes > MaxBytes) && m_Index > 0)
        {
            m_TotalBytes -= m_States.front().Text.size();
            m_States.erase(m_States.begin());
            --m_Index;
        }
    }

    void EditorHistory::Watch(UUID entity)
    {
        m_Watched = entity;
        m_WatchedText = (entity != 0 && m_Scene) ? EntityText(*m_Scene, entity) : std::string();
    }

    uint64_t EditorHistory::Fingerprint(Scene& scene, size_t& entityCount)
    {
        NOX_PROFILE_SCOPE("Undo Fingerprint");

        // Order-independent (a sum of per-entity hashes): what the scene file holds of each saved entity that changes without the
        // Properties panel -- its id, name, parent, folder and transform.
        entt::registry& registry = scene.GetRegistry();
        const std::unordered_set<UUID> animated = AnimatedEntities(registry);
        uint64_t sum = 0;
        entityCount = 0;
        auto view = registry.view<IDComponent, TagComponent, TransformComponent>();
        for (auto handle : view)
        {
            if (IsSpawnedNode(scene, registry, handle))
                continue;

            const UUID id = view.get<IDComponent>(handle).ID;
            const std::string& tag = view.get<TagComponent>(handle).Tag;
            uint64_t hash = Mix(static_cast<uint64_t>(id));
            hash = Mix(hash ^ HashBytes(tag.data(), tag.size()));
            if (!animated.contains(id))
            {
                const TransformComponent& transform = view.get<TransformComponent>(handle);
                hash = Mix(hash ^ HashBytes(&transform.Translation, sizeof(transform.Translation)));
                hash = Mix(hash ^ HashBytes(&transform.Rotation, sizeof(transform.Rotation)));
                hash = Mix(hash ^ HashBytes(&transform.Scale, sizeof(transform.Scale)));
            }
            if (const auto* relationship = registry.try_get<RelationshipComponent>(handle))
                hash = Mix(hash ^ static_cast<uint64_t>(relationship->Parent));
            if (const auto* folder = registry.try_get<FolderComponent>(handle))
                hash = Mix(hash ^ HashBytes(folder->Path.data(), folder->Path.size()));
            // What a model or prefab instance turns into once its asset has loaded (a single-mesh model becomes a mesh entity).
            const uint64_t kinds = (registry.all_of<MeshComponent>(handle) ? 1ull : 0ull) |
                                   (registry.all_of<ModelInstanceComponent>(handle) ? 2ull : 0ull) |
                                   (registry.all_of<PrefabInstanceComponent>(handle) ? 4ull : 0ull);
            hash = Mix(hash ^ kinds);
            sum += hash;
            ++entityCount;
        }

        uint64_t result = Mix(sum ^ static_cast<uint64_t>(entityCount));
        for (const std::string& folder : scene.GetFolders())
            result = Mix(result ^ HashBytes(folder.data(), folder.size()));
        return result;
    }

    std::string EditorHistory::SceneText(const Ref<Scene>& scene)
    {
        NOX_PROFILE_SCOPE("Undo Snapshot");
        SceneSerializer serializer(scene);
        return serializer.SerializeToText();
    }

    std::string EditorHistory::EntityText(Scene& scene, UUID entity)
    {
        Entity found = scene.GetEntityByUUID(entity);
        if (!found)
            return {};
        if (!AnimatedEntities(scene.GetRegistry()).contains(entity))
            return SceneSerializer::EntityToText(scene, found);

        // An animated node: its text with a fixed transform, so the pose the animation is at does not read as an edit.
        TransformComponent& transform = found.GetComponent<TransformComponent>();
        const TransformComponent pose = transform;
        transform = TransformComponent();
        std::string text = SceneSerializer::EntityToText(scene, found);
        transform = pose;
        return text;
    }
}
