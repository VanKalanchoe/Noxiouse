#include "PostProcessSettingsUI.h"

#include <cstdio>

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

namespace Nox
{
    bool DrawPostProcessSettings(PostProcessSettings& post)
    {
        bool changed = false;
        ImGui::PushID(&post);

        changed |= ImGui::Checkbox("Outlines", &post.Outlines);
        ImGui::SetItemTooltip("Lines where the depth jumps (silhouettes) or the surface turns (creases), drawn before anti-aliasing so\n"
                              "TAA / DLSS smooth them. The colour multiplies the image: black is ink, a colour tints the edge.");
        if (post.Outlines)
        {
            ImGui::Indent();
            changed |= ImGui::ColorEdit3("Color##Outlines", glm::value_ptr(post.OutlineColor));
            changed |= ImGui::SliderFloat("Thickness (px)##Outlines", &post.OutlineThickness, 0.5f, 8.0f, "%.1f");
            changed |= ImGui::SliderFloat("Depth Sensitivity##Outlines", &post.OutlineDepthSensitivity, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Silhouettes: 0 none, 1 even a small step in depth (relative to the distance) draws a line.");
            changed |= ImGui::SliderFloat("Normal Sensitivity##Outlines", &post.OutlineNormalSensitivity, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Creases: 0 none, 1 even a gentle bend draws a line.");
            changed |= ImGui::Checkbox("Toon Surfaces Only##Outlines", &post.OutlineToonOnly);
            ImGui::SetItemTooltip("Only on surfaces whose material uses the Toon shading model.");
            changed |= ImGui::DragFloat("Fade Distance (m)##Outlines", &post.OutlineFadeDistanceMeters, 0.5f, 0.0f, 100000.0f, "%.1f");
            ImGui::SetItemTooltip("Lines fade out towards this distance (0: never).");
            ImGui::Unindent();
        }

        changed |= ImGui::Checkbox("Bloom", &post.Bloom);
        ImGui::SetItemTooltip("Bright light bleeding into its surroundings (Call of Duty's bloom: a blur chain over the whole image,\n"
                              "blended in by Intensity; no threshold, as in UE).");
        if (post.Bloom)
        {
            ImGui::Indent();
            changed |= ImGui::SliderFloat("Intensity##Bloom", &post.BloomIntensity, 0.0f, 0.3f, "%.3f");
            changed |= ImGui::SliderFloat("Radius##Bloom", &post.BloomRadius, 0.25f, 3.0f, "%.2f");
            changed |= ImGui::SliderFloat("Lens Flares##Bloom", &post.LensFlares, 0.0f, 2.0f, "%.2f");
            ImGui::SetItemTooltip("Ghosts of bright areas mirrored through the centre, and a halo (UE's image-based flares).");
            ImGui::Unindent();
        }
        changed |= ImGui::Checkbox("Depth of Field", &post.DepthOfField);
        ImGui::SetItemTooltip("A thin lens: sharp at Focus Distance, blurred before and behind it by the f-number (smaller: more\n"
                              "blur) and the camera's field of view (the focal length on a 24 mm-tall sensor).");
        if (post.DepthOfField)
        {
            ImGui::Indent();
            changed |= ImGui::DragFloat("Focus Distance (m)##DOF", &post.FocusDistanceMeters, 0.05f, 0.05f, 10000.0f, "%.2f");
            changed |= ImGui::SliderFloat("F-Stop##DOF", &post.FStop, 0.7f, 32.0f, "f/%.1f", ImGuiSliderFlags_Logarithmic);
            changed |= ImGui::SliderFloat("Max Blur (px)##DOF", &post.MaxBlurPixels, 1.0f, 48.0f, "%.0f");
            ImGui::Unindent();
        }
        changed |= ImGui::Checkbox("Motion Blur", &post.MotionBlur);
        ImGui::SetItemTooltip("Along each pixel's motion (camera and objects) over the open shutter.");
        if (post.MotionBlur)
        {
            ImGui::Indent();
            changed |= ImGui::SliderFloat("Shutter##MotionBlur", &post.MotionBlurShutter, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Max Blur (px)##MotionBlur", &post.MotionBlurMaxPixels, 4.0f, 96.0f, "%.0f");
            ImGui::Unindent();
        }
        changed |= ImGui::SliderFloat("Saturation", &post.Saturation, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Contrast", &post.Contrast, 0.5f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Vignette", &post.Vignette, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Film Grain", &post.FilmGrain, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Chromatic Aberration", &post.ChromaticAberration, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::SliderFloat("Sharpen (CAS)", &post.Sharpen, 0.0f, 1.0f, "%.2f");
        char lutPath[512];
        std::snprintf(lutPath, sizeof(lutPath), "%s", post.LutPath.c_str());
        if (ImGui::InputText("Color LUT", lutPath, sizeof(lutPath), ImGuiInputTextFlags_EnterReturnsTrue))
        {
            post.LutPath = lutPath;
            changed = true;
        }
        ImGui::SetItemTooltip("A colour grading LUT image (a strip of N slices of N x N, e.g. 256 x 16 or 1024 x 32, blue across the\n"
                              "slices; UE's format), applied after the tonemapper. Enter a file path; empty: none.");
        if (!post.LutPath.empty())
            changed |= ImGui::SliderFloat("LUT Strength", &post.LutStrength, 0.0f, 1.0f, "%.2f");

        ImGui::PopID();
        return changed;
    }
}
