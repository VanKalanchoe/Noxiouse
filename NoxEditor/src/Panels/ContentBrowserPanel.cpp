#include "ContentBrowserPanel.h"

#include <algorithm>
#include <cstring>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/TextureImporter.h"
#include "NoxCore/Utils/Utils.h"

namespace Nox
{
    ContentBrowserPanel::ContentBrowserPanel(Ref<Project> project)
        : m_Project(std::move(project)), m_ThumbnailCache(CreateRef<ThumbnailCache>(m_Project)),
          m_BaseDirectory(m_Project->GetAssetDirectory()), m_CurrentDirectory(m_BaseDirectory)
    {
        m_DirectoryIcon = TextureImporter::LoadTexture2D("assets/Icons/DirectoryIcon.ktx2", {.generateMips = false});
        m_FileIcon = TextureImporter::LoadTexture2D("assets/Icons/FileIcon.ktx2", {.generateMips = false});
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
        m_ImportAsStaticMesh = false;
        const auto defaultDest = relativeSource.parent_path() / "Meshes" /
            (relativeSource.stem().string() + ".nmesh");
        SetImportDestination(defaultDest);
        m_ShowImportModal = true;
    }

    void ContentBrowserPanel::OnImGuiRender()
    {
        ImGui::Begin("Content Browser");
        m_WindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

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
        if (m_CurrentDirectory != m_BaseDirectory)
        {
            if (ImGui::Button("<"))
                m_CurrentDirectory = m_CurrentDirectory.parent_path();
            ImGui::SameLine();
        }
        const std::string breadcrumb = std::filesystem::relative(m_CurrentDirectory, m_BaseDirectory).generic_string();
        ImGui::TextUnformatted(breadcrumb.empty() ? "Assets" : breadcrumb.c_str());
        ImGui::Separator();

        static float padding = 16.0f;
        static float thumbnailSize = 96.0f;
        const float cellSize = thumbnailSize + padding;
        int columnCount = static_cast<int>(ImGui::GetContentRegionAvail().x / cellSize);
        columnCount = std::max(columnCount, 1);
        ImGui::Columns(columnCount, nullptr, false);

        if (m_EntriesDirectory != m_CurrentDirectory)
            RefreshAssetTree();

        for (const auto& browserEntry : m_CurrentEntries)
        {
            const auto& path = browserEntry.Path;
            const auto relativePath = path.lexically_relative(m_BaseDirectory);

            if (browserEntry.IsDirectory)
            {
                const std::string id = "##directory_" + relativePath.generic_string();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::ImageButton(id.c_str(), m_DirectoryIcon->getImTextureID(),
                    {thumbnailSize, thumbnailSize}, {0, 1}, {1, 0});
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    m_CurrentDirectory = path;
                ImGui::TextWrapped("%s", path.filename().string().c_str());
                ImGui::NextColumn();
                continue;
            }

            const AssetHandle handle = browserEntry.Handle;
            const auto& metadata = browserEntry.Metadata;

            Ref<Texture2D> thumbnail = GetThumbnail(handle, metadata);
            if (!thumbnail)
                thumbnail = m_FileIcon;

            const std::string id = "##asset_" + relativePath.generic_string();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::ImageButton(id.c_str(), thumbnail->getImTextureID(),
                {thumbnailSize, thumbnailSize}, {0, 1}, {1, 0});
            ImGui::PopStyleColor();

            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", &handle, sizeof(AssetHandle));
                ImGui::TextUnformatted(metadata.FilePath.filename().string().c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                metadata.Type == AssetType::MeshSource)
            {
                m_PendingImportPath = metadata.FilePath;
                m_ShowImportModal = true;
                const auto defaultDest = m_PendingImportPath.parent_path() / "Meshes" /
                    (m_PendingImportPath.stem().string() + (m_ImportAsStaticMesh ? ".nsmesh" : ".nmesh"));
                SetImportDestination(defaultDest);
            }
            ImGui::TextWrapped("%s", metadata.FilePath.filename().string().c_str());
            ImGui::NextColumn();
        }

        ImGui::Columns(1);
        ImGui::SliderFloat("Thumbnail Size", &thumbnailSize, 48.0f, 256.0f);
        ImGui::SliderFloat("Padding", &padding, 0.0f, 32.0f);

        if (m_ShowImportModal)
        {
            ImGui::OpenPopup("Import Settings");
            m_ShowImportModal = false;
        }
        if (ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Importing: %s", m_PendingImportPath.filename().string().c_str());
            if (ImGui::Checkbox("Import as Static Mesh (.nsmesh)", &m_ImportAsStaticMesh))
            {
                std::filesystem::path destination = m_ImportDestPathBuffer;
                destination.replace_extension(m_ImportAsStaticMesh ? ".nsmesh" : ".nmesh");
                SetImportDestination(destination);
            }

            ImGui::TextUnformatted("Destination:");
            ImGui::SameLine();
            std::filesystem::path destinationPath = m_ImportDestPathBuffer;
            ImGui::TextDisabled("%s", destinationPath.parent_path().generic_string().c_str());
            ImGui::InputText("File name", m_ImportFileNameBuffer, sizeof(m_ImportFileNameBuffer));
            if (ImGui::Button("Cook & Import"))
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

                const AssetType type = m_ImportAsStaticMesh ? AssetType::StaticMesh : AssetType::Mesh;
                const auto finalDestination = destinationPath.parent_path() / m_ImportFileNameBuffer;
                m_Project->GetEditorAssetManager()->ImportAsset(m_PendingImportPath, finalDestination, type);
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

    void ContentBrowserPanel::RefreshAssetTree()
    {
        m_ThumbnailCache = CreateRef<ThumbnailCache>(m_Project);
        m_CurrentEntries.clear();

        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(m_CurrentDirectory, error))
        {
            const auto path = entry.path();
            const auto relativePath = path.lexically_relative(m_BaseDirectory);
            if (entry.is_directory())
            {
                m_CurrentEntries.push_back({path, {}, 0, true});
                continue;
            }

            const AssetHandle handle = FindAssetHandle(relativePath);
            if (handle == 0)
                continue;

            const AssetMetadata metadata = m_Project->GetEditorAssetManager()->GetMetadata(handle);
            // Source and cooked files resolve to the same logical asset. Show only
            // the canonical registered path so Fox.gltf/Fox.nmesh and
            // Texture.png/Texture.ntex do not appear twice.
            if (metadata.FilePath.lexically_normal() != relativePath.lexically_normal())
                continue;
            m_CurrentEntries.push_back({path, metadata, handle, false});
        }
        m_EntriesDirectory = m_CurrentDirectory;
    }
}
