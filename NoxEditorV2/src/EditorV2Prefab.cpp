// Prefab Mode, prefab variants and Replace References (F1): EditorV2Layer's prefab work, out of EditorV2Layer.cpp (E2).
#include "EditorV2Layer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include "Panels/AssetPreviewRenderer.h"
#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/PrefabImporter.h"
#include "NoxCore/Audio/AudioMixer.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Scene/Prefab.h"
#include "NoxCore/Scene/PrefabInstance.h"
#include "NoxCore/Scene/SceneSerializer.h"
#include "NoxCore/UI/Core/UILayer.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UIImage.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox
{
    namespace
    {
        // Replaces a handle written as a number of its own (not inside a longer run of digits).
        bool ReplaceHandleText(std::string& text, AssetHandle from, AssetHandle to)
        {
            const std::string fromText = std::to_string(static_cast<uint64_t>(from));
            const std::string toText = std::to_string(static_cast<uint64_t>(to));
            bool changed = false;
            for (size_t at = text.find(fromText); at != std::string::npos; at = text.find(fromText, at))
            {
                const bool digitBefore = at > 0 && std::isdigit(static_cast<unsigned char>(text[at - 1]));
                const bool digitAfter = at + fromText.size() < text.size() && std::isdigit(static_cast<unsigned char>(text[at + fromText.size()]));
                if (digitBefore || digitAfter)
                {
                    at += fromText.size();
                    continue;
                }
                text.replace(at, fromText.size(), toText);
                at += toText.size();
                changed = true;
            }
            return changed;
        }

        bool IsTextAssetType(AssetType type)
        {
            return type == AssetType::Scene || type == AssetType::Prefab || type == AssetType::AnimationGraph || type == AssetType::AudioGraph ||
                   type == AssetType::AudioMixer || type == AssetType::Timeline || type == AssetType::MaterialGraph;
        }
    }

    Ref<UI::UIElement> EditorV2Layer::BuildPrefabBar()
    {
        auto bar = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
        bar->VAlign = UI::UIAlignment::Center;
        Ref<UI::UILabel> label(new UI::UILabel("", 14.0f));
        label->VAlign = UI::UIAlignment::Center;
        label->UseColorOverride = true;
        label->ColorOverride = { 0.45f, 0.70f, 1.0f, 1.0f };
        label->Tooltip = "Prefab Mode: the prefab's entities on their own. The level comes back on Exit.";
        m_PrefabBarLabel = label;
        bar->AddChild(label);
        const auto button = [&bar](const std::string& text, const std::string& tooltip, std::function<void()> onClick, bool primary = false)
        {
            Ref<UI::UIButton> made(new UI::UIButton(text, std::move(onClick)));
            made->Tooltip = tooltip;
            made->Padding = UI::UIThickness::Symmetric(10.0f, 4.0f);
            made->VAlign = UI::UIAlignment::Center;
            if (primary)
                made->StyleVariant = "primary";
            bar->AddChild(made);
        };
        button("Save", "Write the prefab (Ctrl+S); the level's instances spawn again from it", [this]() { SavePrefabMode(); }, true);
        button("Save & Exit", "Save, then back to the level", [this]() { if (SavePrefabMode()) ExitPrefabMode(); });
        button("Exit", "Back to the level (asks first when there are unsaved changes)", [this]() { RequestExitPrefabMode(); });
        bar->Visible = false;
        m_PrefabBar = bar;
        return bar;
    }

    void EditorV2Layer::OpenPrefabMode(AssetHandle handle)
    {
        if (!Project::GetActive() || !AssetManager::IsAssetHandleValid(handle) || AssetManager::GetAssetType(handle) != AssetType::Prefab)
            return;
        if (m_PrefabMode.Active)
        {
            // Another prefab: this one is left first (asked about when it has changes).
            if (m_PrefabMode.Prefab != handle)
                RequestExitPrefabMode([this, handle]() { OpenPrefabMode(handle); });
            return;
        }
        if (m_SceneLoad)
        {
            Notify(UI::UINotificationSeverity::Info, "Prefab not opened", "A scene is still loading: open the prefab when it is done.");
            return;
        }
        CancelHLODBuild("prefab editing");
        m_AssetEditors.EndPreviews();
        if (m_SceneState != SceneState::Edit)
            OnSceneStop();

        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
        Ref<Prefab> prefab = PrefabImporter::ImportPrefab(handle, metadata);
        if (!prefab)
        {
            Notify(UI::UINotificationSeverity::Error, "Prefab not opened", metadata.FilePath.filename().string() + " could not be read -- see the Output Log.");
            return;
        }

        Ref<Scene> prefabScene = CreateRef<Scene>();
        // A variant is edited as an instance of its base carrying the variant's changes; any other prefab as its own entities.
        const bool isVariant = prefab->Base != 0;
        Entity root = isVariant ? PrefabInstance::LoadVariantForEditing(*prefabScene, *prefab) : PrefabInstance::LoadForEditing(*prefabScene, *prefab);
        if (!root)
        {
            Notify(UI::UINotificationSeverity::Error, "Prefab not opened",
                   metadata.FilePath.filename().string() + (isVariant ? ": its base could not be loaded." : " has no root entity."));
            return;
        }

        // The light it is seen in: the level's sky and sun, as preview entities (not below the root: never saved). A level without a sky
        // gets the default one.
        std::vector<UUID> previews;
        bool hasEnvironment = false;
        if (m_EditorScene)
        {
            for (auto levelHandle : m_EditorScene->GetAllEntitiesWith<EnvironmentLightComponent>())
            {
                Entity source(levelHandle, m_EditorScene.get());
                Entity preview = prefabScene->CreateEntity("Preview Environment (not saved)");
                preview.AddComponent<EnvironmentLightComponent>(source.GetComponent<EnvironmentLightComponent>());
                previews.push_back(preview.GetUUID());
                hasEnvironment = true;
            }
            for (auto levelHandle : m_EditorScene->GetAllEntitiesWith<DirectionalLightComponent>())
            {
                Entity source(levelHandle, m_EditorScene.get());
                Entity preview = prefabScene->CreateEntity("Preview Sun (not saved)");
                preview.GetComponent<TransformComponent>() = source.GetComponent<TransformComponent>();
                preview.AddComponent<DirectionalLightComponent>(source.GetComponent<DirectionalLightComponent>());
                previews.push_back(preview.GetUUID());
            }
        }
        if (!hasEnvironment)
        {
            Entity preview = prefabScene->CreateEntity("Preview Environment (not saved)");
            preview.AddComponent<EnvironmentLightComponent>().TexturePath = "EnvironmentMaps/shanghai_bund_4k_cube_bc6u.dds";
            previews.push_back(preview.GetUUID());
        }
        if (m_ViewportImage)
        {
            const UI::UIRect& bounds = m_ViewportImage->GetBounds();
            const uint32_t w = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Width())));
            const uint32_t h = static_cast<uint32_t>(std::max(0.0f, std::floor(bounds.Height())));
            if (w > 0 && h > 0)
                prefabScene->OnViewportResize(w, h);
        }

        // The level waits: its scene, path, history, unsaved state and camera.
        m_PrefabMode.Active = true;
        m_PrefabMode.Prefab = handle;
        m_PrefabMode.Name = metadata.FilePath.stem().string();
        m_PrefabMode.Variant = isVariant;
        m_PrefabMode.Root = root.GetUUID();
        m_PrefabMode.PreviewEntities = std::move(previews);
        m_PrefabMode.FramePending = 3;
        m_PrefabMode.ReturnScene = m_EditorScene;
        m_PrefabMode.ReturnScenePath = m_EditorScenePath;
        m_PrefabMode.ReturnHistory = std::move(m_History);
        m_PrefabMode.ReturnDirty = m_SceneDirty;
        m_PrefabMode.ReturnHistoryPositionSeen = m_HistoryPositionSeen;
        m_PrefabMode.ReturnCamera = m_EditorCamera;

        m_HoveredEntity = Entity();
        m_EditorScene = prefabScene;
        m_ActiveScene = prefabScene;
        m_History.Clear(); // (its own history: bound to the prefab scene on its first update)
        m_EditorScenePath.clear(); // nothing saves the level over the prefab
        m_SceneCleanPending = true;
        SetSceneContext(prefabScene);
        m_Selection.Set({ root });
        m_UnloadUnusedAssetsRequested = true;
        RefreshToolBar();
        Notify(UI::UINotificationSeverity::Info, "Prefab Mode", m_PrefabMode.Name + (isVariant ? " (variant)" : "") + ": Save or Exit in the toolbar.");
    }

    bool EditorV2Layer::SavePrefabMode()
    {
        if (!m_PrefabMode.Active || !Project::GetActive())
            return false;
        Entity root = m_EditorScene->GetEntityByUUID(m_PrefabMode.Root);
        if (!root)
        {
            Notify(UI::UINotificationSeverity::Error, "Prefab not saved", "Its root entity was deleted: undo (Ctrl+Z) to get it back.");
            return false;
        }
        // Entities beside the root are not part of the prefab: said, not dropped silently.
        std::vector<std::string> outside;
        for (auto handle : m_EditorScene->GetAllEntitiesWith<IDComponent>())
        {
            Entity entity(handle, m_EditorScene.get());
            if (entity.GetParent() || entity.GetUUID() == m_PrefabMode.Root ||
                std::find(m_PrefabMode.PreviewEntities.begin(), m_PrefabMode.PreviewEntities.end(), entity.GetUUID()) != m_PrefabMode.PreviewEntities.end())
                continue;
            outside.push_back(entity.GetName());
        }

        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(m_PrefabMode.Prefab);
        const std::filesystem::path path = Project::GetActiveAssetDirectory() / metadata.FilePath;
        if (m_PrefabMode.Variant)
        {
            // The variant file holds what this instance of its base differs in, and the entities added below it.
            Ref<Prefab> variantFile = PrefabImporter::ImportPrefab(m_PrefabMode.Prefab, metadata);
            if (!variantFile || !PrefabInstance::SaveVariant(*m_EditorScene, root, *variantFile, path))
            {
                Notify(UI::UINotificationSeverity::Error, "Variant not saved", m_PrefabMode.Name + ": its base has to be loaded -- see the Output Log.");
                return false;
            }
        }
        else if (!SceneSerializer::SerializePrefab(*m_EditorScene, { root }, m_PrefabMode.Name, path))
        {
            Notify(UI::UINotificationSeverity::Error, "Prefab not saved", path.filename().string() + " could not be written -- see the Output Log.");
            return false;
        }

        // The level's instances take their differences from the prefab first (told apart against the old content), then the loaded asset
        // takes the new content and every instance in the level spawns again from it.
        if (m_PrefabMode.ReturnScene)
            PrefabInstance::CaptureOverrides(*m_PrefabMode.ReturnScene, m_PrefabMode.Prefab);
        if (Ref<Prefab> fresh = PrefabImporter::ImportPrefab(m_PrefabMode.Prefab, metadata))
        {
            if (Prefab* loaded = AssetManager::FindLoadedAsset<Prefab>(m_PrefabMode.Prefab))
            {
                loaded->Name = fresh->Name;
                loaded->Root = fresh->Root;
                loaded->Entities = fresh->Entities;
                loaded->Base = fresh->Base;
                loaded->Overrides = fresh->Overrides;
                loaded->Structure = fresh->Structure;
                loaded->Nested = fresh->Nested;
            }
        }
        if (m_PrefabMode.ReturnScene)
            PrefabInstance::RespawnAll(*m_PrefabMode.ReturnScene, m_PrefabMode.Prefab);
        AssetPreviewRenderer::Get().Invalidate(m_PrefabMode.Prefab);

        m_SceneDirty = false;
        m_HistoryPositionSeen = m_History.GetPosition();
        if (outside.empty())
            Notify(UI::UINotificationSeverity::Success, "Prefab saved", m_PrefabMode.Name);
        else
        {
            std::string names;
            for (size_t i = 0; i < outside.size() && i < 4; ++i)
                names += (i == 0 ? "" : ", ") + outside[i];
            if (outside.size() > 4)
                names += " and " + std::to_string(outside.size() - 4) + " more";
            Notify(UI::UINotificationSeverity::Warning, "Prefab saved without " + std::to_string(outside.size()) + (outside.size() == 1 ? " entity" : " entities"),
                   names + (outside.size() == 1 ? " is" : " are") + " not under " + root.GetName() + ": drag onto it in the Scene Hierarchy to make part of the prefab.");
        }
        return true;
    }

    void EditorV2Layer::RequestExitPrefabMode(std::function<void()> afterwards)
    {
        if (!m_PrefabMode.Active)
        {
            if (afterwards)
                afterwards();
            return;
        }
        if (!m_SceneDirty)
        {
            ExitPrefabMode();
            if (afterwards)
                afterwards();
            return;
        }
        ShowSaveChangesDialog("The prefab " + m_PrefabMode.Name + " has unsaved changes.\nSave them before leaving Prefab Mode?",
            [this, afterwards]()
            {
                if (!SavePrefabMode())
                    return; // (not saved: stays in Prefab Mode, nothing is dropped)
                ExitPrefabMode();
                if (afterwards)
                    afterwards();
            },
            [this, afterwards]()
            {
                ExitPrefabMode();
                if (afterwards)
                    afterwards();
            });
    }

    void EditorV2Layer::ExitPrefabMode()
    {
        if (!m_PrefabMode.Active)
            return;
        m_AssetEditors.EndPreviews();
        m_HoveredEntity = Entity();
        m_EditorScene = m_PrefabMode.ReturnScene;
        m_ActiveScene = m_EditorScene;
        m_EditorScenePath = m_PrefabMode.ReturnScenePath;
        m_History = std::move(m_PrefabMode.ReturnHistory);
        m_SceneDirty = m_PrefabMode.ReturnDirty;
        m_HistoryPositionSeen = m_PrefabMode.ReturnHistoryPositionSeen;
        m_SceneCleanPending = false;
        m_EditorCamera = m_PrefabMode.ReturnCamera;
        m_PrefabMode = {};
        SetSceneContext(m_EditorScene);
        m_Selection.Clear();
        // A save respawned the level's instances: that is no edit of the level (taken as its state, not as an undo step).
        if (m_EditorScene)
            m_History.OnSceneRestored(m_EditorScene);
        m_UnloadUnusedAssetsRequested = true;
        RefreshToolBar();
    }

    void EditorV2Layer::UpdatePrefabMode()
    {
        if (m_PrefabBar)
            m_PrefabBar->Visible = m_PrefabMode.Active;
        if (!m_PrefabMode.Active)
            return;
        if (m_PrefabBarLabel)
        {
            const std::string text = "Prefab Mode: " + m_PrefabMode.Name + (m_PrefabMode.Variant ? " (variant)" : "") + (m_SceneDirty ? "  *" : "");
            if (m_PrefabBarLabel->Text != text)
                m_PrefabBarLabel->Text = text;
        }
        // Framed once its entities have spawned and their world transforms exist (a frame or two after opening).
        if (m_PrefabMode.FramePending > 0 && --m_PrefabMode.FramePending == 0)
            if (Entity root = m_EditorScene ? m_EditorScene->GetEntityByUUID(m_PrefabMode.Root) : Entity())
                FocusOnEntity(root);
    }

    AssetHandle EditorV2Layer::CreateVariant(AssetHandle baseHandle, const std::filesystem::path& relativeFolder)
    {
        if (!Project::GetActive())
            return 0;
        Ref<Prefab> base = AssetManager::GetAsset<Prefab>(baseHandle);
        if (!base)
        {
            NOX_CORE_ERROR("Create Variant: the prefab could not be loaded");
            return 0;
        }
        if (base->Root == 0)
        {
            NOX_CORE_ERROR("Create Variant: '{}' has no root entity", base->Name);
            return 0;
        }
        // "<Base> Variant": the name the Content Browser then lets the user change.
        std::string name = (base->Name.empty() ? std::string("Prefab") : base->Name) + " Variant";
        for (char& character : name)
            if (std::strchr("\\/:*?\"<>|", character) != nullptr)
                character = '_';
        const std::filesystem::path assetDirectory = Project::GetActiveAssetDirectory();
        std::filesystem::path relativePath = relativeFolder / (name + ".nprefab");
        for (int suffix = 2; std::filesystem::exists(assetDirectory / relativePath); ++suffix)
            relativePath = relativeFolder / (name + " " + std::to_string(suffix) + ".nprefab");
        if (!PrefabImporter::CreateVariantFile(relativePath.stem().string(), base->Root, baseHandle, assetDirectory / relativePath))
        {
            NOX_CORE_ERROR("Create Variant: could not write {}", relativePath.generic_string());
            return 0;
        }
        return Project::GetActive()->GetEditorAssetManager()->RegisterExistingFile(relativePath, AssetType::Prefab);
    }

    void EditorV2Layer::ReplaceReferences(AssetHandle from, AssetHandle to)
    {
        if (m_SceneState != SceneState::Edit)
        {
            Notify(UI::UINotificationSeverity::Warning, "References not replaced", "Stop Play / Simulate first: the scene being played would keep the old asset.");
            return;
        }
        if (m_PrefabMode.Active)
        {
            Notify(UI::UINotificationSeverity::Warning, "References not replaced", "Leave Prefab Mode first: the level waiting behind it would keep the old asset.");
            return;
        }
        if (m_HLODBuild || m_SceneLoad)
        {
            Notify(UI::UINotificationSeverity::Warning, "References not replaced", "Wait until the scene has loaded / the HLOD build is done (they read the files).");
            return;
        }
        // An open editor's unsaved edits would write the old asset back over the rewritten file: saved or dropped first.
        ConfirmUnsavedChanges("replacing references", [this, from, to]() { ApplyReplaceReferences(from, to); }, UnsavedAssets | UnsavedMixer);
    }

    void EditorV2Layer::ApplyReplaceReferences(AssetHandle from, AssetHandle to)
    {
        EditorAssetManager* assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager().get() : nullptr;
        if (!assets || from == to || !assets->IsAssetHandleValid(from) || !assets->IsAssetHandleValid(to) ||
            assets->GetMetadata(from).Type != assets->GetMetadata(to).Type)
            return;

        // The loaded assets that read rewritten files: read again afterwards (they held the old asset in memory: a prefab spawned it,
        // a graph or timeline window would have saved it back).
        std::unordered_map<std::string, AssetHandle> handleByPath;
        for (const auto& [handle, metadata] : assets->GetAssetRegistry())
            handleByPath[metadata.FilePath.lexically_normal().generic_string()] = handle;
        std::vector<AssetHandle> reread;
        bool mixerRewritten = false;
        for (const std::filesystem::path& file : assets->FindReferences(from))
        {
            const auto found = handleByPath.find(file.lexically_normal().generic_string());
            if (found == handleByPath.end())
                continue;
            const AssetType type = assets->GetMetadata(found->second).Type;
            if (!IsTextAssetType(type) || type == AssetType::Scene)
                continue;
            mixerRewritten |= type == AssetType::AudioMixer;
            if (AssetManager::IsAssetLoaded(found->second) && !assets->GetMetadata(found->second).SourceFilePath.empty())
                reread.push_back(found->second);
        }
        // Instances of a rewritten prefab keep their own changes over the spawn again (told apart against the old content first).
        if (m_EditorScene)
            for (AssetHandle prefab : reread)
                if (assets->GetMetadata(prefab).Type == AssetType::Prefab)
                    PrefabInstance::CaptureOverrides(*m_EditorScene, prefab);

        const size_t files = assets->ReplaceReferences(from, to);
        for (AssetHandle handle : reread)
        {
            assets->ReimportAsset(handle, true); // (a graph is read again in place; its window shows it)
            AssetPreviewRenderer::Get().Invalidate(handle);
        }
        if (mixerRewritten)
            AudioMixer::LoadProjectMixer(false);
        if (m_EditorScene)
            for (AssetHandle prefab : reread)
                if (assets->GetMetadata(prefab).Type == AssetType::Prefab)
                    PrefabInstance::RespawnAll(*m_EditorScene, prefab);

        // The open scene (unsaved changes included): replaced in its text and restored, one undo step.
        bool sceneChanged = false;
        if (m_EditorScene)
        {
            std::string text = SceneSerializer(m_EditorScene).SerializeToText();
            if (ReplaceHandleText(text, from, to))
            {
                RestoreHistoryState(text);
                sceneChanged = true;
            }
        }
        m_HistoryExternalStep = "Replace References"; // (a step only if the scene's text changed: respawned instances or the replace)
        m_SceneHierarchyPanel.MarkDirty();
        m_ContentBrowserPanel.Refresh();
        Notify(UI::UINotificationSeverity::Success, "References replaced",
               AssetDisplayName(from) + " -> " + AssetDisplayName(to) + ": " + std::to_string(files) + (files == 1 ? " file" : " files") + " rewritten" +
               (sceneChanged ? ", the open scene updated (Ctrl+Z undoes it there)." : "."));
    }
}
