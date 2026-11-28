#include "RendererSettingsPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Panels/InspectorWidgets.h"
#include <NoxUI/Core/UIContext.h>
#include <NoxUI/Widgets/UIPropertyRows.h>
#include "NoxCore/Asset/EditorAssetManager.h"
#include "NoxCore/Asset/TextureStreamer.h"
#include "NoxCore/Core/Application.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Profiling/Profiler.h"
#include "NoxCore/Profiling/StatsOverlayLayer.h"
#include "NoxCore/Profiling/StatsReport.h"
#include "NoxCore/Project/Project.h"
#include "NoxCore/Renderer/EditorCamera.h"
#include "NoxCore/Renderer/GeometryStreamer.h"
#include "NoxCore/Renderer/Renderer.h"
#include "NoxCore/Renderer/VirtualTextureStreamer.h"
#include "NoxCore/Scene/Components.h"
#include <NoxUI/Core/UIBuilder.h>
#include <NoxUI/Layout/UILayout.h>
#include <NoxUI/Widgets/UIScrollView.h>

namespace Nox
{
    namespace
    {
        constexpr NoxUI::UIThemeToken kGood{ "StatusGood", { 0.40f, 0.85f, 0.55f, 1.0f } };
        constexpr NoxUI::UIThemeToken kInfo{ "StatusInfo", { 0.45f, 0.78f, 1.0f, 1.0f } };
        constexpr NoxUI::UIThemeToken kWarn{ "StatusWarning", { 1.0f, 0.62f, 0.25f, 1.0f } };
        constexpr double kMB = 1024.0 * 1024.0;

