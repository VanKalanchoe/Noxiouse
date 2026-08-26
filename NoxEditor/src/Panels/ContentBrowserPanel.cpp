#include "ContentBrowserPanel.h"
#include "NoxCore/Asset/TextureImporter.h"

namespace Nox
{
    ContentBrowserPanel::ContentBrowserPanel(Ref<Project> project)
        : m_Project(project), m_ThumbnailCache(CreateRef<ThumbnailCache>(project)), m_BaseDirectory(m_Project->GetAssetDirectory()), m_CurrentDirectory(m_BaseDirectory)
    {
        m_TreeNodes.push_back(TreeNode(".", 0));
 	
        m_DirectoryIcon = TextureImporter::LoadTexture2D("assets/Icons/DirectoryIcon.ktx2", {  .generateMips = false });
        m_FileIcon = TextureImporter::LoadTexture2D("assets/Icons/FileIcon.ktx2", { .generateMips = false });

        RefreshAssetTree();

        m_Mode = Mode::FileSystem;
    }
    
    void ContentBrowserPanel::OnImGuiRender()
	{
		ImGui::Begin("Content Browser");

 		const char* label = m_Mode == Mode::Asset ? "Asset" : "File";
 		if (ImGui::Button(label))
 		{
 			m_Mode = m_Mode == Mode::Asset ? Mode::FileSystem : Mode::Asset;
 		}

		if (m_CurrentDirectory != std::filesystem::path(m_BaseDirectory))
		{
			
			if (ImGui::Button("<-"))
			{
				m_CurrentDirectory = m_CurrentDirectory.parent_path();
			}
		}

		static float padding = 16.0f;
		static float thumbnailSize = 128.0f;
		float cellSize = thumbnailSize + padding;

		float panelWidth = ImGui::GetContentRegionAvail().x;
		int columnCount = (int)(panelWidth / cellSize);
		if (columnCount < 1)
			columnCount = 1;

		ImGui::Columns(columnCount, 0, false);

		if (m_Mode == Mode::Asset)
		{
			TreeNode* node = &m_TreeNodes[0];

			auto currentDir = std::filesystem::relative(m_CurrentDirectory, Project::GetActiveAssetDirectory());
			for (const auto& p : currentDir)
			{
				// if only one level
				if (node->Path == currentDir)
					break;
				
				if (node->Children.find(p) != node->Children.end())
				{
					node = &m_TreeNodes[node->Children[p]];
					continue;
				} else
				{
					// cant find path
					NOX_CORE_ASSERT(false);
				}
			}

			for (const auto& [item, treeNodeIndex] : node->Children)
			{
				bool isDirectory = std::filesystem::is_directory(Project::GetActiveAssetDirectory() / item);
				std::string itemStr = item.generic_string();
				
				Ref<Texture2D> icon = isDirectory ? m_DirectoryIcon : m_FileIcon;
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				ImGui::ImageButton(itemStr.c_str(), (ImTextureID)icon->getImTextureID(), { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 });

				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem("Delete"))
					{
						NOX_CORE_ASSERT(false, "Not implemented")
					}
					ImGui::EndPopup();
				}

				if (ImGui::BeginDragDropSource())
				{
					AssetHandle handle = m_TreeNodes[treeNodeIndex].Handle;
					ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", &handle, sizeof(AssetHandle));
					ImGui::Text("%s", itemStr.c_str());
					ImGui::EndDragDropSource();
				}

				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					if (isDirectory)
						m_CurrentDirectory /= item.filename();
				}
				ImGui::TextWrapped(itemStr.c_str());

