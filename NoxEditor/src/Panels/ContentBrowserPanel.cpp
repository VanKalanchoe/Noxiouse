#include "ContentBrowserPanel.h"

#include <fstream>
#include <iterator>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <cstring>

#include "NoxCore/Animation/AnimationGraphNodes.h"
#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/MeshImporter.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Asset/NodeGraphSerializer.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    ContentBrowserPanel::ContentBrowserPanel(Ref<Project> project)
        : m_Project(std::move(project)), m_ThumbnailCache(CreateRef<ThumbnailCache>(m_Project)),
          m_BaseDirectory(m_Project->GetAssetDirectory()), m_CurrentDirectory(m_BaseDirectory)
    {
        // See EditorLayer.cpp's icon loads for why flip=true is needed here - these icon .ktx2
        // files are tagged bottom-up (Y=up) but their pixel data is actually top-down, so this
        // cancels out TextureImporter's automatic KTXorientation correction.
        m_DirectoryIcon = TextureImporter::LoadTexture2D("assets/Icons/DirectoryIcon.ktx2", {.flip = true, .generateMips = false});
        m_FileIcon = TextureImporter::LoadTexture2D("assets/Icons/FileIcon.ktx2", {.flip = true, .generateMips = false});
        RefreshAssetTree();
    }

    AssetHandle ContentBrowserPanel::FindAssetHandle(const std::filesystem::path& relativePath) const
    {
        const auto normalized = relativePath.lexically_normal();
        for (const auto& [handle, metadata] : m_Project->GetEditorAssetManager()->GetAssetRegistry())
            if (metadata.FilePath.lexically_normal() == normalized ||
                metadata.SourceFilePath.lexically_normal() == normalized)
                return handle;
        return 0;
    }

    Ref<Texture2D> ContentBrowserPanel::GetThumbnail(AssetHandle handle, const AssetMetadata& metadata)
    {
        return m_ThumbnailCache->GetOrCreateThumbnail(handle, metadata);
    }

    void ContentBrowserPanel::SetImportDestination(const std::filesystem::path& path)
    {
        strncpy_s(m_ImportDestPathBuffer, path.generic_string().c_str(), sizeof(m_ImportDestPathBuffer));
        strncpy_s(m_ImportFileNameBuffer, path.filename().generic_string().c_str(), sizeof(m_ImportFileNameBuffer));
    }

    void ContentBrowserPanel::OnExternalFileDrop(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
            return;

        const auto extension = path.extension().string();
        if (extension == ".nox")
        {
            std::error_code error;
            const auto assetRoot = std::filesystem::weakly_canonical(m_BaseDirectory, error);
            const auto sourcePath = std::filesystem::weakly_canonical(path, error);
            if (error)
                return;

            const auto relativePath = sourcePath.lexically_relative(assetRoot);
            const bool isInsideProject = !relativePath.empty() &&
                relativePath != "." &&
                relativePath.generic_string().rfind("..", 0) != 0;

            if (isInsideProject)
            {
                m_Project->GetEditorAssetManager()->ImportAsset(relativePath, relativePath, AssetType::Scene);
            }
            else
            {
                const auto destination = m_CurrentDirectory / path.filename();
                std::filesystem::copy_file(
                    sourcePath, destination,
                    std::filesystem::copy_options::overwrite_existing, error);
                if (error)
                    return;

                const auto destinationRelative = destination.lexically_relative(m_BaseDirectory);
                m_Project->GetEditorAssetManager()->ImportAsset(
                    destinationRelative, destinationRelative, AssetType::Scene);
            }

            RefreshAssetTree();
            return;
        }

        if (extension != ".gltf" && extension != ".glb")
            return;

        std::error_code error;
        const auto assetRoot = std::filesystem::weakly_canonical(m_BaseDirectory, error);
        const auto sourcePath = std::filesystem::weakly_canonical(path, error);
        if (error)
            return;

        const auto relativeToAssetRoot = sourcePath.lexically_relative(assetRoot);
        const bool isInsideProject = !relativeToAssetRoot.empty() &&
            relativeToAssetRoot != "." &&
            relativeToAssetRoot.generic_string().rfind("..", 0) != 0;

        std::filesystem::path relativeSource;
        if (isInsideProject)
        {
            // A project file already has valid relative references. Do not flatten
            // it into the current folder or its .bin/textures will stop resolving.
            relativeSource = relativeToAssetRoot;
        }
        else
        {
            // Defer copying until Cook & Import so Cancel has no side effects.
            m_PendingExternalSourcePath = sourcePath;
            m_PendingPackageDirectory = std::filesystem::relative(
                m_CurrentDirectory / path.stem(), m_BaseDirectory, error);
            if (error)
                return;
            relativeSource = m_PendingPackageDirectory / path.filename();
        }

        m_PendingImportPath = relativeSource;
        m_ImportSettings = {};
        ReadImportFileInfo(sourcePath);
        const auto defaultDest = relativeSource.parent_path() / "Meshes" /
            (relativeSource.stem().string() + ".nmesh");
        SetImportDestination(defaultDest);
        m_ShowImportModal = true;
    }

    // The file's size for the import dialog (the whole file is parsed once, like the import itself does).
    void ContentBrowserPanel::ReadImportFileInfo(const std::filesystem::path& absoluteSource)
    {
        const MeshImporter::GltfContent content = MeshImporter::InspectGltf(absoluteSource);
        m_ImportHasBounds = content.HasBounds;
        m_ImportBoundsMin = content.BoundsMin;
        m_ImportBoundsMax = content.BoundsMax;
        // Meters (glTF's own default) unless picked otherwise; applied right away, not only once the combo is touched -- the combo
        // showing "Meters" must mean the scale actually reads as meters, not silently stay 1.0 (raw file units = world units).
        m_ImportSourceUnit = 0;
        m_ImportSettings.ImportScale = WorldUnits::PerSourceUnit(WorldUnits::SourceUnit::Meters);
        // m_ImportFitHeightCm is left as it was: it remembers the last height you fit to, instead of defaulting to a human's 180 cm.
    }

    void ContentBrowserPanel::BeginRename(AssetHandle handle)
    {
        if (handle == 0 || !m_Project->GetEditorAssetManager()->CanRename(handle))
            return;
        const AssetMetadata metadata = m_Project->GetEditorAssetManager()->GetMetadata(handle);
        strncpy_s(m_RenameBuffer, metadata.FilePath.stem().string().c_str(), sizeof(m_RenameBuffer) - 1);
        m_RenameHandle = handle;
        m_RenameFocus = true;
    }

    void ContentBrowserPanel::RequestPrefabFromDrop(const ImGuiPayload* payload, const std::filesystem::path& folder)
    {
        if (!payload || payload->DataSize != sizeof(UUID) || !m_CreatePrefab)
            return;
        m_HasPrefabDrop = true;
        m_PrefabDropEntity = *static_cast<const UUID*>(payload->Data);
        m_PrefabDropFolder = folder;
    }

    void ContentBrowserPanel::OnImGuiRender()
    {
        ImGui::Begin("Content Browser");
        m_WindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_ChildWindows);

        DrawToolbar();

        // Entities dropped from the hierarchy: the prefab is created in the folder they were dropped on, and its name goes into
        // rename mode (in a folder tile's folder: the browser opens it).
        if (m_HasPrefabDrop)
        {
            m_HasPrefabDrop = false;
            const std::filesystem::path relativeFolder = m_PrefabDropFolder.lexically_relative(m_BaseDirectory);
            const AssetHandle created = m_CreatePrefab ? m_CreatePrefab(m_PrefabDropEntity, relativeFolder == "." ? std::filesystem::path() : relativeFolder) : AssetHandle(0);
            if (created != 0)
            {
                m_CurrentDirectory = m_PrefabDropFolder;
                RefreshAssetTree();
                m_Selected.clear();
                m_Selected.insert(created);
                BeginRename(created);
            }
        }

        // An asset created from elsewhere (the hierarchy's Create Prefab): shown, selected, its name in rename mode.
        if (m_PendingShowAsset != 0)
        {
            const AssetHandle created = m_PendingShowAsset;
            m_PendingShowAsset = 0;
            RefreshAssetTree();
            m_Selected.clear();
            m_Selected.insert(created);
            BeginRename(created);
        }

        // Create Variant (right-click on a prefab): the variant appears next to it and its name goes into rename mode.
        if (m_HasVariantRequest)
        {
            m_HasVariantRequest = false;
            const std::filesystem::path relativeFolder = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
            const AssetHandle created = m_CreateVariant ? m_CreateVariant(m_VariantBase, relativeFolder == "." ? std::filesystem::path() : relativeFolder) : AssetHandle(0);
            if (created != 0)
            {
                RefreshAssetTree();
                m_Selected.clear();
                m_Selected.insert(created);
                BeginRename(created);
            }
        }

        // A finished rename (Enter, or clicking elsewhere; Escape leaves the default name).
        if (m_RenameCommit)
        {
            m_RenameCommit = false;
            if (m_RenameHandle != 0)
            {
                auto* assets = m_Project->GetEditorAssetManager().get();
                const std::string current = assets->GetMetadata(m_RenameHandle).FilePath.stem().string();
                if (!m_RenameCommitText.empty() && m_RenameCommitText != current && !assets->RenameAsset(m_RenameHandle, m_RenameCommitText))
                    NOX_CORE_WARN("Could not rename '{}' to '{}' (the name is taken or has invalid characters)", current, m_RenameCommitText);
            }
            m_RenameHandle = 0;
            RefreshAssetTree();
        }

        if (m_EntriesDirectory != m_CurrentDirectory)
            RefreshAssetTree();
        // Folders made outside the editor show up within a couple of seconds.
        if (m_FolderTreeDirty || ImGui::GetTime() - m_FolderTreeTime > 2.0)
            RebuildFolderTree();
        UpdateVisibleEntries();

        // Only the visible rows are built (a folder of hundreds of meshes cost milliseconds per frame otherwise); the ids
        // and names are prepared once per refresh.
        // Ctrl+A selects every asset of the folder.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_A, false) && !ImGui::GetIO().WantTextInput)
        {
            m_Selected.clear();
            for (const BrowserEntry& entry : m_VisibleEntries)
            {
                if (!entry.IsDirectory)
                    m_Selected.insert(entry.Handle);
            }
        }

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F2, false) &&
            !ImGui::GetIO().WantTextInput && m_Selected.size() == 1)
        {
            BeginRename(*m_Selected.begin());
        }

        // Folder tree | tiles, each scrolling on its own, with a draggable splitter between them (UE's sources panel).
        // The table does not limit its cells' height: the two children get it explicitly, leaving one line for the status below.
        const float bodyHeight = std::max(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), 64.0f);
        if (ImGui::BeginTable("##content_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::BeginChild("##folder_tree", ImVec2(0.0f, bodyHeight)))
                DrawFolderTree(m_FolderTree);
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(1);
            if (ImGui::BeginChild("##tiles", ImVec2(0.0f, bodyHeight)))
                DrawTiles();
            ImGui::EndChild();
            ImGui::EndTable();
        }
        if (m_SearchBuffer[0] != '\0')
            ImGui::TextDisabled("%zu results below %s, %zu selected", m_VisibleEntries.size(),
                                CurrentFolder().empty() ? "Assets" : CurrentFolder().generic_string().c_str(), m_Selected.size());
        else
            ImGui::TextDisabled("%zu items, %zu selected", m_VisibleEntries.size(), m_Selected.size());

        if (m_ShowCreateScriptModal)
        {
            ImGui::OpenPopup("Create C# Script");
            m_ShowCreateScriptModal = false;
        }
        if (ImGui::BeginPopupModal("Create C# Script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("Class name", m_NewScriptName, sizeof(m_NewScriptName));
            const std::string className = m_NewScriptName;
            std::string scriptNamespace = m_Project->GetConfig().Name;
            std::replace_if(scriptNamespace.begin(), scriptNamespace.end(), [](unsigned char character)
            {
                return !std::isalnum(character) && character != '_';
            }, '_');
            if (scriptNamespace.empty()) scriptNamespace = "Game";
            if (std::isdigit(static_cast<unsigned char>(scriptNamespace.front())))
                scriptNamespace.insert(scriptNamespace.begin(), '_');
            const bool validName = !className.empty() &&
                (std::isalpha(static_cast<unsigned char>(className[0])) || className[0] == '_') &&
                std::all_of(className.begin() + 1, className.end(), [](unsigned char character)
                {
                    return std::isalnum(character) || character == '_';
                });
            const std::filesystem::path sourceDirectory = m_BaseDirectory / "Scripts/Source";
            const std::filesystem::path scriptPath = sourceDirectory / (className + ".cs");
            const bool scriptAlreadyExists = std::filesystem::exists(scriptPath);
            const bool disableCreate = !validName || scriptAlreadyExists;
            if (!validName)
                ImGui::TextColored({0.9f, 0.2f, 0.3f, 1.0f}, "Enter a valid C# class name");
            else if (scriptAlreadyExists)
                ImGui::TextColored({0.9f, 0.2f, 0.3f, 1.0f}, "A script with this name already exists");

            if (disableCreate) ImGui::BeginDisabled();
            if (ImGui::Button("Create"))
            {
                std::error_code error;
                std::filesystem::create_directories(sourceDirectory, error);
                const std::filesystem::path projectTemplate =
                    m_BaseDirectory / "Scripts/Templates/CSharpBehaviour.cs.template";
                const std::filesystem::path editorTemplate =
                    std::filesystem::path(Application::GetExecutableRootPath()) /
                    "assets/Templates/CSharpBehaviour.cs.template";
                const std::filesystem::path workingDirectoryTemplate =
                    "assets/Templates/CSharpBehaviour.cs.template";
                const std::filesystem::path templatePath = std::filesystem::exists(projectTemplate)
                    ? projectTemplate
                    : (std::filesystem::exists(editorTemplate) ? editorTemplate : workingDirectoryTemplate);

                std::ifstream templateFile(templatePath, std::ios::binary);
                const bool templateLoaded = templateFile.is_open();
                std::string contents((std::istreambuf_iterator<char>(templateFile)),
                                     std::istreambuf_iterator<char>());
                const auto replaceAll = [](std::string& text, std::string_view token, std::string_view replacement)
                {
                    for (size_t position = 0; (position = text.find(token, position)) != std::string::npos;)
                    {
                        text.replace(position, token.size(), replacement);
                        position += replacement.size();
                    }
                };
                replaceAll(contents, "{{NAMESPACE}}", scriptNamespace);
                replaceAll(contents, "{{CLASS_NAME}}", className);

                std::ofstream output(scriptPath, std::ios::binary);
                if (templateLoaded && output)
                    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                output.close();
                if (templateLoaded && output)
                {
                    NOX_CORE_INFO("Created C# script '{}' from template '{}'",
                                  scriptPath.string(), templatePath.string());
                    RefreshAssetTree();
                    constexpr char defaultScriptName[] = "NewBehaviour";
                    std::copy_n(defaultScriptName, sizeof(defaultScriptName), m_NewScriptName);
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    std::filesystem::remove(scriptPath, error);
                    NOX_CORE_ERROR("Failed to create C# script '{}' from template '{}'",
                                   scriptPath.string(), templatePath.string());
                }
            }
            if (disableCreate) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (m_ShowCreateGraphModal)
        {
            ImGui::OpenPopup("Create Animation Graph");
            m_ShowCreateGraphModal = false;
        }
        if (ImGui::BeginPopupModal("Create Animation Graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("Name", m_NewGraphName, sizeof(m_NewGraphName));
            const std::string graphName = m_NewGraphName;
            const std::filesystem::path graphPath = m_CurrentDirectory / (graphName + ".nanimgraph");
            const bool validName = !graphName.empty() && graphName.find_first_of("\\/:*?\"<>|") == std::string::npos;
            const bool graphAlreadyExists = std::filesystem::exists(graphPath);
            if (!validName)
                ImGui::TextColored({0.9f, 0.2f, 0.3f, 1.0f}, "Enter a valid file name");
            else if (graphAlreadyExists)
                ImGui::TextColored({0.9f, 0.2f, 0.3f, 1.0f}, "A graph with this name already exists here");

            const bool disableCreate = !validName || graphAlreadyExists;
            if (disableCreate) ImGui::BeginDisabled();
            if (ImGui::Button("Create"))
            {
                if (NodeGraphSerializer::Serialize(graphPath, CreateEmptyAnimationGraph()))
                {
                    // Writing the file isn't enough: the registry (and so the Content Browser, the Animator's
                    // Graph combo and everything else keyed by AssetHandle) only knows registered assets.
                    std::filesystem::path relativeDirectory = m_CurrentDirectory.lexically_relative(m_BaseDirectory);
                    if (relativeDirectory == ".")
                        relativeDirectory.clear(); // asset root: same as scanning everything
                    m_Project->GetEditorAssetManager()->ScanAndRegisterNewAssets(relativeDirectory);
                    RefreshAssetTree();

                    const AssetHandle handle = FindAssetHandle(graphPath.lexically_relative(m_BaseDirectory));
                    if (handle != 0 && m_OpenAsset)
                        m_OpenAsset(handle);

                    constexpr char defaultGraphName[] = "NewAnimationGraph";
                    std::copy_n(defaultGraphName, sizeof(defaultGraphName), m_NewGraphName);
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    NOX_CORE_ERROR("Failed to create animation graph '{}'", graphPath.string());
                }
            }
            if (disableCreate) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (m_ShowImportModal)
        {
            ImGui::OpenPopup("Import Settings");
            m_ShowImportModal = false;
        }
        if (ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Importing: %s", m_PendingImportPath.filename().string().c_str());
            // Mirrors Unreal's Interchange import options. Skinned meshes become one character entity when placed;
            // the file's content decides the assets: skinned meshes -> <name>.nmesh (skeletal mesh + .nskel + clips), the rest -> <name>.nsmesh (static mesh).
            auto combineCombo = [](const char* label, MeshCombineMode& mode, const char* tooltip)
            {
                const char* names[] = { "Do Not Combine", "Combine Visible", "Combine All" };
                int current = static_cast<int>(mode);
                if (ImGui::Combo(label, &current, names, 3))
                    mode = static_cast<MeshCombineMode>(current);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tooltip);
            };
            ImGui::SeparatorText("Meshes");
            ImGui::Checkbox("Import Static Meshes", &m_ImportSettings.ImportStaticMeshes);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Meshes that are not skinned (level geometry, props).");
            if (m_ImportSettings.ImportStaticMeshes)
                combineCombo("Combine Static Meshes", m_ImportSettings.StaticCombine, "Merge all static meshes into one submesh per material, node transforms baked in. Saves draw calls and instances; repeated meshes are copied instead of instanced. glTF has no visibility flag, so Visible = All.");
            ImGui::Checkbox("Import Skeletal Meshes", &m_ImportSettings.ImportSkeletalMeshes);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Skinned meshes and their skeleton (characters).");
            if (m_ImportSettings.ImportSkeletalMeshes)
            {
                ImGui::Checkbox("Import Skin Weights", &m_ImportSettings.ImportSkinWeights);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Off (Unreal's Geometry Only): skinned meshes come in as plain static geometry, without skeleton or animations from them.");
                if (m_ImportSettings.ImportSkinWeights)
                    combineCombo("Combine Skeletal Meshes", m_ImportSettings.SkeletalCombine, "Skinned meshes (they share the file's skeleton): Do Not Combine gives every skinned mesh its own asset when there are several; Combine merges them into one submesh per material.");
            }
            ImGui::SeparatorText("Scene");
            ImGui::Checkbox("Import Into Level", &m_ImportSettings.ImportIntoLevel);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Unreal's File > Import into Level: also place the whole glTF scene in the open level, keeping its node hierarchy, lights and cameras. Meshes are still imported as assets (per-mesh) and the level references them. Off: assets only, no hierarchy.");
            ImGui::SeparatorText("Transform");
            // The unit the file was authored in (glTF says meters; a model from a centimeter tool claims meters too and comes in 100x too
            // big). The scale is world units per file unit, through WorldUnits, so it stays right after the switch to centimeters.
            {
                const char* unitNames[] = { "Meters (glTF default)", "Centimeters", "Millimeters", "Inches", "Feet", "Custom scale" };
                if (ImGui::Combo("Source Unit", &m_ImportSourceUnit, unitNames, 6) && m_ImportSourceUnit < 5)
                    m_ImportSettings.ImportScale = WorldUnits::PerSourceUnit(static_cast<WorldUnits::SourceUnit>(m_ImportSourceUnit));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The unit the numbers in the file mean. Nothing in a glTF says it is not meters, so tell the importer: a model that looks 100x too big was authored in centimeters.");

                if (ImGui::DragFloat("Import Scale", &m_ImportSettings.ImportScale, 0.001f, 0.0001f, 1000.0f, "%.4f"))
                    m_ImportSourceUnit = 5;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("World units per unit of the file. Applied to the entity a drag-in creates (the model itself is untouched).");
            }

            // How big it will be, and a way to say how big it should be.
            if (m_ImportHasBounds)
            {
                const glm::vec3 fileSize = m_ImportBoundsMax - m_ImportBoundsMin;
                const glm::vec3 size = fileSize * m_ImportSettings.ImportScale;
                ImGui::Text("Size: %s x %s x %s  (W x H x D)", WorldUnits::Format(size.x).c_str(), WorldUnits::Format(size.y).c_str(), WorldUnits::Format(size.z).c_str());

                const float largest = std::max({ size.x, size.y, size.z });
                const float largestCm = WorldUnits::ToCentimeters(largest);
                if (largestCm > 10000.0f)
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "That is over 100 m: the file is probably authored in centimeters (or millimeters).");
                else if (largestCm < 2.0f)
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "That is under 2 cm: the file is probably authored in meters, or the unit is set too small.");

                ImGui::SetNextItemWidth(100.0f);
                ImGui::DragFloat("Fit to height (cm)##value", &m_ImportFitHeightCm, 1.0f, 0.0f, 100000.0f, m_ImportFitHeightCm > 0.0f ? "%.0f" : "type a height");
                ImGui::SameLine();
                if (ImGui::Button("Fit") && fileSize.y > 1e-6f && m_ImportFitHeightCm > 0.0f)
                {
                    m_ImportSettings.ImportScale = WorldUnits::FromCentimeters(m_ImportFitHeightCm) / fileSize.y;
                    m_ImportSourceUnit = 5;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Sets the scale so the model is this tall (its height in the file, Y). 180 = a person.");
            }
            else
            {
                ImGui::TextDisabled("Size: unknown (the file does not say)");
            }
            ImGui::SeparatorText("Animation");
            ImGui::Checkbox("Import Animations", &m_ImportSettings.ImportAnimations);
            if (!m_ImportSettings.ImportStaticMeshes && !m_ImportSettings.ImportSkeletalMeshes)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Nothing to import: enable static or skeletal meshes.");


            ImGui::TextUnformatted("Destination:");
            ImGui::SameLine();
            std::filesystem::path destinationPath = m_ImportDestPathBuffer;
            ImGui::TextDisabled("%s", destinationPath.parent_path().generic_string().c_str());
            ImGui::InputText("File name", m_ImportFileNameBuffer, sizeof(m_ImportFileNameBuffer));
            const bool nothingToImport = !m_ImportSettings.ImportStaticMeshes && !m_ImportSettings.ImportSkeletalMeshes;
            if (ImGui::Button("Cook & Import") && !nothingToImport)
            {
                if (!m_PendingExternalSourcePath.empty())
                {
                    std::error_code error;
                    const auto packageDirectory = m_BaseDirectory / m_PendingPackageDirectory;
                    std::filesystem::create_directories(packageDirectory, error);
                    for (const auto& dependency :
                         std::filesystem::directory_iterator(m_PendingExternalSourcePath.parent_path(), error))
                    {
                        if (error)
                            break;
                        std::filesystem::copy(
                            dependency.path(), packageDirectory / dependency.path().filename(),
                            std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                            error);
                        if (error)
                            break;
                    }
                    if (error)
                    {
                        ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                        return;
                    }
                }

                const auto finalDestination = destinationPath.parent_path() / m_ImportFileNameBuffer;
                m_Project->GetEditorAssetManager()->ImportModel(m_PendingImportPath, finalDestination, m_ImportSettings);
                m_PendingExternalSourcePath.clear();
                m_PendingPackageDirectory.clear();
                RefreshAssetTree();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                m_PendingExternalSourcePath.clear();
                m_PendingPackageDirectory.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
    }

    void ContentBrowserPanel::DrawTiles()
    {
        const float cellSize = m_ThumbnailSize + m_Padding;
        const int columnCount = std::max(static_cast<int>(ImGui::GetContentRegionAvail().x / cellSize), 1);

        const int rowCount = (static_cast<int>(m_VisibleEntries.size()) + columnCount - 1) / columnCount;
        const float rowStartX = ImGui::GetCursorPosX();
        ImGuiListClipper clipper;
        clipper.Begin(rowCount);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                for (int column = 0; column < columnCount; ++column)
                {
                    const size_t index = static_cast<size_t>(row) * columnCount + column;
                    if (index >= m_VisibleEntries.size())
                        break;
                    const BrowserEntry& browserEntry = m_VisibleEntries[index];

                    if (column > 0)
                        ImGui::SameLine(rowStartX + column * cellSize);
                    ImGui::BeginGroup();

                    if (browserEntry.IsDirectory)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        ImGui::ImageButton(browserEntry.Id.c_str(), m_DirectoryIcon->getImTextureID(),
                            {m_ThumbnailSize, m_ThumbnailSize}, {0, 1}, {1, 0});
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            m_CurrentDirectory = browserEntry.Path;
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                                RequestPrefabFromDrop(payload, browserEntry.Path);
                            ImGui::EndDragDropTarget();
                        }
                    }
                    else
                    {
                        const AssetHandle handle = browserEntry.Handle;
                        const auto& metadata = browserEntry.Metadata;

                        Ref<Texture2D> thumbnail = GetThumbnail(handle, metadata);
                        if (!thumbnail)
                            thumbnail = m_FileIcon;

                        const bool selected = m_Selected.contains(handle);
                        ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.26f, 0.45f, 0.75f, 0.55f) : ImVec4(0, 0, 0, 0));
                        // Prefabs are blue (icon and name), like prefab instances in the hierarchy.
                        const bool isPrefab = metadata.Type == AssetType::Prefab;
                        const bool clicked = ImGui::ImageButton(browserEntry.Id.c_str(), thumbnail->getImTextureID(),
                            {m_ThumbnailSize, m_ThumbnailSize}, {0, 1}, {1, 0}, ImVec4(0, 0, 0, 0),
                            isPrefab ? ImVec4(0.45f, 0.7f, 1.0f, 1.0f) : ImVec4(1, 1, 1, 1));
                        ImGui::PopStyleColor();

                        if (clicked)
                        {
                            const ImGuiIO& io = ImGui::GetIO();
                            if (io.KeyCtrl)
                            {
                                if (!m_Selected.erase(handle))
                                    m_Selected.insert(handle);
                            }
                            else if (io.KeyShift)
                            {
                                const size_t from = std::min(m_LastClickedEntry, index);
                                const size_t to = std::max(m_LastClickedEntry, index);
                                for (size_t i = from; i <= to && i < m_VisibleEntries.size(); ++i)
                                {
                                    if (!m_VisibleEntries[i].IsDirectory)
                                        m_Selected.insert(m_VisibleEntries[i].Handle);
                                }
                            }
                            else
                            {
                                m_Selected.clear();
                                m_Selected.insert(handle);
                            }
                            m_LastClickedEntry = index;
                        }

                        if (ImGui::BeginDragDropSource())
                        {
                            // Dragging something outside the selection drags just that; inside it, the whole selection.
                            if (!selected)
                            {
                                m_Selected.clear();
                                m_Selected.insert(handle);
                            }
                            if (m_Selected.size() > 1)
                            {
                                const std::vector<AssetHandle> handles(m_Selected.begin(), m_Selected.end());
                                ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEMS", handles.data(), handles.size() * sizeof(AssetHandle));
                                ImGui::Text("%zu assets", handles.size());
                            }
                            else
                            {
                                ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", &handle, sizeof(AssetHandle));
                                ImGui::TextUnformatted(browserEntry.Name.c_str());
                            }
                            ImGui::EndDragDropSource();
                        }

                        if (metadata.Type == AssetType::Prefab && ImGui::BeginPopupContextItem())
                        {
                            if (ImGui::MenuItem("Create Variant"))
                            {
                                m_HasVariantRequest = true;
                                m_VariantBase = handle;
                            }
                            ImGui::EndPopup();
                        }

                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            if (metadata.Type == AssetType::MeshSource)
                            {
                                m_PendingImportPath = metadata.FilePath;
                                m_ShowImportModal = true;
                                ReadImportFileInfo(Project::GetActiveAssetDirectory() / metadata.FilePath);
                                const auto defaultDest = m_PendingImportPath.parent_path() / "Meshes" /
                                    (m_PendingImportPath.stem().string() + ".nmesh");
                                SetImportDestination(defaultDest);
                            }
                            else if (m_OpenAsset)
                            {
                                m_OpenAsset(handle); // EditorLayer ignores types it has no editor window for
                            }
                        }
                    }

                    // One line, cut at the thumbnail's width; the full name is in the tooltip.
                    if (!browserEntry.IsDirectory && browserEntry.Handle != 0 && browserEntry.Handle == m_RenameHandle)
                    {
                        ImGui::SetNextItemWidth(m_ThumbnailSize);
                        if (m_RenameFocus)
                        {
                            ImGui::SetKeyboardFocusHere();
                            ImGui::SetScrollHereY(0.5f);
                            m_RenameFocus = false;
                        }
                        const bool entered = ImGui::InputText("##rename", m_RenameBuffer, sizeof(m_RenameBuffer),
                            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
                        if (entered || ImGui::IsItemDeactivated())
                        {
                            m_RenameCommit = true;
                            m_RenameCommitText = m_RenameBuffer;
                        }
                    }
                    else
                    {
                        const bool bluePrefabName = !browserEntry.IsDirectory && browserEntry.Metadata.Type == AssetType::Prefab;
                        if (bluePrefabName)
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.7f, 1.0f, 1.0f));
                        const ImVec2 nameMin = ImGui::GetCursorScreenPos();
                        ImGui::PushClipRect(nameMin, ImVec2(nameMin.x + m_ThumbnailSize, nameMin.y + ImGui::GetTextLineHeight()), true);
                        ImGui::TextUnformatted(browserEntry.Name.c_str());
                        ImGui::PopClipRect();
                        if (bluePrefabName)
                            ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", browserEntry.Name.c_str());
                    }

                    ImGui::EndGroup();
                }
            }
        }
        clipper.End();

        // The free space below the tiles takes entities dragged from the hierarchy (a new prefab in this folder). An item is
        // needed for the drop target: BeginDragDropTarget binds to the last item.
        {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            ImGui::InvisibleButton("##content_browser_blank", ImVec2(std::max(available.x, 1.0f), std::max(available.y, 24.0f)));
            if (ImGui::IsItemClicked())
                m_Selected.clear();
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                    RequestPrefabFromDrop(payload, m_CurrentDirectory);
                ImGui::EndDragDropTarget();
            }
        }

    }

    namespace
    {
        constexpr const char* kTypeFilterNames[] = { "All types", "Meshes", "Materials", "Textures", "Prefabs", "Scenes", "Animation" };

        std::string ToLower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return lower;
        }

        // `path` is `folder` or somewhere below it.
        bool IsInside(const std::filesystem::path& path, const std::filesystem::path& folder)
        {
            const std::filesystem::path relative = path.lexically_relative(folder);
            return !relative.empty() && *relative.begin() != "..";
        }
    }

    void ContentBrowserPanel::DrawToolbar()
    {
        if (ImGui::Button("+ Add"))
            ImGui::OpenPopup("##content_add");
        if (ImGui::BeginPopup("##content_add"))
        {
            if (ImGui::MenuItem("C# Script"))
                m_ShowCreateScriptModal = true;
            if (ImGui::MenuItem("Animation Graph"))
                m_ShowCreateGraphModal = true;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Import"))
        {
            static constexpr char filter[] = "Scene and glTF files\0nox;gltf;glb";
            const std::string selectedFile = Utility::OpenFile(filter);
            if (!selectedFile.empty())
            {
                const bool wasHovered = m_WindowHovered;
                m_WindowHovered = true;
                OnExternalFileDrop(selectedFile);
                m_WindowHovered = wasHovered;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_CurrentDirectory == m_BaseDirectory);
        if (ImGui::ArrowButton("##parent_folder", ImGuiDir_Up))
            m_CurrentDirectory = m_CurrentDirectory.parent_path();
        ImGui::EndDisabled();
        ImGui::SameLine();
        DrawBreadcrumb();

        // Search, type filter and tile size on the right; on the next line when the window is too narrow.
        const ImGuiStyle& style = ImGui::GetStyle();
        constexpr float searchWidth = 180.0f;
        constexpr float typeWidth = 110.0f;
        constexpr float sizeWidth = 90.0f;
        const float rightWidth = searchWidth + typeWidth + sizeWidth + 2.0f * style.ItemSpacing.x;
        ImGui::SameLine();
        const float available = ImGui::GetContentRegionAvail().x;
        if (available >= rightWidth)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - rightWidth);
        else
            ImGui::NewLine();
        ImGui::SetNextItemWidth(searchWidth);
        ImGui::InputTextWithHint("##search", "Search (this folder and below)", m_SearchBuffer, sizeof(m_SearchBuffer));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(typeWidth);
        ImGui::Combo("##type_filter", &m_TypeFilter, kTypeFilterNames, IM_ARRAYSIZE(kTypeFilterNames));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sizeWidth);
        ImGui::SliderFloat("##tile_size", &m_ThumbnailSize, 48.0f, 256.0f, "%.0f px");
        ImGui::Separator();
    }

    void ContentBrowserPanel::DrawBreadcrumb()
    {
        // Assets > Models > Fox: every part opens its folder and takes entities dragged from the hierarchy (a prefab there).
        std::filesystem::path target;
        auto crumb = [&](const std::string& label, const std::filesystem::path& folder, size_t index)
        {
            if (index > 0)
            {
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled(">");
                ImGui::SameLine(0.0f, 2.0f);
            }
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::SmallButton(label.c_str()))
                target = folder;
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                    RequestPrefabFromDrop(payload, folder);
                ImGui::EndDragDropTarget();
            }
            ImGui::PopID();
        };

        crumb("Assets", m_BaseDirectory, 0);
        std::filesystem::path folder = m_BaseDirectory;
        size_t index = 1;
        for (const std::filesystem::path& part : m_CurrentDirectory.lexically_relative(m_BaseDirectory))
        {
            if (part.empty() || part == ".")
                continue;
            folder /= part;
            crumb(part.string(), folder, index++);
        }
        if (!target.empty())
            m_CurrentDirectory = target;
    }

    void ContentBrowserPanel::RebuildFolderTree()
    {
        m_FolderTreeDirty = false;
        m_FolderTreeTime = ImGui::GetTime();

        std::function<void(FolderNode&)> fill = [&](FolderNode& node)
        {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(node.Path, error))
            {
                std::error_code typeError;
                const std::string name = entry.path().filename().string();
                if (!entry.is_directory(typeError) || name.empty() || name.front() == '.')
                    continue;
                FolderNode child{ entry.path(), name, {} };
                fill(child);
                node.Children.push_back(std::move(child));
            }
            std::sort(node.Children.begin(), node.Children.end(),
                      [](const FolderNode& a, const FolderNode& b) { return ToLower(a.Name) < ToLower(b.Name); });
        };

        m_FolderTree = FolderNode{ m_BaseDirectory, "Assets", {} };
        fill(m_FolderTree);
    }

    void ContentBrowserPanel::DrawFolderTree(const FolderNode& node)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.Children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (node.Path == m_CurrentDirectory)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (node.Path == m_BaseDirectory)
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        // Opening a folder elsewhere (a tile, the path above) unfolds the tree down to it once; it can be folded again after.
        if (m_TreeRevealedDirectory != m_CurrentDirectory && node.Path != m_CurrentDirectory && IsInside(m_CurrentDirectory, node.Path))
            ImGui::SetNextItemOpen(true);

        ImGui::PushID(node.Path.generic_string().c_str());
        const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", node.Name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            m_CurrentDirectory = node.Path;
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
                RequestPrefabFromDrop(payload, node.Path);
            ImGui::EndDragDropTarget();
        }
        if (open)
        {
            for (const FolderNode& child : node.Children)
                DrawFolderTree(child);
            ImGui::TreePop();
        }
        ImGui::PopID();

        if (node.Path == m_BaseDirectory)
            m_TreeRevealedDirectory = m_CurrentDirectory;
    }

    bool ContentBrowserPanel::PassesTypeFilter(AssetType type) const
    {
        switch (m_TypeFilter)
        {
        case 1: return type == AssetType::Mesh || type == AssetType::StaticMesh || type == AssetType::MeshSource || type == AssetType::SkeletalMesh;
        case 2: return type == AssetType::Material;
        case 3: return type == AssetType::Texture2D;
        case 4: return type == AssetType::Prefab;
        case 5: return type == AssetType::Scene;
        case 6: return type == AssetType::AnimationSequence || type == AssetType::Skeleton || type == AssetType::AnimationGraph;
        default: return true;
        }
    }

    void ContentBrowserPanel::UpdateVisibleEntries()
    {
        const std::string search = ToLower(m_SearchBuffer);
        std::string key = search;
        key += '\x1f';
        key += std::to_string(m_TypeFilter);
        key += '\x1f';
        key += std::to_string(m_EntriesVersion);
        key += '\x1f';
        key += m_CurrentDirectory.generic_string();
        if (key == m_VisibleKey)
            return;
        m_VisibleKey = std::move(key);
        m_VisibleEntries.clear();

        if (search.empty())
        {
            // Folders stay visible with a type filter (so the filter can be used while browsing), assets must match it.
            for (const BrowserEntry& entry : m_CurrentEntries)
            {
                if (entry.IsDirectory || PassesTypeFilter(entry.Metadata.Type))
                    m_VisibleEntries.push_back(entry);
            }
            return;
        }

        // Like UE's Content Browser: every registered asset in this folder and below whose name contains the text.
        const std::filesystem::path folder = CurrentFolder();
        const std::string folderPrefix = folder.empty() ? std::string() : folder.generic_string() + "/";
        for (const auto& [handle, metadata] : m_Project->GetEditorAssetManager()->GetAssetRegistry())
        {
            if (!PassesTypeFilter(metadata.Type))
                continue;
            const std::string relative = metadata.FilePath.lexically_normal().generic_string();
            if (!folderPrefix.empty() && relative.rfind(folderPrefix, 0) != 0)
                continue;
            const std::string name = metadata.FilePath.filename().string();
            if (ToLower(name).find(search) == std::string::npos)
                continue;
            std::error_code error;
            const std::filesystem::path path = m_BaseDirectory / metadata.FilePath;
            if (!std::filesystem::exists(path, error)) // registered but not cooked yet (or deleted)
                continue;
            m_VisibleEntries.push_back({ path, metadata, handle, false, "##asset_" + relative, name });
        }
        std::sort(m_VisibleEntries.begin(), m_VisibleEntries.end(),
                  [](const BrowserEntry& a, const BrowserEntry& b) { return ToLower(a.Name) < ToLower(b.Name); });
    }

    void ContentBrowserPanel::RefreshAssetTree()
    {
        m_ThumbnailCache = CreateRef<ThumbnailCache>(m_Project);
        m_CurrentEntries.clear();
        m_Selected.clear();

        // One lookup table for the whole folder: matching every file against the whole registry (two path normalizations
        // per entry) took seconds for a folder of hundreds of meshes.
        std::unordered_map<std::string, AssetHandle> handleOfPath;
        for (const auto& [handle, metadata] : m_Project->GetEditorAssetManager()->GetAssetRegistry())
        {
            handleOfPath.emplace(metadata.FilePath.lexically_normal().generic_string(), handle);
            if (!metadata.SourceFilePath.empty())
                handleOfPath.emplace(metadata.SourceFilePath.lexically_normal().generic_string(), handle);
        }

        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(m_CurrentDirectory, error))
        {
            const auto path = entry.path();
            const auto relativePath = path.lexically_relative(m_BaseDirectory);
            if (entry.is_directory())
            {
                m_CurrentEntries.push_back({path, {}, 0, true, "##directory_" + relativePath.generic_string(), path.filename().string()});
                continue;
            }

            const auto foundHandle = handleOfPath.find(relativePath.lexically_normal().generic_string());
            const AssetHandle handle = foundHandle != handleOfPath.end() ? foundHandle->second : AssetHandle(0);
            if (handle == 0)
                continue;

            const AssetMetadata metadata = m_Project->GetEditorAssetManager()->GetMetadata(handle);
            // Source and cooked files resolve to the same logical asset. Show only
            // the canonical registered path so Fox.gltf/Fox.nmesh and
            // Texture.png/Texture.ntex do not appear twice.
            if (metadata.FilePath.lexically_normal() != relativePath.lexically_normal())
                continue;
            m_CurrentEntries.push_back({path, metadata, handle, false, "##asset_" + relativePath.generic_string(), metadata.FilePath.filename().string()});
        }
        m_EntriesDirectory = m_CurrentDirectory;
        ++m_EntriesVersion;
        m_FolderTreeDirty = true;
    }
}
