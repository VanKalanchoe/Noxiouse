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
        changed |= ImGui::SliderFloat("Diffuse Transmission", &data.DiffuseTransmission, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Light through a thin surface to its other side (leaves, cloth, paper, lampshades). Make it double sided.");
        changed |= ImGui::DragFloat("Depth Bias", &data.DepthBias, 0.00001f, 0.0f, 1.0f, "%.5f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drawn this much closer to the camera (world units), where it lands on screen unchanged: for geometry\n"
                              "modelled exactly onto another surface -- decals, trims, overlay layers lying in its plane -- that\n"
                              "would otherwise z-fight with it. Keep it tiny, just past the 32-bit depth buffer's resolution\n"
                              "(about 1 um per 16 m of distance; 0.02 mm = 0.00002 m covers it): more lets it jump over real\n"
                              "geometry that sits a fraction of a millimetre in front of it.\n"
                              "It moves the whole material, so it cannot order two layers of the same material: those resolve\n"
                              "by draw order (the later triangle in the mesh wins an exact tie).\n"
                              "0: none.");

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
        changed |= ImGui::DragFloat("Normal Strength", &data.NormalScale, 0.01f, 0.0f, 4.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How strongly the normal map bends the surface (glTF's normal texture scale): 1 as authored, 0 flat.");
        drawTextureReference("Occlusion Texture", "Occlusion", data.OcclusionTexturePath);
        changed |= ImGui::SliderFloat("Occlusion Strength", &data.OcclusionStrength, 0.0f, 1.0f);
        drawTextureReference("Emissive Texture", "Emissive", data.EmissiveTexturePath);
        drawTextureReference("Transmission Texture", "Transmission", data.TransmissionTexturePath);

        // The lobes past the core model (the glTF material extensions): a factor of 0 turns a lobe off, a texture scales its factor.
        if (!specularGlossiness && ImGui::CollapsingHeader("Layers"))
        {
            ImGui::SeparatorText("Clear Coat");
            changed |= ImGui::SliderFloat("Clear Coat", &data.ClearcoatFactor, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A smooth varnish layer over the whole material (car paint, lacquered wood).");
            changed |= ImGui::SliderFloat("Clear Coat Roughness", &data.ClearcoatRoughnessFactor, 0.0f, 1.0f);
            drawTextureReference("Clear Coat Texture (R)", "Clearcoat", data.ClearcoatTexture.Path);
            drawTextureReference("Clear Coat Roughness (G)", "ClearcoatRoughness", data.ClearcoatRoughnessTexture.Path);
            drawTextureReference("Clear Coat Normal", "ClearcoatNormal", data.ClearcoatNormalTexture.Path);
            changed |= ImGui::DragFloat("Clear Coat Normal Strength", &data.ClearcoatNormalScale, 0.01f, 0.0f, 4.0f);

            ImGui::SeparatorText("Sheen");
            changed |= ImGui::ColorEdit3("Sheen Color", glm::value_ptr(data.SheenColorFactor));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The glow of fibers at grazing angles (velvet, cloth). Black: no sheen.");
            changed |= ImGui::SliderFloat("Sheen Roughness", &data.SheenRoughnessFactor, 0.0f, 1.0f);
            drawTextureReference("Sheen Color Texture (RGB)", "SheenColor", data.SheenColorTexture.Path);
            drawTextureReference("Sheen Roughness (A)", "SheenRoughness", data.SheenRoughnessTexture.Path);

            ImGui::SeparatorText("Specular");
            changed |= ImGui::SliderFloat("Specular", &data.SpecularStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How strong the non-metal reflection is (1: as the IOR gives it, 0: none).");
            changed |= ImGui::ColorEdit3("Specular Color", glm::value_ptr(data.SpecularColorFactor), ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Tints the non-metal reflection head-on (the IOR's F0 times this, at most 1). Above 1 reflects more than the IOR.");
            drawTextureReference("Specular Texture (A)", "SpecularStrength", data.SpecularStrengthTexture.Path);
            drawTextureReference("Specular Color Texture (RGB)", "SpecularColor", data.SpecularColorTexture.Path);

            ImGui::SeparatorText("Iridescence");
            changed |= ImGui::SliderFloat("Iridescence", &data.IridescenceFactor, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A thin film over the surface, colored by interference (soap bubbles, oil, heat-tinted metal).");
            changed |= ImGui::DragFloat("Film IOR", &data.IridescenceIOR, 0.01f, 1.0f, 3.0f);
            changed |= ImGui::DragFloat("Film Thickness Min (nm)", &data.IridescenceThicknessMinimum, 1.0f, 0.0f, 2000.0f);
            changed |= ImGui::DragFloat("Film Thickness Max (nm)", &data.IridescenceThicknessMaximum, 1.0f, 0.0f, 2000.0f);
            drawTextureReference("Iridescence Texture (R)", "Iridescence", data.IridescenceTexture.Path);
            drawTextureReference("Film Thickness Texture (G)", "IridescenceThickness", data.IridescenceThicknessTexture.Path);

            ImGui::SeparatorText("Anisotropy");
            changed |= ImGui::SliderFloat("Anisotropy", &data.AnisotropyStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stretches the highlights along a direction (brushed metal). Needs the mesh's tangents.");
            float rotationDegrees = glm::degrees(data.AnisotropyRotation);
            if (ImGui::DragFloat("Anisotropy Rotation", &rotationDegrees, 0.5f, -180.0f, 180.0f, "%.1f deg"))
            {
                data.AnisotropyRotation = glm::radians(rotationDegrees);
                changed = true;
            }
            drawTextureReference("Anisotropy Texture (RG dir, B)", "Anisotropy", data.AnisotropyTexture.Path);

            ImGui::SeparatorText("Dispersion");
            changed |= ImGui::DragFloat("Dispersion", &data.Dispersion, 0.01f, 0.0f, 3.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How far refraction splits the colors (20 / Abbe number: glass 0.33, diamond 0.36). Needs Transmission.");
        }

        // The UV channels the textures read (KHR_texture_transform): tiling, offset and rotation of a mesh UV set.
        if (ImGui::CollapsingHeader("UV Channels"))
        {
            for (int index = 0; index < MaxMaterialUVChannels; ++index)
            {
                MaterialUVChannel& channel = data.UVChannels[index];
                ImGui::PushID(index);
                ImGui::SeparatorText(("Channel " + std::to_string(index)).c_str());
                changed |= ImGui::SliderInt("Mesh UV Set", &channel.Source, 0, 1);
                changed |= ImGui::DragFloat2("Tiling", glm::value_ptr(channel.Scale), 0.01f);
                changed |= ImGui::DragFloat2("Offset", glm::value_ptr(channel.Offset), 0.01f);
                float degrees = glm::degrees(channel.Rotation);
                if (ImGui::DragFloat("Rotation", &degrees, 0.5f, -360.0f, 360.0f, "%.1f deg"))
                {
                    channel.Rotation = glm::radians(degrees);
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::TextDisabled("A texture reads the channel its set names (imported from glTF; 0 by default).");
        }

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
