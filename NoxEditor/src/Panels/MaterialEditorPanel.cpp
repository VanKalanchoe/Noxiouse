#include "MaterialEditorPanel.h"
#include "NoxCore/Audio/SpatialAudio.h"
#include "AudioWidgets.h"

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
#include "NoxCore/Renderer/MaterialGraph.h"
#include "NoxCore/NodeGraph/NodeGraphAsset.h"
#include "NoxCore/Core/Application.h"
#include <SDL3/SDL_misc.h>
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
        // What the surface does to sound (Steam Audio's acoustic scene): saved in the asset registry, not the .nmat.
        if (handle != 0 && Project::GetActive())
        {
            auto assets = Project::GetActive()->GetEditorAssetManager();
            const AcousticMaterial chosen = assets->GetMetadata(handle).AudioSettings.Acoustic;
            const AcousticMaterial resolved = ResolveAcousticMaterial(chosen, data.MetallicFactor, data.TransmissionFactor, data.Mode == AlphaMode::Blend);
            const std::string preview = chosen == AcousticMaterial::Auto ? "Auto (" + std::string(AcousticMaterialToString(resolved)) + ")"
                                                                         : std::string(AcousticMaterialToString(chosen));
            if (ImGui::BeginCombo("Acoustic Material", preview.c_str()))
            {
                for (uint8_t i = 0; i < static_cast<uint8_t>(AcousticMaterial::Count); ++i)
                {
                    const AcousticMaterial option = static_cast<AcousticMaterial>(i);
                    if (ImGui::Selectable(std::string(AcousticMaterialToString(option)).c_str(), option == chosen))
                    {
                        assets->SetAcousticMaterial(handle, option);
                        SpatialAudio::MaterialsChanged(); // (a playing scene re-reads its walls)
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
            {
                const AcousticMaterialProperties& properties = GetAcousticMaterialProperties(resolved);
                ImGui::SetTooltip("What this surface does to sound (Steam Audio): how much a wall of it lets through and absorbs.\n"
                                  "Auto: Glass for transparent / transmissive materials, Metal for metallic ones, else Generic.\n"
                                  "%s lets through %.1f %% / %.1f %% / %.1f %% (low / mid / high) and absorbs %.0f %% / %.0f %% / %.0f %%.",
                                  std::string(AcousticMaterialToString(resolved)).c_str(), properties.Transmission[0] * 100.0f,
                                  properties.Transmission[1] * 100.0f, properties.Transmission[2] * 100.0f, properties.Absorption[0] * 100.0f,
                                  properties.Absorption[1] * 100.0f, properties.Absorption[2] * 100.0f);
            }
            // The physical surface it is (Unreal's physical material Surface Type): footstep notifies pick their sound by it.
            std::string surface = assets->GetMetadata(handle).PhysicalSurface;
            if (PhysicalSurfaceCombo("Physical Surface", surface, "Default"))
                assets->SetPhysicalSurface(handle, surface);
            ImGui::SetItemTooltip("What the surface is underfoot: an animation's Play Sound notify with Trace Surface plays the sound it has for\n"
                                  "this surface (grass, wood ...). The list is the project's: Settings > Physical Surfaces. A Rigidbody 3D's\n"
                                  "Physical Surface overrides it for the whole body.");
            ImGui::Separator();
        }
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
            if (!texturePath.empty() && ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Clear"))
                {
                    texturePath.clear();
                    changed = true;
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drag a texture here from the Content Browser; right-click to clear it.");

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
        drawTextureReference("Ambient Occlusion Texture", "Occlusion", data.OcclusionTexturePath);
        changed |= ImGui::SliderFloat("Ambient Occlusion Strength", &data.OcclusionStrength, 0.0f, 1.0f);
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

        // Unreal's names for what the file calls Mode / AlphaMaskCutoff / DoubleSided (glTF's alphaMode / alphaCutoff / doubleSided).
        int blendMode = static_cast<int>(data.Mode);
        const char* blendModes[] = { "Opaque", "Masked", "Translucent" };
        if (ImGui::Combo("Blend Mode", &blendMode, blendModes, 3))
        {
            data.Mode = static_cast<AlphaMode>(blendMode);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opaque: solid. Masked: pixels below the clip value are cut away (leaves, fences, dissolve) -- still lit like\n"
                              "opaque surfaces. Translucent: blended by the base colour's alpha (glass, smoke), drawn after the opaque world.");
        if (data.Mode == AlphaMode::Mask)
            changed |= ImGui::DragFloat("Opacity Mask Clip Value", &data.AlphaMaskCutoff, 0.005f, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Two Sided", &data.DoubleSided);

        // How the surface is lit (Unreal's Shading Model; ToonShading.slang for Toon, CustomLit.slang for Custom Lit).
        int shadingModel = static_cast<int>(data.ShadingModel);
        const char* shadingModels[] = { "Default Lit", "Unlit", "Toon", "Custom Lit" };
        if (ImGui::Combo("Shading Model", &shadingModel, shadingModels, 4))
        {
            data.ShadingModel = static_cast<MaterialShadingModel>(shadingModel);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Default Lit: physically based lighting.\n"
                              "Unlit: the base colour and emissive as they are, no lighting.\n"
                              "Toon: light in hard bands (or a ramp texture), a sharp highlight, an optional rim light. The ambient light\n"
                              "and reflections stay as for Default Lit (Roughness 1: no reflections). Ray-traced reflections and the\n"
                              "path tracer show a toon surface as Default Lit.\n"
                              "Custom Lit: lit by a Material Shader of your own (a .slang file: its light() and ambient() functions).");
        if (data.ShadingModel == MaterialShadingModel::Toon)
        {
            ImGui::SeparatorText("Toon");
            changed |= ImGui::SliderInt("Bands", &data.ToonBands, 1, 8);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Light steps between unlit and fully lit: 1 = two tones (lit / shadow), 3 = shadow, mid, lit ...");
            changed |= ImGui::SliderFloat("Band Softness", &data.ToonBandSoftness, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How soft each band edge is: 0 = razor sharp, 1 = smooth like Default Lit.");
            drawTextureReference("Ramp Texture", "ToonRamp", data.ToonRampTexture.Path);
            ImGui::TextDisabled("(optional: replaces the bands -- left = shadow, right = fully lit)");
            changed |= ImGui::SliderFloat("Highlight Size", &data.ToonSpecularSize, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The sharp highlight's size (0: no highlight). White on non-metals, the base colour on metals.");
            changed |= ImGui::SliderFloat("Highlight Softness", &data.ToonSpecularSoftness, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Highlight Intensity", &data.ToonSpecularIntensity, 0.0f, 8.0f);
            changed |= ImGui::ColorEdit3("Rim Color", glm::value_ptr(data.ToonRimColor));
            changed |= ImGui::SliderFloat("Rim Width", &data.ToonRimWidth, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Rim Intensity", &data.ToonRimIntensity, 0.0f, 8.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A light band along the silhouette, on the side the light reaches (0: none).");
        }
        if (data.ShadingModel == MaterialShadingModel::CustomLit)
        {
            ImGui::SeparatorText("Custom Lit");
            // The Material Shader: a .slang asset (Content Browser > + Add > Material Shader), by its path in Assets.
            auto assets = Project::GetActive()->GetEditorAssetManager();
            // (Renamed or moved since: the registry still finds it by the old path the material names -- shown by its name now.)
            const AssetHandle currentShader = data.CustomShader.empty() ? AssetHandle(0) : assets->FindHandleByPath(data.CustomShader, AssetType::MaterialShader);
            const std::string currentShaderPath = currentShader != 0 ? assets->GetMetadata(currentShader).FilePath.generic_string() : data.CustomShader;
            const std::string shaderLabel = data.CustomShader.empty() ? std::string("None") : std::filesystem::path(currentShaderPath).filename().string();
            if (ImGui::BeginCombo("Material Shader", shaderLabel.c_str()))
            {
                if (ImGui::Selectable("None", data.CustomShader.empty()))
                {
                    data.CustomShader.clear();
                    changed = true;
                }
                for (const auto& [shaderHandle, shaderMetadata] : assets->GetAssetRegistry())
                {
                    if (shaderMetadata.Type != AssetType::MaterialShader)
                        continue;
                    const std::string path = shaderMetadata.FilePath.generic_string();
                    if (ImGui::Selectable(path.c_str(), path == currentShaderPath))
                    {
                        data.CustomShader = path;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The Material Shader (.slang) whose light() and ambient() light this material.\n"
                                  "Pick one here or drag it from the Content Browser; make one with Content Browser > + Add > Material Shader.");
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    const AssetHandle dropped = *(const AssetHandle*)payload->Data;
                    if (AssetManager::GetAssetType(dropped) == AssetType::MaterialShader)
                    {
                        data.CustomShader = assets->GetMetadata(dropped).FilePath.generic_string();
                        changed = true;
                    }
                    else
                        NOX_CORE_WARN("Wrong Asset Type - Expected a Material Shader (.slang)");
                }
                ImGui::EndDragDropTarget();
            }
            if (!data.CustomShader.empty())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open"))
                {
                    const std::string url = "file:///" + std::filesystem::absolute(Project::GetActiveAssetDirectory() / currentShaderPath).generic_string();
                    if (!SDL_OpenURL(url.c_str()))
                        NOX_CORE_ERROR("Could not open {} ({})", data.CustomShader, SDL_GetError());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Opens the .slang file in your code editor. Saving it there builds it again (about half a second later).");
            }

            // Whether the renderer has it built (CustomLitShaders.h): else the material is lit as Default Lit.
            const Renderer* renderer = Application::Get().GetRenderer();
            const ImVec4 warning(0.95f, 0.75f, 0.25f, 1.0f);
            if (renderer && renderer->GetCustomLitStatus().Building)
                ImGui::TextColored(warning, "Compiling the Material Shaders... (the last build draws meanwhile)");
            // The last build's error first: a failed edit keeps the build before it drawing (so the shader still has a slot), and the
            // error was hidden behind "Built" (the user's test).
            const bool built = renderer && Renderer::FindCustomLitSlot(data.CustomShader) != 0xFFFFFFFFu;
            if (!data.CustomShader.empty() && renderer && !renderer->GetCustomLitStatus().Message.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f), "%s %s", renderer->GetCustomLitStatus().Message.c_str(),
                                   built ? "The last build that compiled draws until it does." : "Lit as Default Lit until it compiles.");
                ImGui::PopTextWrapPos();
            }
            else if (data.CustomShader.empty())
                ImGui::TextColored(warning, "No Material Shader: lit as Default Lit.");
            else if (built)
                ImGui::TextDisabled("Built: lit by %s.", shaderLabel.c_str());
            else
                ImGui::TextColored(warning, "Not built yet (lit as Default Lit): is the file in Assets?");

            for (int parameter = 0; parameter < MaterialCustomParameterCount; ++parameter)
            {
                const std::string label = "Parameter " + std::to_string(parameter + 1);
                changed |= ImGui::ColorEdit4(label.c_str(), glm::value_ptr(data.CustomParameters[parameter]),
                                             ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The shader reads it as s.parameters[%d]: a colour (rgb, a) or four numbers (x, y, z, w) -- as it uses them.",
                                      parameter);
            }
            drawTextureReference("Custom Texture 1 (sRGB)", "CustomTexture1", data.CustomTexture1.Path);
            drawTextureReference("Custom Texture 2 (linear)", "CustomTexture2", data.CustomTexture2.Path);
            ImGui::TextDisabled("(the shader reads them with custom_lit_texture(s, 0 or 1, uv))");
            ImGui::TextDisabled("Ray-traced reflections, GI bounces, the path tracer and order-independent\ntransparency show it as Default Lit.");
        }

        // The material graph (MaterialGraph.h; Unreal's material graph with this material as its instance): what its Material Output
        // connects replaces the values above, its parameters take this material's values where it sets them.
        ImGui::SeparatorText("Material Graph");
        {
            auto assets = Project::GetActive()->GetEditorAssetManager();
            const AssetHandle currentGraph = data.SurfaceGraph.empty() ? AssetHandle(0) : assets->FindHandleByPath(data.SurfaceGraph, AssetType::MaterialGraph);
            const std::string currentGraphPath = currentGraph != 0 ? assets->GetMetadata(currentGraph).FilePath.generic_string() : data.SurfaceGraph;
            const std::string graphLabel = data.SurfaceGraph.empty() ? std::string("None") : std::filesystem::path(currentGraphPath).filename().string();
            if (ImGui::BeginCombo("Surface Graph", graphLabel.c_str()))
            {
                if (ImGui::Selectable("None", data.SurfaceGraph.empty()))
                {
                    data.SurfaceGraph.clear();
                    changed = true;
                }
                for (const auto& [graphHandle, graphMetadata] : assets->GetAssetRegistry())
                {
                    if (graphMetadata.Type != AssetType::MaterialGraph)
                        continue;
                    const std::string path = graphMetadata.FilePath.generic_string();
                    if (ImGui::Selectable(path.c_str(), path == currentGraphPath))
                    {
                        data.SurfaceGraph = path;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A Material Graph (.nmatgraph) that computes this surface: the Material Output pins it connects replace\n"
                                  "the values above. Pick one or drag it from the Content Browser; make one with + Add > Material Graph.");
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
                {
                    const AssetHandle dropped = *(const AssetHandle*)payload->Data;
                    if (AssetManager::GetAssetType(dropped) == AssetType::MaterialGraph)
                    {
                        data.SurfaceGraph = assets->GetMetadata(dropped).FilePath.generic_string();
                        changed = true;
                    }
                    else
                        NOX_CORE_WARN("Wrong Asset Type - Expected a Material Graph (.nmatgraph)");
                }
                ImGui::EndDragDropTarget();
            }
            if (currentGraph != 0)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open##SurfaceGraph") && OpenAssetCallback)
                    OpenAssetCallback(currentGraph);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Opens the graph in the node graph editor. Save it there (Ctrl+S) to build it again.");
            }

            if (!data.SurfaceGraph.empty())
            {
                // The graph as it is in memory (the node graph editor edits the same asset), compiled for its parameters and errors.
                const Ref<NodeGraphAsset> graphAsset = currentGraph != 0 ? AssetManager::GetAsset<NodeGraphAsset>(currentGraph) : nullptr;
                const MaterialGraphProgram program = graphAsset ? CompileMaterialGraph(graphAsset->Graph) : MaterialGraphProgram{};
                const Renderer* renderer = Application::Get().GetRenderer();
                const ImVec4 warning(0.95f, 0.75f, 0.25f, 1.0f);
                const bool built = renderer && Renderer::FindMaterialGraph(data.SurfaceGraph) != 0xFFFFFFFFu;
                ImGui::PushTextWrapPos(0.0f);
                if (!graphAsset)
                    ImGui::TextColored(warning, "The graph is not in the project's Assets: the material keeps its own values.");
                else if (!program.Valid)
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f), "The graph does not compile: %s %s", program.Error.c_str(),
                                       built ? "(its last build draws until it does)" : "(the material keeps its own values)");
                else if (renderer && renderer->GetCustomLitStatus().Building)
                    ImGui::TextColored(warning, "Building the material graphs... (the last build draws meanwhile)");
                else if (renderer && !renderer->GetCustomLitStatus().Message.empty())
                    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f), "%s", renderer->GetCustomLitStatus().Message.c_str());
                else if (built)
                    ImGui::TextDisabled("Built: %s computes the surface.", graphLabel.c_str());
                else
                    ImGui::TextColored(warning, "Not built yet (the material keeps its own values): save the graph.");
                ImGui::PopTextWrapPos();

                // Its parameters: ticked ones take this material's value, the others the graph's default.
                for (const MaterialGraphParameter& parameter : program.Parameters)
                {
                    if (parameter.Name.starts_with('#'))
                        continue; // (a Texture Sample without a parameter name: its texture is the graph's)
                    const bool isTexture = parameter.Type == MaterialGraphParameter::Kind::Texture;
                    auto value = std::find_if(data.GraphParameters.begin(), data.GraphParameters.end(), [&](const MaterialGraphParameterValue& v)
                    {
                        return v.Name == parameter.Name && v.IsTexture == isTexture;
                    });
                    bool overridden = value != data.GraphParameters.end();
                    ImGui::PushID(parameter.Name.c_str());
                    if (ImGui::Checkbox("##override", &overridden))
                    {
                        if (overridden)
                        {
                            MaterialGraphParameterValue added;
                            added.Name = parameter.Name;
                            added.IsTexture = isTexture;
                            added.Value = parameter.Default;
                            if (isTexture && parameter.DefaultTexture != 0 && AssetManager::IsAssetHandleValid(parameter.DefaultTexture))
                                added.Texture = assets->GetMetadata(parameter.DefaultTexture).FilePath.generic_string();
                            data.GraphParameters.push_back(std::move(added));
                        }
                        else
                            data.GraphParameters.erase(value);
                        changed = true;
                        value = std::find_if(data.GraphParameters.begin(), data.GraphParameters.end(), [&](const MaterialGraphParameterValue& v)
                        {
                            return v.Name == parameter.Name && v.IsTexture == isTexture;
                        });
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Ticked: this material's value. Unticked: the graph's default.");
                    ImGui::SameLine();
                    const bool active = value != data.GraphParameters.end();
                    if (isTexture)
                    {
                        if (active)
                            drawTextureReference(parameter.Name.c_str(), "GraphTexture", value->Texture);
                        else
                        {
                            const std::string fallback = parameter.DefaultTexture != 0 && AssetManager::IsAssetHandleValid(parameter.DefaultTexture)
                                                             ? assets->GetMetadata(parameter.DefaultTexture).FilePath.filename().string()
                                                             : std::string("None");
                            ImGui::TextDisabled("%s: %s (the graph's)", parameter.Name.c_str(), fallback.c_str());
                        }
                    }
                    else
                    {
                        glm::vec4 shown = active ? value->Value : parameter.Default;
                        ImGui::BeginDisabled(!active);
                        bool edited = false;
                        if (parameter.Type == MaterialGraphParameter::Kind::Scalar)
                            edited = ImGui::DragFloat(parameter.Name.c_str(), &shown.x, 0.01f);
                        else
                            edited = ImGui::ColorEdit4(parameter.Name.c_str(), glm::value_ptr(shown), ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
                        ImGui::EndDisabled();
                        if (edited && active)
                        {
                            value->Value = shown;
                            changed = true;
                        }
                    }
                    ImGui::PopID();
                }
                // Values for parameters the graph no longer has (renamed or removed there): kept, shown so they can be dropped.
                for (size_t index = 0; index < data.GraphParameters.size(); ++index)
                {
                    const MaterialGraphParameterValue& stale = data.GraphParameters[index];
                    const bool known = std::any_of(program.Parameters.begin(), program.Parameters.end(), [&](const MaterialGraphParameter& p)
                    {
                        return p.Name == stale.Name && (p.Type == MaterialGraphParameter::Kind::Texture) == stale.IsTexture;
                    });
                    if (known || !program.Valid)
                        continue;
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::TextDisabled("%s: not in the graph any more", stale.Name.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove"))
                    {
                        data.GraphParameters.erase(data.GraphParameters.begin() + static_cast<std::ptrdiff_t>(index));
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                ImGui::TextDisabled("Opaque and translucent surfaces use it; ray-traced reflections, GI bounces and the path tracer\n"
                                    "show the material's own values. Opacity Mask and World Position Offset come later.");
            }
        }

        if (changed)
        {
            // Every instance using this material shades with the edit from this frame; its Content Browser thumbnail is drawn again.
            Renderer::MarkMaterialChanged(handle);
            AssetPreviewRenderer::Get().Invalidate(handle);
            auto assets = Project::GetActive()->GetEditorAssetManager();
            const auto& metadata = assets->GetMetadata(handle);
            if (MaterialSerializer::Serialize(Project::GetActiveAssetDirectory() / metadata.FilePath, data))
                assets->NoteSourceWritten(handle); // (the watcher's event for this write is then no reimport)
        }
    }
}
