#include "MaterialEditorPanel.h"

#include <filesystem>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include "NoxCore/Asset/AssetManager.h"
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/Material.h"
#include "NoxCore/Asset/MaterialSerializer.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/Renderer.h"
#include "AssetPreviewRenderer.h"

namespace Nox
{
    MaterialEditorPanel::MaterialEditorPanel(AssetHandle material)
        : m_Material(material)
    {
        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(material);
        // "###" keeps the window's identity (docking, position) tied to the asset, not to its name.
        m_Title = "Material: " + metadata.FilePath.stem().string() + "###MaterialEditor" + std::to_string(static_cast<uint64_t>(material));
    }

    void MaterialEditorPanel::OnImGuiRender()
    {
        if (!m_Open)
            return;

        ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(m_Title.c_str(), &m_Open))
        {
            ImGui::End();
            return;
        }

        if (!AssetManager::IsAssetHandleValid(m_Material) || AssetManager::GetAssetType(m_Material) != AssetType::Material)
        {
            ImGui::TextDisabled("This material is no longer in the project.");
            ImGui::End();
            return;
        }

        Ref<Material> material = AssetManager::GetAsset<Material>(m_Material);
        if (!material)
        {
            ImGui::TextDisabled("Loading...");
            ImGui::End();
            return;
        }

        MaterialData& data = material->GetData();
        const AssetMetadata metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(m_Material);
        ImGui::TextDisabled("%s", metadata.FilePath.generic_string().c_str());

        // The material on a sphere, drawn every frame this window shows (AssetPreviewRenderer), so edits show at once. A swatch of
        // its color until the first image is there.
        constexpr float PreviewSize = 160.0f;
        if (Texture2D* preview = AssetPreviewRenderer::Get().GetLivePreview(m_Material, static_cast<uint32_t>(PreviewSize)))
            ImGui::Image(preview->getImTextureID(), ImVec2(PreviewSize, PreviewSize));
        else
        {
            const glm::vec4 swatch = data.Workflow == 1.0f ? data.DiffuseFactor : data.BaseColorFactor;
            ImGui::ColorButton("##swatch", ImVec4(swatch.r, swatch.g, swatch.b, 1.0f),
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(PreviewSize, PreviewSize));
        }
        ImGui::Separator();

        DrawProperties(m_Material, data);
        ImGui::End();
    }

    void MaterialEditorPanel::DrawProperties(AssetHandle handle, MaterialData& data)
    {
        bool changed = false;
        // The renderer shades with the factors of the material's workflow only (PackMaterial):
        // specular-glossiness materials (e.g. Bistro) use Diffuse/Specular/Glossiness.
        const bool specularGlossiness = data.Workflow == 1.0f;
        if (specularGlossiness)
        {
            ImGui::TextDisabled("Workflow: Specular-Glossiness");
            changed |= ImGui::ColorEdit4("Diffuse", glm::value_ptr(data.DiffuseFactor));
            changed |= ImGui::ColorEdit3("Specular", glm::value_ptr(data.SpecularFactor));
            changed |= ImGui::DragFloat("Glossiness", &data.SpecularFactor.a, 0.01f, 0.0f, 1.0f);
        }
        else
        {
            ImGui::TextDisabled("Workflow: Metallic-Roughness");
            changed |= ImGui::ColorEdit4("Base Color", glm::value_ptr(data.BaseColorFactor));
            changed |= ImGui::DragFloat("Metallic", &data.MetallicFactor, 0.01f, 0.0f, 1.0f);
            changed |= ImGui::DragFloat("Roughness", &data.RoughnessFactor, 0.01f, 0.0f, 1.0f);
        }
        changed |= ImGui::ColorEdit3("Emissive", glm::value_ptr(data.EmissiveFactor));
        changed |= ImGui::DragFloat("Emissive Strength", &data.emissiveStrength, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::DragFloat("Transmission", &data.TransmissionFactor, 0.01f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("IOR", &data.IOR, 0.01f, 1.0f, 3.0f);
        changed |= ImGui::DragFloat("Thickness", &data.Thickness, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::ColorEdit3("Attenuation Color", glm::value_ptr(data.AttenuationColor));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Volume absorption (KHR_materials_volume): the color light keeps after travelling Attenuation Distance\n"
                              "through the material -- thick glass tinted by its depth. Needs Transmission and a Thickness.");
        changed |= ImGui::DragFloat("Attenuation Distance", &data.AttenuationDistance, 0.01f, 0.0f, 100000.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World units (scaled with the model). 0: no absorption.");

        auto drawTextureReference = [&](const char* labelName,
                                        const char* id,
                                        std::string& texturePath)
        {
            std::string label = texturePath.empty()
                                    ? "None"
                                    : std::filesystem::path(texturePath).filename().string();
            ImGui::Text("%s", labelName);
            ImGui::SameLine();
            ImGui::Button((label + "##" + id).c_str(), ImVec2(150.0f, 0.0f));

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    AssetHandle textureHandle = *(const AssetHandle*)payload->Data;
                    if (AssetManager::GetAssetType(textureHandle) == AssetType::Texture2D)
                    {
                        const auto& textureMetadata =
                            Project::GetActive()->GetEditorAssetManager()->GetMetadata(textureHandle);
                        texturePath = textureMetadata.SourceFilePath.empty()
                                          ? textureMetadata.FilePath.generic_string()
                                          : textureMetadata.SourceFilePath.generic_string();
                        changed = true;
                    }
                    else
                    {
                        NOX_CORE_WARN("Wrong Asset Type - Expected a Texture (this slot takes a texture, not a material)");
                    }
                }
                ImGui::EndDragDropTarget();
            }
        };

        drawTextureReference(specularGlossiness ? "Diffuse Texture" : "Base Color Texture", "BaseColor", data.BaseColorTexturePath);
        drawTextureReference(specularGlossiness ? "Specular Glossiness" : "Metallic Roughness", "MetallicRoughness", data.MetallicRoughnessTexturePath);
        drawTextureReference("Normal Texture", "Normal", data.NormalTexturePath);
        drawTextureReference("Occlusion Texture", "Occlusion", data.OcclusionTexturePath);
        drawTextureReference("Emissive Texture", "Emissive", data.EmissiveTexturePath);
        drawTextureReference("Transmission Texture", "Transmission", data.TransmissionTexturePath);

        int alphaMode = static_cast<int>(data.Mode);
        const char* alphaModes[] = {"Opaque", "Mask", "Blend"};
        if (ImGui::Combo("Alpha Mode", &alphaMode, alphaModes, 3))
        {
            data.Mode = static_cast<AlphaMode>(alphaMode);
            changed = true;
        }
        if (data.Mode == AlphaMode::Mask)
            changed |= ImGui::DragFloat("Alpha Cutoff", &data.AlphaMaskCutoff, 0.005f, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Double Sided", &data.DoubleSided);
        changed |= ImGui::Checkbox("Unlit", &data.Unlit);

        if (changed)
        {
            // Every instance using this material shades with the edit from this frame; its Content Browser thumbnail is drawn again.
            Renderer::MarkMaterialChanged(handle);
            AssetPreviewRenderer::Get().Invalidate(handle);
            const auto& metadata = Project::GetActive()->GetEditorAssetManager()->GetMetadata(handle);
            MaterialSerializer::Serialize(
                Project::GetActiveAssetDirectory() / metadata.FilePath,
                data
            );
        }
    }
}