        std::string Format(const char* format, auto... args)
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), format, args...);
            return buffer;
        }

        // A uint32 the renderer hands out by reference, as a whole-number row.
        void RefInt(NoxUI::UIPropertyRows& rows, const std::string& label, uint32_t* value, int min, int max, const std::string& tooltip = {})
        {
            rows.Int(label, [value]() { return static_cast<int>(*value); }, [value](int v) { *value = static_cast<uint32_t>(v); }, min, max, tooltip);
        }
        void RefFloat(NoxUI::UIPropertyRows& rows, const std::string& label, float* value, float min, float max, const char* format, const std::string& tooltip = {})
        {
            rows.Slider(label, [value]() { return *value; }, [value](float v) { *value = v; }, min, max, format, tooltip);
        }
        void RefBool(NoxUI::UIPropertyRows& rows, const std::string& label, bool* value, const std::string& tooltip = {})
        {
            rows.Check(label, [value]() { return *value; }, [value](bool v) { *value = v; }, tooltip);
        }
        // A choice between fixed values (resolutions, tile sizes).
        void ValueChoice(NoxUI::UIPropertyRows& rows, const std::string& label, std::vector<uint32_t> values, std::function<uint32_t()> get,
                         std::function<void(uint32_t)> set, const std::string& tooltip = {})
        {
            std::vector<std::string> names;
            for (uint32_t value : values)
                names.push_back(std::to_string(value));
            rows.Choice(label, std::move(names),
                [values, get]()
                {
                    const auto found = std::find(values.begin(), values.end(), get());
                    return found == values.end() ? 0 : static_cast<int>(found - values.begin());
                },
                [values, set](int index) { set(values[static_cast<size_t>(index)]); }, tooltip);
        }
    }

    void AddPostProcessSettingsRows(NoxUI::UIPropertyRows& rows, PostProcessSettings& post, std::function<void()> changed)
    {
        PostProcessSettings* p = &post;
        auto check = [&rows, p, changed](const std::string& label, bool PostProcessSettings::*field, const std::string& tooltip)
        {
            return rows.Check(label, [p, field]() { return p->*field; }, [p, field, changed](bool v) { p->*field = v; if (changed) changed(); }, tooltip);
        };
        auto slider = [p, changed](NoxUI::UIPropertyRows& into, const std::string& label, float PostProcessSettings::*field, float min, float max,
                                   const char* format, const std::string& tooltip = {})
        {
            return into.Slider(label, [p, field]() { return p->*field; }, [p, field, changed](float v) { p->*field = v; if (changed) changed(); },
                               min, max, format, tooltip);
        };

        check("Outlines", &PostProcessSettings::Outlines,
              "Lines where the depth jumps (silhouettes) or the surface turns (creases), drawn before anti-aliasing so TAA / DLSS smooth "
              "them. The colour multiplies the image: black is ink, a colour tints the edge.");
        NoxUI::UIPropertyRows outlines = rows.Group();
        outlines.Color3("Color", &post.OutlineColor, changed);
        slider(outlines, "Thickness (px)", &PostProcessSettings::OutlineThickness, 0.5f, 8.0f, "{:.1f}");
        slider(outlines, "Depth Sensitivity", &PostProcessSettings::OutlineDepthSensitivity, 0.0f, 1.0f, "{:.2f}",
               "Silhouettes: 0 none, 1 even a small step in depth (relative to the distance) draws a line.");
        slider(outlines, "Normal Sensitivity", &PostProcessSettings::OutlineNormalSensitivity, 0.0f, 1.0f, "{:.2f}",
               "Creases: 0 none, 1 even a gentle bend draws a line.");
        outlines.Check("Toon Surfaces Only", [p]() { return p->OutlineToonOnly; }, [p, changed](bool v) { p->OutlineToonOnly = v; if (changed) changed(); },
                       "Only on surfaces whose material uses the Toon shading model.");
        outlines.Drag("Fade Distance (m)", [p]() { return p->OutlineFadeDistanceMeters; },
                      [p, changed](float v) { p->OutlineFadeDistanceMeters = v; if (changed) changed(); }, 0.5f, 0.0f, 100000.0f, "{:.1f}",
                      "Lines fade out towards this distance (0: never).");
        rows.ShowWhen(outlines.GetBody(), [p]() { return p->Outlines; });

        check("Bloom", &PostProcessSettings::Bloom,
              "Bright light bleeding into its surroundings (a blur chain over the whole image, blended in by Intensity; no threshold, as in UE).");
        NoxUI::UIPropertyRows bloom = rows.Group();
        slider(bloom, "Intensity", &PostProcessSettings::BloomIntensity, 0.0f, 0.3f, "{:.3f}");
        slider(bloom, "Radius", &PostProcessSettings::BloomRadius, 0.25f, 3.0f, "{:.2f}");
        slider(bloom, "Lens Flares", &PostProcessSettings::LensFlares, 0.0f, 2.0f, "{:.2f}",
               "Ghosts of bright areas mirrored through the centre, and a halo (UE's image-based flares).");
        rows.ShowWhen(bloom.GetBody(), [p]() { return p->Bloom; });

        check("Depth of Field", &PostProcessSettings::DepthOfField,
              "A thin lens: sharp at Focus Distance, blurred before and behind it by the f-number (smaller: more blur) and the camera's field of view.");
        NoxUI::UIPropertyRows dof = rows.Group();
        dof.Drag("Focus Distance (m)", [p]() { return p->FocusDistanceMeters; }, [p, changed](float v) { p->FocusDistanceMeters = v; if (changed) changed(); },
                 0.05f, 0.05f, 10000.0f, "{:.2f}");
        slider(dof, "F-Stop", &PostProcessSettings::FStop, 0.7f, 32.0f, "f/{:.1f}");
        slider(dof, "Max Blur (px)", &PostProcessSettings::MaxBlurPixels, 1.0f, 48.0f, "{:.0f}");
        rows.ShowWhen(dof.GetBody(), [p]() { return p->DepthOfField; });

        check("Motion Blur", &PostProcessSettings::MotionBlur, "Along each pixel's motion (camera and objects) over the open shutter.");
        NoxUI::UIPropertyRows motion = rows.Group();
        slider(motion, "Shutter", &PostProcessSettings::MotionBlurShutter, 0.0f, 1.0f, "{:.2f}");
        slider(motion, "Max Blur (px)", &PostProcessSettings::MotionBlurMaxPixels, 4.0f, 96.0f, "{:.0f}");
        rows.ShowWhen(motion.GetBody(), [p]() { return p->MotionBlur; });

        slider(rows, "Saturation", &PostProcessSettings::Saturation, 0.0f, 2.0f, "{:.2f}");
        slider(rows, "Contrast", &PostProcessSettings::Contrast, 0.5f, 2.0f, "{:.2f}");
        slider(rows, "Vignette", &PostProcessSettings::Vignette, 0.0f, 1.0f, "{:.2f}");
        slider(rows, "Film Grain", &PostProcessSettings::FilmGrain, 0.0f, 1.0f, "{:.2f}");
        slider(rows, "Chromatic Aberration", &PostProcessSettings::ChromaticAberration, 0.0f, 2.0f, "{:.2f}");
        slider(rows, "Sharpen (CAS)", &PostProcessSettings::Sharpen, 0.0f, 1.0f, "{:.2f}");
        rows.String("Color LUT", &post.LutPath, "LUT image path (empty: none)", changed,
                    "A colour grading LUT image (a strip of N slices of N x N, e.g. 256 x 16 or 1024 x 32, blue across the slices; UE's "
                    "format), applied after the tonemapper.");
        Ref<NoxUI::UIElement> lutStrength = slider(rows, "LUT Strength", &PostProcessSettings::LutStrength, 0.0f, 1.0f, "{:.2f}");
        rows.ShowWhen(lutStrength, [p]() { return !p->LutPath.empty(); });
    }

    const Ref<NoxUI::UIElement>& RendererSettingsPanel::BuildUI(NoxUI::UIContext* context)
    {
        auto root = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 0.0f);
        root->WidthMode = NoxUI::UISizeMode::Fill;
        root->HeightMode = NoxUI::UISizeMode::Fill;
        root->StyleClass = "Surface";
        root->DrawBackground = true;
        auto scroll = NoxUI::MakeUI<NoxUI::UIScrollView>();
        scroll->WidthMode = NoxUI::UISizeMode::Fill;
        scroll->HeightMode = NoxUI::UISizeMode::Fill;
        root->AddChild(scroll);
        auto body = NoxUI::MakeUI<NoxUI::UIStackBox>(NoxUI::UIOrientation::Vertical, 6.0f);
        body->WidthMode = NoxUI::UISizeMode::Fill;
        body->Padding = NoxUI::UIThickness::All(8.0f);
        scroll->AddChild(body);
        m_Root = root;

        Renderer* r = m_Hooks.Render;
        if (!r)
            return m_Root;
        NoxUI::UIPropertyRows rows(body, context);
        Hooks hooks = m_Hooks;

        // ---- View ----
        {
            NoxUI::UIPropertyRows view = rows.Section("View", true, &m_Expanded);
            view.Text([hooks]()
            {
                Entity hovered = hooks.Hovered ? hooks.Hovered() : Entity{};
                return "Under the pointer: " + (hovered && hovered.HasComponent<TagComponent>() ? hovered.GetComponent<TagComponent>().Tag : std::string("nothing"));
            });
            view.Choice("Debug View", {
                "Lit", "Base Color", "Normal Texture", "Occlusion", "Emissive", "Metallic", "Roughness", "Shading Normal (World)",
                "Direct Lights Only", "IBL Ambient Only", "World Position", "Entity ID", "Depth Buffer", "RT Shadow Mask", "RT Reflections",
                "Motion Vectors", "Indirect Diffuse GI Only", "DDGI Probe Spheres", "Path Tracer (1 spp raw)", "Path Tracer (progressive)",
                "Texture Streaming Mips", "Cluster LOD Level", "Ambient Occlusion", "Geometry Streaming Pages", "Reflection Source",
                "Reflections Only" },
                [r]() { return static_cast<int>(r->getDebugMode()); }, [r](int v) { r->setDebugMode(static_cast<uint32_t>(v)); },
                "What the viewport shows (Unreal's View Modes). Texture Streaming Mips: red needs more, green as needed, blue more than "
                "needed. Cluster LOD: green original, then yellow, orange, red, magenta, blue. Reflection Source: green RT / SSR found it, "
                "yellow the probe volume, red the sky.");
            view.Choice("Tonemapping", { "None (clamped linear)", "ACES (Narkowicz)", "ACES (Hill)", "ACES (Hill + exposure boost)",
                                         "Khronos PBR Neutral", "Reinhard", "Reinhard Modified", "Hejl Hable ALU", "Hable UC2 (RTXPT)", "ACES (RTXPT)" },
                [r]() { return static_cast<int>(r->getTonemapMode()); }, [r](int v) { r->setTonemapMode(static_cast<uint32_t>(v)); });
            view.Slider("Exposure Compensation", [r]() { return r->getViewportExposureCompensation(); },
                        [r](float v) { r->setViewportExposureCompensation(v); }, -10.0f, 10.0f, "{:+.1f} EV",
                        "Brighter or darker by stops, on top of the scene camera's exposure (its Manual EV100, or Auto Exposure).");
            view.Check("Auto Exposure", [r]() { return r->getViewportAutoExposure(); }, [r](bool v) { r->setViewportAutoExposure(v); },
                       "Adapts the exposure to the image, like the eye: needed to see a night lit by the moon or lamps (~10 stops darker "
                       "than daylight). On whatever the scene camera's Auto Exposure says.");
            view.Slider("Gamma", [r]() { return r->getGamma(); }, [r](float v) { r->setGamma(v); }, 0.5f, 3.5f, "{:.2f}");
            view.Slider("IBL Ambient Scale", [r]() { return r->getScaleIBLAmbient(); }, [r](float v) { r->setScaleIBLAmbient(v); }, 0.0f, 5.0f, "{:.2f}");
            if (EditorCamera* camera = m_Hooks.Camera)
            {
                // Horizontal, as Unreal's camera FOV; the editor camera works in vertical FOV (converted through the aspect ratio).
                view.Slider("Camera FOV (horizontal)",
                    [camera]()
                    {
                        const float aspect = std::max(camera->GetAspectRatio(), 0.01f);
                        return glm::degrees(2.0f * std::atan(std::tan(glm::radians(camera->GetFOV()) * 0.5f) * aspect));
                    },
                    [camera](float horizontal)
                    {
                        const float aspect = std::max(camera->GetAspectRatio(), 0.01f);
                        camera->SetFOV(glm::degrees(2.0f * std::atan(std::tan(glm::radians(horizontal) * 0.5f) / aspect)));
                    },
                    10.0f, 120.0f, "{:.1f}", "The editor camera's horizontal field of view (the scene camera has its own).");
            }
        }

        // ---- Anti-Aliasing and Transparency ----
        {
            NoxUI::UIPropertyRows aa = rows.Section("Anti-Aliasing and Transparency", false, &m_Expanded);
            const auto pathTracing = [r]() { return r->isPathTracingEnabled(); };
            const auto dlssActive = [r]() { return r->isDLSSSupported() && r->isDLSSEnabled() && r->getUpscaleMode() != NRI_UPSCALER_MODE_OFF; };

            Ref<NoxUI::UIElement> refraction = aa.Check("Glass Refraction", [r]() { return r->getRefractionEnabled(); }, [r](bool v) { r->setRefractionEnabled(v); },
                "Transmissive materials (glass) show the scene behind them bent by their IOR and thickness and blurred by their roughness. "
                "Off: transmission only makes them see-through.");
            aa.EnableWhen(refraction, [=]() { return !pathTracing(); }, []() { return std::string("the path tracer refracts itself"); });
            NoxUI::UIPropertyRows glass = aa.Group();
            Ref<NoxUI::UIElement> rtGlass = glass.Check("Ray Traced Glass", [r]() { return r->getRTRefractionEnabled(); }, [r](bool v) { r->setRTRefractionEnabled(v); },
                "Smooth glass traces what is behind it like the path tracer (in at the surface, out at the mesh's far side), so objects off "
                "screen or hidden behind others show too. Rough glass keeps the screen-space refraction. Needs ray tracing.");
            glass.EnableWhen(rtGlass, [=]() { return !pathTracing() && r->getRefractionEnabled(); }, []() { return std::string("Glass Refraction is off"); });

            Ref<NoxUI::UIElement> oit = aa.Check("Order-Independent Transparency", [r]() { return r->getOITEnabled(); }, [r](bool v) { r->setOITEnabled(v); },
                "Raster transparency only: transparent surfaces and alpha-blended particles blend in any order, so overlapping layers stop "
                "popping as the camera moves. Not used with ray tracing (hybrid sorts glass and traces through it) or by the path tracer.");
            aa.EnableWhen(oit, [=]() { return !pathTracing() && !r->getRayTracingEnabled(); },
                          [=]() { return std::string(pathTracing() ? "path tracer" : "raster only: ray tracing is on"); });
            NoxUI::UIPropertyRows oitRows = aa.Group();
            oitRows.Choice("Method", { "Weighted Blended", "AVBOIT" }, [r]() { return static_cast<int>(r->getOITMethod()); },
                [r](int v) { r->setOITMethod(static_cast<Renderer::OITMethod>(v)); },
                "Weighted Blended: each layer weighted by a fixed curve of its distance; cheap, overlapping layers average out. AVBOIT "
                "(Adaptive Voxel-Based OIT, Unreal's): a low-resolution volume of how much light gets through every depth weights each "
                "layer by what is really in front of it -- near layers hide far ones, tinted glass tints what is behind it.");
            Ref<NoxUI::UIElement> slices = oitRows.Int("Depth Slices", [r]() { return static_cast<int>(r->getAVBOITSlices()); },
                [r](int v) { r->setAVBOITSlices(static_cast<uint32_t>(v)); }, 16, 256,
                "The transmittance volume's depth resolution (only depths with transparent surfaces take slices); 64 separates most scenes.");
            Ref<NoxUI::UIElement> distance = oitRows.Drag("Max Distance (m)", [r]() { return r->getAVBOITDistanceMeters(); },
                [r](float v) { r->setAVBOITDistanceMeters(v); }, 5.0f, 10.0f, 10000.0f, "{:.0f}",
                "How far the volume reaches (logarithmic slices: finer close by).");
            oitRows.ShowWhen(slices, [r]() { return r->getOITMethod() == Renderer::OITMethod::AVBOIT; });
            oitRows.ShowWhen(distance, [r]() { return r->getOITMethod() == Renderer::OITMethod::AVBOIT; });
            aa.EnableWhen(oitRows.GetBody(), [=]() { return r->getOITEnabled() && !pathTracing() && !r->getRayTracingEnabled(); });

            Ref<NoxUI::UIElement> taa = aa.Check("Temporal Anti-Aliasing", [r]() { return r->getTAAEnabled(); }, [r](bool v) { r->setTAAEnabled(v); },
                "Smooth edges without DLSS: the camera shifts by a fraction of a pixel each frame and the frames are blended along the "
                "motion vectors. Used while DLSS is off and not with the path tracer.");
            aa.EnableWhen(taa, [=]() { return !dlssActive() && !pathTracing(); },
                          [=]() { return std::string(dlssActive() ? "DLSS anti-aliases" : "path tracer accumulates"); });
            Ref<NoxUI::UIElement> jitter = aa.Check("Camera Subpixel Jitter",
                [=]() { return dlssActive() || r->isTAAActive() || r->getCameraJitterEnabled(); },
                [r](bool v) { r->setCameraJitterEnabled(v); });
            aa.EnableWhen(jitter, [=]() { return !(dlssActive() || r->isTAAActive()); },
                          [=]() { return std::string(dlssActive() ? "on: DLSS needs it" : "on: TAA needs it"); });
            aa.Text([r]() { const glm::vec2 j = r->getCurrentJitter(); return Format("Jitter offset: (%.3f, %.3f) px", j.x, j.y); }, kInfo);
        }

        // ---- DLSS ----
        {
            NoxUI::UIPropertyRows dlss = rows.Section("DLSS / Upscaling", false, &m_Expanded);
            if (!r->isDLSSSupported())
                dlss.Note("Not supported (Streamline / NGX unavailable on this GPU or driver).", kWarn);
            else
            {
                dlss.Check("DLSS", [r]() { return r->isDLSSEnabled(); }, [r](bool v) { r->setDLSSEnabled(v); });
                NoxUI::UIPropertyRows on = dlss.Group();
                on.Choice("Mode", { "Off", "DLAA (no upscale, best quality)", "Quality", "Balanced", "Performance", "Ultra Performance" },
                    [r]() { return static_cast<int>(r->getUpscaleMode()); }, [r](int v) { r->setUpscaleMode(static_cast<nri::UpscalerMode>(v)); },
                    "Also the ray tracing resolution: every ray traced effect runs at the render resolution and DLSS upscales the result. "
                    "Per axis: DLAA 100 %, Quality 67 %, Balanced 58 %, Performance 50 %, Ultra Performance 33 %.");
                on.Check("Stable Overlays", [r]() { return r->getStableEditorDepth(); }, [r](bool v) { r->setStableEditorDepth(v); },
                    "Draws the scene's depth and entity IDs again without the camera jitter, so the grid, gizmo lines, collider wireframes "
                    "and picking hold still while DLSS is on.");
                Ref<NoxUI::UIElement> rr = on.Check("Ray Reconstruction", [r]() { return r->isDLSSRayReconstructionEnabled(); },
                    [r](bool v) { r->setDLSSRayReconstructionEnabled(v); },
                    "DLSS 3.5 denoising for reflections and indirect light (hybrid), or the whole image (path tracing). Replaces the NRD "
                    "REBLUR / RELAX denoisers (NRD SIGMA shadows stay).");
                on.EnableWhen(rr, [r]() { return r->isDLSSRayReconstructionSupported(); }, []() { return std::string("not supported here"); });
                on.Text([r]()
                {
                    const nri::Extent2D render = r->getRenderSize();
                    const nri::Extent2D output = r->getViewPortSize();
                    return Format("Rendering %ux%u -> %ux%u", render.width, render.height, output.width, output.height);
                }, kInfo);
                dlss.ShowWhen(on.GetBody(), [r]() { return r->isDLSSEnabled(); });
            }
        }

        // ---- Shadows ----
        {
            NoxUI::UIPropertyRows sh = rows.Section("Shadows", false, &m_Expanded);
            sh.Check("Shadows", [r]() { return r->getShadowsEnabled(); }, [r](bool v) { r->setShadowsEnabled(v); },
                     "Every shadow, whatever renders it (Unreal's Show > Shadows). Off: lights shine everywhere they reach. The path tracer "
                     "always traces its shadows.");
            NoxUI::UIPropertyRows all = sh.Group();
            sh.EnableWhen(all.GetBody(), [r]() { return r->getShadowsEnabled(); });
            const auto rtAvailable = [r]() { return r->getRayTracingEnabled() && r->getRayTracingShadows(); };
            const auto fallback = [r]() { return std::string(r->getRayTracingEnabled() ? "shadow maps: Ray Tracing Shadows off" : "shadow maps: ray tracing off"); };

            Ref<NoxUI::UIElement> sun = all.Choice("Sun Shadows", { "Ray Traced", "Shadow Maps" }, [r]() { return static_cast<int>(r->getSunShadowMode()); },
                [r](int v) { r->setSunShadowMode(static_cast<Renderer::SunShadowMode>(v)); },
                "Ray Traced: the sun's soft shadow from one ray a pixel towards its disc, denoised by NRD SIGMA (while ray tracing and its "
                "Ray Tracing Shadows are on -- otherwise shadow maps). Shadow Maps: cascaded shadow maps.");
            // (Not greyed: the choice stays; the note says what runs instead.)
            all.Text([=]() { return r->getSunShadowMode() == Renderer::SunShadowMode::RayTraced && !rtAvailable() ? "Sun: " + fallback() : std::string(); }, kWarn);
            NoxUI::UIPropertyRows csm = all.Group();
            all.ShowWhen(csm.GetBody(), [=]() { return r->getSunShadowMode() == Renderer::SunShadowMode::ShadowMaps || !rtAvailable(); });
            ValueChoice(csm, "Resolution", { 1024, 2048, 4096 }, [r]() { return r->getShadowMapResolution(); }, [r](uint32_t v) { r->setShadowMapResolution(v); },
                        "Texels per side of each cascade.");
            csm.Int("Cascades", [r]() { return static_cast<int>(r->getShadowCascadeCount()); }, [r](int v) { r->setShadowCascadeCount(static_cast<uint32_t>(v)); },
                    1, static_cast<int>(shaderio::MaxShadowCascades));
            csm.Drag("Distance (m)", [r]() { return WorldUnits::ToMeters(r->getShadowDistance()); }, [r](float v) { r->setShadowDistance(WorldUnits::FromMeters(v)); },
                     1.0f, 1.0f, 2000.0f, "{:.0f}", "How far from the camera the cascades reach; past it the sun is unshadowed.");
            csm.Slider("Distribution Exponent", [r]() { return r->getShadowCascadeExponent(); }, [r](float v) { r->setShadowCascadeExponent(v); }, 1.0f, 6.0f, "{:.1f}",
                       "Each cascade this many times longer than the one before; 1 splits the distance evenly.");
            csm.Slider("Transition", [r]() { return r->getShadowCascadeBlend(); }, [r](float v) { r->setShadowCascadeBlend(v); }, 0.0f, 0.5f, "{:.2f}",
                       "The last part of each cascade that dithers into the next one.");
            csm.Check("Cache Cascades", [r]() { return r->getShadowCascadeCaching(); }, [r](bool v) { r->setShadowCascadeCaching(v); },
                      "A cascade is drawn again only when its view or the sun moved, or something inside it changed (UE's cached shadow maps).");
            csm.Choice("Filter", { "PCF", "PCSS (Contact Hardening)" }, [r]() { return static_cast<int>(r->getShadowFilter()); },
                       [r](int v) { r->setShadowFilter(static_cast<uint32_t>(v)); },
                       "PCF: a fixed kernel. PCSS: the penumbra grows with the distance to the caster by the sun's angular radius.");
            csm.Slider("Filter Radius (texels)", [r]() { return r->getShadowFilterTexels(); }, [r](float v) { r->setShadowFilterTexels(v); }, 0.0f, 8.0f, "{:.1f}");
            Ref<NoxUI::UIElement> penumbra = csm.Slider("Max Penumbra (texels)", [r]() { return r->getShadowMaxFilterTexels(); },
                [r](float v) { r->setShadowMaxFilterTexels(v); }, 1.0f, 64.0f, "{:.0f}", "The widest penumbra PCSS searches and filters.");
            csm.ShowWhen(penumbra, [r]() { return r->getShadowFilter() == 1; });
            csm.Slider("Depth Bias", [r]() { return r->getShadowDepthBias(); }, [r](float v) { r->setShadowDepthBias(v); }, 0.0f, 4.0f, "{:.2f}");
            csm.Slider("Normal Bias", [r]() { return r->getShadowNormalBias(); }, [r](float v) { r->setShadowNormalBias(v); }, 0.0f, 4.0f, "{:.2f}");
            csm.Slider("Slope Bias", [r]() { return r->getShadowSlopeBias(); }, [r](float v) { r->setShadowSlopeBias(v); }, 0.0f, 8.0f, "{:.2f}",
                       "Against shadow acne, in texels of each cascade. Too much detaches shadows from their casters (peter panning).");

            all.Choice("Local Light Shadows", { "Ray Traced", "Shadow Maps" }, [r]() { return static_cast<int>(r->getLocalShadowMode()); },
                [r](int v) { r->setLocalShadowMode(static_cast<Renderer::LocalShadowMode>(v)); },
                "Spot, point, rect and disc lights. Ray Traced: shadow rays in the lighting. Shadow Maps: the most important lights get "
                "shadow maps in one atlas, sized by how big they are on screen.");
            all.Text([=]() { return r->getLocalShadowMode() == Renderer::LocalShadowMode::RayTraced && !rtAvailable() ? "Local lights: " + fallback() : std::string(); }, kWarn);
            NoxUI::UIPropertyRows local = all.Group();
            all.ShowWhen(local.GetBody(), [=]() { return r->getLocalShadowMode() == Renderer::LocalShadowMode::ShadowMaps || !rtAvailable(); });
            local.Int("Shadowed Lights", [r]() { return static_cast<int>(r->getLocalShadowMaxLights()); }, [r](int v) { r->setLocalShadowMaxLights(static_cast<uint32_t>(v)); },
                      1, static_cast<int>(Renderer::MaxLocalShadowLights));
            ValueChoice(local, "Atlas Size", { 2048, 4096, 8192 }, [r]() { return r->getLocalShadowAtlasSize(); }, [r](uint32_t v) { r->setLocalShadowAtlasSize(v); });
            ValueChoice(local, "Max Resolution", { 256, 512, 1024, 2048 }, [r]() { return r->getLocalShadowMaxResolution(); },
                        [r](uint32_t v) { r->setLocalShadowMaxResolution(v); },
                        "Texels per side of a light's view at the most (a light filling the screen); halved for all when the atlas is full.");
            local.Text([r]() { return Format("This frame: %u lights, %u views", r->getLocalShadowLightCount(), r->getLocalShadowViewCount()); });

            all.Check("Contact Shadows", [r]() { return r->getContactShadowsEnabled(); }, [r](bool v) { r->setContactShadowsEnabled(v); },
                      "A short ray towards the light per pixel through the depth buffer (UE's contact shadows): small shadows a shadow map "
                      "misses. Only on lights shadowed by shadow maps.");
            NoxUI::UIPropertyRows contact = all.Group();
            all.ShowWhen(contact.GetBody(), [r]() { return r->getContactShadowsEnabled(); });
            contact.Slider("Length (m)", [r]() { return WorldUnits::ToMeters(r->getContactShadowLength()); },
                           [r](float v) { r->setContactShadowLength(WorldUnits::FromMeters(v)); }, 0.01f, 2.0f, "{:.2f}");
            contact.Int("Steps", [r]() { return static_cast<int>(r->getContactShadowSamples()); }, [r](int v) { r->setContactShadowSamples(static_cast<uint32_t>(v)); }, 4, 32);
        }

        // ---- Path Tracing ----
        {
            NoxUI::UIPropertyRows pt = rows.Section("Path Tracing", false, &m_Expanded);
            pt.Check("Path Tracing", [r]() { return r->isPathTracingEnabled(); }, [r](bool v) { r->setPathTracingEnabled(v); },
                     "Unified light transport: the whole image path traced (the ground truth to compare against).");
            RefInt(pt, "Max Bounces", &r->getPathTracerMaxBounces(), 1, 32,
                   "Surfaces a path may reach (1: direct light only). 18 is RTXPT's Balanced preset.");
            RefInt(pt, "Light Samples", &r->getPathTracerLightSamples(), 0, 4,
                   "Light samples (next-event estimation) at every surface a path reaches. 0: none -- only paths that hit a light find it "
                   "(emissive surfaces, the sky, the sun's disc, rect lights; point and spot lights give no light then).");
            pt.Choice("Path Tracer Dispatch", { "Ray Tracing Pipeline", "Compute (Inline Ray Queries)" },
                [r]() { return r->getPathTracerCompute() ? 1 : 0; }, [r](int v) { r->getPathTracerCompute() = v == 1; },
                "How the path tracer traces its rays; both give the same image. Ray Tracing Pipeline: TraceRay with hit and miss shaders. "
                "Compute: the same shaders in one compute shader with inline ray queries -- the default on D3D12, where the ray tracing "
                "pipeline hangs the GPU.");
            NoxUI::UIPropertyRows on = pt.Group();
            pt.ShowWhen(on.GetBody(), [r]() { return r->isPathTracingEnabled(); });
            on.Check("ReSTIR PT", [r]() { return r->getReSTIRPTEnabled(); }, [r](bool v) { r->setReSTIRPTEnabled(v); },
                     "RTXDI's ReSTIR PT reservoir resampling instead of the plain path tracer's accumulation (1 candidate combined via RIS).");
            NoxUI::UIPropertyRows restir = on.Group();
            on.ShowWhen(restir.GetBody(), [r]() { return r->getReSTIRPTEnabled(); });
            restir.Check("Temporal Resampling (experimental)", [r]() { return r->getReSTIRPTTemporalEnabled(); }, [r](bool v) { r->setReSTIRPTTemporalEnabled(v); },
                         "Reuses last frame's path. Known issue: the lighting rotates even with a still camera -- for testing only.");
            RefInt(restir, "Initial Samples", &r->getReSTIRPTNumInitialSamples(), 1, 16, "Candidate paths per pixel, RIS-combined into one reservoir.");
            RefInt(restir, "Max Bounces", &r->getReSTIRPTMaxBounceDepth(), 1, 16);
            RefInt(restir, "NEE Light Samples", &r->getReSTIRPTNumNeeSamples(), 0, 8, "Light samples per bounce surface; 0: no next-event estimation.");
            on.Check("ReSTIR DI / GI With the Path Tracer", [r]() { return r->getPathTracerUsesRTXDI(); }, [r](bool v) { r->setPathTracerUsesRTXDI(v); },
                     "RTXPT-style hybrid: RTXDI ReSTIR DI / GI run first and the path tracer uses their primary-surface lighting.");
            NoxUI::UIPropertyRows hybrid = on.Group();
            on.ShowWhen(hybrid.GetBody(), [r]() { return r->getPathTracerUsesRTXDI(); });
            hybrid.Choice("PT Diffuse GI", { "Off (path tracer only)", "ReSTIR GI (primary diffuse indirect)" },
                [r]() { return r->getDiffuseGIMode() == 2 ? 1 : 0; },
                [r](int v) { r->setDiffuseGIMode(v == 1 ? 2u : 0u); r->setDDGIEnabled(false); });
            hybrid.Choice("PT Direct Lighting", { "Path Tracer NEE", "ReSTIR DI (primary direct)" },
                [r]() { return static_cast<int>(r->getDirectLightingMode()); }, [r](int v) { r->setDirectLightingMode(static_cast<uint32_t>(v)); });
            on.Check("Progressive Ground Truth", [r]() { return r->isPathTracingAccumulation(); }, [r](bool v) { r->setPathTracingAccumulation(v); },
                     "Accumulates while nothing moves (not used by ReSTIR PT).");
            on.Text([r]() { return r->isDLSSRayReconstructionEnabled() ? std::string("Denoiser: DLSS Ray Reconstruction (active)") : std::string(); }, kGood);
            on.Choice("PT Denoiser", { "Off (raw / progressive)", "NRD REBLUR", "NRD RELAX" },
                [r]() { return static_cast<int>(r->getNRDPTDenoiser()); }, [r](int v) { r->setNRDPTDenoiser(static_cast<Nox::NRDDiffuseDenoiser>(v)); },
                "REBLUR or RELAX turn DLSS Ray Reconstruction and progressive accumulation off (the denoiser handles stability).");
        }

        // ---- Raster effects (not with the path tracer) ----
        const auto notPathTracing = [r]() { return !r->isPathTracingEnabled(); };
        {
            NoxUI::UIPropertyRows ao = rows.Section("Ambient Occlusion", false, &m_Expanded);
            rows.ShowWhen(ao.GetCard(), notPathTracing);
            ao.Note("RTAO is under Hybrid Ray Tracing; both feed the same occlusion (one at a time).");
            ao.Check("SSAO", [r]() { return r->getSSAOEnabled(); }, [r](bool v) { r->setSSAOEnabled(v); }, "Screen-space ambient occlusion. Turning RTAO on turns it off.");
            Ref<NoxUI::UIElement> method = ao.Choice("Method", { "Classic SSAO", "GTAO" }, [r]() { return static_cast<int>(r->getSSAOMethod()); },
                [r](int v) { r->setSSAOMethod(static_cast<Renderer::SSAOMethod>(v)); },
                "Classic: random points in a hemisphere tested against the depth buffer. GTAO (UE5's): the horizon on both sides of a few "
                "slices per pixel -- closer to RTAO for the same radius.");
            ao.ShowWhen(method, [r]() { return r->getSSAOEnabled(); });
            NoxUI::UIPropertyRows gtao = ao.Group();
            ao.ShowWhen(gtao.GetBody(), [r]() { return r->getSSAOEnabled() && r->getSSAOMethod() == Renderer::SSAOMethod::GTAO; });
            gtao.Int("Slices", [r]() { return static_cast<int>(r->getGTAOSlices()); }, [r](int v) { r->setGTAOSlices(static_cast<uint32_t>(v)); }, 1, 4);
            gtao.Int("Steps per Side", [r]() { return static_cast<int>(r->getGTAOSteps()); }, [r](int v) { r->setGTAOSteps(static_cast<uint32_t>(v)); }, 2, 12);
            NoxUI::UIPropertyRows shared = ao.Group();
            ao.ShowWhen(shared.GetBody(), [r]() { return r->getSSAOEnabled() || r->getRTAOEnabled(); });
            shared.Slider("Radius (cm)", [r]() { return WorldUnits::ToCentimeters(r->getSSAORadius()); }, [r](float v) { r->setSSAORadius(WorldUnits::FromCentimeters(v)); },
                          1.0f, 200.0f, "{:.1f}");
            shared.Slider("Power", [r]() { return r->getSSAOPower(); }, [r](float v) { r->setSSAOPower(v); }, 0.5f, 4.0f, "{:.2f}");
        }
        {
            NoxUI::UIPropertyRows ssr = rows.Section("Screen-Space Reflections", false, &m_Expanded);
            rows.ShowWhen(ssr.GetCard(), notPathTracing);
            ssr.Check("Screen-Space Reflections", [r]() { return r->getSSREnabled(); }, [r](bool v) { r->setSSREnabled(v); },
                      "The mirror reflection marched through the depth buffer; where it finds nothing the probes / environment show. Runs "
                      "wherever ray traced reflections do not; with them on, they take precedence.");
            ssr.Text([r]() { return r->getSSREnabled() && r->getRayTracingEnabled() && r->getRayTracingReflections() ? std::string("Not used now: ray traced reflections run.") : std::string(); }, kWarn);
            NoxUI::UIPropertyRows on = ssr.Group();
            ssr.ShowWhen(on.GetBody(), [r]() { return r->getSSREnabled(); });
            on.Drag("Max Distance (m)", [r]() { return WorldUnits::ToMeters(r->getSSRMaxDistance()); }, [r](float v) { r->setSSRMaxDistance(WorldUnits::FromMeters(v)); },
                    0.5f, 0.5f, 1000.0f, "{:.1f}");
            on.Slider("Thickness (m)", [r]() { return WorldUnits::ToMeters(r->getSSRThickness()); }, [r](float v) { r->setSSRThickness(WorldUnits::FromMeters(v)); },
                      0.01f, 2.0f, "{:.2f}", "How far behind a surface on screen the ray may pass and still count as hitting it.");
            on.Int("Max Iterations", [r]() { return static_cast<int>(r->getSSRMaxSteps()); }, [r](int v) { r->setSSRMaxSteps(static_cast<uint32_t>(v)); }, 8, 256,
                   "Steps of the Hi-Z trace; 128 reaches across the screen.");
            on.Slider("Max Roughness", [r]() { return r->getSSRMaxRoughness(); }, [r](float v) { r->setSSRMaxRoughness(v); }, 0.05f, 1.0f, "{:.2f}",
                      "Surfaces up to this roughness get SSR (fading over the last 0.15); rougher ones keep the reflection probes.");
        }
        {
            NoxUI::UIPropertyRows gi = rows.Section("Raster GI, Fog and Probes", false, &m_Expanded);
            rows.ShowWhen(gi.GetCard(), notPathTracing);
            gi.Check("Raster Probe Volume (Diffuse GI)", [r]() { return r->getDiffuseGIMode() == 4; },
                     [r](bool v) { r->setDiffuseGIMode(v ? 4u : 0u); r->setDDGIEnabled(false); },
                     "Diffuse light from around without ray tracing: probes capture what surrounds them once and are lit again every frame. "
                     "Without an Irradiance Volume entity the probes cover the whole scene.");
            NoxUI::UIPropertyRows probe = gi.Group();
            gi.ShowWhen(probe.GetBody(), [r]() { return r->getDiffuseGIMode() == 4; });
            probe.Int("Captures per Frame", [r]() { return static_cast<int>(r->getIrradianceProbesPerFrame()); },
                      [r](int v) { r->setIrradianceProbesPerFrame(static_cast<uint32_t>(v)); }, 1, static_cast<int>(shaderio::MaxIrradianceProbesPerFrame));
            probe.Int("Relights per Frame", [r]() { return static_cast<int>(r->getIrradianceRelightsPerFrame()); },
                      [r](int v) { r->setIrradianceRelightsPerFrame(static_cast<uint32_t>(v)); }, 16, static_cast<int>(shaderio::MaxIrradianceRelightsPerFrame));
            probe.Button("Recapture", [r]() { r->recaptureIrradianceVolume(); }, "Capture every probe again, in turn.");
            probe.Text([r]()
            {
                if (r->getIrradianceProbeCount() == 0)
                    return std::string();
                return Format("%s: %u probes, %u captured, %u waiting", r->isIrradianceVolumeAutomatic() ? "Automatic (whole scene)" : "Irradiance Volume entity",
                              r->getIrradianceProbeCount(), r->getIrradianceProbesCaptured(), r->getIrradianceProbesQueued());
            });

            gi.Check("Screen-Space GI (SSGI)", [r]() { return r->getSSGIEnabled(); }, [r](bool v) { r->setSSGIEnabled(v); },
                     "A few rays per pixel through the depth buffer: the light bouncing off what is on screen, denoised over frames. Not used "
                     "with the RT GI modes.");
            NoxUI::UIPropertyRows ssgi = gi.Group();
            gi.ShowWhen(ssgi.GetBody(), [r]() { return r->getSSGIEnabled(); });
            ssgi.Int("Rays per Pixel", [r]() { return static_cast<int>(r->getSSGIRays()); }, [r](int v) { r->setSSGIRays(static_cast<uint32_t>(v)); }, 1, 8);
            ssgi.Int("Steps per Ray", [r]() { return static_cast<int>(r->getSSGISteps()); }, [r](int v) { r->setSSGISteps(static_cast<uint32_t>(v)); }, 4, 64);
            ssgi.Slider("Distance (m)", [r]() { return WorldUnits::ToMeters(r->getSSGIDistance()); }, [r](float v) { r->setSSGIDistance(WorldUnits::FromMeters(v)); },
                        0.1f, 50.0f, "{:.1f}");

            gi.Check("Volumetric Fog", [r]() { return r->getVolumetricFogEnabled(); }, [r](bool v) { r->setVolumetricFogEnabled(v); },
                     "The Environment Light's Height Fog as a volume, lit by the sun and point / spot lights with their shadows (light "
                     "shafts). Needs the Height Fog on.");
            NoxUI::UIPropertyRows fog = gi.Group();
            gi.ShowWhen(fog.GetBody(), [r]() { return r->getVolumetricFogEnabled(); });
            fog.Drag("Distance (m)", [r]() { return WorldUnits::ToMeters(r->getVolumetricFogDistance()); },
                     [r](float v) { r->setVolumetricFogDistance(WorldUnits::FromMeters(v)); }, 1.0f, 1.0f, 1000.0f, "{:.0f}");
            fog.Int("Slices", [r]() { return static_cast<int>(r->getVolumetricFogSlices()); }, [r](int v) { r->setVolumetricFogSlices(static_cast<uint32_t>(v)); }, 16, 128);
            fog.Slider("Scattering Distribution", [r]() { return r->getVolumetricFogAnisotropy(); }, [r](float v) { r->setVolumetricFogAnisotropy(v); },
                       -0.9f, 0.9f, "{:.2f}", "Henyey-Greenstein g: above 0 the fog glows most looking towards a light.");
            fog.Slider("Extinction Scale", [r]() { return r->getVolumetricFogExtinctionScale(); }, [r](float v) { r->setVolumetricFogExtinctionScale(v); },
                       0.0f, 10.0f, "{:.2f}");
            glm::vec3* albedo = &m_FogAlbedo;
            NoxUI::AppendSync(*fog.GetBody(), [r, albedo]() { *albedo = r->getVolumetricFogAlbedo(); }); // (the section's sync runs before its rows')
            fog.Color3("Albedo", albedo, [r, albedo]() { r->setVolumetricFogAlbedo(*albedo); });
            fog.Check("Point / Spot Lights", [r]() { return r->getVolumetricFogLocalLights(); }, [r](bool v) { r->setVolumetricFogLocalLights(v); });

            gi.Check("Reflection Probes", [r]() { return r->getReflectionProbesEnabled(); }, [r](bool v) { r->setReflectionProbesEnabled(v); },
                     "Placed probes (Add Component > Reflection Probe): inside their boxes the environment reflection comes from the probe's capture.");
            gi.Text([r]() { return Format("%u reflection probes in use", r->getReflectionProbeCount()); });
        }

        // ---- Hybrid Ray Tracing ----
        {
            NoxUI::UIPropertyRows rt = rows.Section("Hybrid Ray Tracing", false, &m_Expanded);
            rows.ShowWhen(rt.GetCard(), notPathTracing);
            rt.Check("Hybrid Ray Tracing", [r]() { return r->getRayTracingEnabled(); }, [r](bool v) { r->setRayTracingEnabled(v); });
            NoxUI::UIPropertyRows on = rt.Group();
            rt.ShowWhen(on.GetBody(), [r]() { return r->getRayTracingEnabled(); });
            const auto rrActive = [r]()
            {
                return r->isDLSSSupported() && r->isDLSSEnabled() && r->getUpscaleMode() != NRI_UPSCALER_MODE_OFF &&
                       r->isDLSSRayReconstructionSupported() && r->isDLSSRayReconstructionEnabled();
            };
            Ref<NoxUI::UIElement> resolution = on.Choice("Ray Tracing Resolution", { "Full (every pixel)", "Half (a quarter of the rays)" },
                [=]() { return r->getRayTracingResolutionDivisor() > 1 && !rrActive() ? 1 : 0; },
                [r](int v) { r->setRayTracingResolutionDivisor(v == 1 ? 2u : 1u); },
                "Only the ray traced effects: Half traces one pixel of every 2 x 2 block (a different one each frame) for reflections, "
                "RT sun shadows and RTAO -- about a quarter of their cost, a little softer.");
            on.EnableWhen(resolution, [=]() { return !rrActive(); }, []() { return std::string("full: Ray Reconstruction"); });
            on.Check("Ray Tracing Shadows", [r]() { return r->getRayTracingShadows(); }, [r](bool v) { r->setRayTracingShadows(v); },
                     "Lights set to Ray Traced shadows trace shadow rays. Off: they use shadow maps.");
            Ref<NoxUI::UIElement> perPixel = on.Int("Shadowed Lights per Pixel (0: all)", [r]() { return static_cast<int>(r->getShadowRayLightLimit()); },
                [r](int v) { r->setShadowRayLightLimit(static_cast<uint32_t>(v)); }, 0, 8,
                "Each pixel traces shadow rays only for the lights that light it the most. The sun's soft shadow is always traced.");
            on.ShowWhen(perPixel, [r]() { return r->getRayTracingShadows(); });
            on.Check("Ray Tracing Reflections", [r]() { return r->getRayTracingReflections(); }, [r](bool v) { r->setRayTracingReflections(v); },
                     "Reflections traced against the scene (what is off screen too). Off: Screen-Space Reflections stand in.");
            on.Check("Ray Tracing Ambient Occlusion (RTAO)", [r]() { return r->getRTAOEnabled(); }, [r](bool v) { r->setRTAOEnabled(v); },
                     "Real rays for ambient occlusion (turns SSAO off); same radius and power settings.");
            on.Check("Ray Tracing Culling", [r]() { return r->getRayTracingCulling(); }, [r](bool v) { r->setRayTracingCulling(v); },
                     "Leaves far, small objects out of what rays can hit (Unreal's ray tracing culling).");
            NoxUI::UIPropertyRows cull = on.Group();
            on.ShowWhen(cull.GetBody(), [r]() { return r->getRayTracingCulling(); });
            cull.Drag("Culling Radius (m)", [r]() { return WorldUnits::ToMeters(r->getRayTracingCullRadius()); },
                      [r](float v) { r->setRayTracingCullRadius(WorldUnits::FromMeters(v)); }, 1.0f, 0.0f, 100000.0f, "{:.0f}");
            cull.Slider("Culling Angle (deg)", [r]() { return r->getRayTracingCullAngle(); }, [r](float v) { r->setRayTracingCullAngle(v); }, 0.0f, 10.0f, "{:.2f}");
            on.Text([r]() { return Format("TLAS: %u instances, %u culled, %u BLAS evicted", r->getTlasInstanceCount(), r->getTlasCulledCount(), r->getEvictedBlasCount()); });
            Ref<NoxUI::UIElement> async = on.Check("Async Compute (Screen-Probe GI)", [r]() { return r->getAsyncComputeEnabled(); }, [r](bool v) { r->setAsyncComputeEnabled(v); },
                "The screen-probe GI runs on the GPU's compute queue beside the SSAO / RTAO / RT shadow passes. The image is the same; "
                "compare the GPU frame time.");
            on.EnableWhen(async, [r]() { return r->isAsyncComputeAvailable(); }, []() { return std::string("no compute-only queue on this GPU"); });

            on.Choice("Diffuse Global Illumination", { "Off (IBL ambient)", "DDGI (probe volumes)", "ReSTIR GI (RTXDI)", "Screen Probes (Lumen-style)",
                                                       "Raster Probe Volume (no rays)" },
                [r]() { return static_cast<int>(r->getDiffuseGIMode()); },
                [r](int mode)
                {
                    r->setDiffuseGIMode(static_cast<uint32_t>(mode));
                    r->setDDGIEnabled(mode == 1 || mode == 3); // (the screen probes read DDGI at their hits: the second bounce)
                    // Debug View "Indirect Diffuse GI Only" keeps ReSTIR GI running with no GI chosen: cleared with it.
                    if (mode == 0 && r->getDebugMode() == 16)
                        r->setDebugMode(0);
                },
                "Off: constant or cubemap IBL. DDGI: an irradiance probe grid. ReSTIR GI: screen-space reservoir resampling. Screen Probes: "
                "Lumen's final gather (DDGI underneath for the second bounce) -- the default of every preset.");

            NoxUI::UIPropertyRows screen = on.Group();
            on.ShowWhen(screen.GetBody(), [r]() { return r->getDiffuseGIMode() == 3; });
            screen.Text([r]() { const nri::Extent2D p = r->getScreenProbeCounts(); return Format("Screen probes: %ux%u (%u probes, 64 rays each)", p.width, p.height, p.width * p.height); }, kInfo);
            uint32_t* tile = &r->getScreenProbeTileSize();
            ValueChoice(screen, "Screen Probe Spacing (px)", { 8, 12, 16, 24, 32 }, [tile]() { return *tile; }, [tile](uint32_t v) { *tile = v; },
                        "One probe per this many pixels each way. Finer: more detail in corners, and more rays (cost grows with the square).");
            RefInt(screen, "Spatial Filter Passes", &r->getScreenProbeFilterPasses(), 0, 3, "Each pass averages a probe with its neighbours on the same surface.");
            RefInt(screen, "Temporal Frames", &r->getScreenProbeMaxHistory(), 1, 64, "Frames accumulated at most per pixel.");
            screen.Button("Reset Screen Probe History", [r]() { r->resetScreenProbeHistory(); });

            NoxUI::UIPropertyRows restir = on.Group();
            on.ShowWhen(restir.GetBody(), [r]() { return r->isReSTIRGIRequested(); });
            restir.Note("ReSTIR GI (RTXDI)", kGood);
            RefFloat(restir, "Spatial Radius (px)", &r->getReSTIRGISpatialRadius(), 4.0f, 64.0f, "{:.1f}");
            RefInt(restir, "Spatial Samples", &r->getReSTIRGINumSpatialSamples(), 1, 8);
            RefInt(restir, "Max History Length (M)", &r->getReSTIRGIMaxHistoryLength(), 1, 32);
            RefFloat(restir, "Normal Threshold", &r->getReSTIRGINormalThreshold(), 0.1f, 0.99f, "{:.2f}");
            RefFloat(restir, "Depth Threshold", &r->getReSTIRGIDepthThreshold(), 0.01f, 0.5f, "{:.2f}");
            RefBool(restir, "Boiling Filter", &r->getReSTIRGIEnableBoilingFilter());
            bool* boiling = &r->getReSTIRGIEnableBoilingFilter();
            float* strength = &r->getReSTIRGIBoilingFilterStrength();
            Ref<NoxUI::UIElement> strengthRow = restir.Slider("Boiling Filter Strength", [strength]() { return *strength; }, [strength](float v) { *strength = v; }, 0.0f, 1.0f, "{:.2f}");
            restir.ShowWhen(strengthRow, [boiling]() { return *boiling; });

            NoxUI::UIPropertyRows ddgi = on.Group();
            on.ShowWhen(ddgi.GetBody(), [r]() { return r->getDiffuseGIMode() == 1 || r->getDiffuseGIMode() == 3 || r->getDebugMode() == 17; });
            ddgi.Text([r]()
            {
                return Format("DDGI grid: %ux%ux%u (%u probes, %u rays per probe)", r->getDDGIProbeCountX(), r->getDDGIProbeCountY(), r->getDDGIProbeCountZ(),
                              r->getDDGIProbeCountTotal(), r->getDDGIRaysPerProbe());
            }, kInfo);
            bool* follow = &r->getDDGIFollowCamera();
            RefBool(ddgi, "Follow Camera", follow, "The probe volume is centred on the camera and scrolls with it in whole cells. Off: a fixed grid.");
            NoxUI::UIPropertyRows following = ddgi.Group();
            ddgi.ShowWhen(following.GetBody(), [follow]() { return *follow; });
            {
                InspectorCardBuilder vecs{ following.GetBody(), {}, context };
                vecs.AddVec3("Probe Spacing (m)", &r->getDDGISpacingMeters(), 0.05f, {}, "Between the probes of the first (finest) cascade.");
            }
            RefInt(following, "Cascades", &r->getDDGICascadeSetting(), 1, 4);
            RefFloat(following, "Cascade Scale", &r->getDDGICascadeScale(), 1.5f, 4.0f, "{:.1f}x",
                     "Nested volumes around the camera, each this much wider (and coarser) than the one inside it.");
            following.Text([r]()
            {
                const glm::vec3 cells(r->getDDGIProbeCountX() - 1, r->getDDGIProbeCountY() - 1, r->getDDGIProbeCountZ() - 1);
                const glm::vec3 first = r->getDDGISpacingMeters() * cells;
                const int cascades = static_cast<int>(std::clamp(r->getDDGICascadeSetting(), 1u, 4u));
                const glm::vec3 last = first * std::pow(std::max(r->getDDGICascadeScale(), 1.5f), static_cast<float>(cascades - 1));
                return Format("Around the camera: %.0f x %.0f x %.0f m fine, %.0f x %.0f x %.0f m in all", first.x, first.y, first.z, last.x, last.y, last.z);
            });
            NoxUI::UIPropertyRows fixedGrid = ddgi.Group();
            ddgi.ShowWhen(fixedGrid.GetBody(), [follow]() { return !*follow; });
            {
                InspectorCardBuilder vecs{ fixedGrid.GetBody(), {}, context };
                vecs.AddVec3("Grid Origin", &r->getDDGIGridOrigin(), 0.1f);
                vecs.AddVec3("Grid Spacing", &r->getDDGIGridSpacing(), 0.05f);
            }
            uint32_t* stride = &r->getDDGIUpdateStride();
            ddgi.Choice("Probe Updates", { "Every probe, every frame", "Half of them a frame", "A quarter a frame" },
                [stride]() { return *stride >= 4 ? 2 : (*stride >= 2 ? 1 : 0); }, [stride](int v) { *stride = v == 0 ? 1u : (v == 1 ? 2u : 4u); },
                "How many probes trace rays each frame. Probes the volume just scrolled over always update at once.");
            RefBool(ddgi, "Probe Relocation", &r->getDDGIRelocation(), "Probes inside walls or against a surface move to free space nearby.");
            RefFloat(ddgi, "Temporal Hysteresis", &r->getDDGIHysteresis(), 0.80f, 0.995f, "{:.3f}");
            RefFloat(ddgi, "Normal Bias", &r->getDDGINormalBias(), 0.0f, 1.0f, "{:.2f}");
            RefFloat(ddgi, "Debug Sphere Radius", &r->getDDGIDebugSphereRadius(), 0.02f, 0.5f, "{:.2f}");
            RefBool(ddgi, "Probe Spheres X-Ray", &r->getDDGIDebugXRay(), "The probe spheres (Debug View: DDGI Probe Spheres) through walls.");
            ddgi.Button("Reset Volume to Defaults", [r]() { r->resetDDGIGridToDefaults(); });
            ddgi.Button("Reset DDGI History", [r]() { r->resetDDGIHistory(); });

            on.Choice("Direct Lighting", { "Every light (brute force)", "ReSTIR DI (RTXDI)" },
                [r]() { return static_cast<int>(r->getDirectLightingMode()); }, [r](int v) { r->setDirectLightingMode(static_cast<uint32_t>(v)); },
                "Every light: each with its own shadow ray per pixel -- exact, noise-free, the cost grows with the lights reaching a pixel. "
                "ReSTIR DI: one light per pixel resampled from all of them, then denoised (UE5's MegaLights idea) -- the emissive meshes' "
                "triangles among them (lamps, lit windows: their direct light per pixel, steadier than through the screen probes). Turn "
                "Denoising > DI Denoiser on with it.");
            NoxUI::UIPropertyRows di = on.Group();
            on.ShowWhen(di.GetBody(), [r]() { return r->getDirectLightingMode() == 1; });
            RefInt(di, "Local Light Samples", &r->getReSTIRDINumLocalLightSamples(), 1, 16);
            RefInt(di, "Infinite Light Samples", &r->getReSTIRDINumInfiniteLightSamples(), 1, 2);
            RefFloat(di, "Spatial Radius (px)", &r->getReSTIRDISpatialRadius(), 4.0f, 64.0f, "{:.1f}");
            RefInt(di, "Spatial Samples", &r->getReSTIRDINumSpatialSamples(), 1, 8);
            RefInt(di, "Max History Length (M)", &r->getReSTIRDIMaxHistoryLength(), 1, 32);
            RefFloat(di, "Normal Threshold", &r->getReSTIRDINormalThreshold(), 0.1f, 0.99f, "{:.2f}");
            RefFloat(di, "Depth Threshold", &r->getReSTIRDIDepthThreshold(), 0.01f, 0.5f, "{:.2f}");
            bool* regir = &r->getReGIREnabled();
            RefBool(di, "ReGIR (world-space light grid)", regir, "Power-weighted local-light candidates baked into a world-space grid.");
            NoxUI::UIPropertyRows grid = di.Group();
            di.ShowWhen(grid.GetBody(), [regir]() { return *regir; });
            RefFloat(grid, "Cell Size (m)", &r->getReGIRCellSize(), 0.1f, 8.0f, "{:.2f}");
            {
                InspectorCardBuilder vecs{ grid.GetBody(), {}, context };
                vecs.AddVec3("Grid Center", &r->getReGIRGridCenter(), 0.5f);
            }
            RefFloat(grid, "Cell Jitter", &r->getReGIRSamplingJitter(), 0.0f, 1.0f, "{:.2f}",
                     "How much each cell's sampled position wobbles every frame. Lower it if lights flicker with few lights.");
        }

        // ---- Denoising ----
        {
            NoxUI::UIPropertyRows dn = rows.Section("Denoising", false, &m_Expanded);
            const auto hybrid = [r]() { return !r->isPathTracingEnabled() && r->getRayTracingEnabled(); };
            Ref<NoxUI::UIElement> sigma = dn.Check("NRD SIGMA (shadows)", [r]() { return r->getNRDShadowsEnabled(); }, [r](bool v) { r->setNRDShadowsEnabled(v); },
                "Penumbra filtering for the RT shadow mask; works with DLSS-SR and DLSS-RR. Off: raw 1 spp shadows.");
            dn.ShowWhen(sigma, [=]() { return hybrid() && r->getRayTracingShadows(); });
            Ref<NoxUI::UIElement> rrNote = dn.Text([r]() { return r->isDLSSRayReconstructionEnabled() ? std::string("Reflections: DLSS Ray Reconstruction (active)") : std::string(); }, kGood);
            dn.ShowWhen(rrNote, [=]() { return hybrid() && r->getRayTracingReflections(); });
            Ref<NoxUI::UIElement> refl = dn.Choice("Reflection Denoiser", { "Off (raw 1 spp)", "NRD REBLUR", "NRD RELAX" },
                [r]() { return static_cast<int>(r->getNRDReflectionDenoiser()); }, [r](int v) { r->setNRDReflectionDenoiser(static_cast<Nox::NRDReflectionDenoiser>(v)); },
                "REBLUR or RELAX turn DLSS Ray Reconstruction off (no double filtering).");
            dn.ShowWhen(refl, [=]() { return hybrid() && r->getRayTracingReflections(); });
            Ref<NoxUI::UIElement> giDenoiser = dn.Choice("GI Denoiser", { "Off (raw 1 spp)", "NRD REBLUR Diffuse", "NRD RELAX Diffuse" },
                [r]() { return static_cast<int>(r->getNRDGIDenoiser()); }, [r](int v) { r->setNRDGIDenoiser(static_cast<Nox::NRDDiffuseDenoiser>(v)); });
            dn.ShowWhen(giDenoiser, [=]() { return hybrid() && r->isReSTIRGIRequested(); });
            Ref<NoxUI::UIElement> diDenoiser = dn.Choice("DI Denoiser", { "Off (raw 1 spp)", "NRD REBLUR Diffuse", "NRD RELAX Diffuse" },
                [r]() { return static_cast<int>(r->getNRDDIDenoiser()); }, [r](int v) { r->setNRDDIDenoiser(static_cast<Nox::NRDDiffuseDenoiser>(v)); });
            dn.ShowWhen(diDenoiser, [=]() { return hybrid() && r->getDirectLightingMode() == 1; });
            dn.Text([=]()
            {
                const bool any = hybrid() && (r->getRayTracingShadows() || r->getRayTracingReflections() || r->isReSTIRGIRequested() || r->getDirectLightingMode() == 1);
                if (any)
                    return std::string();
                return std::string(r->isPathTracingEnabled() ? "The path tracer's denoiser is under Path Tracing."
                                                             : "Turn on Ray Tracing Shadows, Reflections or ReSTIR GI (Hybrid Ray Tracing) for their denoisers.");
            });
        }

        // ---- Streaming ----
        {
            NoxUI::UIPropertyRows st = rows.Section("Streaming", false, &m_Expanded);
            st.Header("Textures");
            st.Text([]()
            {
                auto assets = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr;
                if (!assets)
                    return std::string();
                const TextureStreamer::Stats& s = assets->GetTextureStreamer().GetStats();
                return Format("%zu streamed textures | resident %.0f MB | wanted %.0f MB | pool %.0f MB", s.Textures, s.ResidentBytes / kMB, s.WantedBytes / kMB,
                              s.PoolBytes == UINT64_MAX ? 0.0 : s.PoolBytes / kMB);
            });
            st.Int("Texture Pool Override (MB, 0: budget)",
                []() { auto a = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr; return a ? static_cast<int>(a->GetTextureStreamer().GetPoolOverride() / (1024 * 1024)) : 0; },
                [](int v) { if (auto a = Project::GetActive() ? Project::GetActive()->GetEditorAssetManager() : nullptr) a->GetTextureStreamer().SetPoolOverride(static_cast<uint64_t>(v) * 1024 * 1024); },
                0, 4096, "A smaller pool forces mip reduction (to test what a smaller GPU sees).");

            st.Header("Geometry");
            GeometryStreamer* geometry = &r->GetGeometryStreamer();
            st.Check("Stream Geometry Pages", [geometry]() { return geometry->IsEnabled(); }, [geometry](bool v) { geometry->SetEnabled(v); },
                     "Meshes loaded from now on read only their root pages, the rest as the LOD asks. Off: meshes loaded from now on read every page.");
            st.Text([geometry]()
            {
                const GeometryStreamer::Stats& s = geometry->GetStats();
                return Format("%u meshes | pages %u of %u resident, %u loading | resident %.1f MB, wanted %.1f MB, root %.1f MB, pool %.0f MB | "
                              "requests %u (dropped %u), waiting %u, loaded %llu, evicted %llu",
                              s.Meshes, s.ResidentPages, s.Pages, s.LoadingPages, s.ResidentBytes / kMB, s.WantedBytes / kMB, s.RootBytes / kMB, s.PoolBytes / kMB,
                              s.Requests, s.DroppedRequests, s.WantedNotResident, static_cast<unsigned long long>(s.PageLoads), static_cast<unsigned long long>(s.PageEvictions));
            });
            st.Text([geometry]()
            {
                const GeometryStreamer::Stats& s = geometry->GetStats();
                return s.WantedBytes > s.PoolBytes ? Format("Pool too small: %.0f MB wanted, %.0f MB pool (coarser geometry near the camera)", s.WantedBytes / kMB, s.PoolBytes / kMB)
                                                   : std::string();
            }, kWarn);
            st.Int("Geometry Pool Override (MB, 0: budget)", [geometry]() { return static_cast<int>(geometry->GetPoolOverride() / (1024 * 1024)); },
                   [geometry](int v) { geometry->SetPoolOverride(static_cast<uint64_t>(v) * 1024 * 1024); }, 0, 4096);

            st.Header("Virtual Textures");
            VirtualTextureStreamer* virtualTextures = &r->GetVirtualTextures();
            st.Check("Stream Virtual Textures", [virtualTextures]() { return virtualTextures->IsEnabled(); }, [virtualTextures](bool v) { virtualTextures->SetEnabled(v); },
                     "Textures cooked as virtual loaded from now on stream by 128 x 128 tile. Off: they load mip streamed.");
            st.Text([virtualTextures]()
            {
                const VirtualTextureStreamer::Stats& s = virtualTextures->GetStats();
                return Format("%u virtual textures | tiles %u of %u resident, %u loading, %u wanted | %u pools, %u tiles, %.0f MB | requests %u (dropped %u), "
                              "waiting %u, loaded %llu, evicted %llu",
                              s.Textures, s.ResidentTiles, s.Tiles, s.LoadingTiles, s.WantedTiles, s.Pools, s.PoolTiles, s.PoolBytes / kMB, s.Requests,
                              s.DroppedRequests, s.WantedNotResident, static_cast<unsigned long long>(s.TileLoads), static_cast<unsigned long long>(s.TileEvictions));
            });
            // Applied when the value is let go (each change makes the pools again).
            st.Int("Pool Tiles per Side", [virtualTextures]() { return static_cast<int>(virtualTextures->GetPoolTilesPerSide()); },
                   [virtualTextures](int v) { if (static_cast<uint32_t>(v) != virtualTextures->GetPoolTilesPerSide()) virtualTextures->SetPoolTilesPerSide(static_cast<uint32_t>(v)); },
                   4, static_cast<int>(VirtualTextureStreamer::MaxPoolTilesPerSide),
                   "Each format's pool holds this many tiles squared. Changing it makes the pools again: every tile streams in again.", true);
        }

        // ---- Post effects (the editor's) ----
        {
            NoxUI::UIPropertyRows post = rows.Section("Post Effects", false, &m_Expanded,
                "The editor's own, used while no Post Process Volume in the scene applies. Not saved: a volume's settings are what is saved "
                "with the scene and ships (Unreal's Post Process Volume).");
            post.Text([r]() { return r->HasScenePostProcess() ? std::string("A Post Process Volume in the scene applies now: edit its settings in the Inspector.") : std::string(); }, kWarn);
            NoxUI::UIPropertyRows effects = post.Group();
            AddPostProcessSettingsRows(effects, r->getPostEffects());
            post.EnableWhen(effects.GetBody(), [r]() { return !r->HasScenePostProcess(); });
            post.Note("Not saved: add a Post Process Volume to the scene to keep these and ship them with the game.");
        }

        // ---- Diagnostics ----
        {
            NoxUI::UIPropertyRows diag = rows.Section("Diagnostics", false, &m_Expanded);
#if NOX_PROFILE_STATS
            diag.Check("Nox Stats Window (F3)",
                []() { StatsOverlayLayer* stats = Application::Get().GetLayer<StatsOverlayLayer>(); return stats && stats->IsVisible(); },
                [](bool v) { if (StatsOverlayLayer* stats = Application::Get().GetLayer<StatsOverlayLayer>()) stats->SetVisible(v); },
                "Frame times, memory, then Audio, GPU passes, CPU scopes, Memory and Counters.");
            diag.Button("Reset Stats", []() { Profiler::Get().ResetStats(); });
            diag.Button("Save Stats Report", [r]() { SaveStatsReport(*r); }, "Writes the stats report next to Nox.log.");
#endif
            diag.Button("Dump Frame Graph", [hooks]() { if (hooks.DumpFrameGraph) hooks.DumpFrameGraph(); },
                        "Writes SceneUpdate.dot (the scene's update systems) next to Nox.log; open it with GraphViz.");
        }
        return m_Root;
    }

    void RendererSettingsPanel::Update()
    {
    }
}
