#pragma once
#include <string>

#include <glm/glm.hpp>

namespace Nox
{
    // The post-process settings (PP1; Passes/PostEffectPasses.cpp, PostProcess.slang, SceneOutline.slang): every effect off by default.
    // The renderer's own copy is the editor's Renderer Settings (Renderer::getPostEffects); a scene's Post Process Volume
    // (PostProcessVolumeComponent, Unreal's) replaces it while it applies (Renderer::SetScenePostProcess) -- that is what is saved with
    // the scene and ships with the game.
    //   Bloom (Call of Duty's chain: how much of the image becomes its blur, its radius) with image-based lens flares; depth of field
    //   (a thin lens: focus distance, f-number, the focal length from the camera's field of view on a 24 mm sensor, the largest blur);
    //   motion blur (the motion vectors over the shutter); colour grading (saturation, contrast, a LUT strip file) and vignette, film
    //   grain, chromatic aberration, sharpening (CAS); outlines (lines where depth or normals jump, stylized rendering step 2).
    struct PostProcessSettings
    {
        bool Bloom = false;
        float BloomIntensity = 0.04f;
        float BloomRadius = 1.0f;
        float LensFlares = 0.0f;
        bool DepthOfField = false;
        float FocusDistanceMeters = 10.0f;
        float FStop = 2.8f;
        float MaxBlurPixels = 16.0f;
        bool MotionBlur = false;
        float MotionBlurShutter = 0.5f; // fraction of the frame (UE's Motion Blur Amount)
        float MotionBlurMaxPixels = 32.0f;
        float Saturation = 1.0f;
        float Contrast = 1.0f;
        float Vignette = 0.0f;
        float FilmGrain = 0.0f;
        float ChromaticAberration = 0.0f;
        float Sharpen = 0.0f;
        std::string LutPath;
        float LutStrength = 1.0f;

        // Outlines (SceneOutline.slang): drawn into the lit scene before anti-aliasing / upscaling (TAA, DLSS smooth them), multiplying
        // it -- black is ink, a colour tints the edge and keeps its shading. An edge is where the depth jumps (silhouettes) or the normal
        // turns (creases), seen from the nearer side; the sky never gets one.
        bool Outlines = false;
        glm::vec3 OutlineColor{ 0.0f };          // multiplies the scene: (0, 0, 0) black lines, (1, 1, 1) none
        float OutlineThickness = 1.5f;           // pixels at the output resolution
        float OutlineDepthSensitivity = 0.5f;    // 0 none .. 1 the smallest depth step (relative to the distance) draws a line
        float OutlineNormalSensitivity = 0.5f;   // 0 none .. 1 the gentlest crease draws a line
        bool OutlineToonOnly = false;            // only on surfaces whose material uses the Toon shading model
        float OutlineFadeDistanceMeters = 0.0f;  // lines fade out towards this distance (0: never)

        bool operator==(const PostProcessSettings&) const = default;
    };
}