				ImGui::NextColumn();
			}
		}
 		else
		{
			for (auto& directoryEntry : std::filesystem::directory_iterator(m_CurrentDirectory))
			{
				const auto& path = directoryEntry.path();
				std::string filenameString = path.filename().string();

				// THUMBNAIL
				auto relativePath = std::filesystem::relative(path, Project::GetActiveAssetDirectory());
				Ref<Texture2D> thumbnail = m_DirectoryIcon;
				if (!directoryEntry.is_directory())
				{
					thumbnail = m_ThumbnailCache->GetOrCreateThumbnail(relativePath);
					if (!thumbnail)
						thumbnail = m_FileIcon;
				}
				
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				ImGui::ImageButton(filenameString.c_str(),(ImTextureID)thumbnail->getImTextureID(), { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 });

				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem("Import"))
					{
						AssetType type = Project::GetActive()->GetEditorAssetManager()->GetAssetTypeFromExtension(path.extension());
						if (type == AssetType::MeshSource)
						{
							m_PendingImportPath = relativePath;
							m_ShowImportModal = true;
						
							std::filesystem::path defaultDest = m_PendingImportPath;
							defaultDest.replace_extension(m_ImportAsStaticMesh ? ".nsmesh" : ".nmesh");
							strncpy(m_ImportDestPathBuffer, defaultDest.string().c_str(), sizeof(m_ImportDestPathBuffer));
						}
						else
						{
							Project::GetActive()->GetEditorAssetManager()->ImportAsset(relativePath, {}, {});
							RefreshAssetTree();
						}
					}
					ImGui::EndPopup();
				}
				
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					if (directoryEntry.is_directory())
						m_CurrentDirectory /= path.filename();
				}
				ImGui::TextWrapped(filenameString.c_str());

				ImGui::NextColumn();
			}
 			
 			if (m_ShowImportModal)
 			{
 				ImGui::OpenPopup("Import Settings");
 				m_ShowImportModal = false; // Reset trigger so it doesn't loop
 			}
 			
 			if (ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
 			{
 				ImGui::Text("Importing: %s", m_PendingImportPath.filename().string().c_str());
 				ImGui::Separator();

 				// 3. The Checkbox
 				if (ImGui::Checkbox("Import as Static Mesh (.nsmesh)", &m_ImportAsStaticMesh))
 				{
 					std::filesystem::path currentDest = m_ImportDestPathBuffer;
 					currentDest.replace_extension(m_ImportAsStaticMesh ? ".nsmesh" : ".nmesh");
 					strncpy(m_ImportDestPathBuffer, currentDest.string().c_str(), sizeof(m_ImportDestPathBuffer));
 				}

 				ImGui::Text("Destination Path:");
 				ImGui::InputText("##dest", m_ImportDestPathBuffer, sizeof(m_ImportDestPathBuffer));
 				
 				ImGui::Separator();
    
 				if (ImGui::Button("Cook & Import", ImVec2(120, 0)))
 				{
 					// 4. NOW we call the Asset Manager, passing in the user's choice!
 					AssetType targetType = m_ImportAsStaticMesh ? AssetType::StaticMesh : AssetType::Mesh;
 					std::filesystem::path destPath = m_ImportDestPathBuffer;
 					
 					Project::GetActive()->GetEditorAssetManager()->ImportAsset(m_PendingImportPath, destPath, targetType);
        
 					// Refresh the UI
 					RefreshAssetTree(); 
 					ImGui::CloseCurrentPopup();
 				}
    
 				ImGui::SameLine();
    
 				if (ImGui::Button("Cancel", ImVec2(120, 0)))
 				{
 					ImGui::CloseCurrentPopup();
 				}
    
 				ImGui::EndPopup();
 			}
		}

		ImGui::Columns(1);

		ImGui::SliderFloat("Thumbnail Size", &thumbnailSize, 16, 512);
		ImGui::SliderFloat("Padding", &padding, 0, 32);

		// TODO: status bar
		ImGui::End();
	}

	void ContentBrowserPanel::RefreshAssetTree()
	{
 		const auto& assetRegistry = Project::GetActive()->GetEditorAssetManager()->GetAssetRegistry();
 		for (const auto& [handle, metadata] : assetRegistry)
 		{
 			uint32_t currentNodeIndex = 0;
 			
 			for (const auto& p : metadata.FilePath)
 			{
 				auto it = m_TreeNodes[currentNodeIndex].Children.find(p.generic_string());
 				if (it != m_TreeNodes[currentNodeIndex].Children.end())
 				{
 					currentNodeIndex = it->second;
 				}
 				else
 				{
 					// add node
 					TreeNode newNode(p, handle);
 					newNode.Parent = currentNodeIndex;
 					m_TreeNodes.push_back(newNode);
 					
 					m_TreeNodes[currentNodeIndex].Children[p] =  m_TreeNodes.size() - 1;
 					currentNodeIndex = m_TreeNodes.size() - 1;
 				}
 			}
 		}
	}
}
